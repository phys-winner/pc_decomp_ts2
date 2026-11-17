"""Compile the real shader-size walker into a host DLL and check SM1/SM2/SM3.

Uses the installed NDK compiler and Python ctypes; no device or native CRT.
"""
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT


def main():
    source = (ROOT / 'android/host/d3d9_shader.c').read_text()
    start = source.index('uint32_t d3d_shader_bytecode_size(')
    end = source.index('\n}\n', start) + 3
    function = source[start:end]
    harness = '''typedef unsigned int uint32_t;
static uint32_t words[512], count, bad_read;
__declspec(dllexport) uint32_t *test_words(void) { return words; }
__declspec(dllexport) void test_reset(uint32_t n) { count = n; bad_read = 0; }
__declspec(dllexport) uint32_t test_bad_read(void) { return bad_read; }
static uint32_t read_word(uint32_t address) {
    if ((address & 3) || address / 4 >= count) { bad_read = 1; return 0x0000FFFF; }
    return words[address / 4];
}
#define MEM32(address) read_word(address)
__declspec(dllexport)
''' + function
    cases = {
        # A negated source's modifier bits look like a one-token SM2 length.
        'ps_1_1 negated source': [0xFFFF0101, 0x00000001, 0x800F0000, 0xB1E40000, 0x0000FFFF],
        'vs_1_1 modified destination': [0xFFFE0101, 0x00000005, 0x820F0000, 0x90E40000, 0xA0E40000, 0x0000FFFF],
        # DEF literals and comments can contain the END bit pattern as data.
        'ps_1_4 DEF and comment': [0xFFFF0104, 0x00000051, 0xA00F0000, 0x0000FFFF,
                                  0x3F800000, 0x40000000, 0x40400000,
                                  0x0002FFFE, 0x0000FFFF, 0x01000000,
                                  0x00000001, 0x800F0000, 0xA1E40000, 0x0000FFFF],
        'ps_2_0 encoded lengths': [0xFFFF0200, 0x05000051, 0xA00F0000, 0x0000FFFF,
                                   0x3F800000, 0x40000000, 0x40400000,
                                   0x02000001, 0x800F0000, 0xA1E40000, 0x0000FFFF],
        'vs_3_0 comment and instruction': [0xFFFE0300, 0x0001FFFE, 0x0000FFFF,
                                         0x02000001, 0x800F0000, 0x91E40000, 0x0000FFFF],
    }
    with tempfile.TemporaryDirectory(prefix='sims2-shader-size-') as directory:
        base = Path(directory)
        code = base / 'check.c'
        obj = base / 'check.obj'
        library = base / 'check.dll'
        code.write_text(harness)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-stack-protector',
                        '-c', str(code), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)],
                       check=True)
        dll = ctypes.CDLL(str(library))
        dll.test_words.restype = ctypes.POINTER(ctypes.c_uint32)
        dll.test_reset.argtypes = [ctypes.c_uint32]
        dll.d3d_shader_bytecode_size.argtypes = [ctypes.c_uint32]
        dll.d3d_shader_bytecode_size.restype = ctypes.c_uint32
        memory = dll.test_words()
        try:
            for name, words in cases.items():
                dll.test_reset(len(words))
                for index, value in enumerate(words):
                    memory[index] = value
                size = dll.d3d_shader_bytecode_size(0)
                assert size == len(words) * 4 and dll.test_bad_read() == 0, (name, size, len(words) * 4)
        finally:
            # Windows keeps a loaded DLL open, so unload it before temporary cleanup.
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print(f'Shader-size actual C: {len(cases)} SM1/SM2/SM3 modifier, DEF, comment and END cases PASS')


if __name__ == '__main__':
    main()
