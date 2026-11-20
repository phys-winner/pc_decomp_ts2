/* The port's own settings, shown natively in the game's Graphics/Performance
 * Options panel (tools/build_overlay.py adds the radio groups):
 *
 *   s2UIScale         100..300 %  independent UI layout/drawing transform,
 *                                 fitted to the currently visible panels
 *   s2FrameRateLimit  0 (off), 30 and this display's refresh rates
 *   s2RenderScale     50..150 %   of the screen's own pixels, for the
 *                                 backbuffer the game draws into
 *
 * They live in the game's user property set like every other option
 * (Config/userProps.xml, saved by the game): read from the file before the
 * game starts, then followed live through cGZPropertySetString::SetProperty
 * (0xA01BF515, a native override: tools/build_android.py NATIVE_OVERRIDES),
 * which the panel's radio bindings call when a button is clicked. */
#include "host.h"
#include "sims2_settings.h"
#include "input.h"
#include <sys/stat.h>
#include <stdatomic.h>
#include <unistd.h>

static _Atomic int ui_scale = 300;
static int adaptive_ui;
static _Atomic int fps_limit = 60;
static int render_scale = 100;
static int ui_fsr,render_aa,render_upscaler,render_msaa;
static int ui_scale_choice;
static int ui_startup_fit;
static unsigned ui_fit_depth, ui_fit_height_depth;
/* The last fit closed less than ui_fit_linger_ns ago: its size stays (1 its
 * width, 2 also its height) in case another fitting window follows. */
static int ui_fit_linger;
static uint64_t ui_fit_linger_ns = 400000000ull, ui_fit_linger_until;
static int ui_modal_floor_right, ui_modal_floor_bottom;
static uint32_t ui_fit_option;
static uint32_t ui_fit_cas;
static uint32_t ui_fit_chooser;
static unsigned ui_chooser_attach_depth;
static uint32_t ui_fit_loading, ui_fit_loading_screen;
static uint32_t ui_fit_town_loading, ui_fit_lot_loading;
static uint32_t ui_fit_lot_until_scene;
static uint32_t ui_fit_lot_info;
static unsigned ui_option_resize_depth;
static unsigned ui_lot_info_refresh_depth;
static unsigned touch_modal_depth, touch_mode_flags;
static uint32_t touch_modal_window;
static unsigned ui_pie_depth;
static unsigned touch_input_flags;
static uint32_t touch_mode_owner;
static _Atomic unsigned touch_context;
static _Atomic int touch_rewards;
static _Atomic uint32_t touch_photo_owner;
static int phys_w = 2780, phys_h = 1264;
static char props_path[1200];
static int display_rates[10] = {0, 30, 60}, display_rate_count = 3;

static void read_display_rates(void)
{
    const char *text = getenv("S2_DISPLAY_RATES");
    if (!text || !*text) return;
    int rates[10] = {0, 30}, count = 2;
    while (*text) {
        char *end;
        long hz = strtol(text, &end, 10);
        if (end == text || (*end && *end != ',') || hz < 30 || hz > 240) return;
        int found = 0;
        for (int i = 0; i < count; ++i) if (rates[i] == hz) found = 1;
        if (!found) {
            /* shortcut: eight display rates fit the original panel; add scrolling if a display exceeds this. */
            if (count == 10) { void_log("[settings] too many display rates for the FPS panel"); return; }
            rates[count++] = (int)hz;
        }
        if (!*end) break;
        text = end + 1;
        if (!*text) return;
    }
    for (int i = 2; i < count; ++i) for (int j = i; j > 1 && rates[j] < rates[j - 1]; --j) {
        int value = rates[j]; rates[j] = rates[j - 1]; rates[j - 1] = value;
    }
    memcpy(display_rates, rates, sizeof rates);
    display_rate_count = count;
}

int s2_ui_scale(void) { return ui_scale; }
int s2_fps_limit(void) { return fps_limit; }
int s2_render_scale(void) { return render_scale; }
int s2_ui_fsr(void) { return ui_fsr; }
int s2_render_aa(void) { return render_aa; }
int s2_render_msaa(void) { return render_msaa; }
int s2_render_upscaler(void) { return render_upscaler; }
unsigned s2_touch_context(void) { return atomic_load(&touch_context); }
uint32_t s2_touch_modal_window(void) { return touch_modal_window; }
int s2_touch_rewards(void) { return atomic_load(&touch_rewards); }
int s2_touch_photo(void) { return atomic_load(&touch_photo_owner) != 0; }
void s2_physical_size(int *w, int *h) { *w = phys_w; *h = phys_h; }

static void touch_publish(void)
{
    unsigned world=S2_TOUCH_TOWN|S2_TOUCH_LIVE|S2_TOUCH_BUY|S2_TOUCH_BUILD;
    unsigned mode=touch_mode_flags?touch_mode_flags:touch_input_flags&world;
    atomic_store(&touch_context, mode | (touch_input_flags&~world) |
        (touch_modal_depth ? S2_TOUCH_MODAL : 0) | (ui_fit_option ? S2_TOUCH_OPTIONS : 0) |
        (ui_fit_cas ? S2_TOUCH_CAS : 0) | (ui_fit_chooser ? S2_TOUCH_CHOOSER : 0) |
        ((ui_fit_loading || ui_fit_loading_screen || ui_fit_town_loading || ui_fit_lot_loading) ? S2_TOUCH_LOADING : 0) |
        (ui_fit_lot_info ? S2_TOUCH_LOT_INFO : 0));
}

void s2_touch_input_context(unsigned flags)
{
    if (touch_input_flags != flags) { touch_input_flags = flags; touch_publish(); }
}

static void touch_mode(uint32_t self, unsigned flags)
{
    if (flags || touch_mode_owner == self) {
        if (touch_mode_owner != (flags ? self : 0) || touch_mode_flags != flags) s2_touch_reset_cas_pan();
        touch_mode_owner = flags ? self : 0;
        touch_mode_flags = flags;
        atomic_store(&touch_rewards, flags && self && MEM32(self) == 0xA0DE1FB4u);
        touch_publish();
    }
}

