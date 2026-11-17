"""Check real SPI wheel settings and signed WM_MOUSEWHEEL transport."""
import ctypes
from pathlib import Path
import subprocess
import tempfile
import struct

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn import x86_const as X

from build_android import CLANG, ROOT
from check_virtual_query import function


def original_text_wheel():
    pe = pefile.PE(str(ROOT / 'original/Sims2EP9.exe'), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(base, (len(image) + 65535) & ~65535); uc.mem_write(base, image)
    uc.mem_map(0x10000, 0x20000); uc.mem_map(0x60000000, 0x1000)
    window, manager, table, event, stack, stop = 0x10000, 0x11000, 0x12000, 0x13000, 0x2F000, 0x60000000
    rd = lambda a: int.from_bytes(uc.mem_read(a, 4), 'little')
    wr = lambda a, v: uc.mem_write(a, struct.pack('<I', v))
    captured = False
    invalidations = 0

    def callback(c, address, size, user):
        nonlocal invalidations
        if address not in (stop + 0x100, 0xA093186A):
            return
        sp = c.reg_read(X.UC_X86_REG_ESP)
        if address == stop + 0x100:
            assert c.reg_read(X.UC_X86_REG_ECX) == manager
            c.reg_write(X.UC_X86_REG_EAX, window if captured else 0)
        else:
            assert c.reg_read(X.UC_X86_REG_ECX) == window
            invalidations += 1
        c.reg_write(X.UC_X86_REG_ESP, sp + 4); c.reg_write(X.UC_X86_REG_EIP, rd(sp))

    uc.hook_add(UC_HOOK_CODE, callback)
    wr(window, 0xA0E56138); wr(window + 0xF8, 0xA0E56404)
    wr(window + 4, manager); wr(manager, table); wr(table + 0x74, stop + 0x100)
    wr(window + 0x140, 0x16000); wr(window + 0x144, 0x16000 + 100 * 4)
    wr(window + 0x1D8, 5); wr(window + 0x160, 0x99CB)
    cases = 0
    scenarios = [([40], 0), ([40,40,40], 0), ([120], -1), ([120]*5, -5), ([600], -6),
                 ([-40]*3, 0), ([-120], 1), ([-120]*5, 5), ([-600], 6), ([120,-120], 0)]
    for captured in (False, True):
        for deltas, change in scenarios:
            wr(window + 0x1DC, 50); invalidations = 0
            for delta in deltas:
                # Original base-window dispatch routes event14 to TextEdit's +0x280 wheel method.
                uc.mem_write(event, struct.pack('<Iii hH', 14, 600, 500, delta, 0))
                wr(stack, stop); wr(stack + 4, event)
                uc.reg_write(X.UC_X86_REG_ESP, stack); uc.reg_write(X.UC_X86_REG_ECX, window)
                uc.emu_start(0xA0930A99, stop, count=10000)
                assert uc.reg_read(X.UC_X86_REG_ESP) == stack + 8
                assert uc.reg_read(X.UC_X86_REG_EAX) & 255 == 1
            assert rd(window + 0x1DC) == 50 + (0 if captured else change), (captured,deltas,rd(window+0x1DC))
            assert invalidations == (0 if captured else sum(abs(d)//100 for d in deltas))
            cases += 1
    print(f'Original x86 wheel dispatch/TextEdit PASS: {cases} cases; 40-unit fragments lost, complete120 detents scroll, '
          'per-event integer rounding and capture guard preserved')


def main():
    user = (ROOT / 'android/host/user32.c').read_text()
    source = (ROOT / 'android/host/input.c').read_text()
    code = r'''
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned char uint8_t;
static uint32_t args[4],memory[32],input_lock,locks;
static int32_t mouse_dz;
static int cursor_x=321,cursor_y=654;
static uint32_t posted[3],ring[2];
#define MEM32(a) memory[(uint32_t)(a)/4]
#define ARG(n) args[n]
#define WINAPI_EXPORT(d,n,c) static uint32_t spi(void)
static void screen_size(int *w,int *h) { *w=2400;*h=1080; }
static void pthread_mutex_lock(uint32_t *p) { (void)p;++locks; }
static void pthread_mutex_unlock(uint32_t *p) { (void)p;--locks; }
static uint32_t mouse_key_state(void) { return 0xD; }
static uint32_t mouse_ring;
static void ring_push(uint32_t *r,uint32_t ofs,uint32_t value) { (void)r;ring[0]=ofs;ring[1]=value; }
static void pend(uint32_t msg,uint32_t wp,uint32_t lp) { posted[0]=msg;posted[1]=wp;posted[2]=lp; }
'''
    code += function(user, 'WINAPI_EXPORT(U32, SystemParametersInfoA, 4)')
    code += function(source, 'void void_input_mouse_wheel(')
    code += r'''
__declspec(dllexport) uint32_t check(int delta) {
    args[0]=0x68;args[2]=16;memory[3]=0xAABBCCDD;memory[4]=0;memory[5]=0x11223344;
    if(!spi() || memory[4]!=3 || memory[3]!=0xAABBCCDD || memory[5]!=0x11223344)return 1;
    int lines=(int)memory[4];mouse_dz=0;void_input_mouse_wheel(delta);
    if(locks || posted[0]!=0x20A || posted[2]!=((654u<<16)|321u) || (posted[1]&65535u)!=0xD)return 2;
    if((short)(posted[1]>>16)!=delta || mouse_dz!=delta || ring[0]!=8 || (int)ring[1]!=delta)return 3;
    /* WndProc A0158748/A015874C multiplies by SPI lines and divides by120. */
    if((short)(posted[1]>>16)*lines/120!=delta*3/120)return 4;
    args[0]=0x30;args[2]=16;if(!spi() || memory[4] || memory[5] || memory[6]!=2400 || memory[7]!=1080)return 5;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-wheel-') as directory:
        base = Path(directory)
        cfile, obj, library = base / 'check.c', base / 'check.obj', base / 'check.dll'
        cfile.write_text(code)
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library))
        dll.check.argtypes = [ctypes.c_int]
        try:
            for delta in (-4800, -1200, -120, -40, 0, 40, 120, 1200, 4800):
                assert dll.check(delta) == 0, delta
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('Mouse wheel actual C PASS: SPI three lines, signed WM_MOUSEWHEEL delta/position/modifiers, '
          'DirectInput delta and normalized game wheel amount')
    original_text_wheel()


if __name__ == '__main__':
    main()
