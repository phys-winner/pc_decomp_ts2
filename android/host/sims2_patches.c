/* Native replacements for game functions that only make sense on a Windows
 * PC (tools/build_android.py NATIVE_OVERRIDES: the lifted body is compiled
 * as <name>_lifted, calls and the dispatch table bind to these).
 *
 * crashpad::CrashpadClient::StartHandler (0xA0C52180, __thiscall, nine
 * reference/bool stack arguments, `retn 24h`, bool in AL): the Legacy
 * build starts Google's crashpad_handler.exe to upload crash dumps to EA.
 * There is no handler process on Android, and crashpad CHECK-fails (int3)
 * when its named pipe cannot be created. Its caller (0xA0C2B20A) ignores the
 * result, so the port reports "not started" and the game runs without crash
 * uploads. */
#include "host.h"
#include "sims2_settings.h"

void sub_A0C52180_lifted(void);
void sub_A0C52180(void)
{
    VLOG("[patch] crashpad StartHandler skipped (caller %08X)", g_cur_func);
    g_eax &= 0xFFFFFF00u;          /* false */
    g_esp += 4u + 0x24u;           /* ret 24h */
}

/* cGZCOM's unique ID (0xA0145325: __thiscall, an out pointer, `ret 4`, true
 * in AL): the seconds since 2 Feb 1997 (0xA0145774) in the low 29 bits under
 * 3 random ones, at most one ID per second. Under the COM lock the original
 * sleeps 250 ms at a time until time() reaches the second after the last one
 * it used. In the first load of a town the skin catalog reserves IDs for the
 * town's outfits in bulk (below: one second per 8 IDs) and then asks for one
 * more: 6 s with nothing running, the audio loader blocked on the same lock,
 * until the clock passed the reserved seconds. Here an ID that would wait takes the
 * next unused second instead, at most ID_AHEAD seconds past the clock. IDs
 * stay distinct within a session, and a restart cannot reach a borrowed
 * second that soon. A larger gap (the clock set back) waits as before. The
 * calls are the original's, in its order: the same random draws. */
#define ID_AHEAD 30
static int64_t guest_time(void)
{
    const uint32_t null = 0;
    uint32_t low = void_call_guest(0xA082FBF6u, 1, &null, 0);   /* _time64(NULL) */
    return (int64_t)(((uint64_t)g_edx << 32) | low);
}

void sub_A0145325_lifted(void);
void sub_A0145325(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    if (self > UINT32_MAX - 0x6Cu || !void_vm_readable(self, 0x6C) || !void_vm_readable(sp, 8) ||
        !void_vm_writable(MEM32(sp + 4), 4) || !void_vm_readable(MEM32(self + 0x24), 12)) {
        sub_A0145325_lifted();
        return;
    }
    uint32_t out = MEM32(sp + 4), lock = self + 0x24;
    void_call_guest_this(MEM32(MEM32(lock) + 4), lock, 0, NULL);
    int64_t now = guest_time(), next = (int32_t)MEM32(self + 0x68);
    if (now < next && next - now > ID_AHEAD) {
        void_call_guest_this(MEM32(MEM32(lock) + 8), lock, 0, NULL);
        sub_A0145325_lifted();
        return;
    }
    const uint32_t eight = 8;
    uint32_t high = void_call_guest_this(0xA01B8D5Cu, self + 0x50, 1, &eight);   /* random 0..7 */
    uint32_t seconds;
    if (now >= next) {
        seconds = void_call_guest(0xA0145774u, 0, NULL, 0);
        MEM32(out) = high << 29 | (seconds & 0x1FFFFFFFu);
        MEM32(self + 0x68) = (uint32_t)guest_time() + 1u;
    } else {
        /* The 1997 seconds of a clock reading taken in the same second. */
        int64_t before, after;
        int tries = 0;
        do {
            before = guest_time();
            seconds = void_call_guest(0xA0145774u, 0, NULL, 0);
            after = guest_time();
        } while (before != after && ++tries < 4);
        int64_t used = before < next ? next : before;
        MEM32(out) = high << 29 | ((seconds + (uint32_t)(used - before)) & 0x1FFFFFFFu);
        MEM32(self + 0x68) = (uint32_t)used + 1u;
        static unsigned logged;
        if (logged++ < 8) void_log("[patch] unique ID: second %lld taken %lld s ahead", (long long)used,
                                   (long long)(used - before));
    }
    void_call_guest_this(MEM32(MEM32(lock) + 8), lock, 0, NULL);
    g_eax = (g_eax & 0xFFFFFF00u) | 1u;
    g_esp = sp + 8;
}