/* RTTI identifies each mode's constructor/destructor; no keypress inference. */
void sub_A0619442_lifted(void);
void sub_A0619442(void) { uint32_t self = g_ecx; sub_A0619442_lifted(); touch_mode(self, S2_TOUCH_BUILD); }
void sub_A06194E1_lifted(void);
void sub_A06194E1(void) { uint32_t self = g_ecx; sub_A06194E1_lifted(); touch_mode(self, S2_TOUCH_BUY); }
void sub_A0619A89_lifted(void);
void sub_A0619A89(void) { uint32_t self = g_ecx; sub_A0619A89_lifted(); touch_mode(self, S2_TOUCH_LIVE); }
void sub_A0619C09_lifted(void);
void sub_A0619C09(void) { uint32_t self = g_ecx; sub_A0619C09_lifted(); touch_mode(self, S2_TOUCH_TOWN); }
void sub_A0619E26_lifted(void);
void sub_A0619E26(void) { uint32_t self = g_ecx; sub_A0619E26_lifted(); touch_mode(self, S2_TOUCH_OPTIONS); }
void sub_A0619F86_lifted(void);
void sub_A0619F86(void) { uint32_t self = g_ecx; sub_A0619F86_lifted(); touch_mode(self, S2_TOUCH_BUY); }
void sub_A061A29D_lifted(void);
void sub_A061A29D(void) { touch_mode(g_ecx, 0); sub_A061A29D_lifted(); }
void sub_A061A2CB_lifted(void);
void sub_A061A2CB(void) { touch_mode(g_ecx, 0); sub_A061A2CB_lifted(); }
void sub_A061A67F_lifted(void);
void sub_A061A67F(void) { touch_mode(g_ecx, 0); sub_A061A67F_lifted(); }
void sub_A061A796_lifted(void);
void sub_A061A796(void) { touch_mode(g_ecx, 0); sub_A061A796_lifted(); }
void sub_A061A919_lifted(void);
void sub_A061A919(void) { touch_mode(g_ecx, 0); sub_A061A919_lifted(); }
void sub_A061A9F2_lifted(void);
void sub_A061A9F2(void) { touch_mode(g_ecx, 0); sub_A061A9F2_lifted(); }

/* The album manager owns the EP6 framing window. The game's C and Escape handlers finish it. */
void sub_A060AFF1_lifted(void);
void sub_A060AFF1(void)
{
    uint32_t self = g_ecx;
    sub_A060AFF1_lifted();
    if (g_eax & 0xFF) atomic_store(&touch_photo_owner, self);
}

