"""Run the actual ARM64 shadow override and lifted controller with UI stubs.

Checks the old disabled-scene path, every missing-control combination, reference
releases, and x86 stack/nonvolatile registers. No adb, APK build or GPU needed.
"""
import io
from pathlib import Path
import subprocess
import tempfile

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

from build_android import CLANG, ROOT, NATIVE_OVERRIDES
from check_virtual_query import function
from probe_function import extract


def main():
    assert 'sub_A064B9C7' in NATIVE_OVERRIDES
    lifted, _ = extract('sub_A064B9C7')
    prologue, _ = extract('sub_A08248B0')
    lifted = lifted.replace('void sub_A064B9C7(void)', 'void sub_A064B9C7_lifted(void)', 1)
    native = function((ROOT / 'android/host/sims2_patches.c').read_text(), 'void sub_A064B9C7(void)')
    code = r'''
static unsigned char memory[65536];
#define VOID_GUEST_BASE ((uintptr_t)memory)
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
recomp_machine_t g_m;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;
static uint32_t missing, found, changed, released, bad;
static uint32_t index_of(uint32_t id) { return id == 0x94 ? 0 : id - 0x90; }
static void find_child(void) {
    uint32_t i=index_of(MEM32(g_esp+4)), out=MEM32(g_esp+12);
    if(i>3 || MEM32(g_esp+8)!=0x8810 || (found&(1u<<i))) { bad=1; i=0; }
    found|=1u<<i; MEM32(out)=(missing&(1u<<i)) ? 0 : 0x4000+0x100*i;
    g_esp+=16;
}
static void get_window(void) { g_eax=g_ecx; g_esp+=4; }
static void set_enabled(void) {
    uint32_t i=(g_ecx-0x4000)/0x100;
    if(i>3 || MEM32(g_esp+4)!=2 || (changed&(1u<<i))) { bad=2; i=0; }
    changed|=1u<<i; MEM32(g_ecx+8)=MEM32(g_esp+8); g_esp+=12;
}
static void release(void) {
    uint32_t i=(g_ecx-0x4000)/0x100;
    if(i>3 || (released&(1u<<i))) { bad=3; i=0; }
    released|=1u<<i; g_esp+=4;
}
recomp_func_t void_resolve(uint32_t va) {
    if(va==1)return find_child;
    if(va==2)return get_window;
    if(va==3)return set_enabled;
    if(va==4)return release;
    bad=4; return release;
}
void sub_A08248B0(void);
''' + prologue + lifted + native + r'''
uint32_t check(uint32_t patched, uint32_t mask, uint32_t has_panel) {
    for(uint32_t i=0;i<sizeof memory;++i)memory[i]=0;
    found=changed=released=bad=0; missing=mask;
    g_m=(recomp_machine_t){0}; g_fs_base=0x100; g_dispatch_gen=1;
    g_ecx=0x1000; g_esp=0xF000;
    g_ebx=0xBB; g_esi=0x55; g_edi=0xDD; g_ebp=0xEE;
    MEM32(g_esp)=0xDEAD0000; MEM32(g_esp+4)=0;
    MEM32(g_fs_base)=0x1234;
    MEM32(0x10D4)=has_panel ? 0x2000 : 0;
    MEM32(0x2000)=0x3000; MEM32(0x30B4)=1;
    MEM32(0x500C)=2; MEM32(0x514C)=3; MEM32(0x5008)=4;
    for(uint32_t i=0;i<4;++i)MEM32(0x4000+0x100*i)=0x5000;
    if(patched)sub_A064B9C7();else sub_A064B9C7_lifted();
    if(bad)return 10+bad;
    if(g_esp!=0xF008 || g_ebx!=0xBB || g_esi!=0x55 || g_edi!=0xDD || g_ebp!=0xEE)return 20;
    if(MEM32(g_fs_base)!=0x1234)return 21;
    uint32_t present=has_panel ? (15u&~mask) : 0;
    if(found!=(has_panel?15u:0) || changed!=present || released!=present)return 22;
    for(uint32_t i=0;i<4;++i)
        if(MEM32(0x4008+0x100*i)!=((present&(1u<<i))?patched:0))return 23;
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-shadow-options-') as directory:
        base = Path(directory)
        source, binary = base / 'check.c', base / 'check.elf'
        source.write_text(code)
        subprocess.run([str(CLANG), '--target=aarch64-linux-android30', '-O2', '-fno-builtin',
                        '-ffunction-sections', '-fdata-sections', '-fno-stack-protector', '-nostdlib',
                        '-DRECOMP_LOCAL_REGS', '-Iandroid/runtime', '-Wl,--gc-sections,-e,check', str(source), '-o', str(binary)],
                       cwd=ROOT, check=True)
        elf = ELFFile(io.BytesIO(binary.read_bytes()))
        symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
        address, stack, stop = 0x10000000, 0x20000000, 0x30000000
        uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        uc.mem_map(address, 0x200000)
        uc.mem_map(stack, 0x10000)
        uc.mem_map(stop, 0x1000)
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD':
                uc.mem_write(address + segment['p_vaddr'], segment.data())
        relocations = elf.get_section_by_name('.rela.dyn')
        if relocations:
            for relocation in relocations.iter_relocations():
                assert relocation['r_info_type'] == 1027
                uc.mem_write(address + relocation['r_offset'],
                             (address + relocation['r_addend']).to_bytes(8, 'little'))
        for patched in (0, 1):
            for mask in range(16):
                for has_panel in (0, 1):
                    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000)
                    uc.reg_write(A.UC_ARM64_REG_LR, stop)
                    for register, value in zip((A.UC_ARM64_REG_X0, A.UC_ARM64_REG_X1, A.UC_ARM64_REG_X2),
                                               (patched, mask, has_panel)):
                        uc.reg_write(register, value)
                    uc.emu_start(address + symbols['check'], stop, count=1000000)
                    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
                    assert uc.reg_read(A.UC_ARM64_REG_X0) == 0, (patched, mask, has_panel,
                                                               uc.reg_read(A.UC_ARM64_REG_X0),
                                                               bytes(uc.mem_read(address+symbols['g_m'], 32)).hex())
    print('Shadow Options actual ARM64 native + lifted controller: 64 cases, disabled-scene '
          'override, exact shadow controls, absent panel/children, balanced releases, '
          'stack/nonvolatile registers and EH chain PASS')


if __name__ == '__main__':
    main()
