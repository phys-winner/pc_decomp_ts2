"""Verify the actual shader-program lookup on AArch64, without a device.

Also report bucket-chain work for aligned shader pointers. These are synthetic
lookup counts, not frame-rate measurements. Requires Unicorn and pyelftools.
"""
from pathlib import Path
import subprocess

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0
from elftools.elf.elffile import ELFFile

from build_android import CLANG, ROOT

out = ROOT / 'build/test/render-hotpaths/programs'
out.mkdir(parents=True, exist_ok=True)
shader = (ROOT / 'android/host/d3d9_shader.c').read_text()
actual = shader[shader.index('typedef struct { int type, index, count; } umap_t;'):shader.index('static GLuint compile(')]
ff = (ROOT / 'android/host/d3d9_ff.c').read_text()
ff_types = ff[ff.index('typedef struct ff_held {'):ff.index('} ff_locs;') + len('} ff_locs;')]
ff_start = ff.index('void d3d_ff_forget_program(')
ff_cleanup = ff[ff_start:ff.index('\n}\n', ff_start) + 3]
source = '#include "d3d9_internal.h"\n' + ff_types + '\nstatic ff_locs *locs_table[64];\n' + ff_cleanup + actual + r'''
#define COUNT 2048
static program_t entries[COUNT + 4];
static ff_locs locations[COUNT + 4];
static uint32_t old_heads[1024], old_next[COUNT];
static uint8_t freed_entries[COUNT + 4], freed_locations[COUNT + 4], freed_arrays[COUNT + 4];
static uint32_t deleted_programs, deleted_shaders, deleted_borrowed, unexpected_free;
uint64_t stats[6];

void glDeleteProgram(GLuint program) { if (program) ++deleted_programs; }
void d3d_gl_delete_program(GLuint program) { glDeleteProgram(program); }
void glDeleteShader(GLuint shader) { if (shader == 99) ++deleted_borrowed; if (shader) ++deleted_shaders; }
void free(void *ptr) {
    uintptr_t p = (uintptr_t)ptr;
    if (!ptr) return;
    if (p >= (uintptr_t)entries && p < (uintptr_t)(entries + COUNT + 4) &&
        (p - (uintptr_t)entries) % sizeof(program_t) == 0) {
        ++freed_entries[(p - (uintptr_t)entries) / sizeof(program_t)]; return;
    }
    if (p >= (uintptr_t)locations && p < (uintptr_t)(locations + COUNT + 4) &&
        (p - (uintptr_t)locations) % sizeof(ff_locs) == 0) {
        ++freed_locations[(p - (uintptr_t)locations) / sizeof(ff_locs)]; return;
    }
    if (p >= 0x60000000u && p < 0x60000000u + (COUNT + 4) * 8 * 16 && !(p & 15)) {
        uint32_t allocation = (p - 0x60000000u) / 16;
        uint8_t bit = 1u << (allocation & 7);
        if (freed_arrays[allocation / 8] & bit) ++unexpected_free;
        freed_arrays[allocation / 8] |= bit; return;
    }
    ++unexpected_free;
}

static void seed_resources(uint32_t i) {
    program_t *p = &entries[i];
    uintptr_t base = 0x60000000u + i * 8 * 16;
    p->vs_f = (float *)(base + 0 * 16); p->ps_f = (float *)(base + 1 * 16);
    p->vs_i = (int32_t *)(base + 2 * 16); p->ps_i = (int32_t *)(base + 3 * 16);
    p->vs_b = (int32_t *)(base + 4 * 16); p->ps_b = (int32_t *)(base + 5 * 16);
    p->vs_f_loc = (GLint *)(base + 6 * 16); p->ps_f_loc = (GLint *)(base + 7 * 16);
    p->gl = 1000 + i; p->own_vs = i & 1; p->own_ps = (i & 2) != 0;
    p->vs_gl = p->own_vs ? 10000 + i * 2 : 99;
    p->ps_gl = p->own_ps ? 10001 + i * 2 : 99;
    ff_locs *l = &locations[i]; l->program = p->gl;
    l->next = locs_table[p->gl & 63]; locs_table[p->gl & 63] = l;
}

uint32_t selftest(void) {
    for (uint32_t i = 0; i < COUNT; ++i) {
        program_t *p = &entries[i];
        p->vs = (d3d_shader *)(uintptr_t)(0x10000000u + (i & 63) * 64u);
        p->ps = (d3d_shader *)(uintptr_t)(0x20000000u + (i >> 6) * 128u);
        uint32_t h = program_bucket(p->vs, p->ps, 0, 0);
        p->next = programs[h]; programs[h] = p;
        uint32_t old = ((uintptr_t)p->vs * 31u + (uintptr_t)p->ps * 17u) & 1023u;
        old_next[i] = old_heads[old]; old_heads[old] = i + 1;
    }
    for (uint32_t i = 0; i < 1024; ++i) {
        if (programs[i]) ++stats[0];
        if (old_heads[i]) ++stats[1];
    }
    for (uint32_t i = 0; i < COUNT; ++i) {
        program_t *p = &entries[i]; uint32_t h = UINT32_MAX;
        current_program = NULL;
        if (find_program(p->vs, p->ps, 0, 0, &h) != p || h >= 1024) return 1;
        for (program_t *q = programs[h]; q; q = q->next) { ++stats[2]; if (q == p) break; }
        uint32_t old = ((uintptr_t)p->vs * 31u + (uintptr_t)p->ps * 17u) & 1023u;
        for (uint32_t j = old_heads[old]; j; j = old_next[j-1]) { ++stats[3]; if (j == i + 1) break; }
        current_program = p; h = UINT32_MAX;
        if (find_program(p->vs, p->ps, 0, 0, &h) != p || h != UINT32_MAX) return 2;
        if (find_program(p->vs, p->ps, 1, 0, &h)) return 3;
        if (find_program(p->vs, p->ps, 0, 1, &h)) return 4;
        if (find_program((d3d_shader *)((uintptr_t)p->vs + 1), p->ps, 0, 0, &h)) return 5;
        if (find_program(p->vs, (d3d_shader *)((uintptr_t)p->ps + 1), 0, 0, &h)) return 6;
    }
    /* Generated fixed-function halves use a NULL shader plus their ID. */
    for (uint32_t i = 0; i < 2; ++i) {
        program_t *p = &entries[COUNT+i];
        p->vs = i ? entries[0].vs : NULL; p->ps = i ? NULL : entries[0].ps;
        p->bgra_mask = 0xA5; p->ff_id = 100 + i;
        uint32_t h = program_bucket(p->vs,p->ps,p->bgra_mask,p->ff_id);
        p->next=programs[h];programs[h]=p;current_program=NULL;
        if(find_program(p->vs,p->ps,p->bgra_mask,p->ff_id,&h)!=p)return 7;
        current_program=p;
        if(find_program(p->vs,p->ps,p->bgra_mask,p->ff_id+10,&h))return 8;
    }
    /* Retire a shader appearing in many vertex keys and also a pixel key. */
    d3d_shader *retired = entries[0].vs;
    program_t *pixel = &entries[COUNT + 2];
    pixel->vs = entries[1].vs; pixel->ps = retired; pixel->bgra_mask = 0x55;
    uint32_t h = program_bucket(pixel->vs,pixel->ps,pixel->bgra_mask,0);
    pixel->next = programs[h]; programs[h] = pixel;
    uint32_t targets = 0, shaders = 0;
    for (uint32_t i = 0; i < COUNT + 3; ++i) if (entries[i].vs == retired || entries[i].ps == retired) {
        seed_resources(i); ++targets; shaders += entries[i].own_vs + entries[i].own_ps;
    }
    /* A colliding, unrelated location-cache entry must remain untouched. */
    ff_locs *unrelated = &locations[COUNT+3]; unrelated->program = 0xBEEF;
    unrelated->next = locs_table[unrelated->program & 63]; locs_table[unrelated->program & 63] = unrelated;
    current_program = &entries[0];
    programs_forget_shader(retired);
    if (current_program || deleted_programs != targets || deleted_shaders != shaders || deleted_borrowed || unexpected_free) return 9;
    for (uint32_t i = 0; i < COUNT + 3; ++i) {
        program_t *p = &entries[i];
        int target = p->vs == retired || p->ps == retired;
        if (freed_entries[i] != target || freed_locations[i] != target || freed_arrays[i] != (target ? 255 : 0)) return 10;
        if (target && find_program(p->vs,p->ps,p->bgra_mask,p->ff_id,&h)) return 11;
        if (!target && find_program(p->vs,p->ps,p->bgra_mask,p->ff_id,&h) != p) return 12;
    }
    if (freed_locations[COUNT+3]) return 13;
    /* Reuse the exact shader identity and GL program ID with fresh objects. */
    program_t *replacement = &entries[COUNT+3];
    replacement->vs = retired; replacement->ps = entries[0].ps;
    if (find_program(replacement->vs,replacement->ps,0,0,&h)) return 14;
    replacement->next = programs[h]; programs[h] = replacement;
    replacement->gl = entries[0].gl;
    if (find_program(replacement->vs,replacement->ps,0,0,&h) != replacement) return 15;
    for (ff_locs *l = locs_table[replacement->gl & 63]; l; l = l->next)
        if (l->program == replacement->gl) return 16;
    current_program = &entries[1];
    programs_forget_shader(retired);
    if (current_program != &entries[1] || freed_entries[COUNT+3] != 1 || deleted_programs != targets + 1) return 17;
    programs_forget_shader(retired);
    if (deleted_programs != targets + 1 || unexpected_free || freed_locations[COUNT+3]) return 18;
    stats[4] = targets;
    return 0;
}
'''
(out / 'check.c').write_text(source)
binary = out / 'check.elf'
subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11', '-ffreestanding',
                '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie', '-nostdlib', '-no-pie',
                '-Wl,-e,selftest', '-Wl,--build-id=none', '-Iandroid/runtime', '-Iandroid/host',
                str(out / 'check.c'), '-o', str(binary)], cwd=ROOT, check=True)
mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
with binary.open('rb') as stream:
    elf = ELFFile(stream)
    for seg in elf.iter_segments():
        if seg['p_type'] != 'PT_LOAD':
            continue
        lo = seg['p_vaddr'] & ~4095
        hi = (seg['p_vaddr'] + seg['p_memsz'] + 4095) & ~4095
        mu.mem_map(lo, hi - lo)
        mu.mem_write(seg['p_vaddr'], seg.data())
    entry = elf['e_entry']
    stats = elf.get_section_by_name('.symtab').get_symbol_by_name('stats')[0]['st_value']
mu.mem_map(0x10000000, 0x40000)
mu.reg_write(UC_ARM64_REG_SP, 0x1003FFF0)
mu.reg_write(UC_ARM64_REG_LR, 0xF0000000)
mu.emu_start(entry, 0xF0000000, timeout=120000000)
assert mu.reg_read(UC_ARM64_REG_PC) == 0xF0000000, 'lookup check timed out'
assert mu.reg_read(UC_ARM64_REG_X0) == 0, f'lookup check failed: {mu.reg_read(UC_ARM64_REG_X0)}'
values = [int.from_bytes(mu.mem_read(stats + i * 8, 8), 'little') for i in range(5)]
assert values[0] > values[1] and values[2] < values[3], values
print('PASS: 2048 keys, every key field, collisions, current-program hits and fixed-function halves on AArch64')
print(f'Aligned synthetic keys: occupied buckets {values[1]} -> {values[0]}; '
      f'total comparisons {values[3]} -> {values[2]} ({values[2]/2048:.2f} per lookup). No FPS claim.')
print(f'PASS: eviction of {values[4]} VS/PS programs, eight owned arrays each, private shader halves, '
      'FF locations, current-program invalidation, repeated cleanup and exact shader/GL ID reuse')