void sub_A0609486_lifted(void);
void sub_A0609486(void)
{
    uint32_t self = g_ecx;
    sub_A0609486_lifted();
    if (atomic_load(&touch_photo_owner) == self) atomic_store(&touch_photo_owner, 0);
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static double ui_applied_factor = 1.0;
static int ui_layout_pending;

double s2_ui_factor(void) { return ui_applied_factor; }
void s2_fit_scene_attached(void) { ui_fit_lot_until_scene = 0; }

uint32_t s2_ui_pick(uint32_t root, int x, int y)
{
    uint32_t point[2] = { (uint32_t)(int)((float)x / (float)ui_applied_factor), (uint32_t)(int)((float)y / (float)ui_applied_factor) };
    return void_call_guest_this(MEM32(MEM32(root) + 0xBC), root, 2, point);
}

static double ui_desired_factor(void)
{
    int width, height;
    s2_game_resolution(&width, &height);
    double factor = ui_scale / 100.0;
    int full = !adaptive_ui || ui_startup_fit || ui_fit_height_depth || ui_fit_linger == 2 || ui_fit_lot_until_scene;
    int floor_width = adaptive_ui && ui_modal_floor_right > 800 ? ui_modal_floor_right : 800;
    int floor_height = full ? 600 : 360;
    if (adaptive_ui && ui_modal_floor_bottom > floor_height) floor_height = ui_modal_floor_bottom;
    if (factor * floor_width > width) factor = (double)width / floor_width;
    if (factor * floor_height > height) factor = (double)height / floor_height;
    return factor < 1.0 ? 1.0 : factor;
}

static void ui_reposition_card(uint32_t manager, uint32_t root)
{
    if (!ui_fit_lot_info || ui_lot_info_refresh_depth) return;
    uint32_t owner = ui_fit_lot_info + 0xC;
    if (!void_vm_readable(owner, 4) || !void_vm_readable(MEM32(owner), 0x20)) return;
    uint32_t window = void_call_guest_this(MEM32(MEM32(owner) + 0x1C), owner, 0, NULL);
    if (!window || !void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager ||
        MEM32(window + 0x58) != root || !(MEM32(window + 0xE8) & 1)) return;
    int width = (int)MEM32(window + 0xD0) - (int)MEM32(window + 0xC8);
    int height = (int)MEM32(window + 0xD4) - (int)MEM32(window + 0xCC);
    int max_x = (int)MEM32(root + 0x2C) - width, max_y = (int)MEM32(root + 0x30) - height;
    if (width <= 0 || height <= 0 || max_x < 0 || max_y < 0) return;
    int x = clamp((int)MEM32(window + 0xC8), 0, max_x), y = max_y / 2;
    if (x == (int)MEM32(window + 0xC8) && y == (int)MEM32(window + 0xCC)) return;
    uint32_t position[4] = { (uint32_t)x, (uint32_t)y, (uint32_t)(x + width), (uint32_t)(y + height) };
    /* Base SetArea preserves size/clipping; Gen MoveTo also notifies the portrait-rebuilding proc. */
    void_call_guest_this(0xA0932914u, window, 4, position);
}

static void ui_owner_fit(uint32_t *owner, uint32_t self, int open);

static void ui_sync_chooser(void)
{
    uint32_t self = ui_fit_chooser;
    if (!self || ui_chooser_attach_depth) return;
    uint32_t manager = s2_touch_owner_manager();
    if (!manager || !void_vm_readable(manager, 0x48)) return;
    uint32_t root = MEM32(manager + 0x44);
    if (!root) return;
    /* Closing the in-town modal chooser can hide its window without detaching its controller. */
    if (void_vm_readable(self, 0x19C) && MEM32(self) == 0xA0DF9358u) {
        uint32_t window = MEM32(self + 0x198);
        for (unsigned depth = 0; depth < 16 && void_vm_readable(window, 0xEC); ++depth) {
            if (window == root) return;
            if (MEM32(window + 4) != manager || !(MEM32(window + 0xE8) & 1)) break;
            window = MEM32(window + 0x58);
        }
    }
    ui_owner_fit(&ui_fit_chooser, self, 0);
}

void s2_game_resolution(int *w, int *h)
{
    /* UI magnification uses the game's own layout/drawing transform, never D3D Reset. */
    *w = phys_w & ~1;
    *h = phys_h & ~1;
    int min_width = 800, min_height = 600;
    if (*w < min_width) {
        *w = (min_width + 1) & ~1;
        *h = (phys_h * *w / phys_w + 1) & ~1;
    }
    if (*h < min_height) {
        *h = (min_height + 1) & ~1;
        *w = (phys_w * *h / phys_h + 1) & ~1;
    }
}

static void ui_fit(int enter, int full_height)
{
    int old_w, old_h, new_w, new_h;
    s2_game_resolution(&old_w, &old_h);
    if (enter) {
        ui_fit_linger = 0;                        /* takes over a closing fit's size */
        ++ui_fit_depth;
        if (full_height) ++ui_fit_height_depth;
    } else {
        if (ui_fit_depth) --ui_fit_depth;
        if (full_height && ui_fit_height_depth) --ui_fit_height_depth;
        if (!ui_fit_depth && ui_fit_linger_ns) {
            ui_fit_linger = full_height ? 2 : 1;
            ui_fit_linger_until = void_monotonic_ns() + ui_fit_linger_ns;
        }
    }
    s2_game_resolution(&new_w, &new_h);
    if (new_w != old_w || new_h != old_h) s2_ui_scale_requested(ui_scale);
    ui_layout_pending = 1;
}

/* Consecutive dialogs share a fit; apply at an owner-thread pump boundary,
 * or before a hidden card builds its layout, without rebuilding the scene. */
void s2_fit_tick(void)
{
    ui_sync_chooser();
    if (ui_fit_linger && void_monotonic_ns() >= ui_fit_linger_until) {
        ui_fit_linger = 0;
        ui_layout_pending = 1;
    }
    /* The pie menu lays out around the cursor in the current logical coordinates. */
    if (ui_pie_depth) return;
    double factor = ui_desired_factor();
    if (!ui_layout_pending && factor == ui_applied_factor) return;
    uint32_t manager = s2_touch_owner_manager();
    if (!manager || !void_vm_readable(manager, 0x214) || MEM32(manager) != 0xA0E5B240u) return;
    uint32_t root = MEM32(manager + 0x44);
    if (!root || !void_vm_readable(root, 0xEC)) return;
    ui_layout_pending = 0;
    if (factor == ui_applied_factor) return;
    void_regs saved;
    void_regs_save(&saved);
    ui_applied_factor = factor;
    s2_touch_reset_cas_pan();
    uint64_t start = void_monotonic_ns();
    /* The original scale refresh resizes only the window tree; shaders and scene resources stay live. */
    void_call_guest_this(0xA0971A55u, manager, 0, NULL);
    ui_reposition_card(manager, root);
    void_log("[ui-layout] scale %.4f root %ux%u in %.3f ms (no device reset)", factor,
        MEM32(root + 0x2C), MEM32(root + 0x30), (double)(void_monotonic_ns() - start) / 1e6);
    void_regs_load(&saved);
}

static const char *fit_owner_name(const uint32_t *owner)
{
    return owner == &ui_fit_option ? "options" : owner == &ui_fit_cas ? "cas" : owner == &ui_fit_chooser ? "chooser" :
        owner == &ui_fit_loading ? "loading" : owner == &ui_fit_loading_screen ? "loading-screen" :
        owner == &ui_fit_town_loading ? "town-loading" :
        owner == &ui_fit_lot_loading ? "lot-loading" : owner == &ui_fit_lot_info ? "lot-info" : "?";
}

static void ui_owner_fit(uint32_t *owner, uint32_t self, int open)
{
    if (open) {
        int was_open = *owner != 0;
        *owner = self;
        touch_publish();
        if (!was_open) {
            void_log("[fit] %s open (depth %u)", fit_owner_name(owner), ui_fit_depth + 1);
            ui_fit(1, 1);
        }
    } else if (*owner == self) {
        *owner = 0;
        touch_publish();
        void_log("[fit] %s close (depth %u)", fit_owner_name(owner), ui_fit_depth ? ui_fit_depth - 1 : 0);
        ui_fit(0, 1);
    }
}

static _Thread_local unsigned ui_sampling_depth;
int s2_font_drawing(void);
int s2_ui_drawing(void) { return ui_sampling_depth!=0 || s2_font_drawing(); }

void sub_A08B8600_lifted(void);
void sub_A08B8600(void)
{
    ++ui_sampling_depth;
    sub_A08B8600_lifted();
    --ui_sampling_depth;
}

/* Drawing, clipping and the manager's inverse mouse transform share one scale. */
void sub_A08B8390(void)
{
    double factor = 1.0;
    uint32_t rect = MEM32(g_esp + 4);
    int width, height;
    s2_game_resolution(&width, &height);
    /* Portraits and other offscreen surfaces keep their own coordinate system. */
    if (width > 0 && height > 0 && void_vm_readable(rect, 16) && MEM32(rect) == 0 && MEM32(rect + 4) == 0 &&
        MEM32(rect + 8) == (uint32_t)width && MEM32(rect + 12) == (uint32_t)height)
        factor = ui_applied_factor;
    void_x87_push(factor);
    g_esp += 8; /* return address and rect argument (retn 4) */
}

void sub_A09713BE(void)
{
    void_x87_push(ui_applied_factor);
    g_esp += 8;
}

void sub_A05CA399_lifted(void);
void sub_A05CA399(void)
{
    uint32_t menu = g_ecx, rect = MEM32(g_esp + 4), manager = MEM32(menu + 4);
    uint32_t root = manager && void_vm_readable(manager, 0x48) ? MEM32(manager + 0x44) : 0;
    if (ui_applied_factor == 1.0 || !void_vm_readable(root, 0x34)) {
        sub_A05CA399_lifted();
        return;
    }
    /* The original multiplies the edge correction by the UI scale, then adds it to logical coordinates. */
    int left = (int)MEM32(rect), top = (int)MEM32(rect + 4);
    int right = (int)MEM32(rect + 8), bottom = (int)MEM32(rect + 12);
    int margin_x = (int)MEM32(menu + 0x118), margin_y = (int)MEM32(menu + 0x11C);
    int min_x = (int)MEM32(root + 0x24) + margin_x, min_y = (int)MEM32(root + 0x28) + margin_y;
    int max_x = (int)MEM32(root + 0x2C) - margin_x, max_y = (int)MEM32(root + 0x30) - margin_y;
    int dx = left < min_x ? min_x - left : right > max_x ? max_x - right : 0;
    int dy = top < min_y ? min_y - top : bottom > max_y ? max_y - bottom : 0;
    uint32_t bounds[] = { left + dx, top + dy, right + dx, bottom + dy };
    for (unsigned i = 0; i < 4; ++i) MEM32(rect + 4 * i) = bounds[i];
    void_regs saved;
    void_regs_save(&saved);
    void_call_guest_this(0xA0932914u, menu, 4, bounds);
    void_regs_load(&saved);
    MEM32(menu + 0x110) = MEM32(menu + 0x108);
    MEM32(menu + 0x114) = MEM32(menu + 0x10C);
    MEM32(menu + 0x108) += dx;
    g_eax = (MEM32(menu + 0x10C) += dy);
    g_esp += 8;
}

static int ui_modal_gen(uint32_t window)
{
    uint32_t table = MEM32(window);
    return table == 0xA0E54498u && void_vm_readable(table, 0x1A0) &&
        MEM32(table + 0x2C) == 0xA01DFF70u && MEM32(table + 0x30) == 0xA0932FF7u &&
        MEM32(table + 0x4C) == 0xA093061Au && MEM32(table + 0xE0) == 0xA0931190u &&
        MEM32(table + 0x19C) == 0xA0930463u;
}

static int ui_story_window(uint32_t manager, uint32_t window)
{
    if (!manager || manager != s2_touch_owner_manager() || !window ||
        !void_vm_readable(window, 0xA4) || MEM32(window + 4) != manager) return 0;
    uint32_t head = MEM32(window + 0xA0);
    if (!head || !void_vm_readable(head, 12)) return 0;
    uint32_t node = MEM32(head);
    for (unsigned i = 0; node && node != head && i < 64 && void_vm_readable(node, 12); ++i) {
        uint32_t proc = MEM32(node + 8);
        if (proc && void_vm_readable(proc, 0x14) && MEM32(proc + 0x10) == window &&
            (MEM32(proc) == 0xA0DE145Cu || MEM32(proc) == 0xA0DE1564u)) return 1;
        node = MEM32(node);
    }
    return 0;
}

static int ui_modal_fits(uint32_t manager, uint32_t window)
{
    if (!manager || manager != s2_touch_owner_manager() || !void_vm_readable(manager, 0x48) ||
        !window || !void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager) return 0;
    uint32_t root = MEM32(manager + 0x44);
    if (!root || root == window || !void_vm_readable(root, 0xEC) || MEM32(root + 4) != manager ||
        MEM32(root + 0x58) || MEM32(root + 0x24) || MEM32(root + 0x28)) return 0;
    int left = (int)MEM32(window + 0x24), top = (int)MEM32(window + 0x28);
    int right = (int)MEM32(window + 0x2C), bottom = (int)MEM32(window + 0x30);
    int width, height, current_width, current_height;
    s2_game_resolution(&width, &height);
    void_input_screen_size(&current_width, &current_height);
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        width <= 0 || height <= 0 || current_width <= 0 || current_height <= 0 ||
        right > width || bottom > height || right > current_width || bottom > current_height ||
        right > (int)MEM32(root + 0x2C) || bottom > (int)MEM32(root + 0x30)) return 0;
    /* RunModal attaches a registered parentless Gen to this root before Show
     * (A0973044..3050). AddChild then recomputes its global rect (+19C).
     * Accept only initialized canonical geometry whose attachment preserves it. */
    if (!MEM32(window + 0x58)) {
        uint32_t table = MEM32(manager);
        if (!ui_modal_gen(window) || !ui_modal_gen(root) || !void_vm_readable(manager, 0x64) ||
            table != 0xA0E5B240u || !void_vm_readable(table, 0x5C) ||
            MEM32(table + 0x58) != 0xA09737BFu || width != current_width || height != current_height ||
            MEM32(root + 0x2C) != (uint32_t)width || MEM32(root + 0x30) != (uint32_t)height) return 0;
        for (unsigned i = 0; i < 16; i += 4)
            if (MEM32(window + 0x24 + i) != MEM32(window + 0xC8 + i) ||
                MEM32(root + 0x24 + i) != MEM32(root + 0xC8 + i)) return 0;
        /* The original +58 method is a pure manager registration lookup. */
        void_regs saved;
        void_regs_save(&saved);
        int registered = (void_call_guest_this(0xA09737BFu, manager, 1, &window) & 255) != 0 &&
            (void_call_guest_this(0xA09737BFu, manager, 1, &root) & 255) != 0;
        void_regs_load(&saved);
        return registered;
    }
    for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0x5C); ++depth) {
        if (window == root) return 1;
        window = MEM32(window + 0x58);
    }
    return 0;
}

