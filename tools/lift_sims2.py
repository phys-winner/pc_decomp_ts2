"""Lift Sims2EP9.exe (and ijl15.dll) into C for the ARM64 host.

    python tools/lift_sims2.py [--module Sims2EP9|ijl15] [--catalog-only]

Inputs are the hash-pinned copies in original/ (port.json); the Steam
installation is never written. Each module is lifted at its own preferred
base (Sims2EP9.exe 0xA0000000, ijl15.dll 0x60000000), so no rebasing is
needed and the host maps the images exactly where the code expects them.

The function catalog is IDA's (tools/ida_catalog.py), closed by the lifter's
own call-graph walk over every code pointer in data and code
(liftlib.collect_functions). Output: generated/<prefix>-x86/*.c, the module
header, report.json, static-traps.json and functions.json; the merged
dispatch table goes to generated/dispatch/recomp_dispatch.c.
"""
import argparse
import bisect
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

import pefile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from liftlib import (ROOT, CHUNK_BITS, collect_functions, emit_dispatch, emit_functions,  # noqa: E402
                     lift_body, write_chunk, write_header)

CONFIG = json.loads((ROOT / "port.json").read_text())


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def module_spec(name):
    for module in CONFIG["modules"]:
        if module["name"] == name:
            return module
    raise SystemExit(f"unknown module {name}")


NO_CATALOG = False
OUT_SUFFIX = ""
JOBS = 4


def load_catalog(module, image):
    """IDA function starts inside code sections (config/<name>.functions.json)."""
    if NO_CATALOG:
        print(f"{module['name']}: no catalog, call-graph closure from entry/exports/pointers only", flush=True)
        return []
    config = ROOT / f"config/{module['name']}.functions.json"
    source = ROOT / module["ida_catalog"]
    if source.is_file():
        data = json.loads(source.read_text())
        pe = pefile.PE(data=image, fast_load=True)
        base = pe.OPTIONAL_HEADER.ImageBase
        code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
                for s in pe.sections if s.Characteristics & 0x20 and s.Name.rstrip(b"\0") != b".bind"]
        rows = [{"address": f["address"], "size": f["size"]} for f in data["functions"]
                if any(a <= f["address"] < b for a, b in code)]
        rows.sort(key=lambda r: r["address"])
        config.write_text(json.dumps(rows, indent=0) + "\n")
        print(f"{module['name']}: catalog {len(rows)} IDA functions ({data['function_count']} total)", flush=True)
        return rows
    if config.is_file():
        return json.loads(config.read_text())
    raise SystemExit(f"{module['name']}: no catalog ({source} missing); run tools/ida_catalog.py")


