"""Compare native integer lookups against the original x86 code, without adb.

    python tools/check_native_lookup.py

Builds actual sims2_fast.c with the NDK, then runs both instruction sets under
Unicorn on identical hash buckets and integer trees, including collisions,
misses, duplicate/high-bit keys, empty containers, aliased hash outputs and
tree iterator increments over balanced/skewed trees and sentinel transitions.
Also covers persistence filename buckets against the original x86 routine.
"""
import hashlib
import io
import json
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32
from unicorn import arm64_const as A, x86_const as X

import build_android as B

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/test/native-lookup"
OUT.mkdir(parents=True, exist_ok=True)
source = OUT / "check.c"
source.write_text('#include "host.h"\nrecomp_machine_t g_m;\n'
                  'uint32_t fallback_calls; void sub_A01D5B50_lifted(void) { ++fallback_calls; g_eax=0xFACADE; }\n'
                  'static int resource_fixture;\n'
                  'int void_vm_readable(uint32_t a,uint32_t n){return resource_fixture&&a>=0x10000&&(uint64_t)a+n<=0x100000;}\n'
                  'int void_vm_writable(uint32_t a,uint32_t n){return void_vm_readable(a,n);}\n'
                  'void sub_A09AB1A0_lifted(void){++fallback_calls;g_eax=0xFACADE;g_esp+=16;}\n'
                  'uint32_t void_call_guest_this(uint32_t a,uint32_t b,int n,const uint32_t *v){return 0;}\n'
                  'void void_log(const char *format,...){ }\n'
                  'void *memcpy(void *d,const void *s,size_t n){for(size_t i=0;i<n;++i)((char *)d)[i]=((const char *)s)[i];return d;}\n'
                  f'#include "{ROOT.as_posix()}/android/host/sims2_fast.c"\n'
                  'uint32_t check(void) { sub_A014189B(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n'
                  'uint32_t check_tree(void) { sub_A0144C35(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n'
                  'uint32_t check_next(void) { sub_A015DC50(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n'
                  'uint32_t check_material(void) { sub_A0ADFAE0(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n'
                  'uint32_t check_attribute(void) { sub_A052F900(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n'
                  'uint32_t check_resource(void){resource_fixture=1;sub_A09AB1A0();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}\n'
                  'uint32_t check_persistence(void) { sub_A01D5B50(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n')
binary = OUT / "check.elf"
subprocess.run([str(B.CLANG), "--target=aarch64-linux-android30", "-O2", "-DS2_NATIVE_LEAF_BENCHMARK", "-mno-outline-atomics", "-ffunction-sections",
                "-fdata-sections", "-fno-stack-protector", "-nostdlib", "-Iandroid/runtime", "-Iandroid/host",
                "-Wl,--gc-sections,-e,check,-u,check_tree,-u,check_next,-u,check_persistence,-u,check_resource,-u,check_material,-u,check_attribute", str(source), "-o", str(binary)], cwd=ROOT, check=True)

elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest_base, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000)
arm.mem_map(guest_base + 0x10000, 0xF0000)
arm.mem_map(native_stack, 0x10000)
arm.mem_map(stop, 0x1000)
arm.mem_map(guest_base + 0xA10DF000, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type'] == 'PT_LOAD':
        arm.mem_write(base + segment['p_vaddr'], segment.data())
relocations = elf.get_section_by_name('.rela.dyn')
if relocations:
    for r in relocations.iter_relocations():
        assert r['r_info_type'] == 1027, 'unexpected ELF relocation'
        arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))

pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
image = pe.get_memory_mapped_image()
x86 = Uc(UC_ARCH_X86, UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image) + 0xFFFF) & ~0xFFFF)
x86.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
x86.mem_map(0x10000, 0xF0000)
x86.mem_map(stop, 0x1000)
regs = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
        X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
