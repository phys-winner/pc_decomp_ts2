"""Build a stand-alone ARM64 probe for ONE lifted function and run it on the
device: the function's C is taken from the generated chunk with the game
build's own transformations (build_android: direct jump tables, local SSE
file), optionally with a print at every label to see the path it takes, and
linked with a tiny runtime (guest window holding the PE image the code was
lifted from, register file, trap stubs) and a harness that calls it.

    python tools/probe_function.py A0826B4B --args "buf:ABCDEF... buf:ABCDEF... 32" [--trace-labels]

Arguments: `buf:TEXT` places TEXT (NUL-terminated) in guest memory and passes
its address (`bufN:TEXT` at N bytes past a 16-byte boundary); `zero:N` passes
N zeroed bytes; a number is passed as is. Buffers start at 0x01000000.
--dump ADDR:LEN prints guest memory after the call, --ecx sets `this`,
--deep N compiles callees N calls deep (deeper ones trap with their name).
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_android  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CLANG = build_android.CLANG
ADB = Path(r"C:\Users\Xilonen\AppData\Local\Android\Sdk\platform-tools\adb.exe")
DEVICE_DIR = "/data/local/tmp/sims2"

RUNTIME = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
VOID_HIDDEN x87_window_t g_x87;
VOID_HIDDEN int g_fp_bias;
VOID_HIDDEN recomp_machine_t g_m = {
    .m_fpu_cw = 0x027F, .m_mxcsr = 0x1F80, .m_fpu_cmp = 1, .m_dispatch_gen = 1, .m_stp = g_x87.v + X87_CENTRE,
};
VOID_HIDDEN uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
VOID_HIDDEN uint64_t g_mm[8];
VOID_HIDDEN uint32_t g_gs_base, g_es_base;
VOID_HIDDEN ptrdiff_t g_mem_base = (ptrdiff_t)VOID_GUEST_BASE;
VOID_HIDDEN uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
VOID_HIDDEN uint32_t g_icall_trace_idx, g_icall_count;
VOID_HIDDEN void (*recomp_yield_hook)(void);
VOID_HIDDEN uint32_t g_enter_trace[1024];
VOID_HIDDEN uint32_t g_enter_idx;
VOID_HIDDEN void *g_eh_frames;
void void_lift_trap(uint32_t address, const char *instruction) { printf("TRAP %08X %s\n", address, instruction); exit(3); }
recomp_func_t void_resolve(uint32_t va) { printf("RESOLVE %08X (not in probe)\n", va); exit(4); }
void void_cpuid(uint32_t leaf, uint32_t subleaf) { (void)leaf; (void)subleaf; g_eax = g_ebx = g_ecx = g_edx = 0; }
void void_fpu_remainder(uint32_t a, int n) { (void)a; (void)n; }
void recomp_eh_push(recomp_ehframe_t *f) { (void)f; }
void recomp_eh_pop(recomp_ehframe_t *f) { (void)f; }

static void load_image(const char *path)
{
    FILE *pe = fopen(path, "rb");
    if (!pe) { printf("no image %s\n", path); exit(2); }
    static uint8_t file[96u << 20];
    size_t got = fread(file, 1, sizeof file, pe);
    fclose(pe);
    uint32_t nt = *(uint32_t *)(file + 0x3C), base = *(uint32_t *)(file + nt + 24 + 28);
    uint16_t count = *(uint16_t *)(file + nt + 6), optional = *(uint16_t *)(file + nt + 20);
    memcpy((void *)ADDR(base), file, 0x1000);
    for (uint16_t i = 0; i < count; ++i) {
        uint8_t *section = file + nt + 24 + optional + 40u * i;
        uint32_t va = *(uint32_t *)(section + 12), raw_size = *(uint32_t *)(section + 16);
        uint32_t raw = *(uint32_t *)(section + 20);
        if ((size_t)raw + raw_size <= got) memcpy((void *)ADDR(base + va), file + raw, raw_size);
    }
}
'''

_CHUNKS = {}
SUB = re.compile(r"\bsub_[0-9A-F]{8}\b")


def chunk_of(name):
    """The generated chunk defining a lifted function (one chunk per 32 KiB of
    address space: sims2_A0820000.c holds 0xA0820000..0xA0827FFF)."""
    va = int(name[4:], 16)
    for directory in sorted((ROOT / "generated").glob("*-x86")):
        prefix = directory.name[:-len("-x86")]
        chunk = directory / f"{prefix}_{va & ~0x7FFF:08X}.c"
        if chunk.is_file():
            if chunk not in _CHUNKS:
                _CHUNKS[chunk] = chunk.read_bytes()
            if b"\nvoid " + name.encode() + b"(void) {" in _CHUNKS[chunk]:
                return chunk, _CHUNKS[chunk]
    raise SystemExit(f"{name} not found in generated sources")


def extract(name):
    """The body as the game build compiles it, and the image it was lifted from."""
    chunk, text = chunk_of(name)
    m = re.search(rb"^void " + name.encode() + rb"\(void\) \{\r?\n.*?^\}\r?$", text, re.S | re.M)
    body = m.group(0).replace(b"\r\n", b"\n") + b"\n"
    module = chunk.parent.name[:-len("-x86")]
    body = build_android.direct_jump_tables(body, build_android.module_image(module))
    body = build_android.localize_registers(body)
    return body.decode(errors="replace"), ROOT / build_android.MODULE_IMAGES[module]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("va")
    parser.add_argument("--args", default="")
    parser.add_argument("--trace-labels", action="store_true")
    parser.add_argument("--serial", default="192.168.1.103:5555")
    parser.add_argument("--prelude", default="", help="C text placed right after recomp_types.h (macro overrides)")
    parser.add_argument("--cflags", default="", help="extra compiler options")
    parser.add_argument("--deep", type=int, default=0, help="also compile callees this many calls deep")
    parser.add_argument("--ecx", default="0", help="ECX on entry (thiscall)")
    parser.add_argument("--dump", action="append", default=[], help="ADDR:LEN of guest memory to print after the call")
    parser.add_argument("--st", action="append", default=[], help="push this double onto the x87 stack first "
                        "(repeat: the last one is ST(0)); the x87 stack is printed after the call")
    parser.add_argument("--poke", action="append", default=[], help="ADDR:VALUE, a 32-bit guest store before the call")
    args = parser.parse_args()
    name = f"sub_{int(args.va, 16):08X}"
    body, image = extract(name)
    if args.trace_labels:
        body = re.sub(r"^(L_[0-9A-F]{8}):$", r'\1: printf("  %s\\n", "\1");', body, flags=re.M)
    bodies, frontier = {name: body}, [name]
    for _ in range(args.deep):
        frontier = sorted({c for f in frontier for c in SUB.findall(bodies[f])} - bodies.keys())
        for callee in frontier:
            bodies[callee] = extract(callee)[0]
    callees = sorted({c for b in bodies.values() for c in SUB.findall(b)} - bodies.keys())
    stubs = "\n".join(f'void {c}(void) {{ printf("CALL {c} (not in probe)\\n"); exit(5); }}' for c in callees)
    stubs += "\n" + "\n".join(f"void {b}(void);" for b in bodies)
    pushes, setup = [], []
    cursor = 0x01000000
    for token in args.args.split():
        if token.startswith("buf"):
            skew, text = token[3:].split(":", 1)
            at = cursor + int(skew or 0)
            data = text.encode()
            setup.append(f'memcpy((void *)ADDR(0x{at:08X}u), "{text}", {len(data) + 1});')
            pushes.append(f"0x{at:08X}u")
            cursor += (len(data) + 80) & ~15
        elif token.startswith("zero:"):
            pushes.append(f"0x{cursor:08X}u")
            cursor += (int(token[5:], 0) + 79) & ~15
        else:
            pushes.append(f"(uint32_t){int(token, 0)}u")
    dumps = []
    for d in args.dump:
        address, length = (int(x, 0) for x in d.split(":"))
        dumps.append(f'printf("%08X:", {address}u); for (uint32_t i = 0; i < {length}u; ++i) '
                     f'printf(" %02X", MEM8({address}u + i)); printf("\\n");')
    for item in args.poke:
        address, value = (int(x, 0) for x in item.split(":"))
        setup.append(f"MEM32(0x{address:08X}u) = 0x{value & 0xFFFFFFFF:08X}u;")
    for value in args.st:
        setup.append(f"fp_push({float(value)!r});")
    harness = f'''
int main(void)
{{
    void *want = (void *)(uintptr_t)VOID_GUEST_BASE;
    if (mmap(want, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) != want) {{
        printf("mmap failed\\n");
        return 2;
    }}
    load_image("{DEVICE_DIR}/probe_image.bin");
    {" ".join(setup)}
    g_esp = 0x00800000u;
    uint32_t args[] = {{ {", ".join(pushes) or "0"} }};
    for (int i = (int)(sizeof args / sizeof args[0]) - 1; i >= 0; --i) {{ g_esp -= 4; MEM32(g_esp) = args[i]; }}
    g_esp -= 4; MEM32(g_esp) = RECOMP_RETADDR;
    uint32_t entry_esp = g_esp + 4;
    g_ecx = (uint32_t){int(args.ecx, 0)}u;
    {name}();
    printf("EAX=%08X EDX=%08X ESP delta=%d\\n", g_eax, g_edx, (int)(g_esp - entry_esp));
    if ({len(args.st)} || X87_DEPTH(g_stp)) printf("x87 depth %d ST0=%.17g ST1=%.17g XMM0=%.17g\\n", X87_DEPTH(g_stp), g_st[0], g_st[1], g_xmm[0].f64[0]);
    {" ".join(dumps)}
    return 0;
}}
'''
    out = ROOT / "build/probe"
    out.mkdir(parents=True, exist_ok=True)
    source = out / f"{name}.c"
    source.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n' + args.prelude + "\n" + RUNTIME + stubs +
                      "\n" + "\n".join(bodies.values()) + "\n" + harness)
    binary = out / f"{name}"
    subprocess.run([str(CLANG), "--target=aarch64-linux-android30", "-O2", "-w", "-fno-strict-aliasing", "-fwrapv",
                    "-ffp-contract=off", "-fno-math-errno", "-DRECOMP_GENERATED_CODE", "-DRECOMP_TRACE",
                    "-DRECOMP_LOCAL_REGS", "-DRECOMP_PLAIN_MEMORY",
                    "-I" + str(ROOT / "android/runtime"), *args.cflags.split(), str(source), "-o", str(binary), "-lm"],
                   check=True)
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    for local, remote in ((image, "probe_image.bin"), (binary, name)):
        subprocess.run([str(ADB), "-s", args.serial, "push", str(local), f"{DEVICE_DIR}/{remote}"],
                       check=True, capture_output=True, env=env)
    result = subprocess.run([str(ADB), "-s", args.serial, "shell",
                             f"chmod 755 {DEVICE_DIR}/{name} && {DEVICE_DIR}/{name}"],
                            capture_output=True, text=True, env=env)
    print(result.stdout + result.stderr)


if __name__ == "__main__":
    main()
