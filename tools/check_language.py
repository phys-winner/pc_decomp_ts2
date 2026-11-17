"""Check actual language selection C and the original input handler's UTF-8 bridge."""
import _ctypes
import ctypes
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32
from unicorn import x86_const as X

from build_android import CLANG, ROOT, NATIVE_OVERRIDES


def check_original_handler():
    image = pefile.PE(str(ROOT / 'original/Sims2EP9.exe'))
    mapped = image.get_memory_mapped_image()
    cpu = Uc(UC_ARCH_X86, UC_MODE_32)
    cpu.mem_map(image.OPTIONAL_HEADER.ImageBase, (len(mapped) + 4095) & ~4095)
    cpu.mem_write(image.OPTIONAL_HEADER.ImageBase, mapped)
    cpu.mem_map(0, 0x1000)  # the original handler's SEH chain at fs:0
    cpu.mem_map(0x10000, 0x40000)
    cpu.mem_map(0x80000, 0x20000)
    self, table, callback, stop, stack = 0x11000, 0x12000, 0x80000, 0x90000, 0x40000
    cpu.mem_write(self, struct.pack('<I', table))
    cpu.mem_write(table + 0x14, struct.pack('<I', callback))
    cpu.mem_write(self + 0xB4, struct.pack('<I', 8))
    events = []

    def observe(machine, address, size, user):
        if address != callback:
            return
        sp = machine.reg_read(X.UC_X86_REG_ESP)
        ret, kind, flags, message = struct.unpack('<4I', machine.mem_read(sp, 16))
        payload = struct.unpack('<5I', machine.mem_read(message, 20))
        assert kind == 0x5B63748B and flags == 0 and payload[1] == 4 and payload[3] == 1
        events.append(payload[2] & 255)
        machine.reg_write(X.UC_X86_REG_EIP, ret)
        machine.reg_write(X.UC_X86_REG_ESP, sp + 16)
        machine.reg_write(X.UC_X86_REG_EAX, 1)

    cpu.hook_add(UC_HOOK_CODE, observe)
    text = 'ASCII 123 Привет Ёж café 日本語 中文 한국어 ไทย'.encode('utf-8')
    for byte in text:
        cpu.mem_write(stack, struct.pack('<4I', stop, 1, byte, 1))
        cpu.reg_write(X.UC_X86_REG_ESP, stack)
        cpu.reg_write(X.UC_X86_REG_ECX, self)
        cpu.emu_start(0xA0156260, stop, count=1000)
        assert cpu.reg_read(X.UC_X86_REG_EIP) == stop
        assert cpu.reg_read(X.UC_X86_REG_ESP) == stack + 16
    assert bytes(events) == text
    print(f'Original x86 input handler: {len(text)} UTF-8 bytes, Cyrillic/Latin/CJK/Thai, message payloads/stack PASS')