/* Fitting modals retain their measured edges through later UI scale changes.
 * Stack-local saves restore outer floors without retaining destroyed windows. */
void sub_A0972F48_lifted(void);
void sub_A0972F48(void)
{
    uint32_t window = MEM32(g_esp + 4);
    uint32_t previous_window = touch_modal_window;
    int pie = void_vm_readable(window, 4) && MEM32(window) == 0xA0DDA24Cu;
    /* A dialog right after a fitted one fits only by its size: it continues
     * the fit instead (no resize now, and none to a floor in between). */
    /* Story procs stay attached after Close; fit only their RunModal lifetime. */
    int fit = !pie && (ui_fit_linger || ui_story_window(g_ecx, window) || !ui_modal_fits(g_ecx, window));
    int saved_right = ui_modal_floor_right, saved_bottom = ui_modal_floor_bottom;
    if (!fit && !pie) {
        int right = (int)MEM32(window + 0x2C), bottom = (int)MEM32(window + 0x30);
        if (right > ui_modal_floor_right) ui_modal_floor_right = right;
        if (bottom > ui_modal_floor_bottom) ui_modal_floor_bottom = bottom;
    }
    if (pie) ++ui_pie_depth;
    ++touch_modal_depth;
    touch_modal_window = window;
    touch_publish();
    if (fit) { void_log("[fit] modal open %08X (depth %u)", window, ui_fit_depth + 1); ui_fit(1, 1); }
    unsigned previous_depth = void_begin_modal_messages();
    sub_A0972F48_lifted();
    void_end_modal_messages(previous_depth);
    if (fit) { void_log("[fit] modal close %08X (depth %u)", window, ui_fit_depth ? ui_fit_depth - 1 : 0); ui_fit(0, 1); }
    else if (!pie) {
        int old_w, old_h, new_w, new_h;
        s2_game_resolution(&old_w, &old_h);
        ui_modal_floor_right = saved_right;
        ui_modal_floor_bottom = saved_bottom;
        s2_game_resolution(&new_w, &new_h);
        if (new_w != old_w || new_h != old_h) s2_ui_scale_requested(ui_scale);
    }
    if (pie) --ui_pie_depth;
    --touch_modal_depth;
    touch_modal_window = previous_window;
    touch_publish();
}

/* Options replace their nonmodal category directly. Keep the whole options
 * session fitted; category zero closes it. */
void sub_A0648ADF_lifted(void);
void sub_A0648ADF(void)
{
    uint32_t self = g_ecx;
    int open = MEM32(g_esp + 4) != 0;
    if (!ui_option_resize_depth && open) ui_owner_fit(&ui_fit_option, self, 1);
    sub_A0648ADF_lifted();
    if (!ui_option_resize_depth && !open) ui_owner_fit(&ui_fit_option, self, 0);
}

