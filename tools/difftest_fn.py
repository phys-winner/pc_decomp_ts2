"""Does one lifted function compute what the original does? Runs the original
x86 code of Sims2EP9.exe under Unicorn on this machine and the lifted C on the
device (tools/probe_function.py), with the same arguments and buffers, and
compares the buffers, EAX and ST(0).

    python tools/difftest_fn.py A002C55E --args "zero:16 f32:0.1,0.2,0.3,0.927" --deep 3

Arguments are pushed left to right as the first, second... stack argument:
`f32:a,b,c` places floats in a buffer and passes its address, `f64:...`
doubles, `zero:N` N zero bytes, a number is passed as is; --ecx sets `this`
(f32:/f64:/zero: there too). --st pushes x87 values first. Functions that
call the Windows API are out of reach (Unicorn has no Windows).
"""
import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

import pefile
from unicorn import UC_ARCH_X86, UC_HOOK_MEM_UNMAPPED, UC_MODE_32, Uc, UcError
from unicorn import x86_const as X

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "generated/input/Sims2EP9.exe"
BUFFERS = 0x01000000          # probe_function.py places buffers here too
STACK_TOP = 0x00800000
RETURN = 0x00FF0000            # where the function returns to (unmapped code: stops Unicorn)


def parse_args_list(text):
    """[(kind, payload)] for each token; buffers get their guest address in order."""
    out, cursor = [], BUFFERS
    for token in text.split():
        if token.startswith(("f32:", "f64:", "zero:")):
            kind, body = token.split(":", 1)
            if kind == "zero":
                data = bytes(int(body, 0))
            else:
                values = [float(v) for v in body.split(",")]
                data = struct.pack("<%d%s" % (len(values), "f" if kind == "f32" else "d"), *values)
            out.append(("buf", cursor, data))
            cursor += (len(data) + 79) & ~15          # probe_function.py's zero:N stride
        else:
            out.append(("int", int(token, 0), None))
    return out