class TrustedEntries:
    """Which code-shaped pointers found in data or immediates become entries.

    IDA's function starts always do. IDA leaves some functions that only data
    names without a function (the Steamworks accessor init at 0xA0C2A389 sits
    right after a `ret`, referenced from a {init, counter, slot} table), so a
    pointer also counts when the byte before it ends a function (`ret`,
    `ret n`, int3 or nop padding, `jmp rel32`). Hashes and IDs that land in
    .text by chance rarely land right after a terminator, so the noise stays
    out."""

    def __init__(self, module, raw, catalog, extra):
        pe = pefile.PE(data=raw, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.image = pe.get_memory_mapped_image()
        self.starts = {r["address"] for r in catalog} | set(extra)
        data = json.loads((ROOT / module["ida_catalog"]).read_text())
        spans = []
        for f in data["functions"]:
            spans.append((f["address"], f["end"]))
            spans.extend((a, b) for a, b in f.get("tails", []))
        spans.sort()
        self.span_lo = [a for a, _ in spans]
        self.span_hi = [b for _, b in spans]
        self.accepted_outside = 0

    def inside_body(self, va):
        i = bisect.bisect_right(self.span_lo, va) - 1
        return i >= 0 and self.span_lo[i] <= va < self.span_hi[i]

    # The multi-byte no-ops MSVC's assembler pads with before an aligned label
    # (`lea esp, [esp+0]`, `lea ecx, [ecx+0]`, ...): the CRT's x87 dispatch
    # tables (0xA0E0C020) name 0xA0848E30, inside fFASN, past such padding.
    PADDING = tuple(bytes.fromhex(h) for h in (
        "8d4900", "8d4000", "8d642400", "8d9b00000000", "8d8000000000", "8da42400000000",
        "0f1f00", "0f1f4000", "0f1f440000", "660f1f440000", "0f1f8000000000", "0f1f840000000000"))

    def boundary_before(self, va):
        off = va - self.base
        if off < 9 or off >= len(self.image):
            return False
        prev = self.image[off - 1]
        if prev in (0xC3, 0xCC, 0x90):
            return True
        if self.image[off - 3] in (0xC2, 0xCA):
            return True
        if self.image[off - 5] == 0xE9:
            return True
        # A loop's backward `jmp short` ending the function before; only when
        # what follows opens a function (0xA00ADDE6, after 0xA00ADC00, which
        # IDA left out): scope tables name handler code after such jumps too.
        if self.image[off - 2] == 0xEB and self.image[off - 1] >= 0x80 and (
                self.image[off:off + 2] == b"\x8b\xff" or self.image[off:off + 3] == b"\x55\x8b\xec"):
            return True
        return va % 4 == 0 and any(self.image[off - len(p):off] == p for p in self.PADDING)

    def __contains__(self, va):
        """IDA sometimes folds real entries into a neighbour's body: a run of
        this-adjusting thunks (`sub ecx, 4; jmp f`) that vtables name one by
        one (sub_A09A76C6 covers dozens), or a thread procedure right after
        the `ret` of the function that starts the thread (0xA0C9E8AB). An
        address that follows a terminator is taken wherever it is. (MSVC
        keeps switch tables in .text, which the data scan does not read, so
        case labels do not come in this way.)"""
        if va in self.starts:
            return True
        if not self.boundary_before(va):
            return False
        if not self.inside_body(va):
            self.accepted_outside += 1
        return True

    def accepts_immediate(self, va, stored=False):
        """A code address stored to memory (`mov [esi+0xA0], 0xA0B2DAF4`: a
        callback in a struct) also counts outside every IDA body: IDA made no
        function of four such callbacks (0xA08A0397 follows a byte table, not
        a terminator), and in .text gaps nothing else is stored that way."""
        return va in self or (stored and not self.inside_body(va))


def static_traps(directory, sources):
    traps = collections.Counter()
    sites = {}
    for name in sources:
        text = (directory / name).read_text()
        for address, instruction in re.findall(r'void_lift_trap\(0x([0-9A-F]+)u, ("(?:[^"\\]|\\.)*")\);', text):
            sites[address] = json.loads(instruction)
    for instruction in sites.values():
        traps[instruction.split(" ")[0]] += 1
    return [{"address": "0x" + a, "instruction": t} for a, t in sorted(sites.items())], traps


def load_tables(module):
    """Declared jump tables (config/<name>.jump-tables.json) and IDA's switch
    tables (tools/ida_switches.py) by `jmp` address, for make_decoder."""
    tables_path = ROOT / f"config/{module['name']}.jump-tables.json"
    declared = {}
    if tables_path.is_file():
        declared = {int(r["address"], 0): (r.get("first_index", 0), r["count"])
                    for r in json.loads(tables_path.read_text())}
    switches = {}
    if module.get("ida_switches") and not NO_CATALOG:
        data = json.loads((ROOT / module["ida_switches"]).read_text())
        switches = {s["jump"]: (s["table"], s["count"]) for s in data["switches"] if s["element_size"] == 4}
    # Code patches (config/<name>.patches.json): bytes the lifter decodes in
    # place of the image's, each checked against what it replaces. The image
    # itself (generated/input, the runtime copy) stays the original.
    patches = []
    patches_path = ROOT / f"config/{module['name']}.patches.json"
    if patches_path.is_file():
        patches = [(int(r["va"], 0), bytes.fromhex(r["old"]), bytes.fromhex(r["new"]))
                   for r in json.loads(patches_path.read_text())]
    return declared, switches, patches


def patched_image(raw, info, patches):
    """The image bytes with the code patches applied (file offsets from the
    section table)."""
    if not patches:
        return raw
    data = bytearray(raw)
    for va, old, new in patches:
        rva = va - info.image_base
        section = next(s for s in info.sections if s.virtual_address <= rva < s.virtual_address + s.raw_size)
        offset = section.raw_offset + rva - section.virtual_address
        if bytes(data[offset:offset + len(old)]) != old:
            raise SystemExit(f"patch at {va:#x}: expected {old.hex()}, image has {data[offset:offset + len(old)].hex()}")
        data[offset:offset + len(new)] = new
    return bytes(data)


def make_decoder(raw, info, tables):
    from liftlib import BoundedDisassembler
    declared, switches, patches = tables
    return BoundedDisassembler(patched_image(raw, info, patches), info.image_base, info.sections,
                               jump_tables=declared, switches=switches)


def lift_module(module, revision):
    raw = (ROOT / module["file"]).read_bytes()
    if sha256(raw) != module["sha256"]:
        raise SystemExit(f"{module['name']}: {module['file']} hash mismatch")
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    if base != int(module["base"], 16):
        raise SystemExit(f"{module['name']}: preferred base {base:#x} differs from port.json")
    prefix = module["prefix"]
    directory = ROOT / f"generated/{prefix}{OUT_SUFFIX}-x86"
    directory.mkdir(parents=True, exist_ok=True)
    input_path = ROOT / "generated/input" / Path(module["file"]).name
    input_path.parent.mkdir(parents=True, exist_ok=True)
    input_path.write_bytes(raw)
    catalog = load_catalog(module, raw)
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
            for s in pe.sections if s.Characteristics & 0x20 and s.Name.rstrip(b"\0") != b".bind"]
    extra = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        extra += [base + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols
                  if not e.forwarder and any(a <= base + e.address < b for a, b in code)]
    if hasattr(pe, "DIRECTORY_ENTRY_TLS"):
        callbacks = pe.DIRECTORY_ENTRY_TLS.struct.AddressOfCallBacks
        image = pe.get_memory_mapped_image()
        for i in range(64):
            value = struct.unpack_from("<I", image, callbacks - base + 4 * i)[0]
            if not value:
                break
            extra.append(value)
    seeds_path = ROOT / f"config/{module['name']}.seeds.json"
    if seeds_path.is_file():
        extra += [int(a, 0) for a in json.loads(seeds_path.read_text())]
    tables = load_tables(module)
    functions, rejected = collect_parallel(module, input_path, catalog, extra, tables, JOBS, module["name"])
    (directory / "functions.json").write_text(json.dumps(functions) + "\n")
    report = {"status": "GENERATED", "module": Path(module["file"]).name, "base": hex(base),
              "entry": hex(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint), "sha256": module["sha256"],
              "catalog_entries": len(catalog), "rejected_non_catalog_entries": len(rejected),
              "pcrecomp_revision": revision, "input_image": str(input_path.relative_to(ROOT).as_posix())}
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    emit_parallel(module, JOBS)
    report = json.loads((directory / "report.json").read_text())
    print("trap census:", report["trap_census"][:40], flush=True)


