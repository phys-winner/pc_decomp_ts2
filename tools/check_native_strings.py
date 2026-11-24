"""Compare the native CRT _stricmp (0xA0845799) and strchr (0xA08287A0) of sims2_fast.c with the originals.

strchr: the original x86 code (its SSE2 form and its generic form, __isa_available 1/0) under Unicorn
against the actual ARM64 native, over strings at every alignment, misses, the terminator and bytes
above 0x7F. _stricmp without a locale: the original __ascii_stricmp path under Unicorn against the
native. _stricmp with a locale (__locale_changed set): the native against the CRT's _stricmp_l
semantics over a lower-case table (C, CP1251-style, a table folding a non-ASCII capital to ASCII 'i'),
with the original's locale path as the fallback mock: it must be taken exactly when a differing pair
has a byte above 0x7F, and every other answer must equal the table's.
"""
import io
import json
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/native-strings'
FOLD = 0x19000            # the mock locale's lower-case table (guest)
PRELUDE = r'''
#include "host.h"
recomp_machine_t g_m;
uint32_t bad, fallbacks;
void sub_A0845799_lifted(void) {
    uint32_t a = MEM32(g_esp + 4), b = MEM32(g_esp + 8), f, l;
    ++fallbacks;
    do { f = MEM8(0x19000 + MEM8(a++)); l = MEM8(0x19000 + MEM8(b++)); } while (f == l && f);
    g_eax = f - l; g_ecx = 0xCAFE1234; g_edx = 0x1234ABCD; g_esp += 4;
}
recomp_func_t void_resolve(uint32_t va) { bad = 2; return 0; }
void void_log(const char *fmt, ...) { bad = 3; }
void void_lift_trap(uint32_t va, const char *why) { bad = 4; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
int memcmp(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; ++x; ++y; } return 0; }
char *strchr(const char *s, int c) { for (;; ++s) { if (*s == (char)c) return (char *)s; if (!*s) return 0; } }
'''


def tables():
    c = list(range(256))
    for v in range(65, 91):
        c[v] = v + 32
    cp1251 = list(c)
    for v in range(0xC0, 0xE0):
        cp1251[v] = v + 32
    cp1251[0xA8] = 0xB8
    dotted = list(c)
    dotted[0xDD] = ord('i')     # a non-ASCII capital folding to an ASCII letter (cp1254 'I with dot')
    return {'C': bytes(c), 'CP1251': bytes(cp1251), 'dotted': bytes(dotted)}


def reference(a, b, fold):
    i = 0
    while True:
        x = fold[a[i]] if i < len(a) else 0
        y = fold[b[i]] if i < len(b) else 0
        if x != y or not x:
            return (x - y) & 0xFFFFFFFF
        i += 1


