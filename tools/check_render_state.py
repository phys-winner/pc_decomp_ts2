"""Compile and verify the current render-state patches on AArch64 Unicorn.

    python tools/check_render_state.py

Generated C and ELF files stay in build/test/render-state. Requires Unicorn,
pyelftools, and the project's configured Android NDK.
"""
from pathlib import Path
import os
import subprocess
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0
from elftools.elf.elffile import ELFFile

root = Path(__file__).resolve().parents[1]
out = root / 'build/test/render-state'
out.mkdir(parents=True, exist_ok=True)
thread = (root / 'android/host/d3d9_thread.c').read_text()
snippet = thread[thread.index('static d3d_device mirror;'):thread.index('void d3d_rt_sync_state(')]
dx = (root / 'android/host/d3dx9.c').read_text()
dx_type_start = dx.index('typedef struct {', dx.index('constant tables */'))
dx_type_end = dx.index('} ctable_t;', dx_type_start) + len('} ctable_t;')
dx_writers_start = dx.index('static void write_f(')
dx_writers_end = dx.index('/* Set a constant', dx.index('static void write_b('))
source = r'''
#include "d3d9_internal.h"
uint32_t d3d_constant_generation;
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    for (size_t i = 0; i < n; ++i) d[i] = s[i]; return dst;
}
void *memset(void *dst, int value, size_t n) {
    unsigned char *d = dst; for (size_t i = 0; i < n; ++i) d[i] = value; return dst;
}
int memcmp(const void *aa, const void *bb, size_t n) {
    const unsigned char *a = aa, *b = bb;
    while (n >= 8) {
        uint64_t x, y; __builtin_memcpy(&x, a, 8); __builtin_memcpy(&y, b, 8);
        if (x != y) break;
        a += 8; b += 8; n -= 8;
    }
    for (size_t i = 0; i < n; ++i) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

'''+snippet+dx[dx_type_start:dx_type_end]+r'''
d3d_device *g_d3d_device;
/* This fixture exercises GLES writers; entering the DXVK boundary is an error. */
void *void_dxvk_native(uint32_t guest) { (void)guest; return NULL; }
void void_dxvk_constants(uint32_t device,int pixel,int type,uint32_t start,const void *data,uint32_t count) {
    (void)device;(void)pixel;(void)type;(void)start;(void)data;(void)count;__builtin_trap();
}
'''+dx[dx_writers_start:dx_writers_end]+r'''
static d3d_device test_device;
static uint8_t payload[sizeof(d3d_state) * 2];
static uint32_t check_sync(d3d_device *d, int expect_empty) {
    patcher w = { payload, NULL, 0 };
    compare(&w, (uint8_t *)&d->st, (uint8_t *)&sent, d->state_dirty, d->state_dirty_active, sizeof d->state_dirty_active / 4);
    if (expect_empty && w.at != payload) return 1;
    if (memcmp(&sent, &d->st, sizeof sent)) return 2;
    uint16_t end[2] = {0, 0}; memcpy(w.at, end, 4);
    apply_state(payload);
    if (memcmp(&mirror.st, &d->st, sizeof sent)) return 3;
    for (uint32_t i = 0; i < sizeof d->state_dirty / 4; ++i) if (d->state_dirty[i]) return 4;
    for (uint32_t i = 0; i < sizeof d->state_dirty_active / 4; ++i) if (d->state_dirty_active[i]) return 5;
    return 0;
}
uint32_t selftest(void) {
    d3d_device *d = &test_device;
    uint32_t seed = 0x12345678u, result;
    uint8_t *bytes = (uint8_t *)&d->st;
    for (uint32_t i = 0; i < sizeof d->st; ++i) {
        seed = seed * 1664525u + 1013904223u; bytes[i] = seed >> 24;
    }
    d3d_dirty_all(d);
    if ((result = check_sync(d, 0))) return 10 + result;
    if ((result = check_sync(d, 1))) return 20 + result;
    for (uint32_t i = 0; i < sizeof d->st; ++i) {
        bytes[i] ^= 0xAA;
        d3d_dirty_range(d, i, 1);
        if ((i & 63u) == 63u || i + 1 == sizeof d->st) {
            if ((result = check_sync(d, 0))) return 1000000u + i * 10 + result;
        }
    }
    for (uint32_t test = 0; test < 4096; ++test) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t start = seed % sizeof d->st;
        seed = seed * 1664525u + 1013904223u;
        uint32_t count = seed % (sizeof d->st - start + 1u);
        for (uint32_t i = start; i < start + count; ++i) bytes[i] ^= (uint8_t)seed;
        d3d_dirty_range(d, start, count);
        if ((result = check_sync(d, 0))) return 2000000u + test * 10 + result;
        if ((result = check_sync(d, 1))) return 3000000u + test * 10 + result;
    }
    d->st.transforms[511][15] += 1;
    d3d_dirty_transform(d, 511);
    if ((result = check_sync(d, 0))) return 30 + result;
    d->st.clip_planes[5][3] += 1;
    d3d_dirty_field(d, clip_planes[5]);
    if ((result = check_sync(d, 0))) return 40 + result;
    d3d_dirty_range(d, 0, 0);
    if ((result = check_sync(d, 1))) return 50 + result;
    g_d3d_device = d;
    ctable_t table = {0};
    float fv[16]; int32_t iv[16];
    for (int i = 0; i < 16; ++i) { fv[i] = i + 0.25f; iv[i] = i - 8; }
    for (table.pixel = 0; table.pixel < 2; ++table.pixel) {
        uint32_t last = table.pixel ? 224 : D3D_SOFTWARE_VS_CONSTANTS;
        uint32_t ilast = table.pixel ? 16 : D3D_SOFTWARE_VS_INT_CONSTANTS;
        uint32_t blast = table.pixel ? 16 : D3D_SOFTWARE_VS_BOOL_CONSTANTS;
        for (uint32_t start = 0; start < last; ++start) {
            write_f(&table, start, fv, 4);
            if ((result = check_sync(d, 0))) return 4000000u + start * 10 + result;
        }
        for (uint32_t start = 0; start < ilast; ++start) {
            write_i(&table, start, iv, 4);
            if ((result = check_sync(d, 0))) return 5000000u + start * 10 + result;
            write_b(&table, start, iv, 4);
            if ((result = check_sync(d, 0))) return 6000000u + start * 10 + result;
        }
        write_f(&table, last, fv, 4);
        write_i(&table, ilast, iv, 4);
        write_b(&table, blast, iv, 4);
        if ((result = check_sync(d, 1))) return 60 + result;
    }
    return 0;
}
'''
(out / 'selfcheck.c').write_text(source)
ndk = Path(os.environ.get('ANDROID_NDK', r'C:\Users\Xilonen\AppData\Local\Android\Sdk\ndk\27.2.12479018'))
clang = ndk / 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe'
subprocess.run([str(clang), '--target=aarch64-linux-android30', '-O2', '-std=gnu11', '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-nostdlib', '-no-pie', '-Wl,-e,selftest', '-Wl,--build-id=none', '-Iandroid/runtime', '-Iandroid/host', str(out / 'selfcheck.c'), '-o', str(out / 'selfcheck.elf')], cwd=root, check=True)
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
with (out / 'selfcheck.elf').open('rb') as stream:
    elf = ELFFile(stream)
    for seg in elf.iter_segments():
        if seg['p_type'] != 'PT_LOAD': continue
        lo = seg['p_vaddr'] & ~4095
        hi = (seg['p_vaddr'] + seg['p_memsz'] + 4095) & ~4095
        mu.mem_map(lo, hi - lo)
        mu.mem_write(seg['p_vaddr'], seg.data())
    entry = elf['e_entry']
mu.mem_map(0x10000000, 0x40000)
mu.reg_write(UC_ARM64_REG_SP, 0x1003FFF0)
mu.reg_write(UC_ARM64_REG_LR, 0xF0000000)
mu.emu_start(entry, 0xF0000000, timeout=120000000)
assert mu.reg_read(UC_ARM64_REG_PC) == 0xF0000000, 'self-check timed out'
result = mu.reg_read(UC_ARM64_REG_X0)
assert result == 0, f'self-check failed with code {result}'
print('PASS: every state byte, 4096 random ranges, zero-length/unchanged/reset/tail/transform patches and actual D3DX float/int/bool writers for both stages on AArch64 Unicorn')