def main():
    source = (ROOT / 'android/host/sims2_shims.c').read_text()
    start = source.index('static const struct { const char *name; uint32_t id; } game_languages[]')
    end = source.index('\nstatic void steam_unknown(', start)
    code = source[start:end]
    expected = dict(english=1, french=2, german=3, italian=4, spanish=5, swedish=6,
                    finnish=7, dutch=8, danish=9, brazilian=10, czech=11, japanese=14,
                    koreana=15, russian=16, schinese=17, tchinese=18, polish=20,
                    thai=21, norwegian=22, portuguese=23, hungarian=25)
    assert dict((name, int(number)) for name, number in re.findall(r'\{"(\w+)", (\d+)\}', code)) == expected
    activity=(ROOT/'android/app/src/org/sims2/port/GamePreferences.java').read_text(encoding='utf-8')
    codes=json.loads('['+re.search(r'CODES=\{(.*?)\};',activity).group(1)+']')
    assert codes==list(expected), 'The launcher picker must expose every original supported language'
    assert 'sub_A0156260' in NATIVE_OVERRIDES
    # The retained byte-input handler's internal encoding is UTF-8, enum 8.
    original = (ROOT / 'generated/sims2-x86/sims2_A0150000.c').read_text()
    handler = original[original.index('void sub_A0156260(void) {'):]
    handler = handler[:handler.index('\n}\n')]
    assert 'RECOMP_CALL(sub_A01A9200)' in handler and 'MEM32(edi + 0xB4)' in handler
    assert 'MEM8(ebp + 0xC)' in handler and '0x5B63748Bu' in handler
    prelude = r'''
typedef unsigned long long size_t;
typedef unsigned int uint32_t;
typedef void FILE;
#define NULL ((void *)0)
__declspec(dllimport) FILE *fopen(const char *, const char *);
__declspec(dllimport) size_t fread(void *, size_t, size_t, FILE *);
__declspec(dllimport) int fclose(FILE *);
__declspec(dllimport) int strcmp(const char *,const char *);
__declspec(dllimport) int isspace(int);
__declspec(dllimport) int isdigit(int);
__declspec(dllimport) int _stricmp(const char *, const char *);
__declspec(dllimport) int _snprintf(char *, size_t, const char *, ...);
__declspec(dllimport) unsigned long strtoul(const char *, char **, int);
__declspec(dllimport) void *memset(void *, int, size_t);
#define strcasecmp _stricmp
#define snprintf _snprintf
#define void_log(...) ((void)0)
static const char *test_steam, *test_numeric, *test_port, *test_system;
static const char *test_getenv(const char *s) { return !_stricmp(s,"S2_STEAM_LANGUAGE") ? test_steam : !_stricmp(s,"S2_LANGUAGE") ? test_numeric : !_stricmp(s,"PORT_OPTION_GAME_LANGUAGE") ? test_port : test_system; }
#define getenv test_getenv
static uint32_t g_ecx, memory[128], seen;
#define MEM32(a) memory[(a)/4]
void sub_A0156260_lifted(void) { seen=MEM32(g_ecx+0xB4); g_ecx=0; }
'''
    wrappers = r'''
__declspec(dllexport) uint32_t check_init(const char *d, const char *s, const char *n) {
    test_steam=s; test_numeric=n; test_port=test_system=0; return s2_language_init(d);
}
__declspec(dllexport) uint32_t check_options(const char *d,const char *p,const char *system) {
    test_steam=test_numeric=0;test_port=p;test_system=system;return s2_language_init(d);
}
__declspec(dllexport) const char *check_name(void) { return steam_language(); }
__declspec(dllexport) int check_input(uint32_t encoding) {
    g_ecx=0x100; MEM32(g_ecx+0xB4)=encoding;
    sub_A0156260(); return seen==8 && MEM32(0x100+0xB4)==encoding && g_ecx==0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-language-') as directory:
        base = Path(directory)
        definition, library = base / 'crt.def', base / 'crt.lib'
        definition.write_text('LIBRARY msvcrt.dll\nEXPORTS\n' + '\n'.join(
            ['strcmp', 'fopen', 'fread', 'fclose', 'isspace', 'isdigit', '_stricmp', '_snprintf', 'strtoul', 'memset']))
        subprocess.run([str(CLANG.with_name('llvm-dlltool.exe')), '-m', 'i386:x86-64',
                        '-d', str(definition), '-l', str(library)], check=True)
        c, obj, dll_path = base / 'check.c', base / 'check.obj', base / 'check.dll'
        c.write_text(prelude + code + wrappers)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-stack-protector',
                        '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll_path), str(obj), str(library)], check=True)
        dll = ctypes.CDLL(str(dll_path))
        dll.check_init.argtypes = [ctypes.c_char_p] * 3
        dll.check_name.restype = ctypes.c_char_p
        dll.check_options.argtypes = [ctypes.c_char_p] * 3
        data = str(base).encode()
        assert dll.check_init(data, None, None) == 1
        path = base / 'language.txt'
        for name, number in expected.items():
            path.write_text(name + '\r\n')
            assert dll.check_init(data, None, None) == number
            assert dll.check_name() == name.encode()
        path.write_text('russian\n')
        assert dll.check_init(data, b'french', b'3') == 2
        assert dll.check_init(data, b'', b'3') == 3
        assert dll.check_init(data, b'RUSSIAN', None) == 16
        for invalid in (b'no-such-language', b'-1', b'+1', b'16junk', b'0', b'99999999999999999999999999999'):
            assert dll.check_init(data, invalid, None) == 1, invalid
        assert dll.check_options(data, b'system', b'german') == 3
        assert dll.check_options(data, b'system', b'english') == 1
        assert dll.check_options(data, b'french', b'german') == 2
        assert dll.check_options(data, b'unsupported', b'russian') == 1
        for encoding in (8, 16, 1251, 1252, 932, 65001):
            assert dll.check_input(encoding), encoding
        # Init must reevaluate persisted choices on every new launch.
        path.write_text('english\n')
        assert dll.check_init(data, None, None) == 1
        path.write_text('russian\n')
        assert dll.check_init(data, None, None) == 16
        print('Language: 21 mappings, persisted reload, env precedence, invalid inputs, UTF-8 handler/restoration PASS')
        _ctypes.FreeLibrary(dll._handle)
    check_original_handler()


if __name__ == '__main__':
    main()
