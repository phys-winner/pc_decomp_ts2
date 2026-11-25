/* MSVC setjmp needs a live host landing pad in its lifted caller (as in Painkiller). */
#pragma once
#include "recomp_types.h"

typedef struct guest_jmp_frame {
    struct guest_jmp_frame *prev;
    jmp_buf host;
    uint32_t env, caller, entry_esp, fs, eh_obj, eh_info;
    recomp_ehframe_t *eh_frames;
} guest_jmp_frame;

void void_guest_jmp_enter(guest_jmp_frame *frame, uint32_t env, uint32_t caller, uint32_t entry_esp);
void void_guest_jmp_leave(guest_jmp_frame *frame);
void void_guest_jmp_try_resume(void);
void void_guest_jmp_unwind(uint32_t entry_esp);
void void_guest_jmp_reset(void);

#define RECOMP_MSVC_SETJMP(func, frame) do { \
    void_guest_jmp_enter(&(frame), MEM32(esp), RECOMP_CALLER_VA, _sj_entry); \
    if (setjmp((frame).host)) { \
        RECOMP_REGS_IN(); \
    } else { \
        RECOMP_CALL(func); \
    } \
} while (0)
