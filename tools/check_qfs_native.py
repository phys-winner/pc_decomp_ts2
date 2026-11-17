"""Compare actual native QFS decoder with original x86, without adb.

Checks every GPR, arithmetic flags, caller arguments and decoder scratch,
consumed length, complete output and canaries. Real streams are read from the
unchanged original Lot36 package. Instruction counts are operation evidence,
not measured Android load time.
"""
import io
from pathlib import Path
import random
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A, x86_const as X

import build_android as B
from dbpf import Package

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/qfs-native'
OUT.mkdir(parents=True, exist_ok=True)
source = OUT / 'check.c'
source.write_text('#include "host.h"\nrecomp_machine_t g_m;\n'
                  f'#include "{ROOT.as_posix()}/android/host/sims2_fast.c"\n'
                  'uint32_t check(void) { sub_A09CC369(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n')
generated = (ROOT / 'generated/sims2-x86/sims2_A09C8000.c').read_text()
begin = generated.index('void sub_A09CC369(void) {')
end = generated.index('\n}\n', begin) + 3
lifted = OUT / 'lifted.c'
lifted.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                  'void (*recomp_yield_hook)(void);\n' + generated[begin:end].replace('sub_A09CC369', 'check_lifted_body') +
                  '\nuint32_t check_lifted(void) { check_lifted_body(); '
                  'return recomp_eflags(g_flag_k, g_flag_a, g_flag_b, g_flag_cf, 1); }\n')
binary = OUT / 'check.elf'
subprocess.run([str(B.CLANG), '--target=aarch64-linux-android30', '-O2', '-ffunction-sections',
                '-fdata-sections', '-fno-stack-protector', '-nostdlib', '-Iandroid/runtime', '-Iandroid/host',
                '-Wl,--gc-sections,-e,check,-u,check_lifted', str(source), str(lifted), '-o', str(binary)], cwd=ROOT, check=True)
elf = ELFFile(io.BytesIO(binary.read_bytes()))
symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
base, guest_base, native_stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
arm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
arm.mem_map(base, 0x200000)
arm.mem_map(guest_base + 0x10000, 0x2F0000)
arm.mem_map(native_stack, 0x10000)
arm.mem_map(stop, 0x1000)
for segment in elf.iter_segments():
    if segment['p_type'] == 'PT_LOAD': arm.mem_write(base + segment['p_vaddr'], segment.data())
relocations = elf.get_section_by_name('.rela.dyn')
if relocations:
    for r in relocations.iter_relocations():
        assert r['r_info_type'] == 1027
        arm.mem_write(base + r['r_offset'], struct.pack('<Q', base + r['r_addend']))
pe = pefile.PE(str(ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
image = pe.get_memory_mapped_image()
x86 = Uc(UC_ARCH_X86, UC_MODE_32)
x86.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image) + 0xFFFF) & ~0xFFFF)
x86.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
x86.mem_map(0x10000, 0x2F0000)
x86.mem_map(stop, 0x1000)
regs = [X.UC_X86_REG_EAX, X.UC_X86_REG_ECX, X.UC_X86_REG_EDX, X.UC_X86_REG_ESP,
        X.UC_X86_REG_EBX, X.UC_X86_REG_ESI, X.UC_X86_REG_EDI, X.UC_X86_REG_EBP]
rng = random.Random(0xA09CC369)
counts = {'x86': 0, 'arm64': 0, 'lifted_arm64': 0, 'cases': 0}
cases = 0


