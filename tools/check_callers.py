"""Check literal caller identities, call transfers, and EH resume on AArch64.

    python tools/check_callers.py

Requires the Android NDK, Unicorn and pyelftools already used by port checks.
"""
from pathlib import Path
import re
import subprocess

import build_android as build
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/test/caller-va'
OUT.mkdir(parents=True, exist_ok=True)


def main():
    count = 0
    for directory in (ROOT / 'generated').glob('*-x86'):
        for source in directory.glob('*.c'):
            text = source.read_bytes()
            for match in build.FUNCTION_START.finditer(text):
                end = build.FUNCTION_END.search(text, match.end())
                assert end, (source, match.group(0))
                entries = build.FUNCTION_ENTER.findall(build.COMMENT.sub(b' ', text[match.end():end.end()]))
                address = re.search(rb'sub_([0-9A-F]{8})', match.group(0))[1]
                assert len(entries) == 1 and int(entries[0].rstrip(b'u'), 16) == int(address, 16), (source, match.group(0), entries)
                count += 1
    for entries in ('', 'RECOMP_ENTER(0xA0000000u); RECOMP_ENTER(0xA0000010u);'):
        try:
            build.localize_registers(('void sub_A0000000(void) {\n' + entries + '\n}\n').encode())
        except ValueError:
            pass
        else:
            raise AssertionError('localizer accepted missing or multiple entry identities')
    prefix = r'''
#define RECOMP_GENERATED_CODE
#define RECOMP_LOCAL_REGS
#define RECOMP_PLAIN_MEMORY
#define RECOMP_YIELD_EVERY 1
#include "recomp_types.h"
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx;
uint32_t g_enter_trace[1024], g_enter_idx;
void (*recomp_yield_hook)(void);
static recomp_ehframe_t *frames;
static uint32_t status, resolved, yielded;
#define CHECK(test, code) do { if (!(test) && !status) status = (code); } while (0)
void sub_A0001000(void); void sub_A0002000(void); void sub_A0003000(void);
void sub_A0003100(void); void sub_A0004000(void); void sub_A0005000(void);
void recomp_eh_push(recomp_ehframe_t *f) { f->prev = frames; frames = f; }
void recomp_eh_pop(recomp_ehframe_t *f) { frames = f->prev; }
recomp_func_t void_resolve(uint32_t address) {
    ++resolved; CHECK(address == 0xA0002000u, 1); return sub_A0002000;
}
static void yield_test(void) {
    uint32_t saved = g_cur_func;
    CHECK(saved == 0xA0001000u, 2);
    g_cur_func = 0x12345678u;
    g_cur_func = saved;
    ++yielded;
}
static uint32_t recorded_caller(void) {
#ifdef RECOMP_TRACE
    return g_icall_from[(g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1)];
#else
    return g_icall_from[ICALL_TRACE_SIZE - 1];
#endif
}
static void host_fallback(void) {
    uint32_t _cf = 0, _flag_k = FK_NONE, _flag_a = 0, _flag_b = 0;
    RECOMP_REGS_LOCALS
    g_cur_func = 0x12345678u;
    RECOMP_CALL(sub_A0002000);
    CHECK(g_cur_func == 0x12345678u, 3);
    RECOMP_ICALL(0xA0002000u);
    CHECK(g_cur_func == 0x12345678u && recorded_caller() == 0x12345678u, 4);
    RECOMP_REGS_OUT();
}
/* Minimal AAPCS64 setjmp for the freestanding test: no libc or signal state. */
__asm__(".global _setjmp\n_setjmp:\n"
        "stp x19, x20, [x0, #0]\nstp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\nstp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\nstp x29, x30, [x0, #80]\n"
        "mov x2, sp\nstr x2, [x0, #96]\n"
        "stp d8, d9, [x0, #104]\nstp d10, d11, [x0, #120]\n"
        "stp d12, d13, [x0, #136]\nstp d14, d15, [x0, #152]\n"
        "mov w0, #0\nret\n"
        ".global _longjmp\n_longjmp:\nmov x2, x0\n"
        "ldp x19, x20, [x2, #0]\nldp x21, x22, [x2, #16]\n"
        "ldp x23, x24, [x2, #32]\nldp x25, x26, [x2, #48]\n"
        "ldp x27, x28, [x2, #64]\nldp x29, x30, [x2, #80]\n"
        "ldr x3, [x2, #96]\nmov sp, x3\n"
        "ldp d8, d9, [x2, #104]\nldp d10, d11, [x2, #120]\n"
        "ldp d12, d13, [x2, #136]\nldp d14, d15, [x2, #152]\n"
        "cmp w1, #0\ncsinc w0, w1, wzr, ne\nret\n");
'''
    declarations = 'uint32_t _cf = 0, _flag_k = FK_NONE, _flag_a = 0, _flag_b = 0, _itail_tgt = 0;\nRECOMP_REGS_LOCALS\n'

    def body(address, code):
        return f'void sub_{address:08X}(void) {{\n{declarations}RECOMP_ENTER(0x{address:08X}u);\n{code}\n}}\n'

    bodies = body(0xA0002000, 'eax = 0x71; _flag_k = FK_CMP; _flag_a = 7; _flag_b = 3; _cf = 0;\nesp += 4; RECOMP_FLAGS_OUT(); RECOMP_REGS_OUT();')
    bodies += body(0xA0003000, 'RECOMP_ITAIL(0xA0002000u); RECOMP_REGS_OUT();')
    bodies += body(0xA0003100, 'esp += 4; RECOMP_ITAIL(RECOMP_RETADDR); RECOMP_REGS_OUT();')
    bodies += body(0xA0001000, r'''
RECOMP_CALL(sub_A0002000); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0001000u && eax == 0x71 && _flag_a == 7 && _flag_b == 3, 10);
RECOMP_ICALL(0xA0002000u); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0001000u && recorded_caller() == 0xA0001000u, 11);
RECOMP_CALL(sub_A0003000); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0001000u && recorded_caller() == 0xA0003000u, 12);
RECOMP_CALL(sub_A0003100); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0001000u, 13);
RECOMP_BACKEDGE();
CHECK(g_cur_func == 0xA0001000u && yielded == 1, 14);
esp += 4; RECOMP_REGS_OUT();
''')
    bodies += body(0xA0005000, 'frames->resume = 0xA0004FFFu; g_esp = frames->entry_esp; _longjmp(frames->jb, 1);')
    bodies += body(0xA0004000, r'''
RECOMP_EH_ENTER();
RECOMP_CALL(sub_A0005000);
CHECK(0, 20);
_ljump:
CHECK(_itail_tgt == 0xA0004FFFu && g_cur_func == 0xA0005000u, 21);
RECOMP_ICALL(0xA0002000u); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0004000u && recorded_caller() == 0xA0004000u, 22);
RECOMP_CALL(sub_A0002000); RECOMP_FLAGS_IN();
CHECK(g_cur_func == 0xA0004000u && eax == 0x71, 23);
RECOMP_EH_LEAVE(); esp += 4; RECOMP_REGS_OUT();
''')
    suffix = r'''
static void run(recomp_func_t fn) {
    uint32_t stack = g_esp;
    g_esp -= 4; MEM32(g_esp) = RECOMP_RETADDR;
    fn(); CHECK(g_esp == stack, 30);
}
uint32_t selftest(void) {
    g_esp = 0x00808000; g_dispatch_gen = 1; recomp_yield_hook = yield_test;
    run(sub_A0001000); run(sub_A0004000);
    host_fallback();
    CHECK(g_esp == 0x00808000 && !frames, 31);
    CHECK(resolved >= 3, 32);
    return status;
}
'''
    source = OUT / 'check_callers.c'
    source.write_text(prefix + build.localize_registers(bodies.encode()).decode() + suffix)
    for trace in (False, True):
        binary = OUT / ('check_callers_trace.elf' if trace else 'check_callers.elf')
        subprocess.run([str(build.CLANG), '--target=aarch64-linux-android30', '-O2', '-std=gnu11',
                        '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-pie',
                        '-nostdlib', '-no-pie', '-Wl,-e,selftest', '-Wl,--build-id=none',
                        '-Iandroid/runtime', *(['-DRECOMP_TRACE'] if trace else []), str(source), '-o', str(binary)], cwd=ROOT, check=True)
        mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        with binary.open('rb') as stream:
            elf = ELFFile(stream)
            for segment in elf.iter_segments():
                if segment['p_type'] != 'PT_LOAD':
                    continue
                lo = segment['p_vaddr'] & ~4095
                hi = (segment['p_vaddr'] + segment['p_memsz'] + 4095) & ~4095
                mu.mem_map(lo, hi - lo)
                mu.mem_write(segment['p_vaddr'], segment.data())
            entry = elf['e_entry']
        mu.mem_map(0x10000000, 0x40000)
        mu.mem_map(0x2000800000, 0x10000)
        mu.reg_write(UC_ARM64_REG_SP, 0x1003FFF0)
        mu.reg_write(UC_ARM64_REG_LR, 0xF0000000)
        mu.emu_start(entry, 0xF0000000, timeout=10000000)
        assert mu.reg_read(UC_ARM64_REG_PC) == 0xF0000000, 'self-check timed out'
        assert mu.reg_read(UC_ARM64_REG_X0) == 0, f'caller self-check failed: {mu.reg_read(UC_ARM64_REG_X0)} (trace={trace})'
    print(f'PASS: {count} generated entry identities; direct/indirect/tail/yield/EH-resume/host fallback, tracing on and off')


if __name__ == '__main__':
    main()