rng = random.Random(0xA014189B)
def compare(va, entry, memory, stack, initial, case, extra_addresses=()):
    sp = initial[3]
    x86.mem_write(sp - 128, bytes(256))
    arm.mem_write(guest_base + sp - 128, bytes(256))
    x86.mem_write(0x10000, bytes(memory)); x86.mem_write(sp, stack)
    for register, value in zip(regs, initial): x86.reg_write(register, value)
    x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    x86.emu_start(va, stop, count=10000)
    assert x86.reg_read(X.UC_X86_REG_EIP) == stop, f'x86 runaway in {case}'
    arm.mem_write(guest_base + 0x10000, bytes(memory)); arm.mem_write(guest_base + sp, stack)
    arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(32))
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    arm.emu_start(base + symbols[entry], stop, count=10000)
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, f'ARM runaway in {case}'
    actual = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
    expected = tuple(x86.reg_read(r) for r in regs)
    observed=(0,3,4,5,6,7) if entry=='check_resource' else range(8)
    assert all(actual[i]==expected[i] for i in observed), f'{case}: registers {actual} != {expected}'
    assert bytes(arm.mem_read(guest_base + 0x10000, 0x10000)) == bytes(x86.mem_read(0x10000, 0x10000)), f'{case}: guest memory'
    for address in extra_addresses:
        assert bytes(arm.mem_read(guest_base + address, 4)) == bytes(x86.mem_read(address, 4)), f'{case}: output {address:#x}'
    assert (arm.reg_read(A.UC_ARM64_REG_X0) ^ x86.reg_read(X.UC_X86_REG_EFLAGS)) & 0x8D5 == 0, f'{case}: arithmetic flags'
    if entry == 'check_persistence':
        assert bytes(arm.mem_read(guest_base + sp - 64, 128)) == bytes(x86.mem_read(sp - 64, 128)), f'{case}: exact stack scratch'

for case in range(1024):
    memory = bytearray(rng.randbytes(0x10000))
    def put(at, value):
        struct.pack_into('<I', memory, at - 0x10000, value)
    self, vector, head, key, out, sp = 0x11000, 0x12000, 0x13000, 0x16000, 0x17000, 0x80000
    mask, hashed, length = (1 << rng.randrange(7)) - 1, rng.getrandbits(32), rng.randrange(33)
    nodes = [0x14000 + 32 * i for i in range(length)]
    keys = [rng.choice([0, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, rng.getrandbits(32)]) for _ in nodes]
    query = rng.choice(keys) if keys and case % 2 else rng.getrandbits(32)
    put(self + 4, head); put(self + 12, vector); put(self + 24, mask)
    bucket = vector + (mask & hashed) * 8
    put(bucket, nodes[0] if nodes else head); put(bucket + 4, nodes[-1] if nodes else head)
    for i, node in enumerate(nodes):
        put(node, nodes[i + 1] if i + 1 < length else head)
        put(node + 4, nodes[i - 1] if i else head)
        put(node + 8, keys[i])
    put(key, query)
    if case % 11 == 0: out = key
    if case % 13 == 0 and nodes: out = rng.choice(nodes)
    if case % 17 == 0: out = bucket
    if not length and case % 2: key = 0  # an empty bucket must not read the key
    stack = struct.pack('<4I', stop, out, key, hashed)
    initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    compare(0xA014189B, 'check', memory, stack, initial, f'hash {case}')

for case in range(1024):
    memory = bytearray(rng.randbytes(0x10000))
    def put(at, value):
        struct.pack_into('<I', memory, at - 0x10000, value)
    self, head, key, sp = 0x11000, 0x13000, 0x16000, 0x80000
    keys = sorted(rng.choice([0, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, rng.getrandbits(32)]) for _ in range(rng.randrange(33)))
    def tree(lo, hi):
        if lo >= hi: return head
        mid = lo if case % 3 == 0 else hi - 1 if case % 3 == 1 else (lo + hi) // 2
        node = 0x14000 + mid * 32
        memory[node + 13 - 0x10000] = 0
        put(node, tree(lo, mid)); put(node + 8, tree(mid + 1, hi)); put(node + 16, keys[mid])
        return node
    put(self, head); put(head + 4, tree(0, len(keys)))
    memory[head + 13 - 0x10000] = 1
    query = rng.choice(keys) if keys and case % 2 else rng.getrandbits(32)
    put(key, query)
    if not keys and case % 2: key = 0
    initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    compare(0xA0144C35, 'check_tree', memory, struct.pack('<2I', stop, key), initial, f'tree {case}')