# --------------------------------------------------------- parallel closure
# The closure decodes every entry once to find the entries it names (calls,
# tails, fallthroughs, code pointers in immediates, EH funclets). Decoding is
# per entry, so workers take batches and hand back what they found; the
# parent keeps the entry set and the queue.
_CLOSURE = {}


def _closure_init(input_path, tables, module, catalog, extra):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from liftlib import BoundedDisassembler, analyze_pe, build_iat_map
    raw = Path(input_path).read_bytes()
    info = analyze_pe(str(input_path))
    imports = build_iat_map(info)
    decoder = make_decoder(raw, info, tables)
    trusted = TrustedEntries(module, raw, catalog, extra) if catalog else None
    _CLOSURE.update(decoder=decoder, imports=imports, trusted=trusted)


def _closure_batch(addresses):
    from capstone.x86 import X86_OP_IMM, X86_OP_MEM
    from liftlib import complete_eh_owner, function_dispatch_targets
    c = _CLOSURE
    decoder, imports, trusted = c["decoder"], c["imports"], c["trusted"]
    results = []
    for address in addresses:
        function = decoder.disassemble_function(address, imports)
        if function is None or not function.blocks:
            results.append((address, False, []))
            continue
        complete_eh_owner(decoder, function, imports)
        found = set()
        for block in function.blocks.values():
            for instruction in block.instructions:
                if instruction.is_jump or instruction.is_call:
                    continue
                for operand in instruction.operands or []:
                    pointer = operand.imm & 0xFFFFFFFF if operand.type == X86_OP_IMM else None
                    if instruction.mnemonic == "lea" and operand.type == X86_OP_MEM:
                        memory = operand.mem
                        if not memory.base and not memory.index:
                            pointer = memory.disp & 0xFFFFFFFF
                    stored = (instruction.mnemonic == "mov" and operand.type == X86_OP_IMM
                              and instruction.operands[0].type == X86_OP_MEM)
                    if (pointer is not None and decoder.is_code_address(pointer)
                            and (trusted is None or trusted.accepts_immediate(pointer, stored))):
                        found.add(pointer)
        for target in function_dispatch_targets(function):
            if decoder.is_code_address(target):
                found.add(target)
        results.append((address, True, sorted(found)))
    return results