/* The same in bulk (0xA01453AB: __thiscall, out array and count, `ret 8`,
 * true in AL): ID k is (k & 7) << 29 | the second + k / 8, and the call
 * reserves (count + 7) / 8 seconds ahead, which the next single ID waited
 * out (the 6 s above). It waited as well, and now borrows the same way. */
void sub_A01453AB_lifted(void);
void sub_A01453AB(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    if (self > UINT32_MAX - 0x6Cu || !void_vm_readable(self, 0x6C) || !void_vm_readable(sp, 12) ||
        !void_vm_readable(MEM32(self + 0x24), 12) || MEM32(sp + 8) > 0x3FFFFFFu ||
        (MEM32(sp + 8) && !void_vm_writable(MEM32(sp + 4), MEM32(sp + 8) * 4u))) {
        sub_A01453AB_lifted();
        return;
    }
    uint32_t out = MEM32(sp + 4), count = MEM32(sp + 8), lock = self + 0x24;
    void_call_guest_this(MEM32(MEM32(lock) + 4), lock, 0, NULL);
    if (count) {
        int64_t now = guest_time(), next = (int32_t)MEM32(self + 0x68);
        if (now < next && next - now > ID_AHEAD) {
            void_call_guest_this(MEM32(MEM32(lock) + 8), lock, 0, NULL);
            sub_A01453AB_lifted();
            return;
        }
        uint32_t seconds;
        int64_t used = -1;
        if (now >= next) {
            seconds = void_call_guest(0xA0145774u, 0, NULL, 0);
        } else {
            int64_t before, after;
            int tries = 0;
            do {
                before = guest_time();
                seconds = void_call_guest(0xA0145774u, 0, NULL, 0);
                after = guest_time();
            } while (before != after && ++tries < 4);
            used = before < next ? next : before;
            seconds += (uint32_t)(used - before);
        }
        for (uint32_t k = 0; k < count; ++k)
            MEM32(out + 4u * k) = (k & 7u) << 29 | ((seconds + (k >> 3)) & 0x1FFFFFFFu);
        MEM32(self + 0x68) = (used < 0 ? (uint32_t)guest_time() : (uint32_t)used) + ((count + 7u) >> 3);
    }
    void_call_guest_this(MEM32(MEM32(lock) + 8), lock, 0, NULL);
    g_eax = (g_eax & 0xFFFFFF00u) | 1u;
    g_esp = sp + 12;
}

/* Graphics Options::EnableShadows (__thiscall, bool, ret 4). The original
 * panel initializer (A064D92B) disables the shadow heading and its three
 * radio buttons in neighborhood/other non-lot scenes. The original Apply
 * handler already accepts and saves all three choices in those scenes;
 * keep them available before a lot is loaded as well. */
void sub_A064B9C7_lifted(void);
void sub_A064B9C7(void)
{
    MEM32(g_esp + 4u) = 1;
    sub_A064B9C7_lifted();
}

/* The release game's listProps command omits its output. Read the same live
 * property set as the renderer after Apply, while its guest thread is active. */
