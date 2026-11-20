"""Run the production shader constant and GLSL bounds checks on the host."""

from audio_probe import CLANG, ROOT, compile_probe
from check_virtual_query import function


PRELUDE = r'''
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define CT_REGISTERS 256u
#define FOURCC(a,b,c,d) ((a)|((b)<<8)|((c)<<16)|((d)<<24))
static uint32_t memory[4096], readable = sizeof memory, next = 1024;
#define GPTR(a) ((char *)memory + (a))
#define MEM16(a) (*(uint16_t *)GPTR(a))
#define MEM32(a) (*(uint32_t *)GPTR(a))
typedef struct void_com_object { void (*destroy)(struct void_com_object *); } void_com_object;
typedef struct { void_com_object com; uint32_t blob, size, handles; int count, pixel; } ctable_t;
static int ctable_class;
static void_com_object *wrapped;
static int void_vm_readable(uint32_t va, uint32_t size) { return va && va <= readable && size <= readable - va; }
static uint32_t void_galloc(uint32_t size) { uint32_t address = next; next += (size + 3) & ~3u; assert(next < sizeof memory); return address; }
static void void_gfree(uint32_t va) { (void)va; }
static uint32_t void_com_wrap(void_com_object *o, int *cls, unsigned extra)
{ assert(cls == &ctable_class && !extra); wrapped = o; return 123; }
static int ct_handle_index(ctable_t *t, uint32_t handle) { return handle == t->handles ? 0 : -1; }
static float floats[4 * CT_REGISTERS];
static int32_t ints[4 * CT_REGISTERS], bools[CT_REGISTERS];
static uint32_t written;
static void write_f(ctable_t *t,uint32_t reg,const float *data,uint32_t count)
{ (void)t; assert(!reg); memcpy(floats, data, count * 16); written = count; }
static void write_i(ctable_t *t,uint32_t reg,const int32_t *data,uint32_t count)
{ (void)t; assert(!reg); memcpy(ints, data, count * 16); written = count; }
static void write_b(ctable_t *t,uint32_t reg,const int32_t *data,uint32_t count)
{ (void)t; assert(!reg); memcpy(bools, data, count * 4); written = count; }
'''


CHECKS = r'''
int main(void) {
    const uint32_t code = 4, blob = code + 12, info = blob + 28, type = blob + 48, size = 68;
    MEM32(code) = 0xfffe0300;
    MEM32(code + 4) = ((1 + size / 4) << 16) | 0xfffe;
    MEM32(code + 8) = FOURCC('C','T','A','B');
    MEM32(blob + 12) = 1; MEM32(blob + 16) = 28;
    MEM32(info) = 64; MEM32(info + 12) = 48;
    memcpy(GPTR(blob + 64), "c0", 3);
    assert(make_ctable(code) == 123 && wrapped->destroy);
    wrapped->destroy(wrapped);
    MEM32(blob + 12) = 9; assert(!make_ctable(code)); MEM32(blob + 12) = 1;
    MEM32(info + 12) = 60; assert(!make_ctable(code)); MEM32(info + 12) = 48;
    readable = blob + size - 1; assert(!make_ctable(code)); readable = sizeof memory;
    memset(GPTR(blob + 64), 'x', 4); assert(!make_ctable(code));

    ctable_t table = {.blob = blob, .handles = 900};
    MEM16(type + 4) = 1; MEM16(type + 6) = 4;
    MEM16(info + 4) = 2; MEM16(info + 8) = 1;
    float input[16] = {7, 11, 12, 13};
    assert(!set_floats(&table, 0, input, 1) && written == 1);
    assert(floats[0] == 7 && !floats[1] && !floats[2] && !floats[3]);
    MEM16(info + 4) = 1; MEM16(info + 8) = 32;
    assert(!set_floats(&table, 0, input, 1) && written == 32 && ints[0] == 7 && !ints[127]);
    MEM16(info + 4) = 0;
    assert(!set_floats(&table, 0, input, 1) && written == 32 && bools[0] == 1 && !bools[31]);

    MEM16(type) = 2;
    MEM16(info + 4) = 2; MEM16(info + 8) = 2;
    MEM16(type + 4) = 5;
    assert(set_matrices(&table, 900, 500, 1, 0) == D3DERR_INVALIDCALL);
    MEM16(type + 4) = 2; MEM16(type + 6) = 3;
    assert(set_matrices(&table, 900, readable - 4, 1, 0) == D3DERR_INVALIDCALL);
    for (unsigned i = 0; i < 16; ++i) input[i] = (float)i;
    memcpy(GPTR(500), input, sizeof input);
    assert(!set_matrices(&table, 900, 500, 1, 1));
    assert(floats[0] == 0 && floats[1] == 4 && floats[2] == 8 &&
           floats[4] == 1 && floats[5] == 5 && floats[6] == 9);

    char *ok = fix_pixel_varyings("vec4 ps_temp = io_5_0;\nvoid main() {}");
    assert(ok && strstr(ok, "in highp vec4 io_5_0;") && strstr(ok, "\tps_temp = io_5_0;"));
    free(ok);
    char large[8192];
    size_t used = 0;
    for (unsigned i = 0; i < 160; ++i)
        used += (size_t)snprintf(large + used, sizeof large - used, "vec4 ps_%u = io_5_%u;\n", i, i);
    assert(!fix_pixel_varyings(large));
    puts("PASS shader bounds: valid and invalid CTAB, short constants, 32 registers, matrix checks, long GLSL");
    return 0;
}
'''


def main():
    d3dx = (ROOT / "android/host/d3dx9.c").read_text(encoding="utf-8")
    glsl = (ROOT / "android/host/d3d9_shader.c").read_text(encoding="utf-8")
    one_line = ("static uint32_t ct_info(", "static uint32_t ct_type(", "static void ct_destroy(")
    actual = [next(line for line in d3dx.splitlines() if line.startswith(name)) for name in one_line]
    names = ("static uint32_t set_floats(", "static uint32_t set_matrices(", "static uint32_t find_ctab(",
             "static uint32_t make_ctable(")
    actual.extend(function(d3dx, name) for name in names)
    actual.extend(function(glsl, name) for name in ("static char *insert_before(", "static char *fix_pixel_varyings("))
    compile_probe("shader-bounds", PRELUDE + "\n".join(actual) + CHECKS, {}, compiler_override=CLANG)


if __name__ == "__main__":
    main()
