"""Compare the native CRT strncmp (0xA08318B0), memchr (0xA0828340), strcspn (0xA08413C0) and the byte
std::find (0xA08212F0, __std_find_trivial_impl<_Find_traits_1, unsigned char>) of android/host/crt_native.c
with the original x86 code of Sims2EP9.exe.

The originals run under Unicorn x86 (std::find with its SSE2 path and its byte loop, dword_A104F518 = 2 and 0;
its AVX2 path is not run), the natives as an ARM64 image under Unicorn ARM64 on the same guest bytes: random
and adversarial strings at every alignment, counts of 0, short, exact and past the terminator, bytes above
0x7F, hits in the first, middle and last position, misses. EAX and ESP must match.
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

OUT = B.ROOT / 'build/test/native-crt-search'
PRELUDE = r'''
#include "host.h"
recomp_machine_t g_m;
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = s; for (; n; --n, ++p) if (*p == (unsigned char)c) return (void *)p; return 0; }
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    for (; s[n]; ++n) for (const char *r = reject; *r; ++r) if (*r == s[n]) return n;
    return n;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) ++n; return n; }
void *memmove(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; if (p < q) for (size_t i = 0; i < n; ++i) p[i] = q[i]; else while (n) { --n; p[n] = q[n]; } return d; }
void *memset(void *d, int c, size_t n) { unsigned char *p = d; for (size_t i = 0; i < n; ++i) p[i] = (unsigned char)c; return d; }
'''
ENTRIES = {'strncmp': 0xA08318B0, 'memchr': 0xA0828340, 'strcspn': 0xA08413C0, 'find': 0xA08212F0}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(PRELUDE + '#include "' + (B.ROOT / 'android/host/crt_native.c').as_posix() + '"\n' +
                     ''.join(f'void check_{name}(void) {{ sub_{va:08X}(); }}\n' for name, va in ENTRIES.items()))
    keep = ','.join('-u,check_' + name for name in ENTRIES)
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', f'-Wl,--gc-sections,-e,check_strncmp,{keep}',
                    str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for at, size in ((base, 0x200000), (guest + 0x10000, 0xF0000), (stack, 0x10000), (stop, 0x1000)):
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
    x86.mem_map(0x10000, 0xF0000)
    x86.mem_map(stop, 4096)
    rng = random.Random(0xA08318B0)
    sp = 0x80000

    def run_arm(name, memory, args):
        image = bytearray(0xF0000)
        for at, data in memory:
            image[at - 0x10000:at - 0x10000 + len(data)] = data
        struct.pack_into('<4I', image, sp - 0x10000, stop, *args, *([0] * (3 - len(args))))
        arm.mem_write(guest + 0x10000, bytes(image))
        regs = [0x11111111, 0x22222222, 0x33333333, sp, 0x55555555, 0x66666666, 0x77777777, 0x88888888]
        arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *regs) + bytes(32))
        arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000)
        arm.reg_write(A.UC_ARM64_REG_LR, stop)
        arm.emu_start(base + symbols['check_' + name], stop, count=2000000)
        assert arm.reg_read(A.UC_ARM64_REG_PC) == stop
        r = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
        return r[0], r[3], (r[4], r[5], r[6], r[7])

    def run_x86(va, memory, args, isa=2):
        x86.mem_write(0x10000, bytes(0xF0000))
        for at, data in memory:
            x86.mem_write(at, data)
        x86.mem_write(sp, struct.pack('<4I', stop, *args, *([0] * (3 - len(args)))))
        x86.mem_write(0xA104F518, struct.pack('<I', isa))
        for reg, value in ((X.UC_X86_REG_EBX, 0x55555555), (X.UC_X86_REG_EBP, 0x66666666),
                           (X.UC_X86_REG_ESI, 0x77777777), (X.UC_X86_REG_EDI, 0x88888888)):
            x86.reg_write(reg, value)
        x86.reg_write(X.UC_X86_REG_ESP, sp)
        x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
        x86.emu_start(va, stop, count=2000000)
        assert x86.reg_read(X.UC_X86_REG_EIP) == stop
        saved = tuple(x86.reg_read(r) for r in (X.UC_X86_REG_EBX, X.UC_X86_REG_EBP, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI))
        return x86.reg_read(X.UC_X86_REG_EAX), x86.reg_read(X.UC_X86_REG_ESP), saved

    def compare(name, memory, args, isas=(2,)):
        expected = None
        for isa in isas:
            got = run_x86(ENTRIES[name], memory, args, isa)
            assert got[2] == (0x55555555, 0x66666666, 0x77777777, 0x88888888), (name, 'x86 callee-saved')
            if expected is None:
                expected = got[:2]
            assert got[:2] == expected, (name, 'isa', isa, memory, args, got, expected)
        eax, esp, saved = run_arm(name, memory, args)
        assert saved == (0x55555555, 0x66666666, 0x77777777, 0x88888888), (name, 'native callee-saved')
        assert (eax, esp) == expected, (name, memory, args, hex(eax), hex(esp), [hex(v) for v in expected])

    alphabet = b'abcXYZ09 _-\t\x80\xe0\xff'
    counts = {name: 0 for name in ENTRIES}
    a_at, b_at = 0x30000, 0x40000
    for case in range(1500):
        # strncmp: equal prefixes, a differing byte (above/below), the terminator first, counts around both
        length = rng.randrange(0, 40)
        s = bytes(rng.choice(alphabet[:-3] + b'\x80\xe0\xff') for _ in range(length))
        t = bytearray(s)
        if t and rng.random() < 0.7:
            i = rng.randrange(len(t))
            t[i] = rng.choice([1, 0x20, 0x7F, 0x80, 0xFF, t[i] ^ 1])
        if rng.random() < 0.2:
            t = t[:rng.randrange(0, len(t) + 1)]
        a = a_at + rng.randrange(0, 16)
        b = b_at + rng.randrange(0, 16)
        tail = bytes(rng.randrange(1, 256) for _ in range(24))
        memory = [(a, s + b'\0' + tail), (b, bytes(t) + b'\0' + tail)]
        n = rng.choice([0, 1, 2, 3, 4, 5, 7, 8, len(s), len(s) + 1, len(s) + 9, 0xFFFFFFFF, rng.randrange(0, 64)])
        compare('strncmp', memory, (a, b, n))
        counts['strncmp'] += 1
        # memchr: hits at the start, the middle, the end, misses, a value above 0xFF
        buf = bytes(rng.choice(alphabet) for _ in range(rng.randrange(0, 80)))
        at = a_at + rng.randrange(0, 16)
        value = rng.choice([ord('a'), ord('Z'), 0, 0x80, 0xFF, 0x1FF & (0x100 | ord('b')), ord('q')])
        count = rng.choice([0, 1, len(buf), max(0, len(buf) - 1), rng.randrange(0, len(buf) + 1)])
        compare('memchr', [(at, buf + bytes(16))], (at, value, count))
        counts['memchr'] += 1
        # strcspn: control sets with and without high bytes, the empty set
        control = bytes(rng.choice(alphabet) for _ in range(rng.randrange(0, 6)))
        compare('strcspn', [(a, buf.replace(b'\0', b'x') + b'\0'), (b, control.replace(b'\0', b'y') + b'\0')], (a, b))
        counts['strcspn'] += 1
        # std::find: ranges of 0..80 bytes at every alignment, so the SSE2 blocks and the tail both run
        first = a_at + rng.randrange(0, 32)
        last = first + rng.choice([0, 1, 15, 16, 17, 31, 32, 33, len(buf)])
        compare('find', [(first, buf + bytes(rng.randrange(0, 256) for _ in range(96)))], (first, last, value),
                isas=(2, 0))
        counts['find'] += 1
    result = {'cases': counts, 'pass': True,
              'checked': 'original x86 (std::find: SSE2 and byte paths) vs ARM64 native: EAX, ESP and the '
                         'callee-saved EBX/EBP/ESI/EDI'}
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS native strncmp/memchr/strcspn/std::find:', result)


if __name__ == '__main__':
    main()
