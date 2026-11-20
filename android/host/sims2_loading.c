/* Loading screens that pace the game's loaders: the original pacing, as on PC.
 *
 * The game's asynchronous loaders (neighbourhood, lot, startup and the others that report
 * progress) take one step per message: a step posts the next one through the message server.
 * While a loading screen is attached, each step's message (0x6DA2A629, wrapping the loader's
 * own "continue" message) goes to the screen first, and the screen may keep it until an
 * animation of its own is over:
 *
 *   cTSWinProcLotIntro (town and lot loading, handler 0xA05BE5B5, its message-handler base B):
 *     past its pacing table (B+0x58, index B+0x50) it sets the progress animation (B+0x48) to play
 *     to its current frame + 9 and keeps the message (B+0x5C); its per-frame handler (0xA05C00B3,
 *     this = B-8) posts it when the animation stops. About 320 ms a step on the Redmi Note 8 Pro.
 *   cTSWinProcGameLoad (the startup screen, handler 0xA05C041C): keeps the message (+0xA8) while
 *     its caption cross-fades (1.8 s) or the screen fades out.
 *
 * 0.2-dev posted every held step at once: the town/lot progress animation, set from its current
 * frame each step, never moved (Veronaville: 19 frames in 6.8 s instead of 261 in 10.2 s), and the
 * startup captions vanished half-faded. A 110 ms hold (22 November) still looked jerky to the
 * player, so the screens keep the game's own pacing again (2026-11-22, the user's decision).
 *
 * Development A/B only (debug.s2.options): "loading_animations=0" lets the town/lot screens post
 * a step held "loading_hold_ms=N" ms (default 110; 0 = at once, as 0.2-dev). The startup screen
 * is never changed. */
#include "host.h"
#include "sims2_settings.h"
#include <stdlib.h>
#include <string.h>

enum { HOLD_MS = 110 };

static int pacing = -1, hold_ms = -1;

/* 1 (the loaders wait as on PC) unless the development switch above asks otherwise. */
int s2_loading_animations(void)
{
    if (pacing < 0) {
        const char *value = getenv("PORT_OPTION_LOADING_ANIMATIONS");
        pacing = !(value && !strcmp(value, "0"));
        const char *hold = getenv("PORT_OPTION_LOADING_HOLD_MS");
        hold_ms = hold && *hold ? atoi(hold) : HOLD_MS;
        if (hold_ms < 0) hold_ms = 0;
        if (pacing) void_log("[loading] loading screens keep the original pacing");
        else void_log("[loading] development: town and lot loading screens hold a step for %d ms of their animation", hold_ms);
    }
    return pacing;
}

static unsigned released;
static uint32_t held_slot, held_message;
static uint64_t held_since;

/* Posts the step message a loading screen keeps at `slot` and clears the slot, as the screen's
 * own code does when its animation ends (message server vt+0x10 with "no delay", then the
 * reference holder's assignment 0xA0142080). */
static void release_step(uint32_t slot)
{
    uint32_t message = MEM32(slot);
    if (!message) return;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    uint32_t server = MEM32(0xA10F519Cu);
    if (!server) server = void_call_guest_this(0xA09C5A89u, 0, 0, NULL);
    if (server && void_vm_readable(server, 4) && void_vm_readable(MEM32(server) + 0x10, 4)) {
        uint32_t post[2] = { message, 0 }, none = 0;
        void_call_guest_this(MEM32(MEM32(server) + 0x10), server, 2, post);
        void_call_guest_this(0xA0142080u, slot, 1, &none);
        if (!released++) void_log("[loading] a loading screen's step released before its animation ended");
    }
    g_eax = eax; g_ecx = ecx; g_edx = edx;
    held_slot = 0;
}

/* cTSWinProcLotIntro message handler (this: its message-handler base; the step at +0x5C): notes
 * when a step starts being held (or posts it at once with a zero hold). */
void sub_A05BE5B5_lifted(void);
void sub_A05BE5B5(void)
{
    uint32_t self = g_ecx;
    sub_A05BE5B5_lifted();
    if (s2_loading_animations() || !void_vm_readable(self + 0x5C, 4)) return;
    uint32_t message = MEM32(self + 0x5C);
    if (!message) return;
    if (!hold_ms) { release_step(self + 0x5C); return; }
    if (held_slot != self + 0x5C || held_message != message) {
        held_slot = self + 0x5C;
        held_message = message;
        held_since = void_monotonic_ns();
    }
}

/* cTSWinProcLotIntro per-frame handler (this: the message-handler base - 8): after the original
 * (which posts the step when the animation has stopped), a step held HOLD_MS goes on. */
void sub_A05C00B3_lifted(void);
void sub_A05C00B3(void)
{
    uint32_t slot = g_ecx + 8 + 0x5C;
    sub_A05C00B3_lifted();
    if (pacing > 0 || held_slot != slot || !void_vm_readable(slot, 4)) return;
    uint32_t message = MEM32(slot);
    if (message != held_message) { held_slot = 0; return; }   /* the screen posted it itself */
    if (void_monotonic_ns() - held_since >= (uint64_t)hold_ms * 1000000ull) {
        uint32_t eax = g_eax;
        release_step(slot);
        g_eax = eax;
    }
}