/* Hiding the whole Options window retains its selected category, bypassing
 * category zero; restore that category's fit when the window opens again. */
void sub_A06491B9_lifted(void);
void sub_A06491B9(void)
{
    uint32_t self = g_ecx - 8;
    int show = MEM8(g_esp + 4) != 0;
    if (!ui_option_resize_depth && show && MEM32(self + 0x1C)) ui_owner_fit(&ui_fit_option, self, 1);
    sub_A06491B9_lifted();
    if (!ui_option_resize_depth && !show) ui_owner_fit(&ui_fit_option, self, 0);
}

void sub_A0647521_lifted(void);
void sub_A0647521(void)
{
    uint32_t self = g_ecx;
    sub_A0647521_lifted();
    ui_owner_fit(&ui_fit_option, self, 0);
}

/* The CAS mode object lives only while Create-a-Sim is active. Its fixed
 * full-screen layout needs the same fit as Options and modal dialogs. */
void sub_A06195A1_lifted(void);
void sub_A06195A1(void)
{
    s2_touch_reset_cas_pan();
    ui_owner_fit(&ui_fit_cas, g_ecx, 1);
    sub_A06195A1_lifted();
}

void sub_A061A2F9_lifted(void);
void sub_A061A2F9(void)
{
    uint32_t self = g_ecx;
    sub_A061A2F9_lifted();
    ui_owner_fit(&ui_fit_cas, self, 0);
    s2_touch_reset_cas_pan();
}

/* The startup chooser and loading service are persistent objects: fit their
 * attached windows, then restore gameplay's scale when those windows leave. */
void sub_A0769278_lifted(void);
void sub_A0769278(void)
{
    ui_owner_fit(&ui_fit_chooser, g_ecx, 1);
    ++ui_chooser_attach_depth;
    sub_A0769278_lifted();
    --ui_chooser_attach_depth;
}

void sub_A0769B01_lifted(void);
void sub_A0769B01(void)
{
    uint32_t self = g_ecx;
    sub_A0769B01_lifted();
    ui_owner_fit(&ui_fit_chooser, self, 0);
}

void sub_A076662A_lifted(void);
void sub_A076662A(void)
{
    uint32_t self = g_ecx;
    sub_A076662A_lifted();
    ui_owner_fit(&ui_fit_chooser, self, 0);
}

void sub_A05C0B82_lifted(void);
void sub_A05C0B82(void)
{
    ui_owner_fit(&ui_fit_loading, g_ecx, 1);
    ui_startup_fit = 0;
    s2_fit_tick();
    sub_A05C0B82_lifted();
}

void sub_A05C0C89_lifted(void);
void sub_A05C0C89(void)
{
    uint32_t self = g_ecx;
    sub_A05C0C89_lifted();
    ui_owner_fit(&ui_fit_loading, self, 0);
}

/* LotIntro draws town, district and lot screens independently of their loaders. */
void sub_A05BE74D_lifted(void);
void sub_A05BE74D(void)
{
    uint32_t self = g_ecx, message = MEM32(g_esp + 8);
    uint32_t event = MEM32(g_esp + 4) && void_vm_readable(message, 4) ? MEM32(message) : 0;
    if (event == 0xA2BF8AD5u) {
        ui_owner_fit(&ui_fit_loading_screen, self, 1);
        s2_fit_tick();
    }
    sub_A05BE74D_lifted();
    if (event == 0xA2BF8AD6u || (event == 0xA2BF8AD5u && !(g_eax & 0xFF)))
        ui_owner_fit(&ui_fit_loading_screen, self, 0);
}

/* Loaders can draw during setup, before the next window-message pump. */
void sub_A0C47EFC_lifted(void);
void sub_A0C47EFC(void)
{
    ui_fit_lot_until_scene = 0;
    ui_owner_fit(&ui_fit_town_loading, g_ecx, 1);
    s2_fit_tick();
    sub_A0C47EFC_lifted();
}
void sub_A0C471DA_lifted(void);
void sub_A0C471DA(void)
{
    uint32_t self = g_ecx;
    sub_A0C471DA_lifted();
    if (!(g_eax & 0xFF)) ui_owner_fit(&ui_fit_town_loading, self, 0);
}
void sub_A0C46C58_lifted(void);
void sub_A0C46C58(void)
{
    uint32_t self = g_ecx;
    sub_A0C46C58_lifted();
    ui_owner_fit(&ui_fit_town_loading, self, 0);
}
void sub_A0C44B8C_lifted(void);
void sub_A0C44B8C(void)
{
    /* The last loading frame can still draw after the loader controller closes. */
    ui_fit_lot_until_scene = g_ecx;
    ui_owner_fit(&ui_fit_lot_loading, g_ecx, 1);
    s2_fit_tick();
    sub_A0C44B8C_lifted();
}
void sub_A0C42D84_lifted(void);
void sub_A0C42D84(void)
{
    uint32_t self = g_ecx;
    sub_A0C42D84_lifted();
    if (!(g_eax & 0xFF)) {
        /* State 35 means the load finished and the scene is about to attach. */
        if (ui_fit_lot_until_scene == self &&
            (!void_vm_readable(self + 20, 4) || MEM32(self + 20) != 35)) ui_fit_lot_until_scene = 0;
        ui_owner_fit(&ui_fit_lot_loading, self, 0);
    }
}
void sub_A0C42483_lifted(void);
void sub_A0C42483(void)
{
    uint32_t self = g_ecx;
    sub_A0C42483_lifted();
    ui_owner_fit(&ui_fit_lot_loading, self, 0);
}

/* Lot Info stays attached while hidden. Fit only its visible family/lot
 * card; refreshing it temporarily hides and shows the same card. */
void sub_A07B7B27_lifted(void);
void sub_A07B7B27(void)
{
    uint32_t self = g_ecx - 0xC;
    uint32_t mode = MEM32(g_esp + 12);
    int was_open = ui_fit_lot_info != 0;
    if (!ui_lot_info_refresh_depth) ui_owner_fit(&ui_fit_lot_info, self, 1);
    /* Fit while the card is hidden, before Show builds its portraits and layout. */
    if (!ui_lot_info_refresh_depth && !was_open) s2_fit_tick();
    sub_A07B7B27_lifted();
    if (!ui_lot_info_refresh_depth && !was_open && (mode == 0 || mode == 3) && !(g_eax & 0xFF))
        ui_owner_fit(&ui_fit_lot_info, self, 0);
}

void sub_A07B6484_lifted(void);
void sub_A07B6484(void)
{
    uint32_t self = g_ecx - 0xC;
    sub_A07B6484_lifted();
    if (!ui_lot_info_refresh_depth) ui_owner_fit(&ui_fit_lot_info, self, 0);
}