def compare(stream, expanded, capacity, consumed, label, measure=False, declared=None,
            output_address=None, verify_payload=True):
    global cases
    inp, out, sp, result = 0x20000, 0x100000, 0x2C0000, 0x2A0000
    if output_address is not None: out = output_address
    if consumed == 'stack': consumed = sp + 16
    elif consumed == 'result': consumed = result
    elif consumed == 'scratch': consumed = sp - 4
    elif consumed in ['saved_ebx', 'saved_ebp', 'saved_esi', 'saved_edi']:
        consumed = sp - {'saved_ebx': 16, 'saved_ebp': 20, 'saved_esi': 24, 'saved_edi': 28}[consumed]
    consumed = int(consumed)
    data = bytearray(rng.randbytes(0x80000))
    data[0x10000:0x10000 + len(stream)] = stream
    output = bytes([0xCD]) * (len(expanded) + 64)
    stack = rng.randbytes(32) + struct.pack('<5I', stop, inp, consumed, out, capacity)
    initial = [rng.getrandbits(32), rng.getrandbits(32), rng.getrandbits(32), sp] + [rng.getrandbits(32) for _ in range(4)]
    for uc, offset in [(x86, 0), (arm, guest_base)]:
        uc.mem_write(offset + 0x10000, bytes(data))
        uc.mem_write(offset + out - 32, output)
        uc.mem_write(offset + result, b'\xEF\xBE\xAD\xDE')
        uc.mem_write(offset + sp - 32, stack)
    for r, v in zip(regs, initial): x86.reg_write(r, v)
    x86.reg_write(X.UC_X86_REG_EFLAGS, 0x202)
    arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(32))
    arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
    arm.reg_write(A.UC_ARM64_REG_LR, stop)
    local = {'x86': 0, 'arm64': 0}
    hooks = []
    if measure:
        for uc, name in [(x86, 'x86'), (arm, 'arm64')]:
            # Re-instrument translated blocks already used by earlier cases.
            uc.ctl_remove_cache(0xA09CC369, 0xA09CC54D) if name == 'x86' else uc.ctl_remove_cache(base, base + 0x200000)
            def tick(uc, address, size, user, name=name): local[name] += 1
            hooks.append((uc, uc.hook_add(UC_HOOK_CODE, tick)))
    x86.emu_start(0xA09CC369, stop, count=30_000_000)
    arm.emu_start(base + symbols['check'], stop, count=30_000_000)
    for uc, h in hooks: uc.hook_del(h)
    assert x86.reg_read(X.UC_X86_REG_EIP) == stop, f'{label}: x86 runaway'
    assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, f'{label}: ARM runaway'
    expected = tuple(x86.reg_read(r) for r in regs)
    actual = struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32))
    assert actual == expected, f'{label}: GPRs {actual} != {expected}'
    assert not (arm.reg_read(A.UC_ARM64_REG_X0) ^ x86.reg_read(X.UC_X86_REG_EFLAGS)) & 0x8D5, f'{label}: flags'
    for at, size in [(inp, len(stream)), (out - 32, len(output)), (sp - 32, len(stack)), (result, 4)]:
        assert arm.mem_read(guest_base + at, size) == x86.mem_read(at, size), f'{label}: bytes at {at:#x}'
    if measure:
        # Same ARM64 ISA and -O2 for a useful operation-count comparison;
        # production PGO and scheduling still need actual device timing.
        arm.mem_write(guest_base + 0x10000, bytes(data))
        arm.mem_write(guest_base + out - 32, output)
        arm.mem_write(guest_base + result, b'\xEF\xBE\xAD\xDE')
        arm.mem_write(guest_base + sp - 32, stack)
        arm.mem_write(base + symbols['g_m'], struct.pack('<8I', *initial) + bytes(32))
        arm.reg_write(A.UC_ARM64_REG_SP, native_stack + 0xF000)
        arm.reg_write(A.UC_ARM64_REG_LR, stop)
        arm.ctl_remove_cache(base, base + 0x200000)
        def tick_lifted(uc, address, size, user): counts['lifted_arm64'] += 1
        hook = arm.hook_add(UC_HOOK_CODE, tick_lifted)
        arm.emu_start(base + symbols['check_lifted'], stop, count=30_000_000)
        arm.hook_del(hook)
        assert arm.reg_read(A.UC_ARM64_REG_PC) == stop, f'{label}: lifted ARM runaway'
        assert struct.unpack('<8I', arm.mem_read(base + symbols['g_m'], 32)) == expected, f'{label}: lifted GPRs'
    if verify_payload and capacity >= len(expanded):
        assert bytes(arm.mem_read(guest_base + out, len(expanded))) == expanded, f'{label}: independently decoded output'
        expected_return = len(expanded) if declared is None else declared
        if consumed == sp - 4: expected_return = len(stream)
        assert actual[0] == expected_return, f'{label}: return size'
    elif verify_payload:
        assert actual[0] == 0xFFFFFFFF, f'{label}: missing capacity failure'
    if measure:
        counts['cases'] += 1
        for name in local: counts[name] += local[name]
    cases += 1


def header(body, expanded, extended=False, declared=None):
    size = len(expanded) if declared is None else declared
    return (b'\x11\xFB\x12\x34\x56' if extended else b'\x10\xFB') + size.to_bytes(3, 'big') + body


