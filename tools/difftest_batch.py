"""Differential test of many lifted functions at once: each function of an
address range runs on random float buffers under Unicorn (the original x86
code, on this machine) and as lifted C in one probe binary on the device,
and every buffer, EAX and ST(0) are compared.

    python tools/difftest_batch.py A0029000 A002E000 [--cases 3] [--deep 3] [--only A002C55E,...]

Each stack argument (the count from the function's `ret n`, else 3) and ECX
point to their own 96-byte buffer of floats in [-2, 2]. A function the
reference cannot run (Windows API, a fault, a runaway loop) or the probe
cannot (a callee deeper than --deep, an indirect call outside the set) is
skipped, not failed. Floats compare to a relative 1e-4 (x87 keeps 80 bits,
the lift 64).
"""
import argparse
import random
import re
import struct
import subprocess
import sys
from pathlib import Path

import capstone
import pefile
from unicorn import UC_ARCH_X86, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_android  # noqa: E402
import probe_function as P  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "generated/input/Sims2EP9.exe"
BUFS = 0x01000000
BUF_SIZE = 96
STACK_TOP = 0x00800000
RETURN = 0x00FF0000


class Image:
    def __init__(self):
        self.pe = pefile.PE(str(IMAGE), fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def arg_count(self, va, size):
        """From the function's `ret n` (stdcall/thiscall), else 3."""
        code = self.data[va - self.base:va - self.base + size]
        for insn in self.md.disasm(code, va):
            if insn.mnemonic == "ret" and insn.op_str:
                return min(int(insn.op_str, 0) // 4, 6)
        return 3


def cases_for(rng, nargs, count):
    out = []
    for _ in range(count):
        bufs = [struct.pack("<%df" % (BUF_SIZE // 4), *[round(rng.uniform(-2, 2), 3) for _ in range(BUF_SIZE // 4)])
                for _ in range(nargs + 1)]          # the last one is ECX's
        out.append(bufs)
    return out


def run_unicorn(img, va, bufs):
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    mu.mem_map(img.base, (len(img.data) + 0xFFFF) & ~0xFFFF)
    mu.mem_write(img.base, img.data)
    mu.mem_map(0x00700000, 0x00200000)
    mu.mem_map(BUFS, 0x00100000)
    addrs = [BUFS + 128 * i for i in range(len(bufs))]
    for a, b in zip(addrs, bufs):
        mu.mem_write(a, b)
    esp = STACK_TOP
    for a in reversed(addrs[:-1]):
        esp -= 4
        mu.mem_write(esp, struct.pack("<I", a))
    esp -= 4
    mu.mem_write(esp, struct.pack("<I", RETURN))
    mu.reg_write(X.UC_X86_REG_ESP, esp)
    mu.reg_write(X.UC_X86_REG_ECX, addrs[-1])
    mu.reg_write(X.UC_X86_REG_FPCW, 0x027F)
    try:
        mu.emu_start(va, RETURN, count=300_000)
    except UcError:
        pass
    if mu.reg_read(X.UC_X86_REG_EIP) != RETURN:
        return None
    status = mu.reg_read(X.UC_X86_REG_FPSW)
    depth = (8 - ((status >> 11) & 7)) & 7
    st0 = None
    if depth:
        scratch = BUFS + 0xF0000
        mu.mem_write(scratch, b"\xdd\x15" + struct.pack("<I", scratch + 0x100))
        mu.emu_start(scratch, scratch + 6)
        st0 = struct.unpack("<d", bytes(mu.mem_read(scratch + 0x100, 8)))[0]
    return {"eax": mu.reg_read(X.UC_X86_REG_EAX), "st0": st0, "depth": depth,
            "esp": (mu.reg_read(X.UC_X86_REG_ESP) - (esp + 4)) & 0xFFFFFFFF,
            "bufs": [bytes(mu.mem_read(a, BUF_SIZE)) for a in addrs]}


HARNESS_HEAD = r'''
#include <setjmp.h>
static jmp_buf probe_jb;
static int probe_active;
static void probe_abort(int why) { if (probe_active) longjmp(probe_jb, why); printf("ABORT %d outside a test\n", why); exit(9); }
typedef struct { uint32_t va; recomp_func_t fn; } probe_entry;
extern const probe_entry probe_table[];
'''


def build_probe(functions, deep):
    """C source for all functions and their callees `deep` calls down."""
    bodies = {}
    for name in functions:
        try:
            bodies[name] = P.extract(name)[0]
        except SystemExit:
            continue
    frontier = list(bodies)
    for _ in range(deep):
        frontier = sorted({c for f in frontier for c in P.SUB.findall(bodies[f])} - bodies.keys())
        for callee in frontier:
            try:
                bodies[callee] = P.extract(callee)[0]
            except SystemExit:
                pass
    callees = sorted({c for b in bodies.values() for c in P.SUB.findall(b)} - bodies.keys())
    runtime = P.RUNTIME
    runtime = runtime.replace('void void_lift_trap(uint32_t address, const char *instruction) { printf("TRAP %08X %s\\n", address, instruction); exit(3); }',
                              'void void_lift_trap(uint32_t address, const char *instruction) { (void)address; (void)instruction; probe_abort(3); }')
    table = ",\n".join(f"{{0x{name[4:]}u, {name}}}" for name in bodies)
    runtime = runtime.replace('recomp_func_t void_resolve(uint32_t va) { printf("RESOLVE %08X (not in probe)\\n", va); exit(4); }',
                              "recomp_func_t void_resolve(uint32_t va) { for (size_t i = 0; probe_table[i].fn; ++i) "
                              "if (probe_table[i].va == va) return probe_table[i].fn; probe_abort(4); return 0; }")
    stubs = "\n".join(f"void {c}(void) {{ probe_abort(5); }}" for c in callees)
    stubs += "\n" + "\n".join(f"void {b}(void);" for b in bodies)
    return runtime, stubs, bodies, table


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("start")
    parser.add_argument("end")
    parser.add_argument("--cases", type=int, default=3)
    parser.add_argument("--deep", type=int, default=3)
    parser.add_argument("--only", default="")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--serial", default="192.168.1.103:5555")
    args = parser.parse_args()
    img = Image()
    lo, hi = int(args.start, 16), int(args.end, 16)
    catalog = __import__("json").loads((ROOT / "research/ida/Sims2EP9.ida.json").read_text())["functions"]
    funcs = [(f["address"], f["size"]) for f in catalog if lo <= f["address"] < hi]
    if args.only:
        wanted = {int(x, 16) for x in args.only.split(",")}
        funcs = [f for f in funcs if f[0] in wanted]
    rng = random.Random(args.seed)
    tests = []                       # (name, case index, bufs, reference)
    for va, size in funcs:
        name = f"sub_{va:08X}"
        nargs = img.arg_count(va, size)
        for k, bufs in enumerate(cases_for(rng, nargs, args.cases)):
            ref = run_unicorn(img, va, bufs)
            if ref is not None:
                tests.append((name, k, bufs, ref))
    names = sorted({t[0] for t in tests})
    print(f"{len(funcs)} functions, {len(names)} runnable under Unicorn, {len(tests)} cases", flush=True)
    if not tests:
        return
    runtime, stubs, bodies, table = build_probe(names, args.deep)
    tests = [t for t in tests if t[0] in bodies]
    lines = []
    for i, (name, k, bufs, ref) in enumerate(tests):
        init = ", ".join(f"0x{b:02X}" for buf in bufs for b in buf)
        lines.append(f"    {{ static const uint8_t init[] = {{ {init} }}; run_case({i}, {name}, init, {len(bufs)}); }}")
    harness = f'''
const probe_entry probe_table[] = {{ {table}, {{0, 0}} }};
static uint8_t *saved_image;
static uint32_t image_base, image_size;
static void run_case(int id, recomp_func_t fn, const uint8_t *init, int nbufs)
{{
    memcpy((void *)ADDR(image_base), saved_image, image_size);
    /* Unicorn starts each case on zeroed buffers and stack: so must we. */
    memset((void *)ADDR(0x01000000u), 0, 0x100000);
    memset((void *)ADDR(0x00700000u), 0, 0x200000);
    for (int b = 0; b < nbufs; ++b) memcpy((void *)ADDR(0x01000000u + 128u * (uint32_t)b), init + {BUF_SIZE} * b, {BUF_SIZE});
    g_eax = g_ebx = g_edx = g_esi = g_edi = g_ebp = 0;
    g_fp_bias = 0; memset(&g_x87, 0, sizeof g_x87); g_stp = g_stbuf + X87_CENTRE; memset(g_xmm, 0, sizeof g_xmm); g_fpu_cw = 0x027F; g_mxcsr = 0x1F80;
    g_esp = 0x00800000u;
    for (int b = nbufs - 2; b >= 0; --b) {{ g_esp -= 4; MEM32(g_esp) = 0x01000000u + 128u * (uint32_t)b; }}
    g_esp -= 4; MEM32(g_esp) = RECOMP_RETADDR;
    uint32_t entry = g_esp + 4;
    g_ecx = 0x01000000u + 128u * (uint32_t)(nbufs - 1);
    int why = setjmp(probe_jb);
    if (why) {{ probe_active = 0; printf("T %d SKIP %d\\n", id, why); return; }}
    probe_active = 1;
    fn();
    probe_active = 0;
    printf("T %d EAX=%08X ESP=%08X DEPTH=%d ST0=%.17g", id, g_eax, g_esp - entry, X87_DEPTH(g_stp), X87_DEPTH(g_stp) ? g_st[0] : 0.0);
    for (int b = 0; b < nbufs; ++b) {{
        printf(" B");
        for (int i = 0; i < {BUF_SIZE}; ++i) printf("%02X", MEM8(0x01000000u + 128u * (uint32_t)b + (uint32_t)i));
    }}
    printf("\\n");
}}
int main(void)
{{
    void *want = (void *)(uintptr_t)VOID_GUEST_BASE;
    if (mmap(want, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != want) return 2;
    load_image("{P.DEVICE_DIR}/probe_image.bin");
    image_base = 0x{img.base:08X}u; image_size = 0x{len(img.data):X}u;
    saved_image = malloc(image_size); memcpy(saved_image, (void *)ADDR(image_base), image_size);
{chr(10).join(lines)}
    return 0;
}}
'''
    out = ROOT / "build/probe"
    out.mkdir(parents=True, exist_ok=True)
    source = out / "difftest_batch.c"
    source.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n' + HARNESS_HEAD + runtime + stubs + "\n" +
                      "\n".join(bodies.values()) + "\n" + harness)
    binary = out / "difftest_batch"
    subprocess.run([str(P.CLANG), "--target=aarch64-linux-android30", "-O1", "-w", "-fno-strict-aliasing", "-fwrapv",
                    "-ffp-contract=off", "-fno-math-errno", "-DRECOMP_GENERATED_CODE", "-DRECOMP_TRACE",
                    "-DRECOMP_LOCAL_REGS", "-DRECOMP_PLAIN_MEMORY", "-I" + str(ROOT / "android/runtime"),
                    str(source), "-o", str(binary), "-lm"], check=True)
    env = dict(__import__("os").environ, MSYS_NO_PATHCONV="1")
    image_file = ROOT / build_android.MODULE_IMAGES["sims2"]
    for local, remote in ((image_file, "probe_image.bin"), (binary, "difftest_batch")):
        subprocess.run([str(P.ADB), "-s", args.serial, "push", str(local), f"{P.DEVICE_DIR}/{remote}"],
                       check=True, capture_output=True, env=env)
    result = subprocess.run([str(P.ADB), "-s", args.serial, "shell",
                             f"chmod 755 {P.DEVICE_DIR}/difftest_batch && {P.DEVICE_DIR}/difftest_batch"],
                            capture_output=True, text=True, env=env)
    got = {}
    for line in result.stdout.splitlines():
        m = re.match(r"T (\d+) (.*)", line)
        if m:
            got[int(m.group(1))] = m.group(2)
    bad = set()
    skipped = 0
    for i, (name, k, bufs, ref) in enumerate(tests):
        line = got.get(i)
        if line is None or line.startswith("SKIP"):
            skipped += 1
            continue
        m = re.match(r"EAX=([0-9A-F]{8}) ESP=([0-9A-F]{8}) DEPTH=(-?\d+) ST0=(\S+)((?: B[0-9A-F]*)*)", line)
        if not m:
            skipped += 1
            continue
        eax, espd, depth, st0 = int(m.group(1), 16), int(m.group(2), 16), int(m.group(3)), float(m.group(4))
        outs = [bytes.fromhex(b) for b in m.group(5).split(" B")[1:]]
        problems = []
        if espd != ref["esp"]:
            problems.append(f"esp {espd:#x} vs {ref['esp']:#x}")
        if ref["depth"] and abs(st0 - ref["st0"]) > 1e-4 * max(1.0, abs(ref["st0"])):
            problems.append(f"st0 {st0!r} vs {ref['st0']!r}")
        for b, (mine, theirs) in enumerate(zip(outs, ref["bufs"])):
            if mine == theirs:
                continue
            fm = struct.unpack("<%df" % (BUF_SIZE // 4), mine)
            ft = struct.unpack("<%df" % (BUF_SIZE // 4), theirs)
            diffs = [j for j in range(len(fm)) if not (fm[j] == ft[j] or
                     (abs(fm[j] - ft[j]) <= 1e-4 * max(1.0, abs(ft[j]))) or (fm[j] != fm[j] and ft[j] != ft[j]))]
            if diffs:
                problems.append(f"buf{b}[{','.join(map(str, diffs[:6]))}] lifted {[round(fm[j], 5) for j in diffs[:6]]} "
                                f"x86 {[round(ft[j], 5) for j in diffs[:6]]}")
        if eax != ref["eax"] and not (BUFS <= eax < BUFS + 0x10000 and BUFS <= ref["eax"] < BUFS + 0x10000):
            # a float returned through EAX bits, or a pointer: report only if it is not a buffer address
            problems.append(f"eax {eax:08X} vs {ref['eax']:08X}")
        if problems:
            bad.add(name)
            print(f"DIFF {name} case {k}: " + "; ".join(problems), flush=True)
    print(f"{len(tests)} cases: {len(tests) - skipped - len([1 for t in tests if t[0] in bad])} matched, "
          f"{skipped} skipped, differing functions: {sorted(bad)}")


if __name__ == "__main__":
    main()