void sub_A07B6F59_lifted(void);
void sub_A07B6F59(void)
{
    ++ui_lot_info_refresh_depth;
    sub_A07B6F59_lifted();
    --ui_lot_info_refresh_depth;
}

void sub_A07B2A6F_lifted(void);
void sub_A07B2A6F(void)
{
    uint32_t self = g_ecx;
    sub_A07B2A6F_lifted();
    ui_owner_fit(&ui_fit_lot_info, self, 0);
}

/* Show can run before the queued WM_SIZE updates its parent. Reuse the
 * card's native refresh after that resize so it centers in the fitted area. */
void s2_ui_resized(void)
{
    if (ui_fit_lot_info && !ui_lot_info_refresh_depth)
        void_call_guest_this(0xA07B6F59u, ui_fit_lot_info + 0xC, 0, NULL);
}

/* WM_SIZE temporarily selects category zero, then restores the category.
 * Retain its fit across that rebuild, or each resize queues another resize. */
void sub_A064785E_lifted(void);
void sub_A064785E(void)
{
    ++ui_option_resize_depth;
    sub_A064785E_lifted();
    --ui_option_resize_depth;
}

static char *read_text(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0 || (unsigned long)n > SIZE_MAX - 1) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *text = (char *)malloc((size_t)n + 1);
    if (!text) { fclose(f); return NULL; }
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = 0;
    if (size) *size = got;
    return text;
}

/* <AnyUint32 key="NAME" type="...">VALUE</AnyUint32>: VALUE, or -1. */
static int xml_value(const char *text, const char *name)
{
    char needle[96];
    snprintf(needle, sizeof needle, "key=\"%s\"", name);
    const char *at = strstr(text, needle);
    if (!at) return -1;
    at = strchr(at, '>');
    return at ? atoi(at + 1) : -1;
}

/* Set (or add) one AnyUint32 / AnyString property in the file's text. */
static char *xml_set(char *text, const char *element, const char *name, const char *type, const char *value)
{
    char needle[96], line[256];
    snprintf(needle, sizeof needle, "key=\"%s\"", name);
    snprintf(line, sizeof line, "<%s key=\"%s\" type=\"%s\">%s</%s>", element, name, type, value, element);
    char *at = strstr(text, needle);
    size_t n = strlen(text);
    if (at) {
        char *start = at;
        while (start > text && *start != '<') --start;
        char *end = strstr(at, "</");
        end = end ? strchr(end, '>') : NULL;
        if (!end) return text;
        ++end;
        size_t head = (size_t)(start - text), tail = n - (size_t)(end - text);
        char *out = (char *)malloc(head + strlen(line) + tail + 1);
        if (!out) return text;
        memcpy(out, text, head);
        memcpy(out + head, line, strlen(line));
        memcpy(out + head + strlen(line), end, tail + 1);
        free(text);
        return out;
    }
    char *close = strstr(text, "</cGZPropertySetString>");
    if (!close) return text;
    size_t head = (size_t)(close - text);
    char *out = (char *)malloc(n + strlen(line) + 8);
    if (!out) return text;
    memcpy(out, text, head);
    size_t k = head;
    k += (size_t)sprintf(out + k, "  %s\n", line);
    memcpy(out + k, close, n - head + 1);
    free(text);
    return out;
}

void s2_settings_init(const char *data_dir, int screen_w, int screen_h)
{
    read_display_rates();
    if (screen_w > 0 && screen_h > 0) {
        phys_w = screen_w > screen_h ? screen_w : screen_h;    /* the game runs in landscape */
        phys_h = screen_w > screen_h ? screen_h : screen_w;
    }
    snprintf(props_path, sizeof props_path, "%s/Documents/EA Games/The Sims 2 Legacy/Config/userProps.xml", data_dir);
    size_t size = 0;
    char *text = read_text(props_path, &size);
    if (text) {
        int v;
        if ((v = xml_value(text, "s2UIScale")) > 0) ui_scale = v;
        if ((v = xml_value(text, "s2FrameRateLimit")) >= 0) fps_limit = v;
        if ((v = xml_value(text, "s2RenderScale")) > 0) render_scale = v;
    }
    ui_scale = clamp(ui_scale, 100, 300);
    fps_limit = fps_limit ? clamp(fps_limit, 15, 240) : 0;
    render_scale = clamp(render_scale, 25, 200);
    const char *adaptive = getenv("PORT_OPTION_ADAPTIVE_UI");
    adaptive_ui = adaptive && !strcmp(adaptive, "1");
    void_log("[settings] adaptive UI %s", adaptive_ui ? "on" : "off");
    /* The user's values go back to the file; S2_* overrides last one run. */
    int saved_ui = ui_scale, saved_fps = fps_limit, saved_render = render_scale;
    const char *resolution=getenv("PORT_OPTION_SCREEN_RESOLUTION");
    int percent=resolution?atoi(resolution):0;
    if (percent>=33 && percent<=100) saved_render=render_scale=percent;
    const char *cap=getenv("PORT_OPTION_FRAME_RATE_LIMIT");
    if (cap && (!strcmp(cap,"0") || !strcmp(cap,"30") || !strcmp(cap,"60") || !strcmp(cap,"90") || !strcmp(cap,"120")))
        saved_fps=fps_limit=atoi(cap);
    const char *fsr = getenv("PORT_OPTION_UI_FSR"), *aa = getenv("PORT_OPTION_RENDER_AA");
    ui_fsr = fsr && !strcmp(fsr, "1");
    render_aa = aa && !strcmp(aa, "1");
    const char *msaa=getenv("PORT_OPTION_RENDER_MSAA");
    render_msaa=msaa && !strcmp(msaa,"4")?4:msaa && !strcmp(msaa,"2")?2:0;
    const char *upscaler=getenv("PORT_OPTION_RENDER_UPSCALER");
    render_upscaler=upscaler && !strcmp(upscaler,"3")?3:upscaler && !strcmp(upscaler,"1")?1:0;
    void_log("[settings] launcher resolution %d%%, UI FSR1 %d, FXAA %d, render upscaler %d",percent,ui_fsr,render_aa,render_upscaler);
    const char *ui_override = getenv("S2_UI_SCALE"), *fps_override = getenv("S2_FPS_LIMIT"),
               *render_override = getenv("S2_RENDER_SCALE");
    if (ui_override) ui_scale = clamp(atoi(ui_override), 100, 300);
    if (fps_override) {
        int fps = atoi(fps_override);
        fps_limit = fps ? clamp(fps, 5, 240) : 0;
    }
    if (render_override) render_scale = clamp(atoi(render_override), 25, 200);
    ui_scale_choice = ui_scale;
    /* Start with the fitted UI; later owner-thread ticks only update its transform. */
    ui_startup_fit = 1;
    ui_applied_factor = ui_desired_factor();
    ui_layout_pending = 1;
    int gw, gh;
    s2_game_resolution(&gw, &gh);
    void_input_set_screen_size(gw, gh);
    if (text) {
        /* The options panel shows what the file holds, and the game opens
         * with its active display mode: write ours in before it reads them. */
        char value[32], mode[48];
        snprintf(value, sizeof value, "%d", saved_ui);
        text = xml_set(text, "AnyUint32", "s2UIScale", "0xeb61e4f7", value);
        snprintf(value, sizeof value, "%d", saved_fps);
        text = xml_set(text, "AnyUint32", "s2FrameRateLimit", "0xeb61e4f7", value);
        snprintf(value, sizeof value, "%d", saved_render);
        text = xml_set(text, "AnyUint32", "s2RenderScale", "0xeb61e4f7", value);
        snprintf(mode, sizeof mode, "%dx%dx32x60", gw, gh);
        text = xml_set(text, "AnyString", "activeDeviceDisplayMode", "0x0b8bea18", mode);
        FILE *f = fopen(props_path, "wb");
        if (f) { fwrite(text, 1, strlen(text), f); fclose(f); }
        free(text);
    }
    void_log("[settings] screen %dx%d, UI scale %d%% -> game %dx%d, frame rate limit %d, render scale %d%% (render %dx%d, effective UI %.1f%%)",
        phys_w, phys_h, ui_scale, gw, gh, fps_limit, render_scale,
        phys_w * render_scale / 100, phys_h * render_scale / 100,
        (double)phys_w * render_scale / gw);
}