def literal(body, expanded, data):
    while len(data) >= 4:
        n = min(112, len(data) & ~3)
        body.append(0xE0 + (n - 4) // 4); body.extend(data[:n]); expanded.extend(data[:n]); data = data[n:]
    assert not data


def backref(body, expanded, kind, count, distance, plain):
    n = len(plain); d = distance - 1
    if kind == 0: body.extend([(d >> 8) << 5 | (count - 3) << 2 | n, d & 255])
    elif kind == 1: body.extend([0x80 | (count - 4), n << 6 | d >> 8, d & 255])
    else: body.extend([0xC0 | (d >> 16) << 4 | ((count - 5) >> 8) << 2 | n, d >> 8 & 255, d & 255, (count - 5) & 255])
    body.extend(plain); expanded.extend(plain)
    for _ in range(count): expanded.append(expanded[-distance])


for case in range(256):
    body, expanded = bytearray(), bytearray()
    literal(body, expanded, rng.randbytes(112))
    for _ in range(rng.randrange(1, 18)):
        kind = rng.randrange(3); low, high, limit = [(3, 10, 1024), (4, 67, 16384), (5, 1028, 131072)][kind]
        count = rng.choice([low, high, rng.randrange(low, high + 1)])
        plain = rng.randbytes(rng.randrange(4))
        distance = rng.choice([1, min(len(expanded) + len(plain), limit), rng.randrange(1, min(len(expanded) + len(plain), limit) + 1)])
        backref(body, expanded, kind, count, distance, plain)
    final = rng.randbytes(case % 4); body.append(0xFC + len(final)); body.extend(final); expanded.extend(final)
    declared = len(expanded) + (case % 7 == 0)
    stream = header(body, expanded, bool(case % 2), declared)
    for capacity in sorted(set([0, 111, len(expanded) - 1, len(expanded), len(expanded) + 32])):
        compare(stream, bytes(expanded), capacity, ['result', 0, 'stack', 'scratch'][case % 4],
                f'random {case} cap{capacity}', measure=case < 8 and capacity == len(expanded), declared=declared)

for kind, limits in enumerate([(3, 10, 1024), (4, 67, 16384), (5, 1028, 131072)]):
    low, high, limit = limits
    for distance in [1, 2, 15, 16, 17, limit]:
        for count in [low, high]:
            for n in range(4):
                body, expanded = bytearray(), bytearray()
                literal(body, expanded, rng.randbytes((distance + 3) & ~3))
                backref(body, expanded, kind, count, distance, rng.randbytes(n))
                body.append(0xFC)
                compare(header(body, expanded), bytes(expanded), len(expanded), 'result',
                        f'edge kind{kind} d{distance} n{count} plain{n}', measure=distance <= 17 and count == high and n == 0)
                compare(header(body, expanded), bytes(expanded), len(expanded) - 1, 'result',
                        f'edge capacity kind{kind} d{distance} n{count} plain{n}')

for n in range(4):
    data = rng.randbytes(n)
    for extended in [False, True]:
        stream = header(bytes([0xFC + n]) + data, data, extended)
        compare(stream, data, n, 'result', f'terminal-only n{n} extended{extended}', measure=True)
        if n: compare(stream, data, n - 1, 'result', f'terminal capacity n{n} extended{extended}')

for consumed in ['saved_ebx', 'saved_ebp', 'saved_esi', 'saved_edi']:
    for n in range(4):
        data = rng.randbytes(n)
        compare(header(bytes([0xFC + n]) + data, data), data, n, consumed,
                f'consumed aliases {consumed} terminal{n}')

# These aliasing cases deliberately change the original decoder's scratch
# after its capacity check. Original x86 is the oracle for their changed output.
for body, size, output in [
    (b'\x01\x05\x02\xFC', 4, 0x2C0000 + 12),
    (b'\x80\x40\x05\x02\xFC', 5, 0x2C0000 + 12),
    (b'\xC1\x00\x05\x00\x02\xFC', 6, 0x2C0000 - 9),
    (b'\xC1\x00\x00\x00\x02\xFC', 6, 0x2C0000 - 8),
    (b'\xC1\x01\x00\x00\x00\xFC', 6, 0x2C0000 + 12),
]:
    compare(header(body, bytes(size)), bytes(size), size, 'result',
            f'literals alias stack scratch out{output:#x} body{body.hex()}',
            output_address=output, verify_payload=False)

package = Package(Path(r'C:\Games\The Sims 2 Legacy\Base\TSData\Res\UserData\Neighborhoods\N001\Lots\N001_Lot36.package'))
real = 0
for entry in package.entries:
    raw = package.raw(entry)
    if package.key(entry) not in package.compressed or raw[4:6] != b'\x10\xFB': continue
    expanded = package.read(entry)
    for capacity in [len(expanded), max(0, len(expanded) - 1)]:
        compare(raw[4:], expanded, capacity, 'result' if real % 2 else 0,
                f'Lot36 resource{real} cap{capacity}', measure=real < 8 and capacity == len(expanded))
    real += 1

print(f'QFS native PASS: {cases} original-x86/actual-ARM64 cases, {real} real Lot36 streams; '
      'all GPRs, arithmetic flags, scratch/arguments, capacity errors, output and canaries')
print(f'Instruction sample ({counts["cases"]} cases): x86 {counts["x86"]}, ARM64 {counts["arm64"]}; '
      f'lifted ARM64 {counts["lifted_arm64"]}; '
      f'{100 * (1 - counts["arm64"] / counts["lifted_arm64"]):.2f}% fewer ARM64 instructions (not load-time evidence)')
