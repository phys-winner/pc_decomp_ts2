"""Execute the unmodified x86 neighborhood clamp and translation setter."""
import json
import math
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

import build_android as B


def main():
    pe = pefile.PE(str(B.ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(image) + 65535) & ~65535); uc.mem_write(base, image)
    uc.mem_map(0x10000, 0x20000); uc.mem_map(0x60000000, 0x1000)
    camera, vector, world, table, constraints, stack, stop = 0x10000, 0x12000, 0x18000, 0x18100, 0x19000, 0x2F000, 0x60000000
    rd = lambda a: int.from_bytes(uc.mem_read(a, 4), 'little')
    wr = lambda a, v: uc.mem_write(a, struct.pack('<I', v))
    put = lambda a, *v: uc.mem_write(a, struct.pack('<' + 'f' * len(v), *v))
    unconstrained = 0

    def world_predicate(c, address, size, user):
        if address != stop + 0x100: return
        assert c.reg_read(X.UC_X86_REG_ECX) == world
        sp = c.reg_read(X.UC_X86_REG_ESP)
        c.reg_write(X.UC_X86_REG_EAX, unconstrained)
        c.reg_write(X.UC_X86_REG_ESP, sp + 4); c.reg_write(X.UC_X86_REG_EIP, rd(sp))

    uc.hook_add(UC_HOOK_CODE, world_predicate)
    wr(camera + 40, world); wr(world, table); wr(table + 156, stop + 0x100)
    wr(camera + 92, constraints); wr(camera + 96, constraints + 16)
    put(constraints, .35, .35, .35, .35)
    put(camera + 1096, 1000, 500, 7)
    count = 0
    for unconstrained in (0, 1):
        for initialized in (0, 1):
            for x in (-1000., 0., 349., 350., 500., 650., 651., 2000.):
                for y in (-1000., 0., 174., 175., 250., 325., 326., 2000.):
                    put(vector, x, y, 99)
                    put(camera + 364, -1, -2, -3); uc.mem_write(camera + 388, bytes([initialized]))
                    uc.mem_write(stack, struct.pack('<2I', stop, vector))
                    uc.reg_write(X.UC_X86_REG_ESP, stack); uc.reg_write(X.UC_X86_REG_ECX, camera)
                    uc.emu_start(0xA035B8C0, stop, count=10000)
                    assert uc.reg_read(X.UC_X86_REG_EIP) == stop and uc.reg_read(X.UC_X86_REG_ESP) == stack + 8
                    expected = (x, y, 99) if unconstrained else (max(350., min(650., x)), max(175., min(325., y)), 7.)
                    target = struct.unpack('<3f', uc.mem_read(camera + 376, 12))
                    assert all(math.isclose(a, b, abs_tol=1e-4) for a, b in zip(target, expected)), (target, expected)
                    current = struct.unpack('<3f', uc.mem_read(camera + 364, 12))
                    assert current == ((-1., -2., -3.) if initialized else target)
                    count += 1
    out = B.ROOT / 'build/20261028-ui/camera-clamp-x86.json'
    out.write_text(json.dumps(dict(pass_=True, cases=count, original='A035B8C0 + A0B4AEB0 unmodified x86',
        boundary='Only world constraint predicate is replaced; original clamp, projection constraints and current/target setter execute.'), indent=2) + '\n')
    print('PASS original x86 camera clamp/current-target:', count)


if __name__ == '__main__': main()
