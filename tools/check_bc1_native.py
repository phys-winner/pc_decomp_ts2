"""Native D3DX BC1 encoder (android/host/sims2_bc1.c) against the lifted one, bit for bit.

    python -X utf8 -B tools/check_bc1_native.py [CASES]

Builds one bare AArch64 image with the lifted bodies of EncodeBC1 (0xA0084173,
renamed reference_A0084173), OptimizeRGB, Encode565 and Decode565 from
generated/sims2-x86 and the native override, and runs both under Unicorn on
the same guest state: random, few-colour, gradient, flat, out-of-range and
near-flat blocks, with and without the colour key and dithering. The block,
EAX, ESP, the callee-saved registers, the control word and the x87 window
must match; blocks the native code hands back to the lifted body (a step
index below OptimizeRGB's table) are counted. Also checks the constants the
native code names against the game image and that the lifted body has the
`fst st(1)` at 0xA00847A6. No adb, build or install.
"""
import io
import math
from pathlib import Path
import random
import re
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A
import build_android as B

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/bc1-native'
OUT.mkdir(parents=True, exist_ok=True)
CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 6000

generated = (ROOT / 'generated/sims2-x86/sims2_A0080000.c').read_text()
assert '/* 0xA00847A6: fst st(1) */' in generated, 'lifted EncodeBC1 lacks fst st(1) (tools/sse_ext.py)'
bodies = []
for name in ('A0084173', 'A0083BF3', 'A00837C8', 'A00836C6'):
    begin = generated.index('void sub_' + name + '(void) {')
    end = generated.index('\n}\n', begin) + 3
    body = generated[begin:end]
    if name == 'A0084173':
        body = body.replace('void sub_A0084173(void)', 'void reference_A0084173(void)', 1)
    bodies.append(body)
reference = OUT / 'reference.c'
reference.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                     'void sub_A0083BF3(void), sub_A00837C8(void), sub_A00836C6(void);\n' + '\n'.join(bodies))
source = OUT / 'check.c'
source.write_text('''#include "host.h"
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias;
void (*recomp_yield_hook)(void);
uint32_t g_enter_trace[RECOMP_ENTER_SIZE], g_enter_idx;
uint32_t fallback_calls;
void void_log(const char *fmt, ...) { (void)fmt; }
double nearbyint(double v) { double r; __asm__("frintn %d0, %d1":"=w"(r):"w"(v)); return r; }
double floor(double v) { double r; __asm__("frintm %d0, %d1":"=w"(r):"w"(v)); return r; }
double ceil(double v) { double r; __asm__("frintp %d0, %d1":"=w"(r):"w"(v)); return r; }
double trunc(double v) { double r; __asm__("frintz %d0, %d1":"=w"(r):"w"(v)); return r; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; for(size_t i=0;i<n;++i)p[i]=q[i]; return d; }
int memcmp(const void *a,const void *b,size_t n) {const unsigned char *p=a,*q=b;for(size_t i=0;i<n;++i)if(p[i]!=q[i])return p[i]-q[i];return 0;}
void *memmove(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; if(p<q)for(size_t i=0;i<n;++i)p[i]=q[i]; else while(n){--n;p[n]=q[n];} return d; }
void *memset(void *d, int c, size_t n) { unsigned char *p=d; for(size_t i=0;i<n;++i)p[i]=(unsigned char)c; return d; }
int void_vm_readable(uint32_t address, uint32_t size) { return address >= 0x10000u && (uint64_t)address + size <= 0x300000u; }
int void_vm_writable(uint32_t address, uint32_t size) { return address >= 0x10000u && (uint64_t)address + size <= 0x300000u; }
void reference_A0084173(void);
void sub_A0084173_lifted(void) { ++fallback_calls; reference_A0084173(); }
''' + f'#include "{ROOT.as_posix()}/android/host/sims2_bc1.c"\n' + '''
uint32_t check(uint32_t mode) {
    if (mode) reference_A0084173(); else sub_A0084173();
    return fallback_calls;
}
''')
binary = OUT / 'check.elf'
command = [str(B.CLANG), '--target=aarch64-linux-android30', '-O2', '-ffp-contract=off', '-fno-builtin',
           '-ffunction-sections', '-fdata-sections', '-fno-stack-protector', '-nostdlib', '-DRECOMP_TRACE',
           '-Iandroid/runtime', '-Iandroid/host', '-Wl,--gc-sections,-e,check', str(source), str(reference),
           '-o', str(binary)]
build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
(OUT / 'warnings.txt').write_text(build.stderr)
assert build.returncode == 0, build.stderr
assert not build.stderr.strip(), build.stderr

elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000)
arm.mem_map(guest + 0x10000, 0x2F0000)
arm.mem_map(native_stack, 0x10000)
arm.mem_map(stop, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type'] == 'PT_LOAD':
        arm.mem_write(base + segment['p_vaddr'], segment.data())
rel = elf.get_section_by_name('.rela.dyn')
if rel:
    for r in rel.iter_relocations():
        assert r['r_info_type'] == 1027
        arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))
pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
image = pe.get_memory_mapped_image()
imagebase = pe.OPTIONAL_HEADER.ImageBase
arm.mem_map(guest + imagebase, (len(image) + 0xFFFF) & ~0xFFFF)
arm.mem_write(guest + imagebase, image)

# The constants sims2_bc1.c names by their bits are the image's.
native_source = (ROOT / 'android/host/sims2_bc1.c').read_text()
named = re.findall(r'#define F_\w+\s+0x([0-9A-Fa-f]+)u\s+/\* 0x([0-9A-Fa-f]+)', native_source)
assert len(named) >= 18, named
for bits, va in named:
    va = int(va, 16)
    assert struct.unpack('<I', image[va - imagebase:va - imagebase + 4])[0] == int(bits, 16), hex(va)
for va, values in ((0xA0E2AE24, (0, 0x3EAAAAAB, 0x3F2AAAAB, 0x3F800000)), (0xA0E2AE34, (0x3F800000, 0x3F2AAAAB, 0x3EAAAAAB, 0)),
                   (0xA0E2AE44, (0, 0x3F000000, 0x3F800000)), (0xA0E2AE50, (0x3F800000, 0x3F000000, 0)),
                   (0xA0E2AE5C, (0, 2, 3, 1)), (0xA0E2AE6C, (0, 2, 1))):
    assert struct.unpack_from('<%dI' % len(values), image, va - imagebase) == values, hex(va)

rng = random.Random(0xA0084173)
COLORS, OUT_BLOCK, SP = 0x20000, 0x21000, 0x2A0000
machine0 = bytearray(224)
x87_pointer = base + symbols['g_x87'] + 28 * 8


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def run(mode, colors, color_key, dither, regs, cw, stack_fill):
    arm.mem_write(guest + COLORS, colors)
    arm.mem_write(guest + OUT_BLOCK - 8, bytes(range(24)))
    arm.mem_write(guest + SP - 0x800, stack_fill)
    arm.mem_write(guest + SP, struct.pack('<4I', 0xA0084EE5, OUT_BLOCK, color_key, dither))
    machine = bytearray(machine0)
    struct.pack_into('<8I', machine, 0, COLORS, regs[0], regs[1], SP, regs[2], regs[3], regs[4], regs[5])
    struct.pack_into('<H', machine, 72, cw)
    struct.pack_into('<i', machine, 64, 2)
    struct.pack_into('<Q', machine, 80, x87_pointer)
    arm.mem_write(base + symbols['g_m'], bytes(machine))
    arm.mem_write(base + symbols['g_x87'], bytes(1024))
    arm.mem_write(base + symbols['g_fp_bias'], struct.pack('<i', 0))
    arm.mem_write(base + symbols['fallback_calls'], struct.pack('<I', 0))
    arm.mem_write(base + symbols['g_enter_idx'], struct.pack('<I', 0))
    arm.mem_write(base + symbols['recomp_yield_hook'], struct.pack('<Q', 0))
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.reg_write(A.UC_ARM64_REG_X0, mode)
    arm.emu_start(base + symbols['check'], stop, count=200_000_000)
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, 'runaway'
    state = bytes(arm.mem_read(base + symbols['g_m'], 224))
    regs_out = struct.unpack_from('<8I', state, 0)
    return {
        'block': bytes(arm.mem_read(guest + OUT_BLOCK - 8, 24)),
        'eax': regs_out[0], 'esp': regs_out[3], 'saved': regs_out[4:8],
        'cw': struct.unpack_from('<H', state, 72)[0],
        'stp': struct.unpack_from('<Q', state, 80)[0],
        'colors': bytes(arm.mem_read(guest + COLORS, 256)),
        'fallbacks': arm.reg_read(A.UC_ARM64_REG_X0),
    }