next_cases = 0
for shape in range(3):
    for length in (0, 1, 2, 3, 7, 16, 31, 64):
        memory = bytearray(rng.randbytes(0x10000))
        def put(at, value):
            struct.pack_into('<I', memory, at - 0x10000, value)
        out, head, sp = 0x11000, 0x13000, 0x80000
        nodes = [0x14000 + 32 * i for i in range(length)]
        def tree(lo, hi, parent):
            if lo >= hi: return head
            mid = lo if shape == 0 else hi - 1 if shape == 1 else (lo + hi) // 2
            node = nodes[mid]
            memory[node + 13 - 0x10000] = 0
            put(node + 4, parent)
            put(node, tree(lo, mid, node)); put(node + 8, tree(mid + 1, hi, node))
            return node
        put(head + 4, tree(0, length, head))
        put(head, nodes[0] if nodes else head); put(head + 8, nodes[-1] if nodes else head)
        for nil in (1, 0x7F, 0x80, 0xFF):
            memory[head + 13 - 0x10000] = nil
            for index, node in enumerate(nodes + [head]):
                # The real callers often keep the iterator on their guest stack.
                for on_stack in (False, True):
                    iterator = sp + 4 if on_stack else out
                    put(out, node)
                    stack = struct.pack('<2I', stop, node)
                    initial = [rng.getrandbits(32), iterator, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
                    compare(0xA015DC50, 'check_next', memory, stack, initial,
                            f'next shape={shape} n={length} index={index} nil={nil} stack={on_stack}', (iterator,))
                    # Restart each case from the same untouched host-side tree.
                    if index < length:
                        expected = nodes[index + 1] if index + 1 < length else head
                        assert struct.unpack('<I', arm.mem_read(guest_base + iterator, 4))[0] == expected
                    next_cases += 1
print(f'native integer lookups PASS: {2048 + next_cases} original-x86/actual-ARM64 cases, '
      'all GPRs, arithmetic flags, non-stack memory and stack iterator outputs')

# Persistence lookup: case folding and basename termination at either dot/NUL.
alphabet = b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-\x80\xFF'
for case in range(1536):
    memory = bytearray(rng.randbytes(0x10000))
    def put(at, value):
        struct.pack_into('<I', memory, at - 0x10000, value)
    def string(at, value, heap, force_heap=False):
        value = value + b'\0'
        if len(value) <= 16 and not force_heap:
            memory[at - 0x10000:at - 0x10000 + len(value)] = value
            capacity = rng.choice((0, 7, 15))
        else:
            put(at, heap)
            memory[heap - 0x10000:heap - 0x10000 + len(value)] = value
            capacity = rng.choice((32, 127, 0xFFFFFFFF))
        put(at + 16, rng.getrandbits(32))  # The original ignores declared length.
        put(at + 20, capacity)
    self, vector, head, key, out, sp = 0x11000, 0x12000, 0x13000, 0x16000, 0x17000, 0x80000
    mask, hashed, length = (1 << rng.randrange(5)) - 1, rng.getrandbits(32), rng.randrange(12)
    nodes = [0x14000 + 64 * i for i in range(length)]
    names = [bytes(rng.choice(alphabet) for _ in range(rng.randrange(45))) for _ in nodes]
    query = rng.choice(names) if names and case % 2 else bytes(rng.choice(alphabet) for _ in range(rng.randrange(45)))
    if case % 3 == 0: query = query.swapcase()
    if case % 5 == 0: query = query.split(b'.')[0] + b'.package'
    put(self + 4, head); put(self + 12, vector); put(self + 24, mask)
    bucket = vector + (mask & hashed) * 8
    put(bucket, nodes[0] if nodes else head); put(bucket + 4, nodes[-1] if nodes else head)
    for i, node in enumerate(nodes):
        put(node, nodes[i + 1] if i + 1 < length else head)
        put(node + 4, nodes[i - 1] if i else head)
        string(node + 12, names[i], 0x18000 + i * 128, case % 7 == 0)
    string(key + 4, query, 0x1E000, case % 11 == 0)
    if case % 13 == 0: out = key
    if case % 17 == 0 and nodes: out = rng.choice(nodes)
    if case % 19 == 0: out = bucket
    if case % 23 == 0: out = sp + rng.choice((-32, -24, -16, -12, -8, 4, 8))
    if not length and case % 2: key = 0
    stack = struct.pack('<4I', stop, out, key, hashed)
    initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    x86.mem_write(0xA10DFCAC, bytes(4))
    arm.mem_write(guest_base + 0xA10DFCAC, bytes(4))
    compare(0xA01D5B50, 'check_persistence', memory, stack, initial, f'persistence {case}', (out, out + 4))

arm.mem_write(guest_base + 0xA10DFCAC, struct.pack('<I', 1))
arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
arm.reg_write(A.UC_ARM64_REG_LR, stop)
arm.emu_start(base + symbols['check_persistence'], stop, count=1000)
assert struct.unpack('<I', arm.mem_read(base + symbols['fallback_calls'], 4))[0] == 1
assert struct.unpack('<I', arm.mem_read(base + symbols['g_m'], 4))[0] == 0xFACADE
print('native persistence lookup PASS: 1536 original-x86/actual-ARM64 cases, locale fallback, '
      'collisions, case/dot/NUL, non-ASCII, SSO/heap, stale length/capacity, alias outputs, all registers/flags/stack scratch')
for case in range(1024):
    memory=bytearray(rng.randbytes(0x10000))
    def put(at,value):struct.pack_into('<I',memory,at-0x10000,value)
    self,nodes,rows,key,ext,sp=0x11000,0x12000,0x14000,0x16000,0x18000,0x80000
    kind,group=rng.getrandbits(32),rng.getrandbits(32)
    count=1+rng.randrange(16);slot=((kind+group)&0xFFFFFFFF)%count
    for n in range(count+3):
        put(nodes+n*16,kind^0xFFFFFFFF);put(nodes+n*16+4,group);put(nodes+n*16+8,0);put(nodes+n*16+12,0xFFFFFFFF)
    if case%3==0:
        put(nodes+slot*16+12,count+1);slot=count+1
    put(nodes+slot*16,kind);put(nodes+slot*16+4,group)
    pairs=sorted(set((rng.randrange(4)<<32|rng.randrange(10)) if case%2 else rng.getrandbits(64) for _ in range(1+rng.randrange(50))))
    put(nodes+slot*16+8,0);put(nodes+slot*16+0x18,len(pairs))
    put(self+4,rows);put(self+0x10,ext);put(self+0x1C,nodes);put(self+0x28,count if case%29 else 0)
    for i,pair in enumerate(pairs):
        at=rows+i*16;put(at,pair&0xFFFFFFFF);put(at+4,pair>>32);put(at+8,rng.getrandbits(32))
        extended=(i+case)%4==0
        struct.pack_into('<HH',memory,at+12-0x10000,0xFFFF if extended else rng.randrange(0xFFFF),i if extended else rng.randrange(0x10000))
        put(ext+i*8,rng.getrandbits(32));put(ext+i*8+4,rng.getrandbits(32))
    query=rng.choice(pairs) if case%2 else rng.getrandbits(64)
    if case%7==0:query&=0xFFFFFFFF
    if case%11==0:query|=1<<32
    for i,v in enumerate((kind,group,query&0xFFFFFFFF,query>>32)):put(key+i*4,v)
    if case%17==0:put(key,kind^0x55555555)
    initial=[rng.getrandbits(32),self,rng.getrandbits(32),sp]+[rng.getrandbits(32) for _ in range(4)]
    for flags in range(4):
        compare(0xA09AB1A0,'check_resource',memory,struct.pack('<4I',stop,key,0x100 if not flags&1 else 0xFF,0x100 if not flags&2 else 1),initial,f'DBPF {case} flags{flags}')

before=struct.unpack('<I',arm.mem_read(base+symbols['fallback_calls'],4))[0]
for bad_key in (0,0xFFFFFFF8):
    arm.mem_write(guest_base+sp,struct.pack('<4I',stop,bad_key,0,0))
    arm.mem_write(base+symbols['g_m'],struct.pack('<8I',*initial)+bytes(32))
    arm.reg_write(A.UC_ARM64_REG_SP,native_stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
    arm.emu_start(base+symbols['check_resource'],stop,count=10000)
    assert struct.unpack('<I',arm.mem_read(base+symbols['g_m'],4))[0]==0xFACADE
assert struct.unpack('<I',arm.mem_read(base+symbols['fallback_calls'],4))[0]==before+2
print('DBPF index PASS:4096 original-x86/actual-ARM64 exact/wildcard queries, exact-first ordering, collision chains, misses, unsigned64 binary search, small/extended lengths, output canaries, return/callee-saved registers and flags; invalid-key fallback2')

leaf_cases = 0
for case in range(2048):
    memory = bytearray(rng.randbytes(0x10000))
    def put(at, value): struct.pack_into('<I', memory, at - 0x10000, value)
    self, vector, entry, sp = 0x11000, 0x12000, 0x14000, 0x80000
    enabled = rng.choice((0, 1, 0x7F, 0x80, 0xFF))
    memory[self + 4 - 0x10000] = enabled
    count = rng.randrange(32)
    put(self + 0x28, vector); put(self + 0x2C, vector + count * 4)
    for i in range(count):
        node = entry + 64 * i
        put(vector + i * 4, node if i % 3 else 0)
        put(node + 0x14, rng.getrandbits(32))
        put(node + 0x28, rng.choice((0, 14, 15, 16, 0xFFFFFFFF)))
    index = rng.choice((0, count, max(0, count - 1), 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF))
    if case % 7 == 0:
        # A malformed negative vector span still has the original SAR/unsigned bound.
        put(self + 0x2C, vector - 4)
        put(vector, 0)
        index = 0
    if not enabled:
        put(self + 0x28, 0); put(self + 0x2C, 0xFFFFFFFF)
    initial = [rng.getrandbits(32), self, rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    compare(0xA0ADFAE0, 'check_material', memory, struct.pack('<2I', stop, index), initial, f'material {case}')
    index = rng.choice((0, 1, 0x70, 0x71, 0x72, 0x7FFF, 0x8000, 0xFFFF, rng.getrandbits(32)))
    compare(0xA052F900, 'check_attribute', memory, struct.pack('<2I', stop, index), initial, f'attribute {case}')
    leaf_cases += 2
print(f'native render/simulation leaves PASS: {leaf_cases} original-x86/actual-ARM64 cases, '
      'all GPRs, arithmetic flags, exact read bounds, disabled/null entries, SSO/heap and unsigned/SAR boundaries')

(OUT / 'result.json').write_text(json.dumps({
    'result': 'PASS', 'existing_cases': 2048 + next_cases, 'persistence_cases': 1536,
    'locale_fallback_cases': 1, 'dbpf_cases':4096,'invalid_key_fallback_cases':2,'render_simulation_leaf_cases':leaf_cases,'native_source_sha256': hashlib.sha256(
        (ROOT / 'android/host/sims2_fast.c').read_bytes()).hexdigest(),
    'limits': 'Original-x86 versus actual-AArch64 fixtures, not device performance; nondefault locale delegates to lifted original'
}, indent=2))