def run_unicorn(va, items, ecx_item, st_values):
    pe = pefile.PE(str(IMAGE), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    image = pe.get_memory_mapped_image()
    mu = Uc(UC_ARCH_X86, UC_MODE_32)
    size = (len(image) + 0xFFFF) & ~0xFFFF
    mu.mem_map(base, size)
    mu.mem_write(base, image)
    mu.mem_map(0x00700000, 0x00200000)            # stack
    mu.mem_map(BUFFERS, 0x00100000)
    for item in items + ([ecx_item] if ecx_item else []):
        if item[0] == "buf":
            mu.mem_write(item[1], item[2])
    esp = STACK_TOP
    args = [it[1] for it in items]
    for value in reversed(args):
        esp -= 4
        mu.mem_write(esp, struct.pack("<I", value & 0xFFFFFFFF))
    esp -= 4
    mu.mem_write(esp, struct.pack("<I", RETURN))
    mu.reg_write(X.UC_X86_REG_ESP, esp)
    mu.reg_write(X.UC_X86_REG_EBP, 0)
    mu.reg_write(X.UC_X86_REG_ECX, ecx_item[1] if ecx_item else 0)
    mu.reg_write(X.UC_X86_REG_FPCW, 0x027F)
    # x87 pushes for --st: run fld from a scratch buffer.
    if st_values:
        code_at = BUFFERS + 0xF0000
        data_at = BUFFERS + 0xF8000
        code = b""
        for i, v in enumerate(st_values):
            mu.mem_write(data_at + 8 * i, struct.pack("<d", v))
            code += b"\xdd\x05" + struct.pack("<I", data_at + 8 * i)     # fld qword [abs]
        mu.mem_write(code_at, code)
        mu.emu_start(code_at, code_at + len(code))
    try:
        mu.emu_start(va, RETURN, count=50_000_000)
    except UcError as error:
        eip = mu.reg_read(X.UC_X86_REG_EIP)
        if eip != RETURN:
            raise SystemExit(f"unicorn stopped at {eip:08X}: {error}")
    result = {"eax": mu.reg_read(X.UC_X86_REG_EAX), "esp_delta": mu.reg_read(X.UC_X86_REG_ESP) - (esp + 4)}
    # ST(0) through fstp to memory.
    code_at = BUFFERS + 0xF0000
    data_at = BUFFERS + 0xF8000
    status = mu.reg_read(X.UC_X86_REG_FPSW)
    depth = (8 - ((status >> 11) & 7)) & 7
    if depth or st_values:
        mu.mem_write(code_at, b"\xdd\x15" + struct.pack("<I", data_at))   # fst qword [abs]
        mu.emu_start(code_at, code_at + 6)
        result["st0"] = struct.unpack("<d", bytes(mu.mem_read(data_at, 8)))[0]
    result["buffers"] = {it[1]: bytes(mu.mem_read(it[1], len(it[2])))
                         for it in items + ([ecx_item] if ecx_item else []) if it[0] == "buf"}
    return result


def run_device(va, items, ecx_item, st_values, deep):
    tokens = []
    for kind, value, data in items:
        tokens.append(f"zero:{len(data)}" if kind == "buf" else str(value))
    cmd = [sys.executable, str(ROOT / "tools/probe_function.py"), va, "--deep", str(deep)]
    if tokens:
        cmd += ["--args", " ".join(tokens)]
    pokes = []
    for kind, value, data in items + ([ecx_item] if ecx_item else []):
        if kind == "buf":
            for o in range(0, len(data), 4):
                word = struct.unpack_from("<I", data.ljust((len(data) + 3) & ~3, b"\0"), o)[0]
                pokes += ["--poke", f"0x{value + o:08X}:0x{word:08X}"]
            cmd += ["--dump", f"0x{value:08X}:{len(data)}"]
    cmd += pokes
    if ecx_item:
        cmd += ["--ecx", str(ecx_item[1])]
    for v in st_values:
        cmd += ["--st", repr(v)]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    result = {"raw": out}
    m = re.search(r"EAX=([0-9A-F]{8})", out)
    result["eax"] = int(m.group(1), 16) if m else None
    m = re.search(r"ST0=(\S+)", out)
    if m:
        result["st0"] = float(m.group(1))
    result["buffers"] = {}
    for line in out.splitlines():
        m = re.match(r"([0-9A-F]{8}):((?: [0-9A-F]{2})*)", line)
        if m:
            result["buffers"][int(m.group(1), 16)] = bytes(int(b, 16) for b in m.group(2).split())
    return result


def floats(data):
    n = len(data) // 4
    return [round(v, 6) for v in struct.unpack("<%df" % n, data[:4 * n])]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("va")
    parser.add_argument("--args", default="")
    parser.add_argument("--ecx", default=None)
    parser.add_argument("--st", action="append", default=[], type=float)
    parser.add_argument("--deep", type=int, default=4)
    a = parser.parse_args()
    va = int(a.va, 16)
    items = parse_args_list(a.args)
    ecx_item = None
    if a.ecx:
        if a.ecx.startswith(("f32:", "f64:", "zero:")):
            # after the argument buffers
            used = max([it[1] + len(it[2]) for it in items if it[0] == "buf"], default=BUFFERS)
            ecx_item = parse_args_list(a.ecx)[0]
            ecx_item = ("buf", (used + 80 + 15) & ~15, ecx_item[2])
        else:
            ecx_item = ("int", int(a.ecx, 0), None)
    ref = run_unicorn(va, items, ecx_item, a.st)
    got = run_device(a.va, items, ecx_item, a.st, a.deep)
    same = True
    if ref["eax"] != got["eax"]:
        print(f"EAX differs: x86 {ref['eax']:08X}, lifted {got['eax']}")
        same = False
    if "st0" in ref and abs(ref["st0"] - got.get("st0", float("nan"))) > 1e-9 * max(1, abs(ref["st0"])):
        print(f"ST0 differs: x86 {ref['st0']!r}, lifted {got.get('st0')!r}")
        same = False
    for address, data in ref["buffers"].items():
        other = got["buffers"].get(address)
        if other != data:
            same = False
            print(f"buffer {address:08X} differs:\n  x86    {floats(data)}\n  lifted {floats(other or b'')}")
    if same:
        print("same:", {k: (hex(v) if isinstance(v, int) else v) for k, v in ref.items() if k in ("eax", "st0")},
              {hex(k): floats(v) for k, v in ref["buffers"].items()})
    else:
        print(got["raw"][-600:])


if __name__ == "__main__":
    main()