static void changed(const char *key, uint32_t value)
{
    /* A value forced for this run (S2_*) stays, whatever the game loads. */
    if ((!strcmp(key, "s2FrameRateLimit") && getenv("S2_FPS_LIMIT")) ||
        (!strcmp(key, "s2RenderScale") && getenv("S2_RENDER_SCALE")) || (!strcmp(key, "s2UIScale") && getenv("S2_UI_SCALE")))
        return;
    if (!strcmp(key, "s2FrameRateLimit")) {
        fps_limit = value ? clamp((int)value, 15, 240) : 0;
        void_log("[settings] frame rate limit %d", fps_limit);
    } else if (!strcmp(key, "s2RenderScale")) {
        int v = clamp((int)value, 25, 200);
        if (v != render_scale) {
            render_scale = v;
            s2_touch_reset_cas_pan();
            s2_ui_scale_requested(ui_scale);
            void_log("[settings] render scale %d%%", render_scale);
        }
    } else if (!strcmp(key, "s2UIScale")) {
        int v = clamp((int)value, 100, 300);
        if (v != ui_scale_choice) {
            ui_scale_choice = v;
            if (v != ui_scale) {
                ui_scale = v;
                s2_touch_reset_cas_pan();
                s2_ui_scale_requested(v);
            }
        }
    }
}

static int persist_ui_scale(int scale)
{
    struct stat st;
    if (stat(props_path, &st) || st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) return 0;
    size_t bytes = 0;
    char *text = read_text(props_path, &bytes);
    if (!text) return 0;
    if (bytes != (size_t)st.st_size || !strstr(text, "<cGZPropertySetString") ||
        !strstr(text, "</cGZPropertySetString>")) { free(text); return 0; }
    char value[16], temporary[sizeof props_path + 32];
    snprintf(value, sizeof value, "%d", scale);
    text = xml_set(text, "AnyUint32", "s2UIScale", "0xeb61e4f7", value);
    if (xml_value(text, "s2UIScale") != scale) { free(text); return 0; }
    snprintf(temporary, sizeof temporary, "%s.native-ui.tmp", props_path);
    FILE *f = fopen(temporary, "wb");
    int ok = 0;
    if (f) {
        size_t length = strlen(text);
        ok = fwrite(text, 1, length, f) == length && !fflush(f) && !fsync(fileno(f));
        if (fclose(f)) ok = 0;
        if (ok) ok = rename(temporary, props_path) == 0;
        if (!ok) remove(temporary);
    }
    free(text);
    return ok;
}

/* Owner-thread gesture command. The original live property is verified before disk persistence. */
int s2_ui_scale_apply(int scale)
{
    static unsigned diagnostics;
    unsigned log = diagnostics++ < 16;
    const char *stage = "bounds";
    uint32_t slot = 0, set = 0, table = 0, setter = 0, getter = 0, readback = 0, any_table = 0, value = 0;
    int applied = 0, persisted = -1;
    if (scale < 100 || scale > 300 || getenv("S2_UI_SCALE")) goto done;
    stage = "allocation";
    slot = void_galloc(72);
    if (!slot) goto done;
    uint32_t key = slot + 4, chars = slot + 28, any = slot + 60;
    memset(GPTR(slot), 0, 72);
    void_call_guest_this(0xA01CBAF3u, slot, 0, NULL);
    stage = "service";
    set = MEM32(slot);
    if (!set || !void_vm_readable(set, 4)) goto cleanup;
    table = MEM32(set);
    stage = "table";
    if (!table || !void_vm_readable(table, 0x24)) goto cleanup;
    setter = MEM32(table + 0xC);
    getter = MEM32(table + 0x20);
    stage = "methods";
    if ((setter == 0xA01BF515u && getter == 0xA01BF670u) ||
        (table == 0xA0E98C60u && setter == 0xA0C299E2u && getter == 0xA01BF670u)) {
        memcpy(GPTR(chars), "s2UIScale", 10);
        MEM32(key) = chars;
        MEM32(key + 16) = 9;
        MEM32(key + 20) = 31;
        MEM32(any) = 0xA0D9F534u;
        MEM32(any + 4) = (uint32_t)scale;
        uint32_t args[2] = { key, any };
        void_call_guest_this(setter, set, 2, args);
        readback = void_call_guest_this(getter, set, 1, &key);
        stage = "readback";
        if (readback && void_vm_readable(readback, 8)) {
            any_table = MEM32(readback);
            value = MEM32(readback + 4);
            stage = any_table != 0xA0D9F534u ? "valueType" : value != (uint32_t)scale ? "value" : "live";
            applied = any_table == 0xA0D9F534u && value == (uint32_t)scale && s2_ui_scale() == scale;
        }
    }
cleanup:
    if (set) void_call_guest_this(0xA01CBD79u, slot, 0, NULL);
    void_gfree(slot);
    if (applied) {
        stage = "done";
        persisted = persist_ui_scale(scale);
        void_log("[native-ui] scale=%d persisted=%d", scale, persisted);
    }
done:
    if (log) void_log("[native-ui-scale] stage=%s request=%d current=%d set=%08X table=%08X setter=%08X getter=%08X read=%08X any=%08X value=%u applied=%d persisted=%d",
        stage, scale, s2_ui_scale(), set, table, setter, getter, readback, any_table, value, applied, persisted);
    return applied;
}

