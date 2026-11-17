"""Compile and test the actual ProcessVertices GLSL/mask/declaration helpers.

Uses captured preprocessing shaders, the NDK host compiler and Windows CRT.
No device or GPU execution; check_process_vertices.py emits GPU fixtures.
"""
import ctypes
from pathlib import Path
import re
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_process_vertices import CAPTURE, ELEMENT, capture


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]


def main():
    source = (ROOT / 'android/host/d3d9_shader.c').read_text()
    definitions = '\n'.join(function(source, signature) for signature in (
        'static char *insert_before(', 'static char *fix_address_moves(',
        'static char *process_source(', 'static void process_masks(', 'static int process_output('))
    prelude = r'''
typedef unsigned long long size_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
#define NULL ((void *)0)
__declspec(dllimport) void *malloc(size_t);
__declspec(dllimport) void free(void *);
__declspec(dllimport) size_t strlen(const char *);
__declspec(dllimport) char *strchr(const char *, int);
__declspec(dllimport) char *strstr(const char *, const char *);
__declspec(dllimport) int strncmp(const char *, const char *, size_t);
__declspec(dllimport) void *memcpy(void *, const void *, size_t);
__declspec(dllimport) void *memset(void *, int, size_t);
__declspec(dllimport) char *strcpy(char *, const char *);
__declspec(dllimport) int sprintf(char *, const char *, ...);
__declspec(dllimport) int _snprintf(char *, size_t, const char *, ...);
__declspec(dllimport) int sscanf(const char *, const char *, ...);
#define snprintf _snprintf
typedef struct { uint8_t *bytecode; uint32_t size; } d3d_shader;
typedef struct { int outputs, usage[16], index[16]; uint8_t masks[16]; } process_program;
typedef struct { uint16_t stream, offset; uint8_t type, method, usage, usage_index; } d3d_vertex_element;
typedef struct { int count; d3d_vertex_element elements[64]; } d3d_decl;
'''
    wrappers = r'''
__declspec(dllexport) char *check_source(const char *s, int f, int i, int b) { return process_source(s,f,i,b); }
__declspec(dllexport) void check_free(void *p) { free(p); }
__declspec(dllexport) void check_masks(void *code, uint32_t bytes, uint8_t *masks) {
    d3d_shader s = {code,bytes}; process_program p = {0};
    p.outputs=3; p.usage[0]=0; p.usage[1]=3; p.usage[2]=5;
    process_masks(&s,&p); memcpy(masks,p.masks,3);
}
__declspec(dllexport) int check_output(void *elements, int count, uint32_t *stride) {
    d3d_decl d={0}; d.count=count;
    if(count<0||count>64)return 0;
    memcpy(d.elements,elements,count*8); return process_output(&d,stride);
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-process-helpers-') as directory:
        base = Path(directory)
        definition = base / 'crt.def'
        definition.write_text('LIBRARY msvcrt.dll\nEXPORTS\n' + '\n'.join(
            ['malloc', 'free', 'strlen', 'strchr', 'strstr', 'strncmp', 'memcpy', 'memset', 'strcpy', 'sprintf', '_snprintf', 'sscanf']))
        library = base / 'crt.lib'
        subprocess.run([str(CLANG.with_name('llvm-dlltool.exe')), '-m', 'i386:x86-64', '-d', str(definition),
                        '-l', str(library)], check=True)
        code, obj, dll_path = base / 'check.c', base / 'check.obj', base / 'check.dll'
        code.write_text(prelude + definitions + wrappers)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-stack-protector',
                        '-c', str(code), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll_path), str(obj), str(library)], check=True)
        dll = ctypes.CDLL(str(dll_path))
        dll.check_source.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int]
        dll.check_source.restype = ctypes.c_void_p
        dll.check_free.argtypes = [ctypes.c_void_p]
        dll.check_masks.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        dll.check_output.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint32)]
        try:
            for path in sorted(CAPTURE.parent.glob('call_*.vs.bin')):
                shader = path.read_bytes()
                masks = (ctypes.c_uint8 * 3)()
                dll.check_masks(ctypes.create_string_buffer(shader), len(shader), masks)
                assert list(masks) == [7, 7, 3], (path.name, list(masks))
            # Reconstruct raw Mojo output from the diagnostic rendering variant.
            glsl = CAPTURE.with_suffix('.glsl').read_text()
            glsl = '\n'.join(line for line in glsl.splitlines() if 'vpHalfPixel' not in line and not line.startswith('vec4 vs_rel('))
            glsl = re.sub(r'vs_rel\(((?:[^()]|\([^()]*\))*)\)', r'vs_uniforms_vec4[\1]', glsl)
            ptr = dll.check_source(glsl.encode(), 8102, 0, 0)
            assert ptr
            try:
                patched = ctypes.string_at(ptr).decode()
                assert 'uniform vec4 vs_uniforms_vec4[' not in patched
                assert 'vs_uniforms_vec4[' not in patched
                assert 'vpFlip' not in patched and 'gl_Position.z = gl_Position.z *' not in patched
                assert patched.index('vec4 pv_c(') < patched.index('void main()')
                assert '#define vs_c90 pv_c(0)' in patched
                assert 'const vec4 vs_c50 = vec4(765.010009765, -1.0, 1.0, 4.0);' in patched
                assert 'vs_o1.z = dot(vs_r0.xyz, vs_r1.xyz);' in patched
                assert 'i < 8102' in patched
            finally:
                dll.check_free(ptr)
            # Captured skinning shaders use only float constants. Exercise the
            # same native conversion for integer/bool declarations and macros.
            glsl = '''#version 300 es
precision highp float;
uniform ivec4 vs_uniforms_ivec4[2];
uniform bool vs_uniforms_bool[3];
#define vs_i4 vs_uniforms_ivec4[1]
#define vs_b7 vs_uniforms_bool[2]
void main() { gl_Position = vec4(vs_i4); if (vs_b7) gl_Position.x = 0.0; }
'''
            ptr = dll.check_source(glsl.encode(), 0, 2, 3)
            assert ptr
            try:
                patched = ctypes.string_at(ptr).decode()
                assert 'vs_uniforms_ivec4[' not in patched and 'vs_uniforms_bool[' not in patched
                assert '#define vs_i4 pv_i(1)' in patched and '#define vs_b7 pv_b(2)' in patched
                assert 'i < 2' in patched and 'i < 3' in patched and 'int j = 8 + i;' in patched
                assert patched.index('ivec4 pv_i(') < patched.index('void main()')
                assert patched.index('bool pv_b(') < patched.index('void main()')
            finally:
                dll.check_free(ptr)
            declarations, _, _ = capture(CAPTURE)
            packed = b''.join(ELEMENT.pack(*e) for e in declarations[1])
            stride = ctypes.c_uint32()
            assert dll.check_output(ctypes.create_string_buffer(packed), 3, ctypes.byref(stride)) and stride.value == 32
            invalid = bytearray(packed); invalid[5] = 1
            assert not dll.check_output(ctypes.create_string_buffer(bytes(invalid)), 3, ctypes.byref(stride))
            invalid = bytearray(packed); invalid[0] = 1
            assert not dll.check_output(ctypes.create_string_buffer(bytes(invalid)), 3, ctypes.byref(stride))
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('ProcessVertices actual C helpers: 8 captured masks, float/int/bool GLSL, DEF preservation and output declarations PASS')


if __name__ == '__main__':
    main()