def block_kind(n):
    kind = n % 9
    px = []
    if kind == 0:                                   # uniform random
        px = [[rng.random() for _ in range(3)] for _ in range(16)]
    elif kind == 1:                                 # few colours with noise
        palette = [[rng.random() for _ in range(3)] for _ in range(rng.randint(1, 4))]
        noise = rng.choice((0.0, 1e-4, 0.01, 0.05))
        px = [[min(1.0, max(0.0, c + rng.uniform(-noise, noise))) for c in rng.choice(palette)] for _ in range(16)]
    elif kind == 2:                                 # gradients
        a = [rng.random() for _ in range(3)]
        b = [rng.random() for _ in range(3)]
        for y in range(4):
            for x in range(4):
                t = (x + y * rng.choice((0, 1, 4))) / (3 + 12 * (y > 0))
                px.append([a[k] + (b[k] - a[k]) * min(1.0, t) for k in range(3)])
    elif kind == 3:                                 # flat
        c = [rng.random() for _ in range(3)]
        px = [list(c) for _ in range(16)]
    elif kind == 4:                                 # out of range (filters, dithering)
        px = [[rng.uniform(-0.6, 1.6) for _ in range(3)] for _ in range(16)]
    elif kind == 5:                                 # near flat: the FLT_MIN and 1/4096 paths
        c = [rng.random() for _ in range(3)]
        eps = rng.choice((1e-7, 1e-5, 1e-3, 0.004, 0.02))
        px = [[v + rng.uniform(-eps, eps) for v in c] for _ in range(16)]
    elif kind == 6:                                 # 565 grid colours
        px = [[rng.randint(0, 31) / 31.0, rng.randint(0, 63) / 63.0, rng.randint(0, 31) / 31.0] for _ in range(16)]
    elif kind == 7:                                 # extremes
        px = [[rng.choice((0.0, 1.0, 0.5, -0.0)) for _ in range(3)] for _ in range(16)]
    else:                                           # smooth image-like
        c = [rng.random() for _ in range(3)]
        g = [rng.uniform(-0.1, 0.1) for _ in range(6)]
        px = [[c[k] + g[k] * x + g[k + 3] * y + rng.gauss(0, 0.01) for k in range(3)] for y in range(4) for x in range(4)]
    alpha_kind = rng.randrange(5)
    for i, p in enumerate(px):
        a = 1.0
        if alpha_kind == 1:
            a = rng.random()
        elif alpha_kind == 2:
            a = 0.0
        elif alpha_kind == 3:
            a = rng.choice((0.0, 0.4999, 0.5, 1.0))
        p.append(a)
    if n % 97 == 96:                                # non-finite: the lifted body encodes it
        px[rng.randrange(16)][rng.randrange(4)] = rng.choice((math.nan, math.inf, -math.inf))
    return b''.join(struct.pack('<4f', *[f32(v) for v in p]) for p in px)


fallbacks = 0
for n in range(CASES):
    colors = block_kind(n)
    color_key = rng.choice((0, 1, 1, 7))
    dither = rng.choice((0, 0, 1, 0x10000))
    regs = [rng.getrandbits(32) for _ in range(6)]
    cw = rng.choice((0x27F, 0x37F, 0x7F, 0x67F, 0xE7F))
    stack_fill = rng.randbytes(0x800)
    native = run(0, colors, color_key, dither, regs, cw, stack_fill)
    lifted = run(1, colors, color_key, dither, regs, cw, stack_fill)
    label = (n, color_key, dither, colors.hex()[:64])
    assert native['block'] == lifted['block'], (label, 'block', native['block'].hex(), lifted['block'].hex())
    assert native['eax'] == lifted['eax'] == 0, (label, 'eax')
    assert native['esp'] == lifted['esp'] == SP + 16, (label, 'esp')
    assert native['saved'] == lifted['saved'] == tuple(regs[2:6]), (label, 'callee-saved')
    assert native['cw'] == lifted['cw'] == cw, (label, 'control word')
    assert native['stp'] == lifted['stp'] == x87_pointer, (label, 'x87 window')
    assert native['colors'] == lifted['colors'] == colors, (label, 'colours changed')
    fallbacks += native['fallbacks']
    if (n + 1) % 1000 == 0:
        print(f'{n + 1} blocks match ({fallbacks} through the lifted body)', flush=True)
print(f'PASS {CASES} blocks bit-exact; {fallbacks} handed back to the lifted body')

def reused():
    return struct.unpack('<Q',arm.mem_read(base+symbols['bc1_reused'],8))[0]

# Repeat exact portrait backgrounds, then invalidate each component of the cache key.
colors=struct.pack('<4f',.2,.4,.6,1)*16
regs=[0x12345678+i for i in range(6)]
fill=bytes(0x800)
lum=bytes(arm.mem_read(guest+0xA10583C0,12))
for key,dither,change in ((0,0,False),(0,0,True),(1,0,False),(1,1,False)):
    if change:
        bits=struct.unpack_from('<I',lum)[0]
        arm.mem_write(guest+0xA10583C0,struct.pack('<I',bits+1))
    before=reused()
    native=run(0,colors,key,dither,regs,0x27F,fill)
    assert reused()==before,'Changed cache key reused stale block'
    repeated=run(0,colors,key,dither,regs,0x27F,fill)
    lifted=run(1,colors,key,dither,regs,0x27F,fill)
    assert reused()==before+1
    for field in ('block','eax','esp','saved','cw','stp','colors'):
        assert native[field]==repeated[field]==lifted[field],('cache',key,dither,change,field)
arm.mem_write(guest+0xA10583C0,lum)
print('PASS exact-block reuse; luminance, color-key and dithering invalidate the cache; bytes and original ABI agree')