static void log_shadow_settings(void)
{
    static const char *const names[4] = { "simShadows", "objectShadows", "guob", "heightMapShadows" };
    int values[4] = { -1, -1, -1, -1 };
    uint32_t slot = void_galloc(68);
    if (!slot) { void_log("[settings] shadow query allocation failed"); return; }
    uint32_t key = slot + 4, chars = slot + 28, any = slot + 60;
    MEM32(slot) = 0;
    uint32_t com = void_call_guest(0xA014323Fu, 0, NULL, 0);
    if (com) {
        uint32_t args[3] = { 0x0AC1887Eu, 0x4BB0D269u, slot };
        void_call_guest_this(MEM32(MEM32(com) + 20), com, 3, args);
    }
    uint32_t set = MEM32(slot);
    for (int i = 0; set && i < 4; ++i) {
        uint32_t length = (uint32_t)strlen(names[i]);
        memset(GPTR(key), 0, 24);
        memcpy(GPTR(chars), names[i], length + 1);
        MEM32(key) = chars;
        MEM32(key + 16) = length;
        MEM32(key + 20) = 31;
        MEM32(any) = 0xA0D9C670u;                           /* AnyBoolean */
        MEM32(any + 4) = 0;
        uint32_t args[2] = { key, any };
        if (void_call_guest_this(MEM32(MEM32(set) + 36), set, 2, args) & 0xFFu)
            values[i] = MEM8(any + 4) != 0;
    }
    if (set) void_call_guest_this(0xA01CBD79u, slot, 0, NULL);
    void_gfree(slot);
    uint32_t mask = 0xFFFFFFFFu;
    uint32_t scene = void_call_guest(0xA020DEFBu, 0, NULL, 0);
    if (scene) {
        uint32_t shadows = void_call_guest_this(MEM32(MEM32(scene) + 200), scene, 0, NULL);
        if (shadows) {
            uint32_t params = void_call_guest_this(MEM32(MEM32(shadows) + 48), shadows, 0, NULL);
            if (params) mask = MEM32(params + 44);
        }
    }
    void_log("[settings] shadows sim=%d objects=%d guob=%d heightMap=%d managerMask=%08X cachedHeightMap=%u cachedPortalLighting=%u",
        values[0], values[1], values[2], values[3], mask, MEM8(0xA0FFDE4Cu), MEM8(0xA0FFDE40u));
}

void sub_A064ADC6_lifted(void);
void sub_A064ADC6(void)
{
    sub_A064ADC6_lifted();
    uint32_t eax = g_eax;
    log_shadow_settings();
    g_eax = eax;
}

/* The launcher owns the choice; direct development launches preserve the old default. */
void s2_intro_latch(uint32_t owner)
{
    const char *value = getenv("PORT_SKIP_INTRO");
    if (!value || (strcmp(value, "0") && strcmp(value, "false"))) MEM8(owner + 0x11) = 1;
}

void sub_A05C089B_lifted(void);
void sub_A05C089B(void)
{
    const char *value = getenv("PORT_SKIP_INTRO");
    if (value && (!strcmp(value, "1") || !strcmp(value, "true"))) {
        /* The caller's original missing-movie path advances to loading, with its usual cleanup. */
        g_eax = 0;
        g_esp += 12;
        return;
    }
    sub_A05C089B_lifted();
}

void sub_A0ACB780_lifted(void);
void sub_A0ACB780(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    uint32_t event = void_vm_readable(sp, 8) ? MEM32(sp + 4) : 0;
    if (!void_vm_readable(self, 0x10D) || MEM32(self) != 0xA0E78514u ||
        !event || !void_vm_readable(event, 8) || MEM32(event + 4) != 1 || !s2_ui_take_material_resize()) {
        sub_A0ACB780_lifted();
        return;
    }
    /* shortcut: skip only the UI-reset duplicate; the later scene reparse must rebind viewport-dependent shader state. */
    g_eax = void_call_guest_this(0xA0ACBE90u, self, 0, NULL);
    void_log("[settings] UI resize: retained material definitions");
    g_esp = sp + 8;
}

/* Preserve the preview's orientation instead of facing the camera at purchase. */
void sub_A0680D8B_lifted(void);
void sub_A0680D8B(void)
{
    unsigned context = s2_touch_context();
    if ((context == S2_TOUCH_BUY || context == S2_TOUCH_BUILD) &&
        void_vm_readable(g_ecx, 0xA4) && MEM32(g_ecx) == 0xA0DE83FCu) {
        g_eax = 1;
        g_esp += 4;
        return;
    }
    sub_A0680D8B_lifted();
}