/* cGZPropertySetString::SetProperty (__thiscall, std::string const &key,
 * cIGZAny const &value, `retn 8`): the property set behind userProps.xml
 * and many others; keys of the port start with "s2". */
void sub_A01BF515_lifted(void);
void sub_A01BF515(void)
{
    uint32_t name = MEM32(g_esp + 4), any = MEM32(g_esp + 8);
    uint32_t length = MEM32(name + 16), capacity = MEM32(name + 20);
    uint32_t chars = capacity > 15 ? MEM32(name) : name;
    char key[40] = "";
    if (length >= 3 && length < sizeof key && MEM8(chars) == 's' && MEM8(chars + 1) == '2') {
        memcpy(key, GPTR(chars), length);
        key[length] = 0;
    }
    sub_A01BF515_lifted();
    if (key[0]) changed(key, MEM32(any + 4));
}

/* The Graphics/Performance Options controller refreshes its own controls
 * (slot 3 of its second interface, 0xA064CB41) without the base class's
 * refresh of the panel's property bindings (0xA0648AAB), which the other
 * option panels run: the port's radio groups showed no selection. */

/* The game copies only the properties it knows from userProps.xml into the
 * live set, so the port's are written into it (through its own SetProperty)
 * before the bindings read them: the panel shows the values in effect. */
static void put_live_settings(void)
{
    static const char *const names[3] = { "s2UIScale", "s2FrameRateLimit", "s2RenderScale" };
    uint32_t values[3] = { (uint32_t)ui_scale_choice, (uint32_t)fps_limit, (uint32_t)render_scale };
    uint32_t slot = void_galloc(4 + 24 + 32 + 8);
    uint32_t key = slot + 4, chars = slot + 28, any = slot + 60;
    MEM32(slot) = 0;
    void_call_guest_this(0xA01CBAF3u, slot, 0, NULL);          /* the user property set (cTSUserProps) */
    uint32_t set = MEM32(slot);
    for (int i = 0; set && i < 3; ++i) {
        uint32_t length = (uint32_t)strlen(names[i]);
        memset(GPTR(key), 0, 24);
        memset(GPTR(chars), 0, 32);
        memcpy(GPTR(chars), names[i], length);
        MEM32(key) = chars;                                    /* std::string, heap form */
        MEM32(key + 16) = length;
        MEM32(key + 20) = 31;
        MEM32(any) = 0xA0D9F534u;                              /* AnyUint32 */
        MEM32(any + 4) = values[i];
        uint32_t args[2] = { key, any };
        void_call_guest_this(MEM32(MEM32(set) + 12), set, 2, args);
    }
    if (set) void_call_guest_this(0xA01CBD79u, slot, 0, NULL);
    void_gfree(slot);
}

void sub_A064CB41_lifted(void);
void sub_A064CB41(void)
{
    uint32_t self = g_ecx;
    sub_A064CB41_lifted();
    uint32_t eax = g_eax;
    put_live_settings();
    void_call_guest_this(0xA0648AABu, self, 0, NULL);
    g_eax = eax;
}

static int options_caption(uint32_t window, const char *caption)
{
    if (!window || !void_vm_readable(window, 0xEC)) return 0;
    uint32_t table = MEM32(window), method;
    if (table == 0xA0E54498u) method = 0xA0939083u;
    else if (table == 0xA0E53EECu) method = 0xA0628F0Eu;
    else return 0;
    if (!void_vm_readable(table, 0x16C) || MEM32(table + 0x168) != method) return 0;
    size_t length = strlen(caption);
    if (length > 159) return 0;
    uint32_t text = void_galloc(28 + (uint32_t)length + 1);
    if (!text) return 0;
    memset(GPTR(text), 0, 28);
    MEM32(text) = 0xA0D9B470u;                 /* original cGZString, heap-form std::string */
    MEM32(text + 4) = text + 28;
    MEM32(text + 20) = (uint32_t)length;
    MEM32(text + 24) = length > 15 ? (uint32_t)length : 16;
    memcpy(GPTR(text + 28), caption, length + 1);
    void_call_guest_this(method, window, 1, &text);
    void_gfree(text);
    return 1;
}

static void options_display_rates(uint32_t group)
{
    if (!group || !void_vm_readable(group, 0xEC) || MEM32(group) != 0xA0E54498u ||
        MEM32(group + 0x60) != 0xA0D9B470u) return;
    uint32_t string = group + 0x64, length = MEM32(string + 16), capacity = MEM32(string + 20);
    uint32_t chars = capacity > 15 ? MEM32(string) : string;
    const char prefix[] = "radio:s2FrameRateLimit:int:";
    if (length < sizeof prefix - 1 || length > 159 || !void_vm_readable(chars, length + 1) ||
        MEM8(chars + length) || memcmp(GPTR(chars), prefix, sizeof prefix - 1)) return;
    uint32_t buttons[10];
    for (uint32_t id = 1; id <= 10; ++id) {
        buttons[id - 1] = void_call_guest_this(0xA09314F0u, group, 1, &id);
        if (!buttons[id - 1] || !void_vm_readable(buttons[id - 1], 0xEC) || MEM32(buttons[id - 1]) != 0xA0E53EECu ||
            MEM32(0xA0E53EECu + 0x14C) != 0xA093621Du) return;
    }
    char caption[160];
    size_t used = (size_t)snprintf(caption, sizeof caption, "%s", prefix);
    for (int i = 0; i < display_rate_count; ++i)
        used += (size_t)snprintf(caption + used, sizeof caption - used, "%s%d", i ? ":" : "", display_rates[i]);
    if (!options_caption(group, caption)) return;
    for (int i = 0; i < display_rate_count; ++i) {
        if (!display_rates[i]) continue;       /* original STR# localization supplies the Off label */
        char label[16];
        snprintf(label, sizeof label, "%d", display_rates[i]);
        if (!options_caption(buttons[i], label)) void_log("[settings] cannot label FPS button %d", i);
    }
    for (int i = 0; i < 10; ++i) {
        uint32_t flags[2] = { 1, i < display_rate_count };
        void_call_guest_this(0xA093621Du, buttons[i], 2, flags);
    }
    void_log("[settings] FPS options %s", caption);
}

void sub_A0649404_lifted(void);
void sub_A0649404(void)
{
    void_regs saved;
    void_regs_save(&saved);
    options_display_rates(MEM32(g_esp + 4));
    void_regs_load(&saved);
    sub_A0649404_lifted();
}
