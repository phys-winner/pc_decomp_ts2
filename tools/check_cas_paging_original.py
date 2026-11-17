"""Run original CAS callback dispatch, Clothing capacity selection and page clamps."""
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

from build_android import ROOT


def main():
    pe = pefile.PE(str(ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(image) + 65535) & ~65535); uc.mem_write(base, image)
    uc.mem_map(0x10000, 0x30000); uc.mem_map(0x60000000, 0x1000)
    obj, node, sentinel, manager, event, state, table, stack, stop = (
        0x10000, 0x11000, 0x11100, 0x12000, 0x12100, 0x12200, 0x13000, 0x2F000, 0x60000000)
    rd = lambda a: int.from_bytes(uc.mem_read(a, 4), 'little')
    wr = lambda a, v: uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))
    wr(obj, 0xA0DDC264); wr(obj + 0x100, sentinel); wr(node + 0xC, 0xA05ECA86)
    wr(manager, table); wr(table + 0x84, stop + 0x100)
    wr(event, table + 0x100); wr(table + 0x100 + 0x24, stop + 0x110)
    wr(state, table + 0x200); wr(table + 0x200 + 0x14, stop + 0x120)
    assert rd(0xA0DDC264 + 0x2BC) == 0xA05EC867
    keys = {4: 0xA0DDC5FC, 8: 0xA0DDC6B0, 16: 0xA0DDC650}
    expected = {}
    observed = {}

    def callback(c, address, size, user):
        if address not in (0xA0141C15, 0xA0755C4D, 0xA0758E51, 0xA020E087,
                           0xA05EC867, stop + 0x100, stop + 0x110, stop + 0x120):
            return
        sp = c.reg_read(X.UC_X86_REG_ESP)
        this = c.reg_read(X.UC_X86_REG_ECX)
        result, pop = 0, 4
        if address == 0xA0141C15:  # Only the external callback-map lookup is substituted.
            assert this == obj + 0xFC
            key = rd(rd(sp + 8)); assert key == expected['button']
            wr(rd(sp + 4), node if key in (0x120, 0x121) else sentinel)
            observed['lookups'] += 1; pop = 12
        elif address == 0xA0755C4D:
            assert this == obj and rd(sp + 4) == keys[expected['mode']] and rd(sp + 8) == 0
            observed['capacity'] += 1; result, pop = expected['capacity'], 12
        elif address == 0xA0758E51:  # Rebinding/rendering is outside the page-math test.
            assert this == obj
            args = [rd(sp + i * 4) for i in range(1, 6)]
            assert args == [expected['total'], expected['capacity'], 0x120, 0x121, 0xEC8D06FF], args
            assert rd(obj + 0x1B8) == expected['next'], (expected, rd(obj + 0x1B8))
            observed['binds'] += 1
            for offset, value in ((0x1C0,args[0]), (0x1BC,args[1]), (0x1C4,args[2]), (0x1C8,args[3])):
                wr(obj + offset, value)
            pop = 24
        elif address == 0xA020E087:
            observed['services'] += 1; result = manager
        elif address == stop + 0x100:
            assert this == manager; result = event
        elif address == stop + 0x110:
            assert this == event
            observed['event_indices'].append(rd(sp + 4)); result, pop = state, 8
        elif address == stop + 0x120:
            assert this == state and rd(sp + 4) == 1
            observed['state_sets'] += 1; pop = 8
        else:
            assert this == obj
            observed['refresh'] += 1
        c.reg_write(X.UC_X86_REG_EAX, result)
        c.reg_write(X.UC_X86_REG_ESP, sp + pop); c.reg_write(X.UC_X86_REG_EIP, rd(sp))

    uc.hook_add(UC_HOOK_CODE, callback)
    nonvolatile = ((X.UC_X86_REG_EBX,0xBB), (X.UC_X86_REG_ESI,0x55),
                   (X.UC_X86_REG_EDI,0xDD), (X.UC_X86_REG_EBP,0xEE))
    count = 0
    for mode in (4, 8, 16):
        for capacity in (1, 3, 10, 24):
            for total in (0, 1, capacity, capacity + 1, capacity * 3, capacity * 3 + 1):
                for current in sorted({0, min(total,capacity), max(0,total-capacity), total}):
                    for button in (0x120, 0x121, 0xDEAD):
                        next_item = current
                        if button == 0x120 and total-current > capacity:
                            next_item += capacity
                        elif button == 0x121:
                            next_item = max(0,current-capacity)
                        expected = dict(mode=mode,capacity=capacity,total=total,current=current,next=next_item,button=button)
                        observed = dict(lookups=0,capacity=0,binds=0,refresh=0,services=0,event_indices=[],state_sets=0)
                        wr(obj + 0x174,total); wr(obj + 0x1B8,current)
                        wr(obj + 0x1BC,0xBAD); wr(obj + 0x1C4,0xBAD); wr(obj + 0x1C8,0xBAD)
                        uc.mem_write(obj + 0x290,bytes([mode])); wr(stack,stop); wr(stack+4,button)
                        for register, value in nonvolatile: uc.reg_write(register,value)
                        uc.reg_write(X.UC_X86_REG_ESP,stack); uc.reg_write(X.UC_X86_REG_ECX,obj)
                        uc.emu_start(0xA0755C58,stop,count=10000)
                        accepted = button != 0xDEAD
                        assert uc.reg_read(X.UC_X86_REG_ESP) == stack+8
                        assert uc.reg_read(X.UC_X86_REG_EAX)&255 == int(accepted)
                        assert all(uc.reg_read(register)==value for register,value in nonvolatile)
                        assert rd(obj + 0x1B8) == next_item
                        assert observed == dict(lookups=1,capacity=int(accepted),binds=int(accepted),refresh=int(accepted),
                            services=4 if accepted else 0,event_indices=[15,15,1,1] if accepted else [],state_sets=4 if accepted else 0), observed
                        count += 1
    print(f'PASS {count} original x86 CAS dispatcher/Clothing callback/page math cases: '
          'three thumbnail modes, exact five rebind arguments, next/previous/limits/missing binding, refresh and stack/nonvolatile ABI')


if __name__ == '__main__':
    main()
