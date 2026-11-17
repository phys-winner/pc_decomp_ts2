"""Compare packed-name lookup/insert: original x86 versus actual native ARM64.

Both use the same mock locale comparator and vector insertion boundary, so the
check covers traversal and ABI without initializing the entire Windows CRT.
"""
import io
import json
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A, x86_const as X

import build_android as B
from check_virtual_query import function

OUT = B.ROOT / 'build/test/name-lookup'
PRELUDE = r'''
#include "host.h"
recomp_machine_t g_m;
uint32_t bad;
static uint32_t fold(uint32_t c) { return MEM8(0x19000+c); }
static void compare_locale(void) {
    uint32_t a=MEM32(g_esp+4),b=MEM32(g_esp+8),n=MEM32(g_esp+12),result=0;
    for(uint32_t i=0;i<n;++i) { uint32_t x=fold(MEM8(a+i)),y=fold(MEM8(b+i));if(x!=y){result=x-y;break;} }
    g_eax=result;g_ecx=0xCAFE1234;g_edx=0x1234ABCD;g_esp+=4;
}
static void insert_name(void) {
    uint32_t at=MEM32(g_esp+4),from=MEM32(g_esp+8),n=MEM32(g_esp+12);
    if(at!=MEM32(g_ecx+4))bad=1;
    for(uint32_t i=0;i<n;++i)MEM8(at+i)=MEM8(from+i);
    MEM32(g_ecx+4)=at+n;g_eax=at;g_edx=0xAABBCCDD;g_ecx=0xBEAD1234;g_esp+=16;
}
recomp_func_t void_resolve(uint32_t va) {
    if(va==0xA0845872u)return compare_locale;
    if(va==0xA01BE8B6u)return insert_name;
    bad=2;return compare_locale;
}
void void_log(const char *fmt,...) { bad=3; }
void void_lift_trap(uint32_t va,const char *why) { bad=4; }
size_t strlen(const char *s) { size_t n=0;while(s[n])++n;return n; }
int memcmp(const void *a,const void *b,size_t n) { const uint8_t *x=a,*y=b;while(n--){if(*x!=*y)return *x-*y;++x;++y;}return 0; }
'''


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    threads = (B.ROOT / 'android/host/threads.c').read_text()
    cfile, binary = OUT / 'check.c', OUT / 'check.elf'
    cfile.write_text(PRELUDE + function(threads, 'uint32_t void_call_guest(') +
                    function(threads, 'uint32_t void_call_guest_this(') +
                    '#include "' + (B.ROOT / 'android/host/sims2_fast.c').as_posix() + '"\n'
                    'void check_lookup(void) { sub_A01BEFFF(); }\n'
                    'void check_insert(void) { sub_A01BF191(); }\n')
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                    '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,check_lookup,-u,check_insert',
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
    current_fold = bytes(range(256))

    def hook(uc, address, size, unused):
        if address not in (0xA0845872, 0xA01BE8B6):
            return
        sp = uc.reg_read(X.UC_X86_REG_ESP)
        ret, a, b, n = struct.unpack('<4I', uc.mem_read(sp, 16))
        if address == 0xA0845872:
            result = 0
            for first, second in zip(uc.mem_read(a, n), uc.mem_read(b, n)):
                first, second = current_fold[first], current_fold[second]
                if first != second:
                    result = (first - second) & 0xFFFFFFFF
                    break
            uc.reg_write(X.UC_X86_REG_EAX, result)
            uc.reg_write(X.UC_X86_REG_ECX, 0xCAFE1234); uc.reg_write(X.UC_X86_REG_EDX, 0x1234ABCD)
            uc.reg_write(X.UC_X86_REG_ESP, sp + 4)
        else:
            vector = uc.reg_read(X.UC_X86_REG_ECX)
            assert a == struct.unpack('<I', uc.mem_read(vector + 4, 4))[0]
            uc.mem_write(a, bytes(uc.mem_read(b, n))); uc.mem_write(vector + 4, struct.pack('<I', a + n))
            uc.reg_write(X.UC_X86_REG_EAX, a)
            uc.reg_write(X.UC_X86_REG_ECX, 0xBEAD1234); uc.reg_write(X.UC_X86_REG_EDX, 0xAABBCCDD)
            uc.reg_write(X.UC_X86_REG_ESP, sp + 16)
        uc.reg_write(X.UC_X86_REG_EIP, ret)

    x86.hook_add(UC_HOOK_CODE, hook)
    rng = random.Random(0xA01BEFFF)
    registers = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
                 X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
    # Volatile EDX/flags/scratch below ESP are not part of the native lookup ABI.
    observed = (0, 1, 3, 4, 5, 6, 7)
    tested = 0
    for locale in ('C', 'CP1251', 'Turkish'):
        fold = list(range(256))
        for value in range(65, 91):
            fold[value] = value + 32
        if locale == 'CP1251':
            for value in range(0xC0, 0xE0):
                fold[value] = value + 32
            fold[0xA8] = 0xB8
        if locale == 'Turkish':
            fold[ord('I')] = 0xFD
            fold[0xDD] = ord('i')
        current_fold = bytes(fold)
        for case in range(128):
            names = [b'Alpha', b'cameraHeading', b'', b'I', b'\xC0\xC1', b'\xDD', b'longPropertyNameHere']
            names += [bytes(rng.randrange(1, 256) for _ in range(rng.randrange(1, 30))) for _ in range(case % 11)]
            if case % 13 == 0:
                names = []
            pool = [b'alpha', b'CAMERAHEADING', b'', b'i', b'\xE0\xE1', b'\xFD', b'LongPropertyNameHere', b'missing'] + names
            query = pool[case % len(pool)]
            for insertion in (False, True):
                memory = bytearray(0x10000)
                self, key, table, sp = 0x11000, 0x12000, 0x14000, 0x80000
                payload = b''.join(name + b'\0' for name in names)
                struct.pack_into('<3I', memory, self + 0x18 - 0x10000, table, table + len(payload), table + 0x1000)
                memory[table - 0x10000:table - 0x10000 + len(payload)] = payload
                chars = key if len(query) <= 15 else 0x18000
                if chars != key:
                    struct.pack_into('<I', memory, key - 0x10000, chars)
                memory[chars - 0x10000:chars - 0x10000 + len(query) + 1] = query + b'\0'
                struct.pack_into('<2I', memory, key + 16 - 0x10000, len(query), max(15, len(query)))
                memory[0x9000:0x9100] = current_fold
                initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
                args = struct.pack('<2I', stop, key)
                x86.mem_write(0x10000, bytes(memory)); x86.mem_write(sp, args)
                for reg, value in zip(registers, initial):
                    x86.reg_write(reg, value)
                x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
                x86.emu_start(0xA01BF191 if insertion else 0xA01BEFFF, stop, count=200000)
                assert x86.reg_read(X.UC_X86_REG_EIP) == stop
                arm.mem_write(guest + 0x10000, bytes(memory)); arm.mem_write(guest + sp, args)
                arm.mem_write(guest + 0xA10DFCAC, struct.pack('<I', locale != 'C'))
                arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(32))
                arm.mem_write(base + symbols['bad'], bytes(4))
                arm.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); arm.reg_write(A.UC_ARM64_REG_LR, stop)
                arm.emu_start(base + symbols['check_insert' if insertion else 'check_lookup'], stop, count=200000)
                assert arm.reg_read(A.UC_ARM64_REG_PC) == stop
                actual = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
                expected = [x86.reg_read(r) for r in registers]
                assert [actual[i] for i in observed] == [expected[i] for i in observed], (locale, case, insertion, actual, expected)
                assert bytes(arm.mem_read(guest + 0x10000, 0x10000)) == bytes(x86.mem_read(0x10000, 0x10000)), (locale, case, insertion)
                assert int.from_bytes(arm.mem_read(base + symbols['bad'], 4), 'little') == 0
                tested += 1
    result = {'cases': tested, 'pass': True, 'locales': ['C', 'CP1251-style', 'Turkish-style'],
              'method': 'original x86 / actual ARM64 native lookup and shared locale-comparison/insertion boundary mocks',
              'checked': 'return, ECX, ESP, callee-saved GPRs, non-stack guest bytes; empty, inline/heap, misses/insertion, non-ASCII and locale-sensitive I',
              'limits': 'locale CRT internals mocked; volatile EDX, flags and stack scratch excluded from native ABI comparison'}
    (OUT / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS packed-name actual x86/ARM64 differential:', result)


if __name__ == '__main__':
    main()