def needs_locale(a, b):
    """Whether the native must ask the guest: a differing pair with a byte above 0x7F before the answer."""
    ascii_fold = tables()['C']
    i = 0
    while True:
        x = a[i] if i < len(a) else 0
        y = b[i] if i < len(b) else 0
        if x == y:
            if not x:
                return False
        else:
            if (x | y) >= 0x80:
                return True
            if ascii_fold[x] != ascii_fold[y]:
                return False
        i += 1


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    threads = (B.ROOT / 'android/host/threads.c').read_text()
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(PRELUDE + function(threads, 'uint32_t void_call_guest(') +
                     function(threads, 'uint32_t void_call_guest_this(') +
                     '#include "' + (B.ROOT / 'android/host/sims2_fast.c').as_posix() + '"\n'
                     'void check_stricmp(void) { sub_A0845799(); }\n'
                     'void check_strchr(void) { sub_A08287A0(); }\n')
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,check_stricmp,-u,check_strchr',
                    str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for at, size in ((base, 0x200000), (guest + 0x10000, 0xF0000), (stack, 0x10000), (stop, 0x1000),
                     (guest + 0xA10DF000, 0x1000)):
        arm.mem_map(at, size)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD':
            arm.mem_write(base + segment['p_vaddr'], segment.data())
    relocs = elf.get_section_by_name('.rela.dyn')
    if relocs:
        for r in relocs.iter_relocations():
            assert r['r_info_type'] == 1027
            arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))
    pe = pefile.PE(str(B.ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    image = pe.get_memory_mapped_image()
    x86 = Uc(UC_ARCH_X86, UC_MODE_32)
    x86.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image) + 65535) & ~65535)
    x86.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
    x86.mem_map(0x10000, 0xF0000); x86.mem_map(stop, 4096)
    rng = random.Random(0xA08287A0)
    sp, text_a, text_b = 0x80000, 0x30000, 0x40000

    def run_arm(entry, args, locale, fold):
        memory = bytearray(0xF0000)
        for at, data in args['memory']:
            memory[at - 0x10000:at - 0x10000 + len(data)] = data
        memory[FOLD - 0x10000:FOLD - 0x10000 + 256] = fold
        struct.pack_into('<3I', memory, sp - 0x10000, stop, *args['stack'])
        arm.mem_write(guest + 0x10000, bytes(memory))
        arm.mem_write(guest + 0xA10DFCAC, struct.pack('<I', locale))
        regs = [0x11111111, 0x22222222, 0x33333333, sp, 0x55555555, 0x66666666, 0x77777777, 0x88888888]
        arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *regs) + bytes(32))
        arm.mem_write(base + symbols['bad'], bytes(4)); arm.mem_write(base + symbols['fallbacks'], bytes(4))
        arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); arm.reg_write(A.UC_ARM64_REG_LR, stop)
        arm.emu_start(base + symbols[entry], stop, count=1000000)
        assert arm.reg_read(A.UC_ARM64_REG_PC) == stop
        r = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
        assert int.from_bytes(arm.mem_read(base + symbols['bad'], 4), 'little') == 0
        return r[0], r[3], int.from_bytes(arm.mem_read(base + symbols['fallbacks'], 4), 'little')

    def run_x86(entry, args, isa, locale):
        x86.mem_write(0x10000, bytes(0xF0000))
        for at, data in args['memory']:
            x86.mem_write(at, data)
        x86.mem_write(sp, struct.pack('<3I', stop, *args['stack']))
        x86.mem_write(0xA10DF81C, struct.pack('<I', isa))
        x86.mem_write(0xA10DFCAC, struct.pack('<I', locale))
        x86.reg_write(X.UC_X86_REG_ESP, sp)
        x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        x86.emu_start(entry, stop, count=1000000)
        assert x86.reg_read(X.UC_X86_REG_EIP) == stop
        return x86.reg_read(X.UC_X86_REG_EAX), x86.reg_read(X.UC_X86_REG_ESP)

    # strchr: every alignment of the start, hits before/at/after 16-byte blocks, misses, c = 0, high bytes
    chr_cases = 0
    for case in range(600):
        length = rng.randrange(0, 70)
        body = bytes(rng.choice(b'abcXYZ09 _-\x80\xe0\xff') for _ in range(length))
        offset = rng.randrange(0, 32)
        at = text_a + offset
        c = rng.choice([0, ord('a'), ord('Z'), 0x80, 0xE0, 0xFF, ord('q'), 0x100 + ord('b'), -1 & 0xFFFFFFFF])
        args = {'memory': [(at, body + b'\0' + bytes(rng.randrange(1, 256) for _ in range(40)))], 'stack': (at, c)}
        expected = None
        for isa in (1, 0):
            eax, esp = run_x86(0xA08287A0, args, isa, 0)
            assert esp == sp + 4
            if expected is None:
                expected = eax
            assert eax == expected, ('isa', isa, case, hex(eax), hex(expected))
        eax, esp, _ = run_arm('check_strchr', args, 0, tables()['C'])
        assert (eax, esp) == (expected, sp + 4), ('strchr', case, hex(eax), hex(expected), body, c)
        chr_cases += 1

    # _stricmp
    words = [b'', b'a', b'A', b'Alpha', b'alpha', b'ALPHA', b'alphabet', b'cameraHeading', b'CAMERAHEADING',
             b'[', b'_', b'`', b'Z', b'z', b'I', b'i', b'\xdd', b'\xc0\xc1', b'\xe0\xe1', b'\xa8', b'\xb8',
             b'Ui\xc0', b'UI\xe0', b'ui\xdd', b'UIi']
    cmp_cases = fallbacks = 0
    for locale_name, fold in tables().items():
        for case in range(500):
            if case < len(words) ** 2 and case < 400:
                a, b = words[case % len(words)], words[(case // len(words)) % len(words)]
            else:
                a = bytes(rng.choice(b'aAbBzZ[_`\xc0\xe0\xdd\xa8\xff01') for _ in range(rng.randrange(0, 12)))
                b = bytearray(a)
                for _ in range(rng.randrange(0, 3)):
                    if b:
                        i = rng.randrange(len(b))
                        b[i] = b[i] ^ 0x20 if rng.random() < 0.7 else rng.randrange(1, 256)
                b = bytes(b) + (bytes([rng.randrange(1, 256)]) if rng.random() < 0.2 else b'')
            args = {'memory': [(text_a, a + b'\0'), (text_b, b + b'\0')], 'stack': (text_a, text_b)}
            for locale in (0, 1):
                table = fold if locale else tables()['C']
                eax, esp, fb = run_arm('check_stricmp', args, locale, table)
                assert esp == sp + 4
                assert eax == reference(a, b, table), (locale_name, locale, a, b, hex(eax), hex(reference(a, b, table)))
                assert fb == (1 if locale and needs_locale(a, b) else 0), (locale_name, locale, a, b, fb)
                fallbacks += fb
                if not locale:
                    x_eax, x_esp = run_x86(0xA0845799, args, 1, 0)
                    assert (x_eax, x_esp) == (eax, esp), ('x86 ascii', a, b, hex(x_eax), hex(eax))
                cmp_cases += 1
    result = {'strchr_cases': chr_cases, 'stricmp_cases': cmp_cases, 'locale_fallbacks': fallbacks, 'pass': True,
              'checked': 'strchr: original SSE2 and generic x86 vs ARM64 native (EAX, ESP); _stricmp: original '
                         '__ascii_stricmp x86 vs native without a locale, CRT _stricmp_l table semantics with one, '
                         'and the guest fallback taken exactly for differing pairs with a byte above 0x7F',
              'limits': 'the locale path itself is mocked by its table semantics (CRT internals not initialized)'}
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS native _stricmp/strchr:', result)


if __name__ == '__main__':
    main()
