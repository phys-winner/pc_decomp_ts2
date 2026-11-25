"""Original x86 versus actual AArch64 R8G8B8 row writer; no adb/build/install.

Compares sub_A00390A6 in Sims2EP9.exe (Unicorn x86) with the AArch64 native
in android/host/sims2_rgb24.c (Unicorn ARM64). The native path must match the
original's output bytes, the stack bytes its frame leaves, the registers,
[esi+0x7C], the restored control word, and the x87 slots it pops into.
"""
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import sys

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X
import build_android as B

ROOT = Path(__file__).resolve().parents[1]
SMALL_YIELD = '--small-yield' in sys.argv
OUT = ROOT / 'build/test/rgb24-native'
if SMALL_YIELD:
    OUT = OUT / 'small-yield'
OUT.mkdir(parents=True, exist_ok=True)
generated = (ROOT / 'generated/sims2-x86/sims2_A0038000.c').read_text(encoding='utf-8')


def body(name):
    begin = generated.index('void sub_' + name + '(void) {')
    end = generated.index('\n}\n', begin) + 3
    return generated[begin:end]


reference = OUT / 'reference.c'
reference.write_text(
    '#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
    'void sub_A00377E3(void), sub_A0038D18(void);\n'
    + body('A0039034') + '\n'
    + body('A00390A6').replace('void sub_A00390A6(void)', 'void reference_A00390A6(void)', 1),
    encoding='utf-8')
source = OUT / 'check.c'
source.write_text(r'''#include "host.h"
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias;
void (*recomp_yield_hook)(void);
uint32_t fallback_calls, helper_calls;
uint32_t g_enter_trace[RECOMP_ENTER_SIZE], g_enter_idx;
double nearbyint(double v) { double r; __asm__("frintn %d0, %d1":"=w"(r):"w"(v)); return r; }
double floor(double v) { double r; __asm__("frintm %d0, %d1":"=w"(r):"w"(v)); return r; }
double ceil(double v) { double r; __asm__("frintp %d0, %d1":"=w"(r):"w"(v)); return r; }
double trunc(double v) { double r; __asm__("frintz %d0, %d1":"=w"(r):"w"(v)); return r; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; for(size_t i=0;i<n;++i)p[i]=q[i]; return d; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *p=d; const unsigned char *q=s; if(p<q)for(size_t i=0;i<n;++i)p[i]=q[i]; else while(n){--n;p[n]=q[n];} return d; }
void *memset(void *d, int c, size_t n) { unsigned char *p=d; for(size_t i=0;i<n;++i)p[i]=(unsigned char)c; return d; }
void sub_A00377E3(void) { ++helper_calls; g_esp+=8; }
void sub_A0038D18(void) { ++helper_calls; g_esp+=8; }
void reference_A00390A6(void);
void sub_A00390A6_lifted(void) { ++fallback_calls; reference_A00390A6(); }
''' + f'#include "{(ROOT / "android/host/sims2_rgb24.c").as_posix()}"\n' + r'''
uint32_t check(uint32_t mode) {
    if (mode == 1) return rgb24_row();
    sub_A00390A6();
    return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1);
}
''', encoding='utf-8')
binary = OUT / 'check.elf'
command = [str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
           '-DRECOMP_TRACE', '-Iandroid/runtime', '-Iandroid/host',
           '-Wl,--gc-sections,-e,check', str(source), str(reference), '-o', str(binary)]
if SMALL_YIELD:
    command.insert(1, '-DRECOMP_YIELD_EVERY=1')
build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
(OUT / 'warnings.txt').write_text(build.stderr, encoding='utf-8')
assert build.returncode == 0, build.stderr
assert not build.stderr.strip(), build.stderr
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000)
arm.mem_map(guest + 0x10000, 0x2F0000)
arm.mem_map(native_stack, 0x20000)
arm.mem_map(stop, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type'] == 'PT_LOAD':
        arm.mem_write(base + segment['p_vaddr'], segment.data())
rel = elf.get_section_by_name('.rela.dyn')
if rel:
    for r in rel.iter_relocations():
        assert r['r_info_type'] == 1027, r['r_info_type']
        arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))
pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
image = pe.get_memory_mapped_image()
imagebase = pe.OPTIONAL_HEADER.ImageBase
x86 = Uc(UC_ARCH_X86, UC_MODE_32)
for uc, offset in ((arm, guest), (x86, 0)):
    uc.mem_map(imagebase + offset, (len(image) + 0xFFFF) & ~0xFFFF)
    uc.mem_write(imagebase + offset, image)
x86.mem_map(0x10000, 0x2F0000)
x86.mem_map(stop, 0x1000)
regs = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
        X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
rng = random.Random(0xA00390A6)
cases = pixels_checked = rejections = fast_rows = fallback_rows = 0
fell_back = []
rejected = []


def bits(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def raw(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def ext_bits(mant, exp):
    """80-bit x87 register as the float64 the ARM window stores, or raise."""
    mant &= (1 << 64) - 1
    sign = (int(exp) >> 15) & 1
    e = int(exp) & 0x7FFF
    if mant == 0:
        return sign << 63
    if e == 0x7FFF:
        frac = (mant >> 11) & ((1 << 52) - 1)
        if (mant & ((1 << 63) - 1)) == 0:
            frac = 0
        elif frac == 0:
            frac = 1 << 51
        return (sign << 63) | (0x7FF << 52) | frac
    top = mant.bit_length() - 1
    unbiased = (e - 16383) - (63 - top)
    shift = top - 52
    if shift >= 0:
        frac = (mant >> shift) & ((1 << 52) - 1)
        lost = mant & ((1 << shift) - 1) if shift else 0
    else:
        frac = (mant << -shift) & ((1 << 52) - 1)
        lost = 0
    if lost:
        raise AssertionError(('x87 value is not a float64', hex(mant), hex(int(exp)), hex(lost)))
    ef = unbiased + 1023
    if not 0 < ef < 2047:
        raise AssertionError(('x87 exponent', hex(mant), hex(int(exp)), ef))
    return (sign << 63) | (ef << 52) | frac


def fp_reg(uc, index):
    value = uc.reg_read(getattr(X, 'UC_X86_REG_FP%d' % index))
    mant, exp = value[0], value[1]
    return ext_bits(mant, exp), mant, exp


def compare(values, *, x=0, y=0, cw=0x37F, table=0xA10518B0, fpcr=0, expected_fast=True,
            alias=None, label='', try_reject=False, field=None, strides=(0, 0), live=0,
            backedges=0, hook=False, bias_override=None, out_delta=0, align=None,
            diffusion=None, table_bytes=None, loose_x87=False):
    """One row. try_reject checks the guard returns 0 and touches nothing.
    Otherwise the original x86 and the AArch64 wrapper both run."""
    global cases, pixels_checked, rejections, fast_rows, fallback_rows
    n = len(values)
    self, inp, out, sp = 0x11000, 0x20000, 0x100000 + out_delta, 0x2A0000
    if alias == 'input_output':
        out = inp
    elif alias == 'stack_output':
        out = sp - 32
    elif alias == 'self_output':
        out = self
    elif alias == 'bias_output':
        out = table
    elif alias == 'scale_output':
        out = 0xA0D9C79C
    elif alias == 'input_stack':
        inp = sp - 64
    if align == 'self':
        self += 1
    elif align == 'sp':
        sp += 1
    elif align == 'in':
        inp += 1
    diff_at, diff_size = 0x180000, 0x4000
    initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    obj = bytearray(rng.randbytes(128))
    struct.pack_into('<I', obj, 4, 20)
    for at, v in ((0x10, 0), (0x20, out), (0x34, table), (0x54, 0),
                  (0x5C, diff_at if diffusion is not None else 0),
                  (0x60, strides[0]), (0x64, strides[1]), (0x68, n)):
        struct.pack_into('<I', obj, at, v)
    if field:
        struct.pack_into('<I', obj, *field)
    payload = b''.join(struct.pack('<4I', *v) for v in values)
    below = 0x100
    stack = rng.randbytes(below) + struct.pack('<4I', stop, x, y, inp) + rng.randbytes(16)
    span = strides[0] * x + strides[1] * y
    output = rng.randbytes(span + max(n * 3, 1) + 64)
    xmm = rng.randbytes(128)
    regions = [(self, 128), (inp, max(len(payload), 16)), (out - 32, len(output)), (sp - below, len(stack))]
    if diffusion is not None:
        regions.append((diff_at, diff_size))
    if table_bytes is not None:
        regions.append((table, len(table_bytes)))
    for uc, offset in ((x86, 0), (arm, guest)):
        for t in (0xA10518B0, 0xA1051930):
            uc.mem_write(t + offset, image[t - imagebase:t - imagebase + 128])
        uc.mem_write(0xA0D9C79C + offset, image[0xA0D9C79C - imagebase:0xA0D9C7A0 - imagebase])
        uc.mem_write(out - 32 + offset, output)
        uc.mem_write(self + offset, bytes(obj))
        uc.mem_write(sp - below + offset, stack)
        if payload:
            uc.mem_write(inp + offset, payload)
        if alias == 'scale_output':
            uc.mem_write(0xA0D9C79C + offset, image[0xA0D9C79C - imagebase:0xA0D9C7A0 - imagebase])
        if bias_override is not None:
            uc.mem_write(table + 4 * ((y & 3) + 8 * (x & 3)) + offset, raw(bias_override))
        if diffusion is not None:
            blob = diffusion
            if len(blob) < diff_size:
                blob = blob + bytes(diff_size - len(blob))
            uc.mem_write(diff_at + offset, blob[:diff_size])
        if table_bytes is not None:
            uc.mem_write(table + offset, table_bytes)
    for r, v in zip(regs, initial):
        x86.reg_write(r, v)
    x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    x86.reg_write(X.UC_X86_REG_FPCW, cw)
    x86.reg_write(X.UC_X86_REG_FPSW, 0)
    x86.reg_write(X.UC_X86_REG_FPTAG, 0xFFFF & ~((1 << (2 * live)) - 1))
    for i in range(live):
        x86.reg_write(getattr(X, 'UC_X86_REG_FP%d' % i), ((8 + i) << 60, 0x3FFF))
    # Free registers a push from TOP=0 writes: FP7, then FP6, then FP5.
    # A live register occupies FP0..FP(live-1), so FP5 is live when live > 5.
    x86.reg_write(X.UC_X86_REG_FP7, (0x8000000000000000, 0x3FFF))
    x86.reg_write(X.UC_X86_REG_FP6, (0x8000000000000000, 0x3FFF))
    if live <= 5:
        x86.reg_write(X.UC_X86_REG_FP5, (0x8000000000000000, 0x3FFF))
    for i in range(8):
        x86.reg_write(getattr(X, 'UC_X86_REG_XMM%d' % i), int.from_bytes(xmm[16 * i:16 * i + 16], 'little'))
    machine = bytearray(224)
    struct.pack_into('<8I', machine, 0, *initial)
    struct.pack_into('<4I', machine, 32, 0, 0, 0, 0)
    struct.pack_into('<I', machine, 52, backedges)
    struct.pack_into('<i', machine, 64, 2)
    struct.pack_into('<H', machine, 72, cw)
    struct.pack_into('<Q', machine, 80, base + symbols['g_x87'] + 28 * 8)
    machine[96:224] = xmm
    arm.mem_write(base + symbols['g_m'], bytes(machine))
    fp_window = bytearray(rng.randbytes(1024))
    for i in range(live):
        struct.pack_into('<d', fp_window, (28 + i) * 8, 1 + i / 8)
    for slot in (25, 26, 27):
        struct.pack_into('<d', fp_window, slot * 8, 1.0)
    arm.mem_write(base + symbols['g_x87'], bytes(fp_window))
    arm.mem_write(base + symbols['g_fp_bias'], raw(live))
    arm.mem_write(base + symbols['fallback_calls'], raw(0))
    arm.mem_write(base + symbols['helper_calls'], raw(0))
    arm.mem_write(base + symbols['g_enter_idx'], raw(0))
    arm.mem_write(base + symbols['g_enter_trace'], bytes(4096))
    arm.mem_write(base + symbols['recomp_yield_hook'], struct.pack('<Q', stop if hook else 0))
    arm.reg_write(A.UC_ARM64_REG_FPCR, fpcr)
    arm.reg_write(A.UC_ARM64_REG_FPSR, 0x08000000)
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0x1E000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    if try_reject:
        arm.reg_write(A.UC_ARM64_REG_X0, 1)
        arm.emu_start(base + symbols['check'], stop, count=5_000_000)
        assert arm.reg_read(A.UC_ARM64_REG_X0) == 0, (label, 'guard accepted')
        assert bytes(arm.mem_read(base + symbols['g_m'], 224)) == machine, (label, 'rejected machine mutation')
        assert bytes(arm.mem_read(base + symbols['g_x87'], 1024)) == fp_window, (label, 'rejected FPU mutation')
        for at, size in regions:
            assert arm.mem_read(guest + at, size) == x86.mem_read(at, size), (label, 'rejected guest mutation', hex(at))
        assert arm.reg_read(A.UC_ARM64_REG_FPCR) == fpcr, (label, 'rejected FPCR mutation')
        assert arm.reg_read(A.UC_ARM64_REG_FPSR) == 0x08000000, (label, 'rejected FPSR mutation')
        assert not struct.unpack('<I', arm.mem_read(base + symbols['fallback_calls'], 4))[0]
        assert not struct.unpack('<I', arm.mem_read(base + symbols['helper_calls'], 4))[0]
        assert not struct.unpack('<I', arm.mem_read(base + symbols['g_enter_idx'], 4))[0]
        cases += 1
        rejections += 1
        rejected.append(label)
        return
    arm.reg_write(A.UC_ARM64_REG_X0, 0)
    try:
        x86.emu_start(0xA00390A6, stop, count=50_000_000)
        arm.emu_start(base + symbols['check'], stop, count=50_000_000)
    except Exception as error:
        raise AssertionError((label, 'emulation', error)) from error
    assert x86.reg_read(X.UC_X86_REG_EIP) == stop, (label, 'x86 runaway')
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, (label, 'ARM runaway')
    actual = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
    expected = tuple(x86.reg_read(r) for r in regs)
    assert actual == expected, (label, 'GPR', tuple(hex(v) for v in actual), tuple(hex(v) for v in expected))
    assert not (arm.reg_read(A.UC_ARM64_REG_X0) ^ x86.reg_read(X.UC_X86_REG_EFLAGS)) & 0x8D5, (label, 'flags')
    # The lifted call pushes RECOMP_RETADDR (0xDEAD0000). The original call
    # leaves 0xA0039164 in that abandoned slot below the saved registers.
    skip = {(sp - 0x68 + i) for i in range(4)} if diffusion is not None else set()
    for at, size in regions:
        aa = bytes(arm.mem_read(guest + at, size))
        xx = bytes(x86.mem_read(at, size))
        if aa != xx:
            diffs = [i for i in range(size) if aa[i] != xx[i] and at + i not in skip]
            if not diffs:
                continue
            k = diffs[0]
            raise AssertionError((label, 'memory', hex(at + k), aa[max(0, k - 8):k + 12].hex(),
                                  xx[max(0, k - 8):k + 12].hex(), len(diffs)))
    state = bytes(arm.mem_read(base + symbols['g_m'], 224))
    assert struct.unpack_from('<H', state, 72)[0] == x86.reg_read(X.UC_X86_REG_FPCW) == cw, (label, 'CW')
    assert x86.reg_read(X.UC_X86_REG_FPSW) & 0x3800 == 0, (label, 'x87 TOP')
    assert state[96:224] == xmm, (label, 'XMM')
    assert struct.unpack_from('<i', state, 64)[0] == 2, (label, 'fpu_cmp')
    assert struct.unpack_from('<Q', state, 80)[0] == base + symbols['g_x87'] + 28 * 8, (label, 'x87 pointer')
    window = bytes(arm.mem_read(base + symbols['g_x87'], 1024))
    # TOP starts at 0, so the first three pushes land in FP7, FP6, FP5,
    # which are window slots 27, 26 and 25. The fast path is two deep.
    depth = 3 if loose_x87 else 2
    for step in range(depth):
        reg = 7 - step
        slot = 27 - step
        got = struct.unpack_from('<Q', window, slot * 8)[0]
        expect, mant, exp = fp_reg(x86, reg)
        assert got == expect, (label, 'x87', 'FP%d' % reg, hex(got), hex(expect), hex(int(mant) & (1 << 64) - 1), hex(int(exp)))
    low = 28 - depth
    assert window[:low * 8] == fp_window[:low * 8], (label, 'x87 below the popped slots')
    assert window[28 * 8:512 + low * 8] == fp_window[28 * 8:512 + low * 8], (label, 'live x87 window')
    assert window[512 + 28 * 8:] == fp_window[512 + 28 * 8:], (label, 'live x87 shadows')
    if not loose_x87:
        if n:
            assert struct.unpack_from('<q', window, 512 + 26 * 8)[0] == 0, (label, 'shadow -2')
            assert struct.unpack_from('<q', window, 512 + 27 * 8)[0] == 0, (label, 'shadow -1')
        else:
            assert struct.unpack_from('<q', window, 512 + 27 * 8)[0] == 0, (label, 'shadow -1')
            assert window[512 + 26 * 8:512 + 27 * 8] == fp_window[512 + 26 * 8:512 + 27 * 8], (label, 'shadow -2')
    for i in range(live):
        got_fp = x86.reg_read(getattr(X, 'UC_X86_REG_FP%d' % i))
        assert got_fp == ((8 + i) << 60, 0x3FFF), (label, 'live original x87', i, got_fp)
    assert arm.reg_read(A.UC_ARM64_REG_FPCR) == fpcr, (label, 'host FPCR')
    calls = struct.unpack('<I', arm.mem_read(base + symbols['fallback_calls'], 4))[0]
    trace_n = struct.unpack('<I', arm.mem_read(base + symbols['g_enter_idx'], 4))[0]
    trace = struct.unpack('<2I', arm.mem_read(base + symbols['g_enter_trace'], 8))
    assert trace[0] == 0xA00390A6, (label, 'entry trace VA', hex(trace[0]))
    if loose_x87:
        assert trace_n == 2 and trace[1] == 0xA0039034, (label, 'diffusion trace', trace_n, hex(trace[1]))
    else:
        assert trace_n == 1, (label, 'entry trace count', trace_n)
    assert bool(calls) != expected_fast, (label, 'guard', calls)
    if expected_fast:
        out_base = out + span
        produced = bytes(x86.mem_read(out_base, n * 3))
        zeros = sum(1 for i in range(n) if produced[3 * i] == 0)
        got_edges = struct.unpack_from('<I', state, 52)[0]
        assert got_edges == (backedges + n + zeros) & 0xFFFFFFFF, (label, 'backedges', got_edges, backedges, n, zeros)
        assert arm.reg_read(A.UC_ARM64_REG_FPSR) == 0x08000000, (label, 'host FPSR')
        fast_rows += 1
    else:
        fallback_rows += 1
        fell_back.append(label)
    cases += 1
    pixels_checked += n


def pixel_ok(channels):
    for c in channels[:3]:
        if c and (c < 0x33800000 or c > 0x3F800000):
            return False
    return True


def rand_pixel(random_source):
    def chan():
        k = random_source.randrange(8)
        if k == 0:
            return 0
        if k == 1:
            return bits(1.0)
        if k == 2:
            return 0x33800000
        if k == 3:
            return bits(0.5)
        if k == 4:
            return bits((random_source.randrange(255) + 0.5) / 255)
        if k == 5:
            return bits(random_source.randrange(256) / 255)
        return random_source.randrange(0x33800000, 0x3F800001)
    # The fourth float is stored and never read. A NaN there stays on the fast path.
    unused = random_source.choice((0, bits(0.5), 0x7FC00001, 0x7F800000, random_source.getrandbits(32)))
    return (chan(), chan(), chan(), unused)


if __name__ == '__main__':
    if SMALL_YIELD:
        for n, x, blue in ((1, 0, 1), (1, 1, 1), (7, 0, 1), (7, 1, 0), (4096, 0, 1), (600, 1, 0)):
            v = bits(0.5) if blue else 0
            compare([(bits(0.25), bits(0.5), v, 0)] * n, x=x, hook=True, try_reject=True,
                    label=f'yield threshold 1 n={n} x={x} blue={blue}')
        print(f'PASS RECOMP_YIELD_EVERY=1/non-null hook: {cases} no-effect rejections')
        sys.exit(0)

    compare([(bits(0.25), bits(0.5), bits(0.75), bits(0.5))], label='smoke')
    compare([(bits(0.0), bits(0.0), bits(0.0), 0)], label='all zero')
    compare([(bits(1.0), bits(1.0), bits(1.0), 0)], label='all one')
    compare([(0, 0x33800000, bits(1.0), 0x7FC00001)], label='zero tiny one, unread NaN')
    print(f'smoke ok, {cases} cases', flush=True)

    for table in (0xA10518B0, 0xA1051930):
        for x_low in range(4):
            for y_low in range(4):
                for pc in (0, 0x200, 0x300):
                    for rc in (0, 0x400, 0x800, 0xC00):
                        for mode in (0, 0x400000, 0x800000, 0xC00000):
                            row = [rand_pixel(rng) for _ in range(4)]
                            compare(row, x=0xBEEF0000 + x_low, y=0xCAFE0000 + y_low,
                                    cw=pc | rc | 0x7F, table=table, fpcr=mode,
                                    label=f'environment {table:x} {x_low} {y_low} {pc | rc | 0x7F:x} {mode:x}')
    print(f'environment ok, {cases} cases', flush=True)

    for width in range(1, 601):
        row = [rand_pixel(rng) for _ in range(width)]
        compare(row, x=width & 1, y=0xA1000000 | (width & 3),
                cw=(0x7F, 0x27F, 0x37F)[width % 3] | ((0, 0xC00)[width & 1]),
                table=(0xA10518B0, 0xA1051930)[width & 1],
                fpcr=(0, 0x400000, 0x800000, 0xC00000)[width & 3],
                out_delta=width % 8,
                label=f'width {width}')
        if width % 100 == 0:
            print(f'widths through {width}: {cases} cases', flush=True)

    for i in range(2400):
        n = rng.randrange(1, 81)
        row = [rand_pixel(rng) for _ in range(n)]
        compare(row, x=rng.randrange(4) | (0x10000 if i & 1 else 0),
                y=(0xCAFE0000 if i & 2 else 0) | rng.randrange(4),
                cw=rng.choice((0x7F, 0x27F, 0x37F, 0xC7F, 0xE7F, 0xF7F)),
                table=rng.choice((0xA10518B0, 0xA1051930)),
                fpcr=rng.choice((0, 0x400000, 0x800000, 0xC00000)),
                out_delta=i % 8,
                label=f'random {i}')
        if i % 400 == 399:
            print(f'random {i + 1}: {cases} cases', flush=True)

    for n in (0, 1, 2, 3, 4, 5, 16, 31, 64, 255, 600, 4096):
        for parity in (0, 1):
            row = [tuple(bits(rng.randrange(256) / 255) for _ in range(4)) for _ in range(n)]
            compare(row, x=parity, y=3, expected_fast=bool(n), label=f'count {n} {parity}')
    for value in (0x80000000, 0x80000001, 1, 0x337FFFFF, 0x3F800001, 0xBF800000,
                  0x7F800000, 0xFF800000, 0x7FC00001, 0x7F800001):
        compare([(value, value, value, value)], try_reject=True, label=f'reject value {value:x}')
    for alias in ('input_output', 'stack_output', 'self_output', 'bias_output', 'input_stack', 'scale_output'):
        compare([(bits(0.25),) * 4] * 4, alias=alias, try_reject=True, label=f'alias {alias}')
    for align in ('self', 'sp', 'in'):
        compare([(bits(0.25),) * 4], align=align, try_reject=True, label=f'unaligned {align}')
    compare([(bits(0.25), bits(0.5), bits(0.75), 0)] * 5, out_delta=1, label='unaligned output')
    compare([(bits(0.25), bits(0.5), bits(0.75), 0)] * 5, out_delta=2, x=1, y=2, label='unaligned output backward')
    for field in ((0x10, 1), (0x54, 1), (0x5C, 1), (4, 21), (4, 22), (0x68, 4097), (0x34, 0x18000)):
        compare([(bits(0.25),) * 4], field=field, try_reject=True, label=f'unsupported {field}')
    compare([(bits(0.25),) * 4], cw=0x17F, try_reject=True, label='reserved precision')
    compare([(bits(0.25),) * 4], cw=0x37E, try_reject=True, label='unmasked invalid')
    for live in (0, 2, 4, 6):
        for x in range(4):
            compare([(bits(0.25), bits(0.5), bits(0.75), bits(1))] * 8,
                    x=x, y=3, strides=(4096, 256), live=live, label=f'offset/live {x} {live}')
    compare([(bits(0.5),) * 4], live=7, try_reject=True, label='x87 capacity')
    compare([(bits(0.25),) * 4], bias_override=bits(0.1), try_reject=True, label='off-grid bias')
    compare([(bits(0.25),) * 4], bias_override=0x7F800001, try_reject=True, label='signaling NaN bias')
    compare([(bits(0.25), bits(0.5), bits(0.75), 0x7FC00001)] * 4, label='unread NaN in X')

    zero_b = [(bits(0.5), bits(0.5), 0, 0)]
    nonzero_b = [(bits(0.5), bits(0.5), bits(0.5), 0)]
    compare(nonzero_b, backedges=65535, hook=True, try_reject=True, label='yield one edge hits')
    compare(zero_b, backedges=65534, hook=True, try_reject=True, label='yield blue zero adds the extra edge')
    compare(nonzero_b, backedges=65534, hook=True, label='yield one edge still fits')
    compare(zero_b, backedges=65534, hook=False, label='null hook counts both edges')
    compare(nonzero_b * 3, backedges=23, x=1, label='backedge count backward')
    compare([(bits(0.5), bits(0.25), 0, 0), (bits(0.5),) * 4], y=1, table=0xA1051930,
            label='mixed zero blue')

    # Values the guard refuses. Exact sums agree in every host rounding mode;
    # the lifted body is what runs, and it has to match the original.
    for v in (0x7F800000, 0xFF800000, 0x7FC00001, 0x7F800001, 0x80000000,
              bits(-0.25), bits(2.0), bits(2.0 ** 24)):
        compare([(v, v, v, 0)], expected_fast=False, fpcr=0, label=f'original fallback RN {v:x}')
        compare([(v, v, v, 0)], expected_fast=False, fpcr=0xC00000, label=f'original fallback RZ {v:x}')
    compare([(bits(2.0), bits(-0.25), bits(0.5), 0x7FC00001)], x=1, y=0x11110002,
            expected_fast=False, label='original fallback mixed out of range')

    # Error diffusion is on, so the native guard returns and the lifted body
    # (including sub_A0039034) runs. Exact addends keep the chop stores identical
    # to round-nearest; the second row is inexact and is run with host chop.
    # The helper reads pixel errors at buffer+0x10, one float4 per pixel.
    exact = struct.pack('<4f', 0, 0, 0, 0)
    exact += struct.pack('<4f', 0.25, 0.0, 0.5, 0.0)
    exact += struct.pack('<4f', 0.0, 0.25, 0.0, 0.0)
    # Host chop: the lifted fstp follows FPCR, and this row has already set
    # the x87 rounding to chop. Round-nearest stores a different error term.
    compare([(0, 0, 0, 0), (bits(0.5), bits(0.5), bits(0.5), 0)],
            diffusion=exact, loose_x87=True, expected_fast=False, fpcr=0xC00000,
            label='diffusion exact addends')
    messy = struct.pack('<4f', 0, 0, 0, 0) + struct.pack('<4f', 0.1, -0.02, 0.3, 0)
    compare([(bits(0.2), bits(0.4), bits(0.6), 0)], diffusion=messy, loose_x87=True,
            expected_fast=False, fpcr=0xC00000, cw=0x37F, label='diffusion inexact host chop')

    copied = image[0xA1051930 - imagebase:0xA1051930 - imagebase + 128]
    compare([rand_pixel(rng) for _ in range(5)], table=0x190000, table_bytes=copied,
            expected_fast=False, fpcr=0xC00000, label='wrong table address, real dither bytes')

    boundary = []
    for k in range(255):
        mid = bits((k + 0.5) / 255)
        boundary += [(v, v, v, 0) for v in (mid - 1, mid, mid + 1)]
    for pix in boundary:
        assert pixel_ok(pix), hex(pix[0])
    for table in (0xA10518B0, 0xA1051930):
        for x in (0, 1):
            for y in range(4):
                for pc in (0, 0x200, 0x300):
                    for mode in (0, 0xC00000):
                        compare(boundary, x=x, y=y, cw=pc | 0x7F, table=table, fpcr=mode,
                                label=f'byte thresholds {table:x} {x} {y} {pc:x} {mode:x}')
    specials = (0, bits(1.0), 0x33800000, bits(0.5), bits(1 / 255), bits(254 / 255))
    row = [(a, b, c, 0) for a in specials for b in (0, bits(1.0), 0x33800000) for c in (0, bits(0.5), bits(1.0))]
    for y in range(4):
        for x in (0, 1):
            compare(row, x=x, y=y | 0x55000000, table=0xA1051930, label=f'endpoints {x} {y}')

    print(f'PASS original-x86/actual-ARM64 R8G8B8: {cases} cases, {pixels_checked} pixels, '
          f'{fast_rows} fast rows, {fallback_rows} lifted compares, {rejections} no-effect rejections')
    small = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--small-yield'],
                           cwd=ROOT, capture_output=True, text=True)
    (OUT / 'small-yield.txt').write_text(small.stdout + small.stderr, encoding='utf-8')
    assert small.returncode == 0, small.stdout + small.stderr
    print(small.stdout.strip())
    (OUT / 'results.json').write_text(json.dumps(dict(
        cases=cases, pixels_checked=pixels_checked, fast_rows=fast_rows,
        lifted_compares=fallback_rows, no_effect_rejections=rejections,
        lifted_labels=fell_back, rejection_labels=rejected,
        small_yield_no_effect_rejections=6), indent=2), encoding='utf-8')
    print(OUT)
