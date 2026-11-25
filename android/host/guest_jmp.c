/* Keep the lifted CRT's buffer capture, register restoration and SEH unwind;
 * its final jump must also discard the intervening native call frames. */
#include "host.h"
#include "guest_jmp.h"

static __thread guest_jmp_frame *frames;

void void_guest_jmp_leave(guest_jmp_frame *frame)
{
    for (guest_jmp_frame **p = &frames; *p; p = &(*p)->prev) {
        if (*p == frame) { *p = frame->prev; return; }
    }
}

void void_guest_jmp_enter(guest_jmp_frame *frame, uint32_t env, uint32_t caller, uint32_t entry_esp)
{
    if (!env || !void_vm_writable(env, 0x40)) void_fatal("setjmp with invalid guest buffer %08X", env);
    void_guest_jmp_leave(frame);
    frame->prev = frames;
    frame->env = env;
    frame->caller = caller;
    frame->entry_esp = entry_esp;
    frame->fs = g_fs_base;
    frame->eh_frames = g_eh_frames;
    frame->eh_obj = g_eh_obj;
    frame->eh_info = g_eh_info;
    frames = frame;
}

void void_guest_jmp_try_resume(void)
{
    uint32_t env = g_edx;
    for (guest_jmp_frame *frame = frames; frame; frame = frame->prev) {
        if (frame->env != env) continue;
        if (g_esp != MEM32(env + 0x10) + 4 || g_ebp != MEM32(env) ||
            g_ebx != MEM32(env + 4) || g_edi != MEM32(env + 8) || g_esi != MEM32(env + 12))
            void_fatal("longjmp guest registers differ from buffer %08X", env);
        /* The target stays armed: Lua can throw to it again after an inner pcall. */
        frames = frame;
        g_fs_base = frame->fs;
        g_eh_frames = frame->eh_frames;
        g_eh_obj = frame->eh_obj;
        g_eh_info = frame->eh_info;
        g_cur_func = frame->caller;
        static __thread unsigned trace;
        if (trace++ < 16) void_log("[jmp] resume env=%08X eax=%08X caller=%08X", env, g_eax, g_cur_func);
        longjmp(frame->host, 1);
    }
    void_fatal("longjmp without a live lifted caller for buffer %08X", env);
}

void void_guest_jmp_unwind(uint32_t entry_esp)
{
    guest_jmp_frame **p = &frames;
    while (*p) {
        if ((*p)->entry_esp < entry_esp) *p = (*p)->prev;
        else p = &(*p)->prev;
    }
}

void void_guest_jmp_reset(void) { frames = NULL; }