def unaligned_tables(data, decoder, trusted):
    """Code pointers in packed tables, at offsets the aligned scan skips: the
    CRT's x87 descriptors (`_expinfo` at 0xA0E16B0A: a counted name, a type
    byte, then the handlers from +0x10) give __trandisp1 its `jmp [ebx]`
    targets, 0xA0857440 among them. Only runs of three or more trusted
    pointers count; single unaligned hits are noise (.reloc, byte tables)."""
    def trusted_at(offset):
        if offset < 0 or offset + 4 > len(data):
            return False
        pointer = struct.unpack_from("<I", data, offset)[0]
        return decoder.is_code_address(pointer) and pointer in trusted
    found = set()
    for offset in range(0, len(data) - 3):
        if offset % 4 == 0 or not trusted_at(offset):
            continue
        after = 1
        while trusted_at(offset + 4 * after):
            after += 1
        before = 1
        while trusted_at(offset - 4 * before):
            before += 1
        if after + before - 1 >= 3:
            found.add(struct.unpack_from("<I", data, offset)[0])
    return found


def collect_parallel(module, input_path, catalog, extra, tables, jobs, label):
    """The same closure as liftlib.collect_functions, across processes."""
    from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from liftlib import BoundedDisassembler, analyze_pe
    raw = Path(input_path).read_bytes()
    info = analyze_pe(str(input_path))
    decoder = make_decoder(raw, info, tables)
    trusted = TrustedEntries(module, raw, catalog, extra) if catalog else None
    entries = {r["address"] for r in catalog} | set(extra)
    catalog_entries = set(entries)
    entries.add(info.image_base + info.entry_point_rva)
    for section in info.sections:
        if section.is_code:
            continue
        data = raw[section.raw_offset:section.raw_offset + section.raw_size]
        for offset in range(0, len(data) - 3, 4):
            pointer = struct.unpack_from("<I", data, offset)[0]
            if decoder.is_code_address(pointer) and (trusted is None or pointer in trusted):
                entries.add(pointer)
        if trusted is not None:
            entries.update(unaligned_tables(data, decoder, trusted))
    pending = sorted(entries)
    functions, rejected = set(), []
    print(f"{label}: closing the call graph from {len(entries)} entries on {jobs} processes", file=sys.stderr, flush=True)
    batch = 48
    with ProcessPoolExecutor(jobs, initializer=_closure_init,
                             initargs=(input_path, tables, module, catalog, extra)) as pool:
        running = set()
        reported = 0
        while pending or running:
            while pending and len(running) < jobs * 3:
                chunk, pending = pending[:batch], pending[batch:]
                running.add(pool.submit(_closure_batch, chunk))
            done, running = wait(running, return_when=FIRST_COMPLETED)
            for future in done:
                for address, ok, found in future.result():
                    if not ok:
                        if address in catalog_entries:
                            raise ValueError(f"Cannot decode catalog entry {address:#x}")
                        rejected.append(hex(address))
                        continue
                    functions.add(address)
                    for target in found:
                        if target not in entries:
                            entries.add(target)
                            pending.append(target)
            if len(functions) - reported >= 5000:
                reported = len(functions)
                print(f"{label}: decoded {len(functions)} functions; {len(pending)} pending", file=sys.stderr, flush=True)
    print(f"{label}: decoded {len(functions)} functions", file=sys.stderr, flush=True)
    return sorted(functions), rejected


# ------------------------------------------------------- parallel emission
# Once a closure has produced generated/<prefix>-x86/functions.json, the C can
# be regenerated (after a lifter change) without walking the call graph
# again: every entry decodes on its own, so worker processes take whole
# 32 KiB buckets each.
_WORKER = {}


def _worker_init(input_path, tables, entries, prefix, directory):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from liftlib import BoundedDisassembler, CheckedLifter, analyze_pe, build_iat_map
    info = analyze_pe(str(input_path))
    imports = build_iat_map(info)
    decoder = make_decoder(Path(input_path).read_bytes(), info, tables)
    lifter = CheckedLifter(iat_map=imports, lifted=set(entries), precise_sbb=True, precise_carry=True)
    _WORKER.update(decoder=decoder, imports=imports, lifter=lifter, prefix=prefix, directory=Path(directory))


def _worker_bucket(job):
    bucket, addresses = job
    w = _WORKER
    bodies, emitted, traps, owners, cold = [], 0, [], 0, 0
    for address in addresses:
        function = w["decoder"].disassemble_function(address, w["imports"])
        if function is None or not function.blocks:
            raise ValueError(f"cannot decode entry {address:#x}")
        body, count = lift_body(w["lifter"], w["decoder"], w["imports"], function, f"sub_{address:08X}")
        bodies.append(body)
        emitted += count
        owners += bool(function.eh_resume)
        cold += function.eh_added_instructions
        if "void_lift_trap(" in body:
            traps.append(hex(address))
    filename = write_chunk(w["directory"], w["prefix"], bucket, bodies)
    return filename, emitted, traps, owners, cold


def emit_parallel(module, jobs):
    import multiprocessing
    prefix = module["prefix"]
    directory = ROOT / f"generated/{prefix}{OUT_SUFFIX}-x86"
    entries = json.loads((directory / "functions.json").read_text())
    input_path = ROOT / "generated/input" / Path(module["file"]).name
    tables = load_tables(module)
    buckets = {}
    for address in entries:
        buckets.setdefault(address >> CHUNK_BITS, []).append(address)
    work = sorted(buckets.items(), key=lambda item: -len(item[1]))
    write_header(directory, f"{prefix}_funcs.h", entries)
    sources, emitted, traps, owners, cold = [], 0, [], 0, 0
    with multiprocessing.Pool(jobs, initializer=_worker_init,
                              initargs=(input_path, tables, entries, prefix, directory)) as pool:
        for done, (filename, count, chunk_traps, chunk_owners, chunk_cold) in enumerate(
                pool.imap_unordered(_worker_bucket, work), 1):
            sources.append(filename)
            emitted += count
            traps += chunk_traps
            owners += chunk_owners
            cold += chunk_cold
            if done % 50 == 0 or done == len(work):
                print(f"{module['name']}: emitted chunk {done}/{len(work)}", file=sys.stderr, flush=True)
    sources.sort()
    known = set(sources)
    for stale in directory.glob(f"{prefix}_*.c"):
        if stale.name not in known:
            stale.unlink()
    trap_list, census = static_traps(directory, sources)
    (directory / "static-traps.json").write_text(json.dumps(trap_list, indent=1) + "\n")
    report = json.loads((directory / "report.json").read_text())
    report.update(functions=len(entries), emitted_instructions_with_overlap=emitted, eh_owners=owners,
                  cold_eh_added_instructions=cold, functions_containing_traps=len(traps), sources=sources,
                  unique_static_trap_sites=len(trap_list), trap_census=census.most_common())
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("module", "functions", "unique_static_trap_sites")}), flush=True)


def finish():
    """One dispatch table over every lifted module."""
    entries, headers = [], []
    for module in CONFIG["modules"]:
        directory = ROOT / f"generated/{module['prefix']}-x86"
        report_path = directory / "report.json"
        if not report_path.is_file():
            continue
        functions = json.loads((directory / "functions.json").read_text())
        if set(entries) & set(functions):
            raise SystemExit(f"{module['name']}: dispatch overlap")
        entries += functions
        headers.append(f"../{module['prefix']}-x86/{module['prefix']}_funcs.h")
    out = ROOT / "generated/dispatch"
    out.mkdir(parents=True, exist_ok=True)
    emit_dispatch(out / "recomp_dispatch.c", sorted(entries), headers)
    print(f"dispatch: {len(entries)} entries from {len(headers)} modules")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--module", action="append", help="Lift only these modules (default: all)")
    parser.add_argument("--finish", action="store_true", help="Only merge the dispatch table")
    parser.add_argument("--no-catalog", action="store_true",
                        help="Seed only from entry, exports and code pointers (a quick pass before IDA finishes)")
    parser.add_argument("--out-suffix", default="", help="Generate into generated/<prefix><suffix>-x86 (trial runs)")
    parser.add_argument("--emit-only", action="store_true",
                        help="Regenerate the C from the last closure's functions.json, in parallel")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2))
    args = parser.parse_args()
    global NO_CATALOG, OUT_SUFFIX, JOBS
    NO_CATALOG = args.no_catalog
    OUT_SUFFIX = args.out_suffix
    JOBS = args.jobs
    revision = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))["pcrecomp"]["revision"]
    if args.emit_only:
        for name in args.module or [m["name"] for m in CONFIG["modules"]]:
            emit_parallel(module_spec(name), args.jobs)
    elif not args.finish:
        names = args.module or [m["name"] for m in CONFIG["modules"]]
        for name in names:
            lift_module(module_spec(name), revision)
    finish()


if __name__ == "__main__":
    main()
