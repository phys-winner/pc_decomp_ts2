/* Read-only UI picks and explicit native commands on the game's pump thread. */
#include "host.h"
#include "input.h"
#include "sims2_settings.h"
#include "sims2_extender.h"
#include "sims2_cheats.h"
#include <time.h>
#include <stdatomic.h>
#include <math.h>

enum { TOUCH_DRAG_FILTER = 1 << 17, TOUCH_DRAG_STEP_SHIFT = 18 };

static uint32_t win_manager;
static uint32_t win_manager_thread;
static uint32_t scene_owner, scene_window;
static pthread_mutex_t pick_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pick_ready = PTHREAD_COND_INITIALIZER;
static int pick_x, pick_y, pick_pending, pick_diagnostic, pick_result;
static int pick_radius;
static uint64_t pick_point;
static uint64_t pick_world_key;
static s2_world_targets pick_world;
static unsigned pick_serial, pick_done;
static unsigned query_zero_logs, slider_reject_logs, pick_owner_context;
static unsigned target_logs;
static unsigned ui_target_logs, fling_logs;
static uint32_t pick_owner_root, pick_owner_hit;
enum { SLIDER_IDLE, SLIDER_QUEUED, SLIDER_STARTED, SLIDER_DONE, SLIDER_CANCELLED };
static unsigned slider_serial, slider_state;
static int slider_delta, slider_result, slider_before, slider_after;
static struct timespec slider_expiry;
static struct {
    uint64_t tool, size;
    unsigned context, epoch;
} rotate_request;
static uint32_t tool_handler;
static unsigned last_world_context;
static uint32_t town_camera, lot_camera, last_clamp_camera, last_clamp_vtable;
static uint32_t camera_view, camera_view_owner;
static _Atomic uint64_t shown_tool;
static _Atomic unsigned shown_fill_kind;
static _Atomic int shown_cost = -1;
static _Atomic uint32_t shown_object, preview_object;
static uint32_t shown_pointer, shown_vtable, tool_generation;
static _Atomic unsigned ui_command_kind, ui_command_started, camera_contact, camera_navigation_intent;
static _Atomic unsigned camera_hover_enabled = 1;
static _Atomic uint64_t camera_hover_queries;
static uint32_t ui_command_value, cas_editor_window;
static struct timespec ui_command_expiry;
static _Atomic unsigned cas_pan_epoch;
static _Atomic uint64_t shown_swipe;
static _Atomic unsigned shown_swipe_context, shown_swipe_epoch;
static _Atomic unsigned swipe_version;
static unsigned swipe_sequence;
static _Atomic int floor_button_used;
static unsigned cas_pan_seen_epoch;
static float cas_pan_y;
static int cas_pan_selected;
static struct { float dx, dy, radians, zoom, pitch; unsigned epoch, context; uint64_t size; int absolute; float pose[6]; } camera_request;
static struct { int x, y, amount, info; unsigned context, epoch; uint64_t size, owner; } catalog_request;
static const char *touch_type(uint32_t object);

static void ui_command_expire(void)
{
    struct timespec now;
    if (ui_command_kind && (clock_gettime(CLOCK_MONOTONIC, &now) ||
        (now.tv_sec > ui_command_expiry.tv_sec ||
         (now.tv_sec == ui_command_expiry.tv_sec && now.tv_nsec >= ui_command_expiry.tv_nsec)))) {
        ui_command_kind = 0;
        pick_pending = 0;
        if (!++pick_serial) ++pick_serial;
    }
}

void s2_touch_reset_cas_pan(void)
{
    s2_touch_cancel_text();
    atomic_fetch_add(&cas_pan_epoch, 1);
    pthread_mutex_lock(&pick_lock);
    if (ui_command_kind == S2_UI_CAS_PAN) {
        ui_command_kind = 0;
        pick_pending = 0;
        if (!++pick_serial) ++pick_serial;
    }
    pthread_mutex_unlock(&pick_lock);
}

unsigned s2_touch_layout_epoch(void) { return atomic_load(&cas_pan_epoch); }
uint64_t s2_touch_swipe_feedback(void)
{
    unsigned version = atomic_load(&swipe_version);
    if (version & 1) return 0;
    uint64_t receipt = atomic_load(&shown_swipe);
    return (atomic_load(&shown_swipe_context) & ~S2_TOUCH_FOCUSED_TEXT) == (s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT) &&
        atomic_load(&shown_swipe_epoch) == s2_touch_layout_epoch() && atomic_load(&swipe_version) == version ? receipt : 0;
}

void s2_touch_camera_contact(int active) { atomic_store(&camera_contact, active != 0); }
void s2_touch_camera_pointer_intent(void) { atomic_store(&camera_navigation_intent, 0); }

void s2_touch_note_floor_button(void) { atomic_store(&floor_button_used, 1); }
int s2_touch_floor_button_used(void) { return atomic_load(&floor_button_used); }

void s2_touch_camera_hover_enable(int enabled)
{
    atomic_store(&camera_hover_enabled, enabled != 0);
    void_log("[camera-hover] %s queries %llu contact %u pending %u started %u intent %u", enabled ? "on" : "off",
        (unsigned long long)atomic_load(&camera_hover_queries), atomic_load(&camera_contact),
        atomic_load(&ui_command_kind), atomic_load(&ui_command_started), atomic_load(&camera_navigation_intent));
}

int s2_touch_camera_hover_active(void)
{
    if (!atomic_load(&camera_hover_enabled)) return 0;
    int active = atomic_load(&camera_contact) || atomic_load(&ui_command_kind) == S2_UI_CAMERA ||
        atomic_load(&ui_command_started) == S2_UI_CAMERA || atomic_load(&camera_navigation_intent);
    if (active) atomic_fetch_add(&camera_hover_queries, 1);
    return active;
}

static int touch_layout_matches(unsigned context, uint64_t size, unsigned epoch)
{
    int w, h;
    void_input_screen_size(&w, &h);
    return w > 0 && h > 0 && size == ((uint64_t)(uint32_t)h << 32 | (uint32_t)w) &&
        s2_touch_context() == context && s2_touch_layout_epoch() == epoch;
}

static void cas_pan_sync(void)
{
    unsigned epoch = atomic_load(&cas_pan_epoch);
    if (cas_pan_seen_epoch != epoch) {
        cas_pan_seen_epoch = epoch;
        cas_editor_window = 0;
        cas_pan_y = 0;
        cas_pan_selected = 0;
    }
}

static int cas_editor_ready(uint32_t editor, int idle, int *height)
{
    if ((s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT) != S2_TOUCH_CAS ||
        !win_manager || win_manager_thread != void_thread_id() || !void_vm_readable(win_manager, 0x48) ||
        !editor || !void_vm_readable(editor, 0xEC) || MEM32(editor) != 0xA0DDB6E8u ||
        strcmp(touch_type(editor), ".?AVcTSUISimEditor@@") || !(MEM32(editor + 0xE8) & 1)) return 0;
    uint32_t root = MEM32(win_manager + 0x44);
    if (!root || !void_vm_readable(root, 0xEC) || MEM32(editor + 0x58) != root ||
        MEM32(root + 0x58) || MEM32(root + 0x24) || MEM32(root + 0x28) ||
        MEM32(root + 0x2C) < 800 || MEM32(root + 0x2C) > 8192 ||
        !MEM32(root + 0x30) || MEM32(root + 0x30) > 8192 ||
        MEM32(editor + 0xC8) || MEM32(editor + 0xD0) != 800 ||
        (int)MEM32(editor + 0xCC) < -600 || (int)MEM32(editor + 0xCC) > 8192 ||
        (int)MEM32(editor + 0xD4) != (int)MEM32(editor + 0xCC) + 600) return 0;
    *height = (int)MEM32(root + 0x30);
    return !idle || (!MEM32(win_manager + 0x34) &&
        !(void_input_key_state(1) & 0x8000) && !(void_input_key_state(2) & 0x8000) &&
        !(void_input_key_state(4) & 0x8000));
}

static void cas_pan_move(uint32_t editor, int height)
{
    if (height >= 600) return;
    if (!cas_pan_selected) cas_pan_y = (float)(height - 600);
    if (cas_pan_y < height - 600) cas_pan_y = (float)(height - 600);
    if (cas_pan_y > 0) cas_pan_y = 0;
    int y = (int)(cas_pan_y - 0.5f);
    if ((int)MEM32(editor + 0xC8) || (int)MEM32(editor + 0xCC) != y) {
        uint32_t args[2] = { 0, (uint32_t)y };
        void_call_guest_this(0xA0930F79u, editor, 2, args);
    }
}

void sub_A05E01E5_lifted(void);
void sub_A05E01E5(void)
{
    uint32_t self = g_ecx;
    sub_A05E01E5_lifted();
    void_regs saved;
    void_regs_save(&saved);
    cas_pan_sync();
    int height;
    /* Keep the chosen native geometry during drags; only new pan commands need idle input. */
    if (cas_editor_ready(self, 0, &height)) {
        if (cas_editor_window && cas_editor_window != self) { cas_pan_y = 0; cas_pan_selected = 0; }
        cas_editor_window = self;
        cas_pan_move(self, height);
    }
    void_regs_load(&saved);
}

int s2_touch_ui_command(unsigned kind, uint32_t value)
{
    struct timespec expiry;
    if (clock_gettime(CLOCK_MONOTONIC, &expiry)) return 0;
    /* Settings and cancellation tolerate a long game frame; their owner snapshots reject stale UI. */
    if (kind == S2_UI_SCALE || kind == S2_UI_CLEAR_CATALOG) expiry.tv_sec += 2;
    else expiry.tv_nsec += 500000000;
    if (expiry.tv_nsec >= 1000000000) { ++expiry.tv_sec; expiry.tv_nsec -= 1000000000; }
    float delta = 0;
    if (kind == S2_UI_SCALE) {
        if (value < 100 || value > 300) return 0;
    } else if (kind == S2_UI_CAS_PAN) {
        memcpy(&delta, &value, 4);
        if (!isfinite(delta) || delta < -1 || delta > 1 || !delta ||
            (s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT) != S2_TOUCH_CAS) return 0;
    } else if (kind == S2_UI_CLEAR_CATALOG) {
        value = s2_catalog_clear_request();
        if (!value) return 0;
    } else if (kind != S2_UI_TEXT_READ && kind != S2_UI_TEXT_WRITE && kind != S2_UI_CHEAT) return 0;
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    if (slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED || ui_command_started ||
        ui_command_kind >= S2_UI_TEXT_READ ||
        (kind >= S2_UI_TEXT_READ && ui_command_kind) ||
        (kind == S2_UI_CAS_PAN && ui_command_kind == S2_UI_SCALE)) {
        pthread_mutex_unlock(&pick_lock);
        return 0;
    }
    if (kind == S2_UI_CAS_PAN && ui_command_kind == S2_UI_CAS_PAN) {
        float previous;
        memcpy(&previous, &ui_command_value, 4);
        delta += previous;
        if (delta < -1) delta = -1;
        if (delta > 1) delta = 1;
        memcpy(&value, &delta, 4);
    }
    unsigned serial = pick_serial + 1;
    if (!serial) ++serial;
    int accepted = void_input_touch_barrier(serial);
    if (accepted) {
        pick_serial = serial;
        pick_pending = 1;
        pick_diagnostic = 0;
        ui_command_kind = kind;
        ui_command_value = value;
        ui_command_expiry = expiry;
    }
    pthread_mutex_unlock(&pick_lock);
    return accepted;
}

static int touch_camera_queue(float dx, float dy, float radians, float zoom, float pitch)
{
    unsigned context = s2_touch_context();
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(radians) || !isfinite(zoom) || !isfinite(pitch) || __builtin_fabsf(zoom) > 1 ||
        __builtin_fabsf(pitch) > 1.570797f ||
        __builtin_fabsf(dx) > 1 || __builtin_fabsf(dy) > 1 || __builtin_fabsf(radians) > 3.141593f ||
        (!dx && !dy && !radians && !zoom && !pitch) || (context != S2_TOUCH_TOWN && context != S2_TOUCH_LIVE &&
        context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD)) return 0;
    struct timespec expiry;
    if (clock_gettime(CLOCK_MONOTONIC, &expiry)) return 0;
    expiry.tv_nsec += 500000000;
    if (expiry.tv_nsec >= 1000000000) { ++expiry.tv_sec; expiry.tv_nsec -= 1000000000; }
    int w, h;
    void_input_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return 0;
    unsigned epoch = s2_touch_layout_epoch();
    uint64_t size = (uint64_t)(uint32_t)h << 32 | (uint32_t)w;
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    int accepted = 0;
    /* The running camera owns a copy; the pending slot retains the next MOVE. */
    if ((!ui_command_started || ui_command_started == S2_UI_CAMERA) && slider_state != SLIDER_QUEUED && slider_state != SLIDER_STARTED &&
        (!pick_pending || ui_command_kind == S2_UI_CAMERA)) {
        unsigned serial = pick_serial + 1;
        if (!serial) ++serial;
        accepted = void_input_touch_barrier(serial);
        if (accepted) {
            if (ui_command_kind != S2_UI_CAMERA || camera_request.epoch != epoch || camera_request.size != size || camera_request.context != context ||
                camera_request.absolute)
                memset(&camera_request, 0, sizeof camera_request);
            camera_request.dx = __builtin_fmaxf(-1, __builtin_fminf(1, camera_request.dx + dx));
            camera_request.dy = __builtin_fmaxf(-1, __builtin_fminf(1, camera_request.dy + dy));
            camera_request.radians = __builtin_fmaxf(-3.141593f, __builtin_fminf(3.141593f, camera_request.radians + radians));
            camera_request.zoom = __builtin_fmaxf(-1, __builtin_fminf(1, camera_request.zoom + zoom));
            camera_request.pitch = __builtin_fmaxf(-1.570796327f, __builtin_fminf(1.570796327f, camera_request.pitch + pitch));
            camera_request.epoch = epoch; camera_request.size = size;
            camera_request.context = context;
            pick_serial = serial; pick_pending = 1; pick_diagnostic = 0;
            ui_command_kind = S2_UI_CAMERA; ui_command_value = 0; ui_command_expiry = expiry;
            /* Camera-generated hover records can outlive the finger and the owner command. */
            atomic_store(&camera_navigation_intent, 1);
        }
    }
    pthread_mutex_unlock(&pick_lock);
    return accepted;
}

int s2_touch_camera(float dx, float dy, float radians, float zoom, float pitch)
{
    return touch_camera_queue(dx, dy, radians, zoom, pitch);
}

int s2_touch_pan(float from_x, float from_y, float to_x, float to_y)
{
    if (!isfinite(from_x) || !isfinite(from_y) || !isfinite(to_x) || !isfinite(to_y) ||
        from_x < 0 || from_x > 1 || from_y < 0 || from_y > 1 || to_x < 0 || to_x > 1 || to_y < 0 || to_y > 1) return 0;
    return touch_camera_queue(to_x - from_x, to_y - from_y, 0, 0, 0);
}

/* Benchmarks (script camset): put the town or lot camera at an absolute pose,
 * target x y z, yaw, distance and pitch as camget logs them, through the same
 * queue and owner checks as a touch gesture. */
int s2_touch_camera_pose(const float pose[6])
{
    unsigned context = s2_touch_context();
    for (int i = 0; i < 6; ++i) if (!isfinite(pose[i])) return 0;
    if (pose[4] <= 0 || pose[4] > 10000 || (context != S2_TOUCH_TOWN && context != S2_TOUCH_LIVE &&
        context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD)) return 0;
    struct timespec expiry;
    if (clock_gettime(CLOCK_MONOTONIC, &expiry)) return 0;
    expiry.tv_sec += 2;
    int w, h;
    void_input_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return 0;
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    int accepted = 0;
    if (!ui_command_started && slider_state != SLIDER_QUEUED && slider_state != SLIDER_STARTED && !pick_pending) {
        unsigned serial = pick_serial + 1;
        if (!serial) ++serial;
        accepted = void_input_touch_barrier(serial);
        if (accepted) {
            memset(&camera_request, 0, sizeof camera_request);
            camera_request.absolute = 1;
            memcpy(camera_request.pose, pose, sizeof camera_request.pose);
            camera_request.epoch = s2_touch_layout_epoch();
            camera_request.size = (uint64_t)(uint32_t)h << 32 | (uint32_t)w;
            camera_request.context = context;
            pick_serial = serial; pick_pending = 1; pick_diagnostic = 0;
            ui_command_kind = S2_UI_CAMERA; ui_command_value = 0; ui_command_expiry = expiry;
            atomic_store(&camera_navigation_intent, 1);
        }
    }
    pthread_mutex_unlock(&pick_lock);
    return accepted;
}

static int touch_catalog_queue(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch, int info, uint64_t owner)
{
    if (info < 0 || info > 3 || amount < -8 || amount > (info == 2 ? 9 : 8) || !touch_layout_matches(context, size, epoch) ||
        (info == 3 && (!owner || !context || (context & S2_TOUCH_LOADING))) ||
        (info != 3 && context != S2_TOUCH_CAS && !(context == (S2_TOUCH_CAS | S2_TOUCH_FOCUSED_TEXT) && !info) &&
        context != S2_TOUCH_LIVE && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD && context != S2_TOUCH_TOWN &&
        !(context == S2_TOUCH_CHOOSER && !info)) || (info == 2 && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) ||
        x < 0 || y < 0 || (unsigned)x >= (uint32_t)size || (unsigned)y >= (uint32_t)(size >> 32)) return 0;
    struct timespec expiry;
    if (clock_gettime(CLOCK_MONOTONIC, &expiry)) return 0;
    expiry.tv_nsec += 500000000;
    if (expiry.tv_nsec >= 1000000000) { ++expiry.tv_sec; expiry.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    int accepted = 0;
    if (!ui_command_started && slider_state != SLIDER_QUEUED && slider_state != SLIDER_STARTED &&
        (!pick_pending || (ui_command_kind == S2_UI_CATALOG && (info == 0 || info == 3) && catalog_request.info == info &&
        catalog_request.owner == owner &&
        catalog_request.context == context && catalog_request.size == size && catalog_request.epoch == epoch &&
        catalog_request.x == x && catalog_request.y == y))) {
        unsigned serial = pick_serial + 1;
        if (!serial) ++serial;
        accepted = void_input_touch_barrier(serial);
        if (accepted) {
            if ((info == 0 || info == 3) && ui_command_kind == S2_UI_CATALOG) amount += catalog_request.amount;
            if (info != 2 && amount < -8) amount = -8;
            if (info != 2 && amount > 8) amount = 8;
            catalog_request.x = x; catalog_request.y = y; catalog_request.amount = amount;
            catalog_request.info = info;
            catalog_request.context = context; catalog_request.size = size; catalog_request.epoch = epoch;
            catalog_request.owner = owner;
            pick_serial = serial; pick_pending = 1; pick_diagnostic = 0;
            ui_command_kind = S2_UI_CATALOG; ui_command_value = 0; ui_command_expiry = expiry;
        }
    }
    pthread_mutex_unlock(&pick_lock);
    return accepted;
}

int s2_touch_scroll(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch)
{
    return amount && touch_catalog_queue(x, y, amount, context, size, epoch, 0, 0);
}

int s2_touch_fling(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch, uint64_t owner)
{
    return amount && touch_catalog_queue(x, y, amount, context, size, epoch, 3, owner);
}

uint64_t s2_touch_scroll_owner(void)
{
    pthread_mutex_lock(&pick_lock);
    uint64_t owner = pick_done == pick_serial ? (uint64_t)pick_owner_root << 32 | pick_owner_hit : 0;
    pthread_mutex_unlock(&pick_lock);
    return owner;
}

void s2_touch_cancel_fling(void)
{
    pthread_mutex_lock(&pick_lock);
    if (ui_command_kind == S2_UI_CATALOG && catalog_request.info == 3) {
        ui_command_kind = 0; pick_pending = 0;
        if (!++pick_serial) ++pick_serial;
        pthread_cond_broadcast(&pick_ready);
    }
    pthread_mutex_unlock(&pick_lock);
}

int s2_touch_catalog_info(int x, int y, unsigned context, uint64_t size, unsigned epoch)
{
    return touch_catalog_queue(x, y, 0, context, size, epoch, 1, 0);
}

int s2_touch_tool_menu(int action, unsigned context, uint64_t size, unsigned epoch)
{
    return action >= 0 && action < 9 && touch_catalog_queue(0, 0, action + 1, context, size, epoch, 2, 0);
}

static void touch_camera_capture(uint32_t self)
{
    if (!self || !void_vm_readable(self, 0x458)) return;
    if (self != last_clamp_camera || MEM32(self) != last_clamp_vtable) {
        last_clamp_camera = self; last_clamp_vtable = MEM32(self);
        const char *type = touch_type(self);
        if (!strcmp(type, ".?AVcCameraNeighborhood@nTSSG@@")) town_camera = self;
        if (!strcmp(type, ".?AVcCameraLive@nTSSG@@") || !strcmp(type, ".?AVcCameraBuild@nTSSG@@")) lot_camera = self;
    }
}

void sub_A0B46E90_lifted(void);
void sub_A0B46E90(void)
{
    uint32_t self = g_ecx;
    sub_A0B46E90_lifted();
    touch_camera_capture(self);
}

/* A window resize may detach the scene without reconstructing its camera. */
void sub_A0B4C1B0_lifted(void);
void sub_A0B4C1B0(void)
{
    uint32_t self = g_ecx;
    uint32_t view = void_vm_readable(g_esp + 8, 4) ? MEM32(g_esp + 8) : 0;
    sub_A0B4C1B0_lifted();
    if (self >= 0x454) {
        touch_camera_capture(self - 0x454);
        camera_view_owner = self - 0x454;
        camera_view = view;
    }
}

/* Capture cameras at construction: initial touch must not wait for a zoom/clamp call. */
void sub_A03582A0_lifted(void);
void sub_A03582A0(void) { sub_A03582A0_lifted(); lot_camera = g_eax; }
void sub_A035A1B0_lifted(void);
void sub_A035A1B0(void) { sub_A035A1B0_lifted(); lot_camera = g_eax; }
void sub_A035AB10_lifted(void);
void sub_A035AB10(void) { sub_A035AB10_lifted(); town_camera = g_eax; }

void s2_touch_reset_camera(void)
{
    s2_touch_camera_pointer_intent();
    town_camera = lot_camera = last_clamp_camera = last_clamp_vtable = 0;
    camera_view = camera_view_owner = 0;
}

uint64_t s2_touch_tool_token(void) { return atomic_load(&shown_tool); }
int s2_touch_tool(void) { return (int)(uint32_t)s2_touch_tool_token(); }
int s2_touch_fill_kind(void) { return (int)atomic_load(&shown_fill_kind); }
int s2_touch_object_cost(void) { return atomic_load(&shown_cost); }

static void touch_tool_clear(void)
{
    shown_pointer = shown_vtable = 0;
    atomic_store(&shown_tool, 0);
    atomic_store(&shown_fill_kind, 0);
    atomic_store(&shown_cost, -1);
    atomic_store(&shown_object, 0);atomic_store(&preview_object, 0);
}

void sub_A076C5EB_lifted(void);
void sub_A076C5EB(void)
{
    uint32_t self = g_ecx;
    sub_A076C5EB_lifted();
    if (g_eax & 0xFF) { tool_handler = self; touch_tool_clear(); }
}

void sub_A076CABE_lifted(void);
void sub_A076CABE(void)
{
    if (tool_handler == g_ecx) { tool_handler = 0; touch_tool_clear(); }
    sub_A076CABE_lifted();
}

/* MSVC CompleteObjectLocator names describe real widget/proc types. */
static const char *touch_type(uint32_t object)
{
    if (!object || !void_vm_readable(object, 4)) return "";
    uint32_t vtable = MEM32(object);
    if (vtable < 4 || !void_vm_readable(vtable - 4, 4)) return "";
    uint32_t locator = MEM32(vtable - 4);
    if (!locator || !void_vm_readable(locator, 16)) return "";
    uint32_t descriptor = MEM32(locator + 12);
    if (descriptor > UINT32_MAX - 8 || !void_vm_readable(descriptor + 8, 128)) return "";
    const char *name = (const char *)GPTR(descriptor + 8);
    return memchr(name, 0, 128) ? name : "";
}

static float touch_float(uint32_t address)
{
    float value;
    memcpy(&value, GPTR(address), sizeof value);
    return value;
}

int s2_touch_camera_projection(float projection[3])
{
    unsigned context=s2_touch_context();uint32_t camera=context==S2_TOUCH_TOWN?town_camera:lot_camera;
    if ((context!=S2_TOUCH_TOWN&&context!=S2_TOUCH_LIVE&&context!=S2_TOUCH_BUY&&context!=S2_TOUCH_BUILD) ||
        !camera||!void_vm_readable(camera,0x458))return 0;
    float near=touch_float(camera+0xFC),far=touch_float(camera+0x118),fov=touch_float(camera+0x134);
    if(!isfinite(near)||!isfinite(far)||!isfinite(fov)||near<=0||far<=near||fov<=0||fov>=3.141593f)return 0;
    projection[0]=near;projection[1]=far;projection[2]=fov;return 1;
}

static int touch_camera_basis(uint32_t camera, float distance, float eye[3], float matrix[9])
{
    uint32_t scratch = g_esp - 96;
    if (!void_vm_writable(scratch, 96)) return 0;
    uint32_t distance_bits;
    memcpy(&distance_bits, &distance, 4);
    uint32_t args[4] = { scratch, scratch + 16, distance_bits, 1 };
    g_esp = scratch;
    /* The native camera owns zoom-preset tilt, orbit offsets and the extra rotation. */
    void_call_guest_this(0xA0B4CEC0u, camera, 4, args);
    uint32_t matrix_args[2] = { scratch + 32, scratch + 16 };
    void_call_guest(0xA01A77F0u, 2, matrix_args, 0);
    g_esp = scratch + 96;
    memcpy(eye, GPTR(scratch), 3 * sizeof(float));
    memcpy(matrix, GPTR(scratch + 32), 9 * sizeof(float));
    for (unsigned n = 0; n < 3; ++n) if (!isfinite(eye[n])) return 0;
    for (unsigned n = 0; n < 9; ++n) if (!isfinite(matrix[n])) return 0;
    return 1;
}

static int touch_camera_translation(uint32_t camera, float distance, float dx, float dy, uint64_t size, float movement[2])
{
    float eye[3], matrix[9];
    if (!touch_camera_basis(camera, distance, eye, matrix)) return 0;
    float span = 2 * distance * tanf(touch_float(camera + 0x134) * .5f);
    float aspect = (float)(uint32_t)size / (float)(uint32_t)(size >> 32);
    /* Use the ground-plane derivative at the center of the view. A ray through the finger changes scale with position and terrain. */
    float down = matrix[7] < -.20f ? matrix[7] : -.20f;
    for (unsigned k = 0; k < 2; ++k) {
        float forward = matrix[k * 3 + 1];
        float right = matrix[k * 3] - forward * matrix[6] / down;
        float up = matrix[k * 3 + 2] - forward * matrix[8] / down;
        movement[k] = span * (-dx * right + dy / aspect * up);
        if (!isfinite(movement[k])) return 0;
    }
    return 1;
}

static int touch_camera_apply(float dx, float dy, float radians, float zoom, float pitch, unsigned epoch, uint64_t size)
{
    unsigned context = s2_touch_context();
    int town = context == S2_TOUCH_TOWN;
    if (!town && context != S2_TOUCH_LIVE && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) return 0;
    s2_extender_camera_sensitivity(&radians, &pitch);
    uint32_t camera = town ? town_camera : lot_camera;
    if (!touch_layout_matches(context, size, epoch) || !camera || !void_vm_writable(camera, 0x458)) return 0;
    uint32_t table = MEM32(camera), setter;
    const char *type = touch_type(camera);
    if (town && table == 0xA0DBAABCu && !strcmp(type, ".?AVcCameraNeighborhood@nTSSG@@")) setter = 0xA035B8C0u;
    else if (!town && table == 0xA0DBA6FCu && !strcmp(type, ".?AVcCameraLive@nTSSG@@")) setter = 0xA0359AC0u;
    else if (!town && table == 0xA0DBA8F4u && !strcmp(type, ".?AVcCameraBuild@nTSSG@@")) setter = 0xA035A850u;
    else return 0;
    float angle = touch_float(camera + 196), distance = touch_float(camera + 224);
    float x = touch_float(camera + 364), y = touch_float(camera + 368), z = touch_float(camera + 372);
    if (!isfinite(angle) || __builtin_fabsf(angle) > 100 || !isfinite(distance) || distance <= 0 || distance > 10000 ||
        !isfinite(x) || !isfinite(y) || !isfinite(z)) return 0;
    if (zoom) {
        uint32_t levels = MEM32(camera + 0x70), end = MEM32(camera + 0x74);
        if (end <= levels || end - levels > 256 || (end - levels) % 4 || !void_vm_readable(levels, end - levels)) return 0;
        float maximum = touch_float(levels), minimum = touch_float(end - 4);
        if (!isfinite(minimum) || !isfinite(maximum) || minimum <= 0 || maximum < minimum) return 0;
        distance = __builtin_fmaxf(minimum, __builtin_fminf(maximum, distance * expf(-zoom)));
    }
    angle += radians;
    while (angle < 0) angle += 6.283185307f;
    while (angle >= 6.283185307f) angle -= 6.283185307f;
    float fov = touch_float(camera + 0x134);
    if (!isfinite(fov) || fov <= 0 || fov >= 3.141593f) return 0;
    float tilt=touch_float(camera+0x154);
    if(pitch&&(!isfinite(tilt)||tilt<0||tilt>3.141593f))return 0;
    if (dx || dy) {
        float movement[2];
        if (!touch_camera_translation(camera, distance, dx, dy, size, movement) ||
            !touch_layout_matches(context, size, epoch) || camera != (town ? town_camera : lot_camera) ||
            !void_vm_writable(camera, 0x458) || MEM32(camera) != table) return 0;
        float position[3] = { x + movement[0], y + movement[1], z };
        uint32_t scratch = g_esp - 16;
        if (!void_vm_writable(scratch, 12)) return 0;
        memcpy(GPTR(scratch), position, 12);
        MEM8(camera + 388) = 0;
        g_esp = scratch;
        void_call_guest_this(setter, camera, 1, &scratch);
        g_esp += 16;
    }
    memcpy(GPTR(camera + 196), &angle, 4);
    memcpy(GPTR(camera + 200), &angle, 4);
    MEM8(camera + 204) = 1;
    if (radians) MEM8(camera + 0x201) = 1;
    if (pitch) {
        tilt=__builtin_fmaxf(.017453293f,__builtin_fminf(1.553343034f,tilt+pitch));
        memcpy(GPTR(camera+0x150),&tilt,4);memcpy(GPTR(camera+0x154),&tilt,4);
        MEM8(camera+0x158)=1;
    }
    memset(GPTR(camera + 424), 0, 12);
    MEM32(camera + 508) = 0;
    if (zoom) {
        MEM8(camera + 0x200) = 1; /* Use continuous distance instead of the preset zoom index. */
        memcpy(GPTR(camera + 0x6C), &distance, 4);
        memcpy(GPTR(camera + 0xE0), &distance, 4);
        memcpy(GPTR(camera + 0xE4), &distance, 4);
        MEM8(camera + 0xE8) = 1;
    }
    return 1;
}


/* The absolute form of touch_camera_apply (script camset): the same camera
 * checks, setter and fields, with the pose's position, yaw, distance (clamped
 * to the camera's zoom range) and pitch (clamped as the tilt gesture is). */
static int touch_camera_pose_apply(const float *pose, unsigned epoch, uint64_t size)
{
    unsigned context = s2_touch_context();
    int town = context == S2_TOUCH_TOWN;
    if (!town && context != S2_TOUCH_LIVE && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) return 0;
    uint32_t camera = town ? town_camera : lot_camera;
    if (!touch_layout_matches(context, size, epoch) || !camera || !void_vm_writable(camera, 0x458)) return 0;
    uint32_t table = MEM32(camera), setter;
    const char *type = touch_type(camera);
    if (town && table == 0xA0DBAABCu && !strcmp(type, ".?AVcCameraNeighborhood@nTSSG@@")) setter = 0xA035B8C0u;
    else if (!town && table == 0xA0DBA6FCu && !strcmp(type, ".?AVcCameraLive@nTSSG@@")) setter = 0xA0359AC0u;
    else if (!town && table == 0xA0DBA8F4u && !strcmp(type, ".?AVcCameraBuild@nTSSG@@")) setter = 0xA035A850u;
    else return 0;
    uint32_t levels = MEM32(camera + 0x70), end = MEM32(camera + 0x74);
    if (end <= levels || end - levels > 256 || (end - levels) % 4 || !void_vm_readable(levels, end - levels)) return 0;
    float maximum = touch_float(levels), minimum = touch_float(end - 4);
    if (!isfinite(minimum) || !isfinite(maximum) || minimum <= 0 || maximum < minimum) return 0;
    float distance = __builtin_fmaxf(minimum, __builtin_fminf(maximum, pose[4]));
    float angle = pose[3];
    while (angle < 0) angle += 6.283185307f;
    while (angle >= 6.283185307f) angle -= 6.283185307f;
    float tilt = __builtin_fmaxf(.017453293f, __builtin_fminf(1.553343034f, pose[5]));
    uint32_t scratch = g_esp - 16;
    if (!void_vm_writable(scratch, 12)) return 0;
    memcpy(GPTR(scratch), pose, 12);
    MEM8(camera + 388) = 0;
    g_esp = scratch;
    void_call_guest_this(setter, camera, 1, &scratch);
    g_esp += 16;
    memcpy(GPTR(camera + 196), &angle, 4);
    memcpy(GPTR(camera + 200), &angle, 4);
    MEM8(camera + 204) = 1;
    MEM8(camera + 0x201) = 1;
    memcpy(GPTR(camera + 0x150), &tilt, 4);
    memcpy(GPTR(camera + 0x154), &tilt, 4);
    MEM8(camera + 0x158) = 1;
    memset(GPTR(camera + 424), 0, 12);
    MEM32(camera + 508) = 0;
    MEM8(camera + 0x200) = 1;
    memcpy(GPTR(camera + 0x6C), &distance, 4);
    memcpy(GPTR(camera + 0xE0), &distance, 4);
    memcpy(GPTR(camera + 0xE4), &distance, 4);
    MEM8(camera + 0xE8) = 1;
    return 1;
}

/* Benchmarks (script camget): the current camera's pose in camset's order. */
void s2_touch_camera_log(void)
{
    void_machine_enter();
    unsigned context = s2_touch_context();
    uint32_t camera = context == S2_TOUCH_TOWN ? town_camera :
        context & (S2_TOUCH_LIVE | S2_TOUCH_BUY | S2_TOUCH_BUILD) ? lot_camera : 0;
    if (camera && void_vm_readable(camera, 0x458))
        void_log("[camera] context %X camset %.4f %.4f %.4f %.5f %.4f %.5f", context, (double)touch_float(camera + 364),
            (double)touch_float(camera + 368), (double)touch_float(camera + 372), (double)touch_float(camera + 196),
            (double)touch_float(camera + 224), (double)touch_float(camera + 0x154));
    else
        void_log("[camera] context %X: no town or lot camera", context);
    void_machine_leave();
}

static void touch_log_camera_value(uint32_t camera)
{
    const char *type = touch_type(camera);
    if (strcmp(type, ".?AVcCameraNeighborhood@nTSSG@@") && strcmp(type, ".?AVcCameraLive@nTSSG@@") &&
        strcmp(type, ".?AVcCameraBuild@nTSSG@@")) return;
    if (!void_vm_readable(camera, 0xE8)) return;
    uint32_t zooms = MEM32(camera + 0x70), zoom_end = MEM32(camera + 0x74);
    if (zoom_end <= zooms || zoom_end - zooms < 4 || zoom_end - zooms > 512 ||
        (zoom_end - zooms) % 4 || !void_vm_readable(zooms, zoom_end - zooms)) return;
    void_log("[touch] active-camera=%08X type=%s distance=%.6g target=%.6g clamp-max=%.6g clamp-min=%.6g levels=%u",
        camera, type, (double)touch_float(camera + 0xE0), (double)touch_float(camera + 0xE4),
        (double)touch_float(zooms), (double)touch_float(zoom_end - 4), (zoom_end - zooms) / 4);
    if (camera_view_owner == camera && camera_view && void_vm_readable(camera_view, 4))
        void_log("[touch] camera-view=%08X vtable=%08X type=%s", camera_view, MEM32(camera_view), touch_type(camera_view));
}

static void touch_log_camera(void)
{
    unsigned context = s2_touch_context();
    uint32_t direct = context & S2_TOUCH_TOWN ? town_camera :
        context & (S2_TOUCH_LIVE | S2_TOUCH_BUY | S2_TOUCH_BUILD) ? lot_camera : 0;
    if (direct) touch_log_camera_value(direct);
}

static int touch_text(uint32_t window)
{
    const char *type = touch_type(window);
    return !strcmp(type, ".?AVcGZWinTextEdit@@") || !strcmp(type, ".?AVcGZWinLineInput@@");
}

static int touch_tool_kind(const char *type)
{
    static const struct { const char *name; int kind; } known[] = {
        {"StandardAction", 1}, {"SelectSim", 1}, {"CameraBase", 1},
        {"CameraHandDrag", 1}, {"CameraMiddleClick", 1}, {"CameraRightClick", 1},
        {"Object", 2}, {"ObjectForSale", 2},
        {"Eyedropper", 6}, {"ObjectDesign", 6},
        {"Wall", 3}, {"FloorPattern", 3}, {"Wallpaper", 3}, {"DeckFoundation", 3},
        {"CurvedPool", 3}, {"Water", 3}, {"Roof", 3}, {"RoofAngle", 3}, {"RoofPattern", 3},
        {"Awning", 3}, {"ModularStairs", 3}, {"StairsRailing", 3}, {"GardenPlot", 3},
        {"Fireplace", 3}, {"Elevator", 3}, {"AutoRoof", 3},
        {"FloorElevation", 3}, {"FloorLevel", 3}, {"FloorVisibility", 3},
        {"TerrainBrush", 4}, {"TerrainPaint", 4}, {"TerrainSculpt", 4}, {"FlattenLot", 4},
        {"SledgeHammer", 5}, {"BulldozeLot", 5}, {"EvictFamily", 5}, {"MoveLotToCatalog", 5},
        {"PlaceNewLot", 7}, {"PlaceLotFromInventory", 7}, {"MoveLot", 7},
        {"InventoryObject", 8} /* The inventory Hand uses the game's pickup and drop panel. */
    };
    const char *prefix = ".?AVcTSUserTool";
    size_t length = strlen(prefix);
    if (strncmp(type, prefix, length)) return 0;
    type += length;
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; ++i) {
        length = strlen(known[i].name);
        if (!strncmp(type, known[i].name, length) && !strcmp(type + length, "@@")) return known[i].kind;
    }
    return 0;
}

static int touch_cas_rotation_window(uint32_t window)
{
    /* Original CAS resource CB980E50 calls this ID the rotation window. */
    if (!void_vm_readable(window, 0xA4) ||
        MEM32(window + 0x10) != 0x4D097B6Du || strcmp(touch_type(window), ".?AVcGZWinFlatRect@@")) return 0;
    uint32_t head = MEM32(window + 0xA0);
    if (!head || !void_vm_readable(head, 4)) return 0;
    uint32_t node = MEM32(head);
    for (unsigned n = 0; node && node != head && n < 64 && void_vm_readable(node, 12); ++n) {
        if (!strcmp(touch_type(MEM32(node + 8)), ".?AVcTSUISimEditor@@")) return 1;
        node = MEM32(node);
    }
    return 0;
}

static int touch_cas_viewport(uint32_t window)
{
    return (s2_touch_context() & S2_TOUCH_CAS) && touch_cas_rotation_window(window);
}

static int touch_scene_ancestry(uint32_t window)
{
    for (unsigned depth = 0; window && depth < 16; ++depth) {
        if (!void_vm_readable(window, 0xEC)) return 1;
        if (window == scene_window || !strcmp(touch_type(window), ".?AVcTSWinSceneGraph@@") ||
            touch_cas_rotation_window(window)) return 1;
        window = MEM32(window + 0x58);
    }
    return window != 0;
}

static int touch_ui_button(uint32_t window)
{
    /* Rewards and other panels can outlive Live mode; the picked enabled button still owns input. */
    return void_vm_readable(window, 0xEC) &&
        MEM32(window) == 0xA0E53EECu && (MEM32(window + 0xE8) & 3) == 3 &&
        !strcmp(touch_type(window), ".?AVcGZWinBtn@@");
}

static int touch_live_hud_context(unsigned context)
{
    return context == S2_TOUCH_LIVE || (context == S2_TOUCH_BUY && s2_touch_rewards());
}

static int touch_live_hud_background(uint32_t window)
{
    /* The unfilled Live HUD container's rectangle extends over the visible lot. */
    if (!void_vm_readable(window, 0xEC) || MEM32(window) != 0xA0E54B18u || MEM32(window + 0x10)) return 0;
    uint32_t parent = MEM32(window + 0x58);
    return void_vm_readable(parent, 0xEC) && MEM32(parent) == 0xA0DE3704u && MEM32(parent + 0x10) == 0x77D986A1u;
}

/* The pick result of a tap on the unfilled Live HUD container, which TouchLayer pans like the lot. */
static int touch_pan_background(unsigned context, uint32_t manager, uint32_t root, uint32_t hit, int result)
{
    return touch_live_hud_context(context) && result == 1 && s2_touch_context() == context &&
        manager && manager == s2_touch_owner_manager() && void_vm_readable(manager, 0x48) &&
        MEM32(manager + 0x44) == root && !MEM32(manager + 0x34) && touch_live_hud_background(hit);
}

static int touch_empty_procs(uint32_t window)
{
    if (!window || !void_vm_readable(window, 0xA8) || MEM32(window + 0xA4)) return 0;
    uint32_t head = MEM32(window + 0xA0);
    return head && void_vm_readable(head, 8) && MEM32(head) == head && MEM32(head + 4) == head;
}

static int touch_town_callout(uint32_t window, uint32_t root)
{
    if (s2_touch_context() != S2_TOUCH_TOWN || !root || !win_manager || !void_vm_readable(win_manager, 0x48) ||
        MEM32(win_manager + 0x44) != root || !window || !void_vm_readable(window, 0xEC) ||
        MEM32(window) != 0xA0E89208u || MEM32(window + 0x10) != 0x2AAB8CC1u ||
        strcmp(touch_type(window), ".?AVcTSWinMultiIconCalloutBox@@")) return 0;
    uint32_t parent = MEM32(window + 0x58);
    if (!parent || !void_vm_readable(parent, 0xEC) || MEM32(parent) != 0xA0DE3704u ||
        MEM32(parent + 0x10) != 0xC9615A3Fu || MEM32(parent + 0x58) != root ||
        strcmp(touch_type(parent), ".?AVcTSWinGenTransp@@") || !touch_empty_procs(window) || !touch_empty_procs(parent)) return 0;
    uint32_t table = MEM32(window);
    if (!void_vm_readable(table, 0x284) || MEM32(table + 0x280) != 0xA0147110u) return 0;
    /* Only the picked label has no mouse handlers; its parent/children stay UI. */
    for (unsigned offset = 0x26C; offset <= 0x27C; offset += 4)
        if (MEM32(table + offset) != 0xA01474A0u) return 0;
    return 1;
}

static int touch_grid_proc(uint32_t proc)
{
    return proc && void_vm_readable(proc, 0x58) && !strncmp(touch_type(proc), ".?AVcTS", 7) &&
        void_vm_readable(MEM32(proc), 0x6C) && MEM32(MEM32(proc) + 0x68) == 0xA07479F0u;
}

static uint32_t touch_bound_catalog_proc(uint32_t window)
{
    unsigned context = s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT;
    uint32_t manager = s2_touch_owner_manager();
    if ((context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD && context != S2_TOUCH_TOWN) || !manager ||
        !void_vm_readable(window, 0x130) || MEM32(window + 4) != manager) return 0;
    uint32_t table = MEM32(window), interface = MEM32(window + 0xF8);
    if (!((table == 0xA0E54498u && interface == 0xA0E54754u) ||
          (table == 0xA0DE3704u && interface == 0xA0DE39C0u))) return 0;
    /* A0939392 binds the Gen delegate at +12C; +A0 only lists auxiliary routers. */
    uint32_t proc = MEM32(window + 0x12C);
    if (proc && void_vm_readable(proc, 0x44) && MEM32(proc) == 0xA0E023D0u && MEM32(proc + 8) == window)
        return proc;
    return touch_grid_proc(proc) && MEM32(proc + 0x54) == window ? proc : 0;
}

static uint32_t touch_town_catalog_proc(uint32_t window)
{
    uint32_t manager = s2_touch_owner_manager();
    if (s2_touch_context() != S2_TOUCH_TOWN || !manager || !void_vm_readable(window, 0x130) ||
        MEM32(window) != 0xA0DE3704u || MEM32(window + 4) != manager || !(MEM32(window + 0xE8) & 1)) return 0;
    uint32_t delegate = MEM32(window + 0x12C), interface, expected;
    if (!delegate || !void_vm_readable(delegate, 0x2C) || MEM32(delegate + 0xC) != window) return 0;
    if (MEM32(delegate) == 0xA0DDADA8u && !strcmp(touch_type(delegate), ".?AVcTSWinProcNeighborhoodUCPSubPanelFamilies@@")) {
        interface = MEM32(delegate + 0x18); expected = 0xA0DDAF60u;
    } else if (MEM32(delegate) == 0xA0DDB1DCu && !strcmp(touch_type(delegate), ".?AVcTSWinProcNeighborhoodUCPSubPanelTools@@")) {
        interface = MEM32(delegate + 0x20); expected = 0xA0DDB374u;
    } else return 0;
    /* These subpanels hold the catalog's +160 COM interface, outside the window's proc list. */
    if (interface < 0x160 || !void_vm_readable(interface, 4) || MEM32(interface) != expected) return 0;
    uint32_t catalog = interface - 0x160;
    if (!touch_grid_proc(catalog) || MEM32(catalog) != (expected == 0xA0DDAF60u ? 0xA0DDAE7Cu : 0xA0DDB290u)) return 0;
    uint32_t owner = MEM32(catalog + 0x54);
    for (unsigned depth = 0; owner && depth < 16 && void_vm_readable(owner, 0xEC); ++depth) {
        if (MEM32(owner + 4) != manager) return 0;
        if (owner == window) return catalog;
        owner = MEM32(owner + 0x58);
    }
    return 0;
}

static uint32_t touch_cas_grid_proc(uint32_t proc, uint32_t window)
{
    if (!(s2_touch_context() & S2_TOUCH_CAS) || proc < 0x16C || !void_vm_readable(proc, 16) ||
        !void_vm_readable(MEM32(proc), 16) || MEM32(MEM32(proc) + 12) != 0xA075739Au) return 0;
    uint32_t owner = proc - 0x16C;
    if (!void_vm_readable(owner, 0x1CC) || !void_vm_readable(MEM32(owner), 0x2CC) ||
        strncmp(touch_type(owner), ".?AVcTSUICASComponent", sizeof ".?AVcTSUICASComponent" - 1) ||
        MEM32(MEM32(owner) + 0x2C8) != 0xA075843Eu ||
        !void_vm_readable(window, 0xEC) || strcmp(touch_type(window), ".?AVcGZWinGrid@@")) return 0;
    uint32_t first = MEM32(owner + 0x1B8), total = MEM32(owner + 0x174), page = MEM32(owner + 0x1BC);
    if (total > 1000000 || first > total || !page || page > 512) return 0;
    /* The component's +0x184 is its SimEditor interface; the grid owns the registered proc. */
    uint32_t head = MEM32(window + 0xA0);
    if (!head || !void_vm_readable(head, 4)) return 0;
    uint32_t node = MEM32(head);
    for (unsigned n = 0; node && node != head && n < 64 && void_vm_readable(node, 12); ++n) {
        if (MEM32(node + 8) == proc) {
            for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0x5C); ++depth) {
                if (window == owner) return owner;
                window = MEM32(window + 0x58);
            }
            return 0;
        }
        node = MEM32(node);
    }
    return 0;
}

/* IDs and order come from the original EP9 UI resources, including the pet/roommate variants. */
static const uint32_t touch_swipe_ids[2][10] = {
    { 0x27D7258Fu, 0x57E01CC1u, 0x37E01CC3u, 0x47E01CC5u, 0, 0 },
    { 0x0D8AB291u, 0x17DEFBF5u, 0x57DEFBF4u, 0x47DEFBF2u, 0x8B85BA4Eu, 0x8B85BADDu }
};

static uint32_t touch_chooser_window(uint32_t root)
{
    uint32_t manager = s2_touch_owner_manager();
    if (s2_touch_context() != S2_TOUCH_CHOOSER || !manager || MEM32(manager + 0x44) != root ||
        !void_vm_readable(root, 0xEC) || !void_vm_readable(MEM32(root), 0xA0)) return 0;
    uint32_t id = 0xCDA360CEu;
    uint32_t frame = void_call_guest_this(MEM32(MEM32(root) + 0x9C), root, 1, &id);
    if (!void_vm_readable(frame, 0xEC) || MEM32(frame + 4) != manager || MEM32(frame + 0x10) != id ||
        !(MEM32(frame + 0xE8) & 1) || !void_vm_readable(MEM32(frame), 0xA0)) return 0;
    /* Runtime wraps the menu in a FlatRect with the same ID; use the arrows' real owner. */
    id = 0x1005u;
    uint32_t arrow = void_call_guest_this(MEM32(MEM32(frame) + 0x9C), frame, 1, &id);
    if (s2_touch_context() != S2_TOUCH_CHOOSER || s2_touch_owner_manager() != manager || MEM32(manager + 0x44) != root ||
        !void_vm_readable(arrow, 0xEC) || MEM32(arrow) != 0xA0E53EECu || MEM32(arrow + 4) != manager || MEM32(arrow + 0x10) != id) return 0;
    uint32_t window = MEM32(arrow + 0x58);
    if (!void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager || MEM32(window + 0x10) != 0xCDA360CEu ||
        (MEM32(window) != 0xA0DE3704u && MEM32(window) != 0xA0E54498u) || !(MEM32(window + 0xE8) & 1)) return 0;
    for (uint32_t owner = window, depth = 0; owner && depth < 16 && void_vm_readable(owner, 0xEC); ++depth) {
        if (owner == root) return window;
        if (MEM32(owner + 4) != manager || !(MEM32(owner + 0xE8) & 1)) return 0;
        owner = MEM32(owner + 0x58);
    }
    return 0;
}

/* IDs come from the original vertical panels; visual Y order also covers pet/CAS variants. */
static const uint32_t touch_vertical_ids[][10] = {
    {0x49064820u,0x40000000u,0x50000000u,0x60000000u,0x70000000u,0x80000000u},
    {0xFEED2008u,0xB1u,0xB2u,0xB3u,0xB4u},
    {0x4C6B1D69u,0x8C6B0000u,0x8C6B0001u,0x8C6B0002u,0x8C6B0003u,0x8C6B0004u,0x8C6B0005u,0x8C6B0006u},
    {0xCB980E82u,0x4CB80000u,0x4CB80001u,0x4CB80002u,0x4CB80003u,0x4CB80004u,0x4CB80005u,0x4CB80006u},
    {0xCB980E85u,0x6C6E0000u,0x6C6E0001u,0x6C6E0002u,0x6C6E0003u,0x6C6E0004u,0x6C6E0005u,0x6C6E0006u}
};

static const uint32_t *touch_vertical_set(uint32_t window, uint32_t *parent)
{
    unsigned context = s2_touch_context();
    if (context == (S2_TOUCH_CAS | S2_TOUCH_FOCUSED_TEXT)) context = S2_TOUCH_CAS;
    if (context != S2_TOUCH_CAS && !touch_live_hud_context(context)) return NULL;
    if (!void_vm_readable(window, 0xFC) || !(MEM32(window + 0xE8) & 1)) return NULL;
    uint32_t id = MEM32(window + 0x10);
    for (unsigned group = 0; group < sizeof touch_vertical_ids / sizeof touch_vertical_ids[0]; ++group) {
        const uint32_t *ids = touch_vertical_ids[group];
        if ((group >= 2) != (context == S2_TOUCH_CAS)) continue;
        if (!touch_ui_button(window)) continue;
        uint32_t owner = MEM32(window + 0x58);
        if (!void_vm_readable(owner, 0xEC)) continue;
        uint32_t component = owner;
        if (group == 4 && MEM32(owner) == 0xA0E54B18u && !MEM32(owner + 0x10) && (MEM32(owner + 0xE8) & 1))
            component = MEM32(owner + 0x58);
        if (!void_vm_readable(component, 0xEC) || MEM32(component + 0x10) != ids[0]) continue;
        for (unsigned n = 1; n < 10 && ids[n]; ++n)
            if (id == ids[n]) { *parent = owner; return ids + 1; }
    }
    return NULL;
}

static int touch_swipe_group(uint32_t window, int *index)
{
    uint32_t vertical_parent;
    if (touch_vertical_set(window, &vertical_parent)) { *index = -1; return 4; }
    unsigned context = s2_touch_context();
    if (context == S2_TOUCH_CHOOSER && void_vm_readable(window, 0xEC) &&
        MEM32(window + 0x10) == 0xCDA360CEu && (MEM32(window) == 0xA0DE3704u || MEM32(window) == 0xA0E54498u) &&
        (MEM32(window + 0xE8) & 1)) { *index = -1; return 3; }
    if ((context != S2_TOUCH_LIVE && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) ||
        !void_vm_readable(window, 0xFC)) return 0;
    uint32_t id = MEM32(window + 0x10);
    if ((MEM32(window) == 0xA0E54B18u || MEM32(window) == 0xA0DE3D88u) && (MEM32(window + 0xE8) & 1)) {
        if (id == 0xA94E97E8u || id == 0xA94E97F4u || id == 0x31D5BA3Au) { *index = -1; return 2; }
        if (id == 0x2E9AF844u || id == 0x2E9AF849u) { *index = -1; return 1; }
    }
    /* The original clock is deliberately disabled; its visible label can still own a swipe. */
    if (id == 0x37D82A93u && MEM32(window) == 0xA0E53EECu && (MEM32(window + 0xE8) & 1) &&
        !strcmp(touch_type(window), ".?AVcGZWinBtn@@")) { *index = -1; return 1; }
    if (!touch_ui_button(window)) return 0;
    for (unsigned group = 0; group < 2; ++group)
        for (unsigned n = 0; n < 6 && touch_swipe_ids[group][n]; ++n)
            if (id == touch_swipe_ids[group][n]) { *index = (int)n; return (int)group + 1; }
    return 0;
}

static int touch_swipe_owner(uint32_t window, uint32_t parent, int group, uint32_t manager)
{
    if (!void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager) return 0;
    if (window == parent) return group >= 2;
    uint32_t owner = MEM32(window + 0x58);
    if (owner == parent) return 1;
    /* Age, clock and speed widgets share the HUD, including nested age indicators. */
    for (unsigned depth = 0; group == 1 && owner && depth < 16 && void_vm_readable(owner, 0xEC); ++depth) {
        if (!void_vm_readable(MEM32(owner), 0xA0) || MEM32(owner + 4) != manager || !(MEM32(owner + 0xE8) & 1)) return 0;
        if (MEM32(owner + 0x58) == parent) return 1;
        owner = MEM32(owner + 0x58);
    }
    return 0;
}

/* The HUD's mouse router and tooltips can cover the original tab buttons. */
static uint32_t touch_vertical_band(uint32_t root, int x, int y)
{
    x = (int)((float)x / (float)s2_ui_factor()); y = (int)((float)y / (float)s2_ui_factor());
    unsigned context = s2_touch_context(), mode = context & ~S2_TOUCH_FOCUSED_TEXT, epoch = s2_touch_layout_epoch();
    int live_hud = touch_live_hud_context(context);
    uint32_t manager = s2_touch_owner_manager();
    if ((!live_hud && mode != S2_TOUCH_CAS) || !manager ||
        MEM32(manager + 0x44) != root || !void_vm_readable(root, 0xEC) || !void_vm_readable(MEM32(root), 0xA0)) return 0;
    uint32_t scope = live_hud ? scene_window : root;
    if (!scope || !void_vm_readable(scope, 0xEC) || MEM32(scope + 4) != manager ||
        !void_vm_readable(MEM32(scope), 0xA0)) return 0;
    static const uint32_t main_strip_ids[] = {0xA94E97E8u, 0xA94E97F4u, 0x31D5BA3Au};
    const unsigned subgroups = sizeof touch_vertical_ids / sizeof touch_vertical_ids[0];
    const unsigned strips = sizeof main_strip_ids / sizeof main_strip_ids[0];
    for (unsigned group = live_hud ? 0 : 2; group < (live_hud ? 2 + strips : subgroups); ++group) {
        int main_strip = live_hud && group >= 2;
        uint32_t parent_id = main_strip ? main_strip_ids[group - 2] : touch_vertical_ids[group][0];
        const uint32_t *ids = main_strip ? touch_swipe_ids[1] : touch_vertical_ids[group] + 1;
        uint32_t parent = void_call_guest_this(MEM32(MEM32(scope) + 0x9C), scope, 1, &parent_id);
        if (s2_touch_context() != context || s2_touch_layout_epoch() != epoch || s2_touch_owner_manager() != manager ||
            MEM32(manager + 0x44) != root || (live_hud && scene_window != scope)) return 0;
        int index;
        if (!parent || !void_vm_readable(parent, 0xEC) || MEM32(parent + 4) != manager || MEM32(parent + 0x10) != parent_id ||
            (main_strip && touch_swipe_group(parent, &index) != 2) ||
            !void_vm_readable(MEM32(parent), 0xA0) || x < (int)MEM32(parent + 0x24) || x >= (int)MEM32(parent + 0x2C) ||
            y < (int)MEM32(parent + 0x28) || y >= (int)MEM32(parent + 0x30)) continue;
        uint32_t owner = parent;
        for (unsigned depth = 0; owner && owner != root && depth < 16 && void_vm_readable(owner, 0xEC); ++depth) {
            if (MEM32(owner + 4) != manager || !(MEM32(owner + 0xE8) & 1)) { owner = 0; break; }
            owner = MEM32(owner + 0x58);
        }
        if (owner != root) continue;
        uint32_t first = 0;
        int left = INT32_MAX, top = INT32_MAX, right = 0, bottom = 0;
        for (unsigned n = 0; n < 9 && ids[n]; ++n) {
            uint32_t button = void_call_guest_this(MEM32(MEM32(parent) + 0x9C), parent, 1, &ids[n]);
            if (s2_touch_context() != context || s2_touch_layout_epoch() != epoch || s2_touch_owner_manager() != manager ||
                MEM32(manager + 0x44) != root || !void_vm_readable(parent, 0xEC) || MEM32(parent + 4) != manager ||
                MEM32(parent + 0x10) != parent_id || !(MEM32(parent + 0xE8) & 1)) return 0;
            uint32_t holder = parent;
            if (!button || !touch_ui_button(button) || MEM32(button + 4) != manager || MEM32(button + 0x10) != ids[n] ||
                (main_strip ? MEM32(button + 0x58) != parent : touch_vertical_set(button, &holder) != ids) || (holder != parent &&
                (!void_vm_readable(holder, 0xEC) || MEM32(holder + 4) != manager || MEM32(holder + 0x58) != parent))) continue;
            int l = (int)MEM32(button + 0x24), t = (int)MEM32(button + 0x28);
            int r = (int)MEM32(button + 0x2C), b = (int)MEM32(button + 0x30);
            if (r <= l || b <= t) continue;
            if (!first) first = button;
            if (l < left) left = l; if (t < top) top = t;
            if (r > right) right = r; if (b > bottom) bottom = b;
        }
        if (first && x >= left && x < right && y >= top && y < bottom) return first;
    }
    return 0;
}

static int touch_time_index(void)
{
    if (!void_vm_readable(0xA10CC2ACu, 4)) return -1;
    uint32_t simulator = MEM32(0xA10CC2ACu);
    if (!simulator || !void_vm_readable(simulator, 0x40) || MEM32(simulator) != 0xA0DCA99Cu) return -1;
    /* Original HUD A0779676 uses A0480CE0/A0480FE0, not the potentially stale toggles. */
    uint32_t state = MEM32(simulator + 0x3C), speed = state & 65535;
    return state >> 16 ? 0 : speed >= 2 && speed <= 4 ? (int)speed - 1 : -1;
}

static unsigned touch_time_state(uint32_t window)
{
    uint32_t manager = s2_touch_owner_manager(), parent = MEM32(window + 0x58);
    for (unsigned depth = 0; parent && depth < 16 && void_vm_readable(parent, 0xEC) &&
        MEM32(parent + 0x10) != 0x77D986A1u; ++depth) parent = MEM32(parent + 0x58);
    if (!manager || !void_vm_readable(parent, 0xEC) || MEM32(parent) != 0xA0DE3704u ||
        MEM32(parent + 4) != manager || MEM32(parent + 0x10) != 0x77D986A1u ||
        !void_vm_readable(MEM32(parent), 0xA0) || !touch_swipe_owner(window, parent, 1, manager)) return 0;
    int current = touch_time_index();
    return current < 0 ? 0 : 1024 | (current << 11);
}

static uint32_t touch_time_band(uint32_t root, int x, int y)
{
    x = (int)(x / s2_ui_factor()); y = (int)(y / s2_ui_factor());
    unsigned context = s2_touch_context(), epoch = s2_touch_layout_epoch();
    uint32_t manager = s2_touch_owner_manager();
    if ((context != S2_TOUCH_LIVE && context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) || !manager ||
        MEM32(manager + 0x44) != root || !void_vm_readable(root, 0xEC) || !void_vm_readable(MEM32(root), 0xA0)) return 0;
    uint32_t view = scene_window;
    if (!scene_owner || !void_vm_readable(view, 0xEC) || MEM32(view) != 0xA0E54498u ||
        MEM32(view + 4) != manager || MEM32(view + 0x10) != 0x480E8304u || MEM32(view + 0x58) != root ||
        !(MEM32(view + 0xE8) & 1)) return 0;
    /* GetWindowByID searches hidden descendants too; bind the clock to the current scene's HUD. */
    uint32_t id = 0x77D986A1u;
    uint32_t parent = void_call_guest_this(MEM32(MEM32(view) + 0x9C), view, 1, &id);
    if (!void_vm_readable(parent, 0xEC) || MEM32(parent) != 0xA0DE3704u || MEM32(parent + 4) != manager ||
        MEM32(parent + 0x10) != id || MEM32(parent + 0x58) != view || !(MEM32(parent + 0xE8) & 1)) return 0;
    id = 0x37D82A93u;
    uint32_t clock = void_call_guest_this(MEM32(MEM32(parent) + 0x9C), parent, 1, &id);
    int index;
    if (!void_vm_readable(clock, 0xFC) || MEM32(clock + 0x10) != id || MEM32(clock + 0x58) != parent ||
        touch_swipe_group(clock, &index) != 1 || !touch_time_state(clock) || scene_window != view) return 0;
    uint32_t owner = parent;
    for (unsigned depth = 0; owner && owner != root && depth < 16 && void_vm_readable(owner, 0xEC); ++depth) {
        if (MEM32(owner + 4) != manager || !(MEM32(owner + 0xE8) & 1)) return 0;
        owner = MEM32(owner + 0x58);
    }
    if (owner != root) return 0;
    int left = (int)MEM32(clock + 0x24), top = (int)MEM32(clock + 0x28);
    int right = (int)MEM32(clock + 0x2C), bottom = (int)MEM32(clock + 0x30);
    if (right <= left || bottom <= top) return 0;
    static const uint32_t ids[] = {0x27D7258Fu,0x57E01CC1u,0x37E01CC3u,0x47E01CC5u,0x2E9AF844u,0x2E9AF849u};
    for (unsigned n = 0; n < sizeof ids / sizeof ids[0]; ++n) {
        uint32_t window = void_call_guest_this(MEM32(MEM32(parent) + 0x9C), parent, 1, &ids[n]);
        if (s2_touch_context() != context || s2_touch_layout_epoch() != epoch || s2_touch_owner_manager() != manager || scene_window != view ||
            MEM32(manager + 0x44) != root || !void_vm_readable(parent, 0xEC) || MEM32(parent + 4) != manager ||
            MEM32(parent + 0x10) != 0x77D986A1u || !touch_time_state(clock)) return 0;
        int index;
        if (!void_vm_readable(window, 0xFC) || MEM32(window + 0x10) != ids[n] || touch_swipe_group(window, &index) != 1 ||
            !touch_swipe_owner(window, parent, 1, manager)) continue;
        int l = (int)MEM32(window + 0x24), t = (int)MEM32(window + 0x28);
        int r = (int)MEM32(window + 0x2C), b = (int)MEM32(window + 0x30);
        if (r <= l || b <= t) continue;
        if (l < left) left = l; if (t < top) top = t;
        if (r > right) right = r; if (b > bottom) bottom = b;
    }
    return x >= left && x < right && y >= top && y < bottom ? clock : 0;
}

static int touch_swipe_apply(uint32_t window, int amount, unsigned context, uint64_t size, unsigned epoch)
{
    int current, group = touch_swipe_group(window, &current);
    if (!group || !amount || amount < -8 || amount > 8 || !touch_layout_matches(context, size, epoch)) return 0;
    uint32_t source_parent = MEM32(window + 0x58), parent = source_parent, manager = s2_touch_owner_manager();
    const uint32_t *ids = group == 4 ? touch_vertical_set(window, &parent) : touch_swipe_ids[group == 3 ? 0 : group - 1];
    if (!ids) return 0;
    if (group == 1) {
        for (unsigned depth = 0; parent && depth < 16 && void_vm_readable(parent, 0xEC) &&
            MEM32(parent + 0x10) != 0x77D986A1u; ++depth) parent = MEM32(parent + 0x58);
    } else if (group != 4 && (group == 3 || current < 0)) parent = window;
    if (!parent || !manager || !void_vm_readable(parent, 0xEC) || MEM32(parent + 4) != manager ||
        !void_vm_readable(MEM32(parent), 0xA0) || !touch_swipe_owner(window, parent, group, manager)) return 0;
    if (group == 1 && (MEM32(parent) != 0xA0DE3704u || MEM32(parent + 0x10) != 0x77D986A1u)) return 0;
    uint32_t root = MEM32(manager + 0x44);
    if (group == 3) {
        /* The chooser's original arrows page neighborhoods; a left swipe means Next. */
        uint32_t id = amount < 0 ? 0x1005u : 0x1004u;
        uint32_t target = void_call_guest_this(MEM32(MEM32(parent) + 0x9C), parent, 1, &id);
        if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
            MEM32(manager + 0x44) != root || !void_vm_readable(target, 0xEC) ||
            MEM32(target + 4) != manager || MEM32(target + 0x58) != parent || MEM32(target + 0x10) != id) return 0;
        if (!touch_ui_button(target)) return 1;
        if (target < 4 || !void_vm_readable(target - 4, 0x100) || MEM32(target - 4) != 0xA0E53E14u) return 0;
        void_call_guest_this(0xA093550Au, target - 4, 0, NULL);
        return 1;
    }
    uint32_t buttons[10] = {0};
    for (unsigned n = 0; n < 9 && ids[n]; ++n) {
        uint32_t id = ids[n];
        uint32_t button = void_call_guest_this(MEM32(MEM32(parent) + 0x9C), parent, 1, &id);
        if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
            MEM32(manager + 0x44) != root || !void_vm_readable(parent, 0xEC) ||
            !void_vm_readable(window, 0xEC) || MEM32(window + 0x58) != source_parent ||
            !touch_swipe_owner(window, parent, group, manager)) return 0;
        if (button && touch_ui_button(button) && void_vm_readable(button, 0xFC) &&
            MEM32(button + 0x10) == id && touch_swipe_owner(button, parent, group, manager)) {
            buttons[n] = button;
            /* cGZWinBtn's toggle bit is +FC on the base, +F8 on its window interface. */
            if (group != 1 && (MEM32(button + 0xF8) & 1)) current = (int)n;
        }
    }
    if (group == 4) {
        for (unsigned i = 0; i < 9; ++i) for (unsigned j = i + 1; j < 9; ++j) {
            if (buttons[j] && (!buttons[i] || (int)MEM32(buttons[j] + 0x28) < (int)MEM32(buttons[i] + 0x28))) {
                uint32_t swap = buttons[i]; buttons[i] = buttons[j]; buttons[j] = swap;
            }
        }
        current = -1;
        for (unsigned n = 0; n < 9 && buttons[n]; ++n)
            if (MEM32(buttons[n] + 0xF8) & 1) current = (int)n;
    }
    if (group == 1) current = touch_time_index();
    if (current < 0 || current >= 9 || !buttons[current]) return 0;
    int direction = amount > 0 ? 1 : -1, next = current;
    for (int n = 0; n < (amount > 0 ? amount : -amount); ++n) {
        int candidate = next + direction;
        while (candidate >= 0 && candidate < 9 && !buttons[candidate]) candidate += direction;
        if (candidate < 0 || candidate >= 9) break;
        next = candidate;
    }
    if (next == current) return 1;
    uint32_t target = buttons[next];
    if (!touch_layout_matches(context, size, epoch) || !touch_ui_button(target) || target < 4 ||
        !void_vm_readable(target - 4, 0x100) || MEM32(target - 4) != 0xA0E53E14u) return 0;
    /* Match left release: toggle on, then F6; F0 is the other button notification. */
    uint32_t toggle[] = { 1, 1 };
    void_call_guest_this(0xA0936F94u, target - 4, 2, toggle);
    if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
        MEM32(manager + 0x44) != root || !touch_ui_button(target) ||
        !touch_swipe_owner(target, parent, group, manager) ||
        !void_vm_readable(target - 4, 0x100) || MEM32(target - 4) != 0xA0E53E14u) return 0;
    void_call_guest_this(0xA093550Au, target - 4, 0, NULL);
    if (s2_touch_owner_manager() == manager && MEM32(manager + 0x44) == root &&
        (s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT) == (context & ~S2_TOUCH_FOCUSED_TEXT) &&
        s2_touch_layout_epoch() == epoch && touch_ui_button(target) &&
        touch_swipe_owner(target, parent, group, manager) &&
        (group == 1 ? touch_time_index() == next : (MEM32(target + 0xF8) & 1) != 0)) {
        unsigned family = group == 1 ? 0 : 1, position = 0, count = 0;
        if (group == 4) for (unsigned row = 0; row < sizeof touch_vertical_ids / sizeof touch_vertical_ids[0]; ++row)
            if (ids == touch_vertical_ids[row] + 1) family = row + 2;
        if (family == 4) {
            int human = 0;
            for (unsigned n = 0; n < 9; ++n) if (buttons[n] && MEM32(buttons[n] + 0x10) == 0x8C6B0000u) human = 1;
            if (!human) family = 7;
        }
        for (unsigned n = 0; n < 9; ++n) if (buttons[n]) { ++count; if ((int)n == next) position = count; }
        uint64_t receipt = (uint64_t)(++swipe_sequence & 0xFFFFFu) << 44 | (uint64_t)family << 40 |
            (uint64_t)count << 36 | (uint64_t)position << 32 | MEM32(target + 0x10);
        atomic_fetch_add(&swipe_version, 1);
        atomic_store(&shown_swipe_context, s2_touch_context());
        atomic_store(&shown_swipe_epoch, epoch);
        atomic_store(&shown_swipe, receipt);
        atomic_fetch_add(&swipe_version, 1);
        void_log("[touch-section] family %u section %u/%u id=%08X", family, position, count, MEM32(target + 0x10));
    }
    return 1;
}

static unsigned touch_widget(uint32_t window)
{
    unsigned flags = 0;
    for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0xA4); ++depth) {
        const char *type = touch_type(window);
        int index, group = touch_swipe_group(window, &index);
        if (group) flags |= group == 1 ? 256 | touch_time_state(window) : group == 3 ? 256 : 512;
        if (!depth && touch_ui_button(window)) {
            uint32_t id = MEM32(window + 0x10);
            if (id == 0x2BF08BECu || id == 0xCCB70002u || id == 0x67D97B2Fu) flags |= 128;
        }
        if (strstr(type, "cGZWinSlider@@") || strstr(type, "cGZWinScrollbar") || strstr(type, "cGZWinSpinner@@"))
            flags |= 4;
        if (!depth && s2_touch_text_editable(window)) flags |= 16;
        if (strstr(type, "cGZWinGrid@@") || strstr(type, "cGZWinListBox@@") || strstr(type, "cGZWinTreeView") ||
            !strcmp(type, ".?AVcTSWinListBox@@"))
            flags |= 8;
        /* Description fields can enable both editing and scrolling (town text uses 0x99CB). */
        if (!strcmp(type, ".?AVcGZWinTextEdit@@") &&
            void_vm_readable(window, 0x164) && (MEM32(window + 0x160) & 8)) flags |= 8;
        uint32_t bound = touch_bound_catalog_proc(window);
        if (bound) {
            flags |= MEM32(bound) == 0xA0E023D0u ? 8 | 64 : 8 | 32;
            if (MEM32(bound) == 0xA0E023D0u) s2_catalog_track(bound);
        }
        if (touch_town_catalog_proc(window)) flags |= 8 | 32;
        uint32_t head = MEM32(window + 0xA0);
        if (head && void_vm_readable(head, 4)) {
            uint32_t node = MEM32(head);
            for (unsigned n = 0; node != head && node && n < 64 && void_vm_readable(node, 12); ++n) {
                uint32_t object = MEM32(node + 8);
                const char *proc = touch_type(object);
                if (!depth && !strcmp(proc, ".?AVcTSWinDragAndDropMessageFilter@@")) flags |= 4 | TOUCH_DRAG_FILTER;
                if (strstr(proc, "cTSWinProc") && strstr(proc, "Catalog")) flags |= 8;
                if (touch_grid_proc(object) || touch_cas_grid_proc(object, window)) flags |= 8 | 32;
                if (!strcmp(proc, ".?AVcTSWinProcCatalogThumbnail@@")) { flags |= 64; s2_catalog_track(object); }
                node = MEM32(node);
            }
        }
        window = MEM32(window + 0x58);
    }
    return flags;
}

/* A decorative winflag_ignoremouse overlay can cover an interactive widget. */
static uint32_t touch_input_pick(uint32_t root, int x, int y)
{
    uint32_t hit = s2_ui_pick(root, x, y);
    if (!hit || !void_vm_readable(hit, 0xEC) || !(MEM32(hit + 0xE8) & 0x200000u) ||
        !void_vm_readable(MEM32(root), 0xC4)) return hit;
    uint32_t widget = s2_ui_mouse_pick(root, x, y);
    return (touch_widget(widget) & (4 | 8 | 16)) ? widget : hit;
}

static uint32_t touch_pie_owner(uint32_t window)
{
    for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0xEC); ++depth) {
        if (MEM32(window) == 0xA0DDA24Cu)
            return (MEM32(window + 0xE8) & 3) == 3 ? window : 0;
        window = MEM32(window + 0x58);
    }
    return 0;
}

static int touch_pie_item(uint32_t window, uint32_t menu)
{
    if (!menu || !void_vm_readable(window, 0xEC) || MEM32(window + 0x58) != menu) return 0;
    uint32_t head = MEM32(window + 0xA0);
    if (!head || !void_vm_readable(head, 4)) return 0;
    uint32_t node = MEM32(head);
    for (unsigned n = 0; node && node != head && n < 16 && void_vm_readable(node, 12); ++n) {
        uint32_t proc = MEM32(node + 8);
        if (void_vm_readable(proc, 4) && MEM32(proc) == 0xA0DDA548u) return 1;
        node = MEM32(node);
    }
    return 0;
}

static uint32_t touch_pie_label(uint32_t row)
{
    uint32_t list = MEM32(row + 0x54);
    if (!list || !void_vm_readable(list, 12)) return 0;
    uint32_t head = MEM32(list);
    if (!head || !void_vm_readable(head, 12)) return 0;
    uint32_t node = MEM32(head);
    for (unsigned n = 0; node && node != head && n < 32 && void_vm_readable(node, 12); ++n) {
        uint32_t child = MEM32(node + 8);
        if (void_vm_readable(child, 0xEC) && MEM32(child + 0x58) == row &&
            MEM32(child + 0x10) == 0xCB93E6FCu && (MEM32(child + 0xE8) & 1)) return child;
        node = MEM32(node);
    }
    return 0;
}

/* cTSWinPieMenu (A05C88C0) highlights the item nearest the cursor's direction
 * once the cursor leaves kGesturePickBoundary, and button up (message 10,
 * A05C819C) selects it. A tap has no hover feedback, so a tap that reaches no
 * item closes the menu the way the original closes on a press with nothing
 * highlighted: outside its rectangle grown by kCancelBoundary (message 7,
 * A05C816C). The menu receives every click while it is open and hit-tests its
 * own item windows (A05C929E: a child of the item window at the point), not the
 * global z-order. Escape (A05C8148) also closes it but then loses the next
 * click; a right press (message 8, A05C818D) always cancels it. */
static uint32_t touch_open_pie(uint32_t manager, uint32_t root)
{
    uint32_t modal = s2_touch_modal_window();
    if (modal) return touch_pie_owner(modal);
    uint32_t capture = MEM32(manager + 0x34);
    if (capture) return touch_pie_owner(capture);
    /* Live mode opens the pie without mouse capture: find it among the visible windows. */
    uint32_t stack[512] = {root};
    unsigned pending = 1, visited = 0;
    while (pending && ++visited <= 4096) {
        uint32_t window = stack[--pending];
        if (!void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager || !(MEM32(window + 0xE8) & 1)) continue;
        if (MEM32(window) == 0xA0DDA24Cu) {
            if ((MEM32(window + 0xE8) & 3) == 3) return window;
            continue;
        }
        uint32_t list = MEM32(window + 0x54);
        if (!list || !void_vm_readable(list, 12)) continue;
        uint32_t head = MEM32(list), node = head && void_vm_readable(head, 12) ? MEM32(head) : 0;
        for (unsigned n = 0; node && node != head && n < 4096 && void_vm_readable(node, 12); ++n) {
            uint32_t child = MEM32(node + 8);
            if (child && void_vm_readable(child, 0xEC) && MEM32(child + 0x58) == window) {
                if (pending == 512) return 0;
                stack[pending++] = child;
            }
            node = MEM32(node);
        }
    }
    return 0;
}

static int touch_pie_rect(uint32_t window, double px, double py, double grow)
{
    int left = (int)MEM32(window + 0x24), top = (int)MEM32(window + 0x28);
    int right = (int)MEM32(window + 0x2C), bottom = (int)MEM32(window + 0x30);
    return left <= right && top <= bottom && right - left <= 8192 && bottom - top <= 8192 &&
        px >= left - grow && px < right + grow && py >= top - grow && py < bottom + grow;
}

/* The menu's own direct hit (A05C929E) for an item id at a logical point, after
 * the conversion A05C88C0 applies (the parent's vtable +128). */
static int touch_pie_direct(uint32_t pie, uint32_t id, int lx, int ly)
{
    uint32_t parent = MEM32(pie + 0x58);
    if (!parent || !void_vm_readable(parent, 4) || !void_vm_readable(MEM32(parent), 0x12C)) return 0;
    uint32_t convert = MEM32(MEM32(parent) + 0x128);
    if (!void_dispatch_find(convert)) return 0;
    uint32_t stack = g_esp, scratch = stack - 64;
    if (!void_vm_writable(scratch, 64)) return 0;
    MEM32(scratch) = (uint32_t)lx;
    MEM32(scratch + 4) = (uint32_t)ly;
    g_esp = scratch;
    uint32_t points[2] = { scratch, scratch + 4 };
    void_call_guest_this(convert, parent, 2, points);
    uint32_t args[3] = { id, MEM32(scratch), MEM32(scratch + 4) };
    g_esp = stack;
    return (void_call_guest_this(0xA05C929Eu, pie, 3, args) & 255) != 0;
}

/* Any item (or the head) the menu hits directly at a game pixel. */
static int touch_pie_hits(uint32_t pie, double factor, int x, int y)
{
    int lx = (int)(x / factor), ly = (int)(y / factor);
    if (touch_pie_direct(pie, MEM32(pie + 0x1A0), lx, ly)) return 1;
    uint32_t sentinel = MEM32(pie + 0x254);
    if (!sentinel || !void_vm_readable(sentinel, 4)) return 0;
    uint32_t node = MEM32(sentinel);
    for (unsigned n = 0; node && node != sentinel && n < 64 && void_vm_readable(node, 0x6C); ++n) {
        if (touch_pie_direct(pie, MEM32(node + 0x68), lx, ly)) return 1;
        node = MEM32(node);
    }
    return 0;
}

static void touch_window_center(uint32_t window, double factor, int *x, int *y)
{
    int left = (int)MEM32(window + 0x24), top = (int)MEM32(window + 0x28);
    float sx = (left + ((int)MEM32(window + 0x2C) - left) / 2) * (float)factor;
    float sy = (top + ((int)MEM32(window + 0x30) - top) / 2) * (float)factor;
    /* The inverse input transform truncates to an integer logical pixel. */
    *x = (int)sx; *y = (int)sy;
    *x += *x < sx; *y += *y < sy;
}

static unsigned pie_pick_logs;

/* Logs the first open menus' windows and item parts, for touch diagnostics. */
static void touch_pie_describe(uint32_t pie, uint32_t manager)
{
    static uint32_t described[2];
    static unsigned count;
    for (unsigned i = 0; i < count; ++i) if (described[i] == pie) return;
    if (count == 2) return;
    described[count++] = pie;
    void_log("[touch-pie] menu %08X rect=%d,%d,%d,%d center=%d,%d cancel=%d gesture=%d factor=%.3f", pie,
        (int)MEM32(pie + 0x24), (int)MEM32(pie + 0x28), (int)MEM32(pie + 0x2C), (int)MEM32(pie + 0x30),
        (int)MEM32(pie + 0x108), (int)MEM32(pie + 0x10C), (int)MEM32(pie + 0x100), (int)MEM32(pie + 0x104), s2_ui_factor());
    uint32_t list = MEM32(pie + 0x54);
    if (!list || !void_vm_readable(list, 12)) return;
    uint32_t head = MEM32(list), node = head && void_vm_readable(head, 12) ? MEM32(head) : 0;
    for (unsigned n = 0; node && node != head && n < 32 && void_vm_readable(node, 12); ++n) {
        uint32_t child = MEM32(node + 8);
        if (child && void_vm_readable(child, 0xEC) && MEM32(child + 0x58) == pie) {
            uint32_t label = touch_pie_item(child, pie) ? touch_pie_label(child) : 0;
            void_log("[touch-pie]   child %08X vt=%08X id=%08X flags=%X rect=%d,%d,%d,%d item=%d label=%08X %d,%d,%d,%d mgr=%d",
                child, MEM32(child), MEM32(child + 0x10), MEM32(child + 0xE8),
                (int)MEM32(child + 0x24), (int)MEM32(child + 0x28), (int)MEM32(child + 0x2C), (int)MEM32(child + 0x30),
                touch_pie_item(child, pie), label,
                label ? (int)MEM32(label + 0x24) : 0, label ? (int)MEM32(label + 0x28) : 0,
                label ? (int)MEM32(label + 0x2C) : 0, label ? (int)MEM32(label + 0x30) : 0, MEM32(child + 4) == manager);
            uint32_t sub = MEM32(child + 0x54);
            uint32_t sub_head = sub && void_vm_readable(sub, 12) ? MEM32(sub) : 0;
            uint32_t sub_node = sub_head && void_vm_readable(sub_head, 12) ? MEM32(sub_head) : 0;
            for (unsigned k = 0; sub_node && sub_node != sub_head && k < 8 && void_vm_readable(sub_node, 12); ++k) {
                uint32_t grand = MEM32(sub_node + 8);
                if (grand && void_vm_readable(grand, 0xEC))
                    void_log("[touch-pie]     part %08X vt=%08X id=%08X flags=%X rect=%d,%d,%d,%d parent=%d kids=%d", grand,
                        MEM32(grand), MEM32(grand + 0x10), MEM32(grand + 0xE8), (int)MEM32(grand + 0x24), (int)MEM32(grand + 0x28),
                        (int)MEM32(grand + 0x2C), (int)MEM32(grand + 0x30), MEM32(grand + 0x58) == child, MEM32(grand + 0x54) != 0);
                sub_node = MEM32(sub_node);
            }
        }
        node = MEM32(node);
    }
}

/* Where a tap reaches an open menu, in game pixels: 1 when the menu itself hits
 * an item or the head there (the point is kept), 2 for an item row's own area
 * or a near miss (moved to the centre of its label), 0 for none. The bubble and
 * stem images are no direct hit, and the rows' rectangles overlap: the row
 * whose label is nearest wins, and only a label centre the menu hits counts.
 * `side` is the touch target in game pixels. */
static int touch_pie_pick(uint32_t pie, uint32_t manager, int *x, int *y, int side)
{
    double factor = s2_ui_factor();
    if (factor < 1 || factor > 8 || side < 1 || side > 512) return 0;
    if (touch_pie_hits(pie, factor, *x, *y)) return 1;
    double px = *x / factor, py = *y / factor, minimum = side / factor;
    uint32_t labels[8];
    double scores[8];
    unsigned count = 0;
    uint32_t list = MEM32(pie + 0x54);
    if (!list || !void_vm_readable(list, 12)) return 0;
    uint32_t head = MEM32(list), node = head && void_vm_readable(head, 12) ? MEM32(head) : 0;
    for (unsigned n = 0; node && node != head && n < 64 && void_vm_readable(node, 12); ++n) {
        uint32_t child = MEM32(node + 8);
        node = MEM32(node);
        if (!child || !void_vm_readable(child, 0xEC) || MEM32(child + 0x58) != pie || MEM32(child + 4) != manager ||
            (MEM32(child + 0xE8) & 3) != 3 || (MEM32(child + 0xE8) & 0x200000)) continue;
        uint32_t label = touch_pie_label(child);
        if (!label || MEM32(label + 4) != manager) continue;
        int left = (int)MEM32(label + 0x24), top = (int)MEM32(label + 0x28);
        int right = (int)MEM32(label + 0x2C), bottom = (int)MEM32(label + 0x30);
        int width = right - left, height = bottom - top;
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) continue;
        double cx = left + width * .5, cy = top + height * .5;
        double half_w = fmax(width, minimum) * .5 + minimum / 8, half_h = fmax(height, minimum) * .5 + minimum / 8;
        if (!touch_pie_rect(child, px, py, 0) && (fabs(px - cx) > half_w || fabs(py - cy) > half_h)) continue;
        double dx = px < left ? left - px : px > right ? px - right : 0;
        double dy = py < top ? top - py : py > bottom ? py - bottom : 0;
        double score = dx * dx + dy * dy;
        unsigned at = count < 8 ? count++ : 8;
        while (at > 0 && scores[at - 1] > score) {
            if (at < 8) { scores[at] = scores[at - 1]; labels[at] = labels[at - 1]; }
            --at;
        }
        if (at < 8) { scores[at] = score; labels[at] = label; }
    }
    for (unsigned i = 0; i < count; ++i) {
        int tx, ty;
        touch_window_center(labels[i], factor, &tx, &ty);
        /* Only a point the menu itself resolves to an item. */
        if (!touch_pie_hits(pie, factor, tx, ty)) continue;
        *x = tx; *y = ty;
        return 2;
    }
    return 0;
}

/* The click that closes the menu: a press outside its rectangle grown by
 * kCancelBoundary cancels it (A05C816C), and the release then reaches the
 * window under the cursor. Over Live HUD controls that release swallows the next
 * tap, so the point is the nearest on-screen one over the lot (the scene) when
 * there is one, else the nearest outside point. 0 when there is none. */
static int touch_pie_outside(uint32_t pie, uint32_t root, int *x, int *y)
{
    double factor = s2_ui_factor();
    int width, height;
    void_input_screen_size(&width, &height);
    if (factor < 1 || factor > 8 || width < 1 || height < 1 || !void_vm_readable(pie, 0x104)) return 0;
    int boundary = (int)MEM32(pie + 0x100);
    if (boundary < 0 || boundary > 512) return 0;
    /* Logical pixels, as the menu sees the cursor; a margin absorbs rounding. */
    double grow = boundary + 4;
    double left = (int)MEM32(pie + 0x24) - grow, top = (int)MEM32(pie + 0x28) - grow;
    double right = (int)MEM32(pie + 0x2C) + grow, bottom = (int)MEM32(pie + 0x30) + grow;
    double px = *x / factor, py = *y / factor, w = (width - 1) / factor, h = (height - 1) / factor;
    if (!(left < right && top < bottom)) return 0;
    double cx[20], cy[20], cd[20];
    unsigned count = 0;
    if (px < left || px > right || py < top || py > bottom) { cx[0] = px; cy[0] = py; cd[0] = 0; count = 1; }
    static const double steps[4] = { 0, 40, 100, 200 };
    for (unsigned step = 0; step < 4; ++step) {
        double ex[4] = { left - steps[step], right + steps[step], px, px };
        double ey[4] = { py, py, top - steps[step], bottom + steps[step] };
        for (unsigned i = 0; i < 4; ++i) {
            if (ex[i] < 0 || ey[i] < 0 || ex[i] > w || ey[i] > h) continue;
            double d = fabs(ex[i] - px) + fabs(ey[i] - py);
            unsigned at = count++;
            while (at > 0 && cd[at - 1] > d) { cx[at] = cx[at - 1]; cy[at] = cy[at - 1]; cd[at] = cd[at - 1]; --at; }
            cx[at] = ex[i]; cy[at] = ey[i]; cd[at] = d;
        }
    }
    if (!count) return 0;
    unsigned chosen = 0;
    for (unsigned i = 0; i < count; ++i) {
        /* Both coordinates are on screen (non-negative): truncation is floor. */
        int gx = (int)(cx[i] * factor + (cx[i] < px ? 0 : cx[i] > px ? 1 : .5));
        int gy = (int)(cy[i] * factor + (cy[i] < py ? 0 : cy[i] > py ? 1 : .5));
        if (gx >= width) gx = width - 1;
        if (gy >= height) gy = height - 1;
        uint32_t hit = s2_ui_pick(root, gx, gy);
        if (hit && (hit == scene_window || !strcmp(touch_type(hit), ".?AVcTSWinSceneGraph@@"))) { chosen = i; break; }
    }
    int gx = (int)(cx[chosen] * factor + (cx[chosen] < px ? 0 : cx[chosen] > px ? 1 : .5));
    int gy = (int)(cy[chosen] * factor + (cy[chosen] < py ? 0 : cy[chosen] > py ? 1 : .5));
    *x = gx < 0 ? 0 : gx >= width ? width - 1 : gx;
    *y = gy < 0 ? 0 : gy >= height ? height - 1 : gy;
    return 1;
}

static uint32_t touch_ui_target(uint32_t root, uint32_t hit, int *x, int *y, int side)
{
    double factor = s2_ui_factor();
    uint32_t manager = s2_touch_owner_manager();
    unsigned context = s2_touch_context(), epoch = s2_touch_layout_epoch();
    if (factor < 1 || side < 1 || side > 512 || !manager || !void_vm_readable(manager, 0x48) ||
        MEM32(manager + 0x44) != root) return 0;
    uint32_t capture = MEM32(manager + 0x34), pie = touch_pie_owner(capture ? capture : hit);
    if (capture && !pie) return 0;
    for (uint32_t parent = hit, depth = 0; parent && depth < 16 && void_vm_readable(parent, 0xEC); ++depth) {
        if (pie ? parent != hit && touch_pie_item(parent, pie) && touch_pie_label(parent) == hit
                : touch_ui_button(parent)) return 0;
        parent = MEM32(parent + 0x58);
    }
    if (!pie && hit && (touch_widget(hit) & (4 | 8 | 16))) return 0;
    int px = (int)(*x / factor), py = (int)(*y / factor);
    double minimum = side / factor, best_distance = 1e30;
    uint32_t stack[512] = {pie ? pie : root}, best = 0;
    unsigned pending = 1, visited = 0;
    int best_x = 0, best_y = 0;
    while (pending && ++visited <= 4096) {
        uint32_t window = stack[--pending];
        if (!void_vm_readable(window, 0xEC) || MEM32(window + 4) != manager || !(MEM32(window + 0xE8) & 1)) continue;
        uint32_t parent = MEM32(window + 0x58);
        uint32_t row_menu = pie;
        if (!row_menu && !(context & (S2_TOUCH_MODAL | S2_TOUCH_OPTIONS | S2_TOUCH_LOT_INFO)) &&
            (!hit || hit == root || hit == scene_window) && void_vm_readable(parent, 4) &&
            MEM32(parent) == 0xA0DDA24Cu) row_menu = touch_pie_owner(parent);
        if (row_menu ? (MEM32(window + 0xE8) & 3) == 3 && touch_pie_item(window, row_menu)
                     : touch_ui_button(window)) {
            /* The native pie hit test requires a child of the row, including its label. */
            uint32_t bounds = row_menu ? touch_pie_label(window) : window;
            if (!bounds) continue;
            int left = (int)MEM32(bounds + 0x24), top = (int)MEM32(bounds + 0x28);
            int right = (int)MEM32(bounds + 0x2C), bottom = (int)MEM32(bounds + 0x30);
            if (left < -8192 || top < -8192 || right > 8192 || bottom > 8192 || right < left || bottom < top) continue;
            int width = right - left, height = bottom - top;
            if (width > 0 && height > 0 && width <= 8192 && height <= 8192 &&
                (row_menu || width < minimum || height < minimum)) {
                double cx = (left + right) * .5, cy = (top + bottom) * .5;
                double half_w = fmax(width, minimum) * .5, half_h = fmax(height, minimum) * .5;
                if (row_menu) { half_w += minimum / 8; half_h += minimum / 8; }
                double distance = (px - cx) * (px - cx) + (py - cy) * (py - cy);
                if (fabs(px - cx) <= half_w && fabs(py - cy) <= half_h && distance < best_distance) {
                    float sx = (left + width / 2) * (float)factor, sy = (top + height / 2) * (float)factor;
                    /* The inverse input transform truncates to an integer logical pixel. */
                    int tx = (int)sx, ty = (int)sy;
                    tx += tx < sx;
                    ty += ty < sy;
                    uint32_t picked = s2_ui_pick(root, tx, ty), ancestor = picked;
                    if (s2_touch_context() != context || s2_touch_layout_epoch() != epoch ||
                        s2_touch_owner_manager() != manager || MEM32(manager + 0x44) != root ||
                        MEM32(manager + 0x34) != capture) return 0;
                    for (unsigned depth = 0; ancestor && depth < 16 && void_vm_readable(ancestor, 0xEC); ++depth) {
                        if (ancestor == window) break;
                        ancestor = MEM32(ancestor + 0x58);
                    }
                    if (ancestor != window && !row_menu) {
                        uint32_t target_parent = 0, band_parent = 0;
                        const uint32_t *set = touch_vertical_set(window, &target_parent);
                        uint32_t band = set ? touch_vertical_band(root, tx, ty) : 0;
                        if (band && touch_vertical_set(band, &band_parent) == set && target_parent == band_parent) ancestor = window;
                    }
                    if (ancestor == window && (!row_menu || picked != window)) {
                        best = window; best_x = tx; best_y = ty; best_distance = distance;
                    }
                }
            }
        }
        if (pie && window != pie) continue;
        /* Original cGZWin child list: +54 COW header, sentinel nodes carry the child at +8. */
        uint32_t list = MEM32(window + 0x54);
        if (!list || !void_vm_readable(list, 12)) continue;
        uint32_t head = MEM32(list), node = head && void_vm_readable(head, 12) ? MEM32(head) : 0;
        for (unsigned n = 0; node && node != head && n < 4096 && void_vm_readable(node, 12); ++n) {
            uint32_t child = MEM32(node + 8);
            if (child && void_vm_readable(child, 0xEC) && MEM32(child + 0x58) == window) {
                if (pending == 512) return 0;
                stack[pending++] = child;
            }
            node = MEM32(node);
        }
        if (node && node != head) return 0;
    }
    if (pending || s2_touch_context() != context || s2_touch_layout_epoch() != epoch ||
        s2_touch_owner_manager() != manager || MEM32(manager + 0x44) != root ||
        MEM32(manager + 0x34) != capture) return 0;
    if (best) {
        if (ui_target_logs++ < 32) void_log("[touch-ui-target] %d,%d -> %d,%d button=%08X side=%d scale=%.3f visited=%u",
            *x, *y, best_x, best_y, MEM32(best + 0x10), side, factor, visited);
        *x = best_x; *y = best_y;
    }
    return best;
}


static void touch_log_procs(uint32_t window)
{
    if (!void_vm_readable(window, 0xA4)) return;
    uint32_t head = MEM32(window + 0xA0);
    if (!head || !void_vm_readable(head, 4)) return;
    uint32_t node = MEM32(head);
    for (unsigned n = 0; node && node != head && n < 16 && void_vm_readable(node, 12); ++n) {
        uint32_t proc = MEM32(node + 8);
        void_log("[touch] proc %u object=%08X type=%s", n, proc, touch_type(proc));
        node = MEM32(node);
    }
}

static int touch_cas_grid_apply(uint32_t proc, uint32_t window, int amount, unsigned context, uint64_t size, unsigned epoch)
{
    if (!amount || amount < -8 || amount > 8) return 0;
    int applied = 0;
    for (int n = 0; n < (amount < 0 ? -amount : amount); ++n) {
        if (!touch_layout_matches(context, size, epoch) || context != S2_TOUCH_CAS) break;
        uint32_t owner = touch_cas_grid_proc(proc, window);
        if (!owner) break;
        uint32_t id = MEM32(owner + (amount < 0 ? 0x1C4 : 0x1C8));
        if (!id) break;
        uint32_t arrow = void_call_guest_this(MEM32(MEM32(owner) + 0x9C), owner, 1, &id);
        if (!touch_layout_matches(context, size, epoch) || touch_cas_grid_proc(proc, window) != owner ||
            !arrow || !void_vm_readable(arrow, 0xEC) || MEM32(arrow + 0x10) != id ||
            (MEM32(arrow + 0xE8) & 3) != 3) break;
        /* Registered callbacks retain each CAS section's page size, arrows and scrollbar ID. */
        if (!(void_call_guest_this(0xA0755C58u, owner, 1, &id) & 255)) break;
        applied = 1;
    }
    return applied;
}

static int touch_grid_apply(uint32_t proc, uint32_t window, int amount)
{
    if (!touch_grid_proc(proc) || MEM32(proc + 0x54) != window || !amount || amount < -8 || amount > 8) return 0;
    uint32_t current = MEM32(proc + 0x20), count = MEM32(proc + 0x24), step = MEM32(proc + 0x28);
    if (current > 1000000 || !count || count > 512 || step > 512) return 0;
    /* A07479F0 multiplies the position by +28 (or a whole page at zero).
     * Keep one-item steps for touch catalogs; native arrows use the same position. */
    if (s2_touch_context() != S2_TOUCH_CAS && step != 1) {
        current *= step ? step : count;
        if (current > 1000000 || !void_vm_writable(proc, 0x2C)) return 0;
        MEM32(proc + 0x20) = current;
        MEM32(proc + 0x28) = 1;
    }
    int64_t page = (int64_t)current - amount;
    if (page < 0) page = 0;
    uint32_t next = (uint32_t)page;
    if (next != current) void_call_guest_this(0xA07479F0u, proc, 1, &next);
    return 1;
}

static int touch_catalog_apply(int x, int y, int amount, int info, unsigned context, uint64_t size, unsigned epoch)
{
    if (!touch_layout_matches(context, size, epoch) || !win_manager ||
        !void_vm_readable(win_manager, 0x48) || MEM32(win_manager + 0x34)) return 0;
    uint32_t manager = win_manager, root = MEM32(manager + 0x44);
    if (!root || !void_vm_readable(root, 0xEC) || !void_vm_readable(MEM32(root), 0xC0)) return 0;
    if (!info && context == S2_TOUCH_CHOOSER) {
        uint32_t chooser = touch_chooser_window(root);
        return chooser && touch_swipe_apply(chooser, amount, context, size, epoch);
    }
    if (!info) {
        uint32_t vertical = touch_vertical_band(root, x, y);
        if (vertical) return touch_swipe_apply(vertical, amount, context, size, epoch);
        uint32_t clock = touch_time_band(root, x, y);
        if (clock) return touch_swipe_apply(clock, amount, context, size, epoch);
    }
    if (info == 2) {
        static const uint32_t ids[2][9] = {
            {0xCC607031u,0xCC607032u,0xCC607030u,0xAEu,0xCC607035u,0xCC6070AFu,0xCC607036u,0xCC607033u,0xCC607034u},
            {0xA5u,0xA4u,0xABu,0xAEu,0xAAu,0xAFu,0xA3u,0xA1u,0xA2u}
        };
        if ((context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) || amount < 1 || amount > 9) return 0;
        unsigned build = context == S2_TOUCH_BUILD, action = (unsigned)amount - 1;
        uint32_t owner_id = build ? 0x17D986A3u : 0x37D986A2u;
        uint32_t toolbar = void_call_guest_this(MEM32(MEM32(root) + 0x9C), root, 1, &owner_id);
        if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
            MEM32(manager + 0x44) != root || MEM32(manager + 0x34) || !void_vm_readable(toolbar, 0xEC) ||
            (MEM32(toolbar) != 0xA0DE3704u && MEM32(toolbar) != 0xA0E54498u) || MEM32(toolbar + 4) != manager || MEM32(toolbar + 0x10) != owner_id ||
            !(MEM32(toolbar + 0xE8) & 1) || !void_vm_readable(MEM32(toolbar), 0xA0)) return 0;
        uint32_t id = ids[build][action];
        uint32_t target = void_call_guest_this(MEM32(MEM32(toolbar) + 0x9C), toolbar, 1, &id);
        for (unsigned stage = 0; stage < 2; ++stage) {
            if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
                MEM32(manager + 0x44) != root || MEM32(manager + 0x34) || !void_vm_readable(toolbar, 0xEC) ||
                MEM32(toolbar + 4) != manager || MEM32(toolbar + 0x10) != owner_id || !(MEM32(toolbar + 0xE8) & 1) ||
                !touch_ui_button(target) || MEM32(target + 4) != manager || MEM32(target + 0x58) != toolbar ||
                MEM32(target + 0x10) != id || target < 4 || !void_vm_readable(target - 4, 0x100) ||
                MEM32(target - 4) != 0xA0E53E14u) return 0;
            if (stage == 0 && action < 7 && !(build && action == 4)) {
                uint32_t toggle[] = { action < 4 ? 1 : !(MEM32(target + 0xF8) & 1), 1 };
                void_call_guest_this(0xA0936F94u, target - 4, 2, toggle);
            }
        }
        void_call_guest_this(0xA093550Au, target - 4, 0, NULL);
        return 1;
    }
    uint32_t window = touch_input_pick(root, x, y);
    if (!touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager ||
        !void_vm_readable(manager, 0x48) || MEM32(manager + 0x44) != root || MEM32(manager + 0x34)) return 0;
    for (unsigned depth = 0; window && depth < 16 && void_vm_readable(window, 0xA4); ++depth) {
        int index;
        if (!info && touch_swipe_group(window, &index)) return touch_swipe_apply(window, amount, context, size, epoch);
        uint32_t town_catalog = touch_town_catalog_proc(window);
        if (!info && town_catalog) return touch_grid_apply(town_catalog, MEM32(town_catalog + 0x54), amount);
        uint32_t bound = touch_bound_catalog_proc(window);
        if (bound && info && MEM32(bound) == 0xA0E023D0u) return s2_catalog_info(bound);
        if (bound && !info && touch_grid_proc(bound)) return touch_grid_apply(bound, window, amount);
        uint32_t head = MEM32(window + 0xA0);
        uint32_t node = head && void_vm_readable(head, 4) ? MEM32(head) : 0;
        for (unsigned n = 0; node && node != head && n < 64 && void_vm_readable(node, 12); ++n) {
            uint32_t proc = MEM32(node + 8);
            const char *type = touch_type(proc);
            if (info && !strcmp(type, ".?AVcTSWinProcCatalogThumbnail@@"))
                return s2_catalog_info(proc);
            if (!info && touch_cas_grid_proc(proc, window))
                return touch_cas_grid_apply(proc, window, amount, context, size, epoch);
            if (!info && touch_grid_proc(proc) && MEM32(proc + 0x54) == window)
                return touch_grid_apply(proc, window, amount);
            node = MEM32(node);
        }
        window = MEM32(window + 0x58);
    }
    return 0;
}

static int touch_fling_apply(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch, uint64_t owner)
{
    uint32_t manager = s2_touch_owner_manager(), root = (uint32_t)(owner >> 32), window = (uint32_t)owner;
    if (!amount || amount < -8 || amount > 8 || !touch_layout_matches(context, size, epoch) || !manager ||
        !root || !window || MEM32(manager + 0x44) != root || MEM32(manager + 0x34) ||
        !void_vm_readable(root, 0xEC) || !void_vm_readable(MEM32(root), 0xC0) || touch_input_pick(root, x, y) != window ||
        !touch_layout_matches(context, size, epoch) || s2_touch_owner_manager() != manager || MEM32(manager + 0x44) != root) return 0;
    unsigned flags = touch_widget(window);
    if (!(flags & 8) || (flags & (4 | 64 | 256 | 512))) return 0;
    if (fling_logs++ < 24) void_log("[touch-fling] window=%08X amount=%d flags=%X", window, amount, flags);
    if (flags & 32) return touch_catalog_apply(x, y, amount, 0, context, size, epoch);
    void_input_mouse_warp(x, y);
    for (int n = 0; n < (amount < 0 ? -amount : amount); ++n) void_input_mouse_wheel(amount > 0 ? 120 : -120);
    return 1;
}

void sub_A05BDFD7_lifted(void);
void sub_A05BDFD7(void)
{
    uint32_t self = g_ecx, window = MEM32(g_esp + 4);
    sub_A05BDFD7_lifted();
    if (g_eax & 0xFF) {
        scene_owner = self;
        scene_window = window;
        s2_fit_scene_attached();
    }
    void_log("[touch] scene attach self=%08X type=%s window=%08X result=%08X", self, touch_type(self), window, g_eax);
}

void sub_A05BE0F2_lifted(void);
void sub_A05BE0F2(void)
{
    if (scene_owner == g_ecx) { scene_owner = 0; scene_window = 0; s2_touch_reset_camera(); }
    sub_A05BE0F2_lifted();
}

void sub_A0973692_lifted(void);
void sub_A0973692(void)
{
    uint32_t self = g_ecx;
    sub_A0973692_lifted();
    /* Init/Shutdown use the +4 COM interface; mouse dispatch uses the base. */
    win_manager = self - 4;
    win_manager_thread = void_thread_id();
    touch_tool_clear();
}

void sub_A0973E55_lifted(void);
void sub_A0973E55(void)
{
    if (win_manager == g_ecx - 4) { s2_touch_reset_cas_pan(); s2_touch_text_pump(win_manager); win_manager = 0; touch_tool_clear(); s2_touch_input_context(0); s2_touch_reset_camera(); last_world_context = 0; }
    sub_A0973E55_lifted();
}

void s2_touch_request_pick(int x, int y)
{
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    if (slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED || ui_command_kind || ui_command_started) { pthread_mutex_unlock(&pick_lock); return; }
    pick_x = x; pick_y = y; pick_radius = 0; pick_pending = 1; pick_diagnostic = 1; ++pick_serial;
    if (!pick_serial) ++pick_serial;
    if (!void_input_touch_barrier(pick_serial)) pick_pending = 0;
    pthread_mutex_unlock(&pick_lock);
}

static int touch_query(int x, int y, int radius, uint64_t *point, uint64_t key, s2_world_targets *targets)
{
    struct timespec until, started = {0};
    int timed = clock_gettime(CLOCK_MONOTONIC, &started) == 0;
    if (clock_gettime(CLOCK_REALTIME, &until)) return 0;
    /* A fresh owner pick can take two frames at 5 FPS; expiry still only cancels. */
    until.tv_nsec += 500000000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    if (slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED || ui_command_kind || ui_command_started) {
        int trace = query_zero_logs < 16;
        if (trace) ++query_zero_logs;
        unsigned busy = slider_serial;
        pthread_mutex_unlock(&pick_lock);
        if (trace) void_log("[touch-query] zero reason=sliderBusy point=%d,%d sliderSerial=%u", x, y, busy);
        return 0;
    }
    unsigned serial = ++pick_serial;
    if (!serial) serial = ++pick_serial;
    pick_x = x; pick_y = y; pick_radius = radius; pick_world_key = key; pick_pending = 1; pick_diagnostic = 0;
    int queued = void_input_touch_barrier(serial);
    int wait_result = 0;
    while (queued && pick_done != serial) {
        wait_result = pthread_cond_timedwait(&pick_ready, &pick_lock, &until);
        if (wait_result) break;
    }
    int result = pick_done == serial ? pick_result : 0;
    if (point && pick_done == serial) *point = pick_point;
    if (targets && (result & 3) == 2) {
        targets->count = pick_world.count;
        memcpy(targets->items, pick_world.items, targets->count * sizeof targets->items[0]);
    }
    unsigned done = pick_done, current = pick_serial, owner_context = done == serial ? pick_owner_context : 0;
    uint32_t owner_root = done == serial ? pick_owner_root : 0, owner_hit = done == serial ? pick_owner_hit : 0;
    int pending = pick_pending, trace = !result && query_zero_logs < 16;
    if (trace) ++query_zero_logs;
    if (pick_done != serial && pick_serial == serial) { pick_pending = 0; ++pick_serial; }
    pthread_mutex_unlock(&pick_lock);
    if (trace) {
        struct timespec now;
        long long elapsed = timed && !clock_gettime(CLOCK_MONOTONIC, &now) ?
            (long long)(now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000 : -1;
        void_log("[touch-query] zero serial=%u point=%d,%d queued=%d wait=%d ms=%lld done=%u current=%u pending=%d ownerContext=%X root=%08X hit=%08X",
            serial, x, y, queued, wait_result, elapsed, done, current, pending, owner_context, owner_root, owner_hit);
    }
    return result;
}

int s2_touch_pick(int x, int y) { return touch_query(x, y, 0, NULL, 0, NULL); }

int s2_touch_ui_pick(int x, int y, int side, uint64_t *point)
{
    return point && side > 0 && side <= 512 ? touch_query(x, y, -side, point, 0, NULL) : 0;
}

int s2_touch_world_targets(int x, int y, int radius, s2_world_targets *targets)
{
    if (radius < 1) radius = 1;
    if (radius > S2_WORLD_PICK_RADIUS) radius = S2_WORLD_PICK_RADIUS;
    return (touch_query(x, y, radius, NULL, 0, targets) & 3) == 2;
}

int s2_touch_world_confirm(int x, int y, uint64_t key)
{
    return key && (touch_query(x, y, 1, NULL, key, NULL) & 3) == 2;
}

static int touch_world_owner(uint32_t manager, uint32_t tool, uint32_t root, unsigned epoch)
{
    return s2_touch_context() == S2_TOUCH_LIVE && s2_touch_layout_epoch() == epoch &&
        win_manager == manager && void_vm_readable(manager, 0x48) &&
        MEM32(manager + 0x44) == root && !MEM32(manager + 0x34) &&
        tool_handler && void_vm_readable(tool_handler, 0x24) && MEM32(tool_handler + 0x20) == tool;
}

static uint64_t touch_world_object(uint32_t node, uint32_t hit_position)
{
    if (!node || !void_vm_readable(node, 4) || !void_vm_readable(MEM32(node), 0x1EC)) return 0;
    /* The original click resolves the scene hit to a simulation object at the hit point. */
    uint32_t object = void_call_guest_this(MEM32(MEM32(node) + 0x1E8), node, 1, &hit_position);
    if (!object || !void_vm_readable(object, 4) || !void_vm_readable(MEM32(object), 0x4AC)) return 0;
    uint32_t table = MEM32(object), slot = 59;
    if (void_call_guest_this(MEM32(table + 0x188), object, 0, NULL) == 19 &&
        void_call_guest_this(MEM32(table + 0x34), object, 1, &slot) != 167) return 0;
    uint32_t primary = void_call_guest_this(MEM32(table + 0x1E0), object, 0, NULL);
    if (primary && void_vm_readable(primary, 4) && void_vm_readable(MEM32(primary), 0x4AC)) object = primary;
    uint16_t id = (uint16_t)void_call_guest_this(MEM32(MEM32(object) + 0xBC), object, 0, NULL);
    return (uint64_t)id << 32 | object;
}

static void touch_world_name(s2_world_target *target, uint32_t string)
{
    uint32_t object = (uint32_t)target->key;
    void_call_guest_this(0xA0144F5Bu, string, 0, NULL);
    void_call_guest_this(MEM32(MEM32(object) + 0x4A8), object, 1, &string);
    unsigned n = void_call_guest_this(0xA0149D00u, string, 0, NULL);
    uint32_t data = void_call_guest_this(0xA01ACD4Fu, string, 0, NULL);
    if (n && n < sizeof target->name && data && void_vm_readable(data, n))
        memcpy(target->name, GPTR(data), n);
    void_call_guest_this(0xA0141455u, string + 4, 0, NULL);
}

static uint32_t touch_scene_method(uint32_t node, unsigned offset)
{
    if (!node || !void_vm_readable(node, 4)) return 0;
    uint32_t table = MEM32(node);
    if (!table || !void_vm_readable(table, offset + 4)) return 0;
    uint32_t method = MEM32(table + offset);
    return void_dispatch_find(method) ? method : 0;
}

static uint32_t touch_world_member(uint32_t object, unsigned offset)
{
    uint32_t method = touch_scene_method(object, offset);
    return method ? void_call_guest_this(method, object, 0, NULL) : 0;
}

static uint32_t touch_world_guid(uint32_t object)
{
    uint32_t definition = touch_world_member(object, 0x100);
    if (!void_vm_readable(definition, 4) || MEM32(definition) != 0xA0DC3B30u) return 0;
    return void_call_guest_this(0xA041B670u, definition, 1, (uint32_t[]){201});
}

static int touch_world_phone_base(uint32_t object)
{
    switch (touch_world_guid(object)) {
    case 0x6C734D8Cu: case 0x6C7EF3B7u: case 0xECA6E041u:
    case 0x11243517u: case 0x3124350Fu: case 0x34250E7Eu: case 0x74250E9Cu:
        return 1;
    default: return 0;
    }
}

static int touch_world_phone_duplicate(const s2_world_targets *targets, uint32_t object)
{
    uint32_t guid = touch_world_guid(object);
    if (guid != 0x6C734DB3u && guid != 0xCCA8324Cu && guid != 0x112437F8u && guid != 0x342B59C8u) return 0;
    uint32_t holder = touch_world_member(object, 0x11C);
    uint32_t attribute = touch_scene_method(object, 0x3C);
    /* Create Handset stores the base's object ID in handset attribute zero. */
    uint16_t base = attribute && guid != 0xCCA8324Cu
        ? (uint16_t)void_call_guest_this(attribute, object, 1, (uint32_t[]){0}) : 0;
    for (unsigned i = 0; i < targets->count; ++i) {
        uint64_t key = targets->items[i].key;
        uint32_t candidate = (uint32_t)key;
        if (candidate != object && (candidate == holder || (base && base == (uint16_t)(key >> 32))) &&
            touch_world_phone_base(candidate)) return 1;
    }
    return 0;
}

static void touch_world_bounds(uint32_t node, uint32_t scratch, float bounds[6], unsigned *remaining, int root)
{
    uint32_t is_type = touch_scene_method(node, 0x64);
    if (!is_type || !*remaining) return;
    --*remaining;
    /* A slotted object has its own target and must not pull the supporting object's center. */
    if (!root && (void_call_guest_this(is_type, node, 1, (uint32_t[]){0xFA1C39EDu}) & 255)) return;

    /* cIRenderableNode extends cIBoundedNode. Ignore room, cutout and collision bounds. */
    uint32_t get_bounds = (void_call_guest_this(is_type, node, 1, (uint32_t[]){0x1C585E30u}) & 255)
        ? touch_scene_method(node, 0x120) : 0;
    if (get_bounds && void_call_guest_this(get_bounds, node, 1, &scratch) == scratch) {
        float part[6];
        memcpy(part, GPTR(scratch), sizeof part);
        int valid = 1, extent = 0;
        for (unsigned i = 0; i < 3; ++i) {
            valid &= isfinite(part[i + 3] - part[i]) && part[i] <= part[i + 3];
            extent |= part[i] < part[i + 3];
        }
        if (valid && extent) for (unsigned i = 0; i < 3; ++i) {
            bounds[i] = fminf(bounds[i], part[i]);
            bounds[i + 3] = fmaxf(bounds[i + 3], part[i + 3]);
        }
    }
    uint32_t first_child = touch_scene_method(node, 0x9C);
    uint32_t child = first_child ? void_call_guest_this(first_child, node, 0, NULL) : 0;
    while (child && *remaining) {
        uint32_t next_sibling = touch_scene_method(child, 0x98);
        unsigned before = *remaining;
        touch_world_bounds(child, scratch, bounds, remaining, 0);
        if (!next_sibling || *remaining == before) break;
        child = void_call_guest_this(next_sibling, child, 0, NULL);
    }
}

static void touch_world_anchor(s2_world_target *target, uint32_t node, uint32_t view, uint32_t scratch, int width, int height)
{
    if (!void_vm_readable(view, 16) || MEM32(view) != 0xA0DD8CB8u || !MEM32(view + 12)) return;
    float bounds[6] = { INFINITY, INFINITY, INFINITY, -INFINITY, -INFINITY, -INFINITY };
    unsigned remaining = 128;
    touch_world_bounds(node, scratch, bounds, &remaining, 1);
    /* shortcut: use the hit when the scene has no usable model bounds or the center is offscreen. */
    if (!remaining || !isfinite(bounds[0])) return;
    float center[3];
    for (unsigned i = 0; i < 3; ++i) center[i] = bounds[i] * .5f + bounds[i + 3] * .5f;
    memcpy(GPTR(scratch + 24), center, sizeof center);
    /* cTSWinSceneGraph projects world coordinates into the same screen space as its picker. */
    uint32_t args[] = { scratch, scratch + 24 };
    if (void_call_guest_this(0xA05BD6B6u, view, 2, args) != scratch) return;
    int x = (int)MEM32(scratch), y = (int)MEM32(scratch + 4);
    if (x >= 0 && x < width && y >= 0 && y < height) {
        target->anchor_x = x;
        target->anchor_y = y;
    }
}

static int touch_world_scan(uint32_t manager, uint32_t tool, uint32_t root, int x, int y,
    int radius, uint64_t expected, s2_world_targets *out)
{
    unsigned epoch = s2_touch_layout_epoch();
    uint32_t stack = g_esp, scratch = stack - 64;
    if (!tool || !void_vm_readable(tool, 8) || MEM32(tool) != 0xA0DE9374u ||
        !touch_world_owner(manager, tool, root, epoch) || !void_vm_writable(scratch, 64)) {
        if (!expected) {
            out->count = 1;
            out->items[0] = (s2_world_target){ .x = x, .y = y, .anchor_x = x, .anchor_y = y };
        }
        return expected == 0;
    }

    /* A single reported surface represents the terrain anywhere in the tap area. */
    s2_world_target ground = {0};
    int distances[S2_WORLD_TARGET_LIMIT];
    uint32_t nodes[S2_WORLD_TARGET_LIMIT];
    int ground_distance = radius * radius + 1;
    struct { uint32_t node; uint64_t key; } cache[32];
    unsigned cached = 0;
    int width, height;
    void_input_screen_size(&width, &height);
    g_esp = scratch;
    if (expected) radius = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            int sx = x + dx, sy = y + dy, distance = dx * dx + dy * dy;
            if (distance > radius * radius || sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
            /* shortcut: sample farther rings more coarsely to keep large taps bounded until bulk picking is available. */
            int step = distance > 32 * 32 ? 8 : distance > 16 * 16 ? 4 : distance > 8 ? 2 : 1;
            if (dx % step || dy % step) continue;
            uint32_t hit = s2_ui_pick(root, sx, sy);
            if (!touch_world_owner(manager, tool, root, epoch)) goto stale;
            if (!hit || (hit != scene_window && strcmp(touch_type(hit), ".?AVcTSWinSceneGraph@@"))) continue;

            uint32_t args[5] = { (uint32_t)sx, (uint32_t)sy, 0, scratch, 0 };
            uint32_t node = void_call_guest_this(0xA078D789u, tool, 5, args);
            uint64_t key = 0;
            if (node) {
                unsigned i = 0;
                while (i < cached && cache[i].node != node) ++i;
                if (i < cached) key = cache[i].key;
                else {
                    key = touch_world_object(node, scratch);
                    if (cached < sizeof cache / sizeof cache[0]) {
                        cache[cached].node = node;
                        cache[cached++].key = key;
                    }
                }
            }
            if (!key && distance < ground_distance &&
                (void_call_guest_this(0xA068C4C1u, tool, 3, (uint32_t[]){(uint32_t)sx, (uint32_t)sy, scratch}) & 255)) {
                ground = (s2_world_target){ .key = UINT64_MAX, .x = sx, .y = sy, .anchor_x = sx, .anchor_y = sy };
                ground_distance = distance;
            }
            if (expected) {
                g_esp = stack;
                return (key ? key == expected : ground.key == expected) && touch_world_owner(manager, tool, root, epoch);
            }
            if (!key) continue;
            unsigned i = 0;
            while (i < out->count && out->items[i].key != key) ++i;
            if (i == out->count) {
                ++out->count;
                distances[i] = radius * radius + 1;
            }
            if (distance < distances[i]) {
                out->items[i] = (s2_world_target){ .key = key, .x = sx, .y = sy, .anchor_x = sx, .anchor_y = sy };
                nodes[i] = node;
                distances[i] = distance;
            }
        }
    }
    if (expected || !touch_world_owner(manager, tool, root, epoch)) goto stale;
    for (unsigned i = 1; i < out->count; ++i) {
        s2_world_target target = out->items[i];
        uint32_t node = nodes[i];
        int distance = distances[i];
        unsigned j = i;
        for (; j > 0 && distance < distances[j - 1]; --j) {
            out->items[j] = out->items[j - 1];
            nodes[j] = nodes[j - 1];
            distances[j] = distances[j - 1];
        }
        out->items[j] = target;
        nodes[j] = node;
        distances[j] = distance;
    }
    if (out->count && void_vm_readable(0xA10CC288u, 4)) {
        /* StandardAction uses this test for the currently selected Sim. */
        uint32_t simulator = MEM32(0xA10CC288u);
        uint32_t actor = touch_world_member(touch_world_member(simulator, 104), 156);
        uint32_t service = actor ? touch_world_member(simulator, 60) : 0;
        uint32_t test = touch_scene_method(service, 72);
        struct timespec started;
        int timed = !clock_gettime(CLOCK_MONOTONIC, &started);
        unsigned count = 0;
        for (unsigned i = 0; test && i < out->count; ++i) {
            struct timespec now;
            /* shortcut: show confirmed options only, until action testing can be spread across frames. */
            if (i && timed && !clock_gettime(CLOCK_MONOTONIC, &now) &&
                (now.tv_sec - started.tv_sec) * 1000000000LL + now.tv_nsec - started.tv_nsec >= 120000000) break;
            uint32_t args[] = { actor, (uint32_t)out->items[i].key };
            int available = void_call_guest_this(test, service, 2, args) & 255;
            if (!touch_world_owner(manager, tool, root, epoch)) goto stale;
            if (!available) continue;
            out->items[count] = out->items[i];
            nodes[count++] = nodes[i];
        }
        if (test) out->count = count;
    }
    unsigned count = 0;
    for (unsigned i = 0; i < out->count; ++i) {
        if (out->count > 1 && touch_world_phone_duplicate(out, (uint32_t)out->items[i].key)) continue;
        out->items[count] = out->items[i];
        nodes[count++] = nodes[i];
    }
    out->count = count;
    if (!touch_world_owner(manager, tool, root, epoch)) goto stale;
    if (out->count > 1) {
        uint32_t view = MEM32(tool + 4);
        for (unsigned i = 0; i < out->count; ++i) {
            touch_world_anchor(&out->items[i], nodes[i], view, scratch, width, height);
            if (!touch_world_owner(manager, tool, root, epoch)) goto stale;
            touch_world_name(&out->items[i], scratch + 16);
            if (!touch_world_owner(manager, tool, root, epoch)) goto stale;
        }
    }
    /* A separate target choice is needed only when at least two objects were hit. */
    if (out->count == 0 || (out->count > 1 && ground.key)) out->items[out->count++] = ground.key ? ground :
        (s2_world_target){ .x = x, .y = y, .anchor_x = x, .anchor_y = y };
    g_esp = stack;
    return 1;

stale:
    out->count = 0;
    g_esp = stack;
    return 0;
}

static uint64_t slider_receipt(unsigned serial, int result, int before, int after)
{
    unsigned b = before >= -100 && before <= 100 ? (unsigned)(before + 101) : 0;
    unsigned a = after >= -100 && after <= 100 ? (unsigned)(after + 101) : 0;
    return (uint64_t)serial << 32 | (unsigned)result | b << 8 | a << 16;
}

static void slider_expire(void)
{
    struct timespec now;
    if (slider_state == SLIDER_QUEUED && (clock_gettime(CLOCK_MONOTONIC, &now) ||
        now.tv_sec > slider_expiry.tv_sec ||
        (now.tv_sec == slider_expiry.tv_sec && now.tv_nsec >= slider_expiry.tv_nsec))) {
        slider_state = SLIDER_CANCELLED; slider_result = S2_SLIDER_CANCELLED;
        pick_pending = 0; ++pick_serial;
        pthread_cond_broadcast(&pick_ready);
    }
}

uint64_t s2_touch_slider_status(unsigned serial)
{
    pthread_mutex_lock(&pick_lock);
    slider_expire();
    uint64_t result = serial && slider_serial == serial ? slider_receipt(serial,
        slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED ? S2_SLIDER_IN_PROGRESS : slider_result,
        slider_before, slider_after) : 0;
    pthread_mutex_unlock(&pick_lock);
    return result;
}

static uint64_t touch_begin_command(int x, int y, int delta, uint64_t tool, unsigned context, uint64_t size, unsigned epoch)
{
    struct timespec until, expiry;
    if (clock_gettime(CLOCK_REALTIME, &until) || clock_gettime(CLOCK_MONOTONIC, &expiry)) return S2_SLIDER_REJECTED;
    until.tv_nsec += 80000000;
    /* Tool commands outlive the UI wait: two slow frames must still reach the owner. */
    if (tool) expiry.tv_sec += 2; else expiry.tv_nsec += 80000000;
    if (until.tv_nsec >= 1000000000) { ++until.tv_sec; until.tv_nsec -= 1000000000; }
    if (expiry.tv_nsec >= 1000000000) { ++expiry.tv_sec; expiry.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    if (slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED || ui_command_kind || ui_command_started) { pthread_mutex_unlock(&pick_lock); return S2_SLIDER_BUSY; }
    unsigned serial = ++pick_serial;
    if (!serial) serial = ++pick_serial;
    slider_serial = serial; slider_state = SLIDER_QUEUED; slider_delta = delta;
    rotate_request.tool = tool; rotate_request.context = context;
    rotate_request.size = size; rotate_request.epoch = epoch;
    slider_result = S2_SLIDER_IN_PROGRESS; slider_before = slider_after = -101; slider_expiry = expiry;
    pick_x = x; pick_y = y; pick_pending = 1; pick_diagnostic = 0;
    int queued = void_input_touch_barrier(serial);
    while (queued && slider_serial == serial && (slider_state == SLIDER_QUEUED || slider_state == SLIDER_STARTED))
        if (pthread_cond_timedwait(&pick_ready, &pick_lock, &until)) break;
    if (slider_serial == serial && slider_state == SLIDER_QUEUED && (!queued || !tool)) {
        slider_state = SLIDER_CANCELLED; slider_result = S2_SLIDER_CANCELLED;
        pick_pending = 0; ++pick_serial;
    }
    uint64_t result = slider_serial == serial ? slider_receipt(serial, slider_result, slider_before, slider_after) :
        slider_receipt(serial, S2_SLIDER_UNKNOWN, -101, -101);
    pthread_mutex_unlock(&pick_lock);
    return result;
}

uint64_t s2_touch_slider_step(int x, int y, int delta)
{
    if (delta != -1 && delta != 1) return S2_SLIDER_REJECTED;
    return touch_begin_command(x, y, delta, 0, 0, 0, 0);
}

uint64_t s2_touch_rotate_object(int x, int y, uint64_t tool, unsigned context, uint64_t size, unsigned epoch, int direction)
{
    if ((direction != -1 && direction != 1) || (context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) || !tool || (uint32_t)tool != 2 ||
        !touch_layout_matches(context, size, epoch)) return S2_SLIDER_REJECTED;
    return touch_begin_command(x, y, direction > 0 ? 3 : 0, tool, context, size, epoch);
}

uint64_t s2_touch_fill_room(int x, int y, uint64_t tool, unsigned context, uint64_t size, unsigned epoch)
{
    if (context != S2_TOUCH_BUILD || !tool || (uint32_t)tool != 3 ||
        !touch_layout_matches(context, size, epoch)) return S2_SLIDER_REJECTED;
    return touch_begin_command(x, y, 2, tool, context, size, epoch);
}

/* Explicit touchpick diagnostics: query the original interface, never focus/set. */
static void touch_log_slider(uint32_t manager, uint32_t hit)
{
    if (strcmp(touch_type(hit), ".?AVcGZWinSlider@@") || !void_vm_readable(hit, 0x12D) ||
        !void_vm_readable(MEM32(hit), 0x29C)) return;
    uint32_t table = MEM32(hit), qi_fn = MEM32(table), slot = g_esp - 4;
    uint32_t slider = 0, iface_table = 0, value_fn = 0, min_fn = 0, max_fn = 0;
    int retained = 0, value = 0, minimum = 0, maximum = 0, read = 0, accept = -1, member = -1;
    if (qi_fn == 0xA0957FE3u && void_vm_writable(slot, 4)) {
        uint32_t old = MEM32(slot), args[2] = { 0x21325207u, slot };
        g_esp = slot; MEM32(slot) = 0;
        retained = void_call_guest_this(qi_fn, hit, 2, args) & 0xFFu;
        slider = MEM32(slot); MEM32(slot) = old; g_esp += 4;
        if (retained) {
            if (slider && void_vm_readable(slider, 0x14) && void_vm_readable(MEM32(slider), 0x28)) {
                iface_table = MEM32(slider); value_fn = MEM32(iface_table + 0x10);
                min_fn = MEM32(iface_table + 0x1C); max_fn = MEM32(iface_table + 0x24);
                if (value_fn == 0xA0957FA3u) { value = (int)void_call_guest_this(value_fn, slider, 0, NULL); read |= 1; }
                if (min_fn == 0xA0046390u) { minimum = (int)void_call_guest_this(min_fn, slider, 0, NULL); read |= 2; }
                if (max_fn == 0xA0145770u) { maximum = (int)void_call_guest_this(max_fn, slider, 0, NULL); read |= 4; }
            }
            uint32_t mask = 0x8000u;
            accept = void_call_guest_this(MEM32(table + 0x148), hit, 1, &mask) & 0xFFu;
            if (manager && void_vm_readable(manager, 4) && void_vm_readable(MEM32(manager), 0x5C))
                member = void_call_guest_this(MEM32(MEM32(manager) + 0x58), manager, 1, &hit) & 0xFFu;
            void_call_guest_this(MEM32(MEM32(hit) + 8), hit, 0, NULL);
        }
    }
    uint32_t focus = manager && void_vm_readable(manager, 0x3C) ? MEM32(manager + 0x38) : 0;
    void_log("[touch] slider win=%08X vtable=%08X qiFn=%08X qi=%d iface=%08X ifaceVtable=%08X valueFn=%08X minFn=%08X maxFn=%08X read=%X value=%d min=%d max=%d focus=%08X accept=%d member=%d",
        hit, table, qi_fn, retained, slider, iface_table, value_fn, min_fn, max_fn, read, value, minimum, maximum, focus, accept, member);
}

static int touch_slider_ready(uint32_t manager, uint32_t hit, int focused)
{
    unsigned context = s2_touch_context();
    return manager && manager == win_manager && void_vm_readable(manager, 0x48) &&
        (context == S2_TOUCH_CAS || (!focused && context == (S2_TOUCH_CAS | S2_TOUCH_FOCUSED_TEXT))) &&
        !MEM32(manager + 0x34) && hit && (!focused || MEM32(manager + 0x38) == hit) &&
        !strcmp(touch_type(hit), ".?AVcGZWinSlider@@") && void_vm_readable(hit, 0x12D) &&
        void_vm_readable(MEM32(hit), 0x29C) && MEM32(MEM32(hit)) == 0xA0957FE3u;
}

static int touch_slider_apply(uint32_t manager, uint32_t hit, int x, int y, int delta, int *before, int *after)
{
    const char *stage = "preFocus";
    uint32_t slider = 0, iface_table = 0, focus_return = 0, post_pick = 0;
    int retained = 0, minimum = 0, maximum = 0, current = 0, read = 0, accept_value = -1;
    int result = S2_SLIDER_REJECTED;
    if (!touch_slider_ready(manager, hit, 0)) goto slider_release;
    stage = "stack";
    uint32_t slot = g_esp - 4;
    if (!void_vm_writable(slot, 4)) goto slider_release;
    uint32_t old_slot = MEM32(slot);
    g_esp = slot; MEM32(slot) = 0;
    uint32_t qi[2] = { 0x21325207u, slot };
    stage = "QI";
    retained = void_call_guest_this(0xA0957FE3u, hit, 2, qi) & 0xFFu;
    slider = MEM32(slot);
    MEM32(slot) = old_slot; g_esp += 4;
    if (retained && slider == hit + 0xF8 && void_vm_readable(slider, 4) && void_vm_readable(MEM32(slider), 0x28)) {
        stage = "interface";
        uint32_t table = MEM32(slider);
        iface_table = table;
        if (MEM32(table + 0x10) == 0xA0957FA3u && MEM32(table + 0x14) == 0xA0958268u &&
            MEM32(table + 0x1C) == 0xA0046390u && MEM32(table + 0x24) == 0xA0145770u) {
            stage = "rangeBeforeFocus";
            minimum = (int)void_call_guest_this(0xA0046390u, slider, 0, NULL); read |= 1;
            if (minimum != 0 && minimum != -100) goto slider_release;
            maximum = (int)void_call_guest_this(0xA0145770u, slider, 0, NULL); read |= 2;
            if (maximum != 100) goto slider_release;
            uint32_t accept = 0x8000u;
            stage = "acceptBeforeFocus";
            accept_value = void_call_guest_this(MEM32(MEM32(hit) + 0x148), hit, 1, &accept) & 0xFFu;
            if (!accept_value) goto slider_release;
            if (MEM32(manager + 0x38) != hit) {
                stage = "focusMethod";
                if (!void_vm_readable(MEM32(manager), 0x9C) || MEM32(MEM32(manager) + 0x98) != 0xA09734CBu) goto slider_release;
                stage = "FocusReturn";
                focus_return = void_call_guest_this(0xA09734CBu, manager, 1, &hit);
                if (focus_return != hit) goto slider_release;
            }
            /* Focus callbacks can replace the UI; never reuse their old pick. */
            s2_touch_pump();
            stage = "postFocusGuard";
            if (!touch_slider_ready(manager, hit, 1)) goto slider_release;
            uint32_t root = MEM32(manager + 0x44), point[2] = { (uint32_t)x, (uint32_t)y };
            stage = "postRoot";
            if (!root || !void_vm_readable(root, 4) || !void_vm_readable(MEM32(root), 0xC0)) goto slider_release;
            stage = "postPick";
            post_pick = s2_ui_pick(root, x, y);
            if (post_pick != hit) goto slider_release;
            stage = "postPickGuard";
            if (!touch_slider_ready(manager, hit, 1)) goto slider_release;
            stage = "acceptAfterFocus";
            accept_value = void_call_guest_this(MEM32(MEM32(hit) + 0x148), hit, 1, &accept) & 0xFFu;
            if (!accept_value) goto slider_release;
            stage = "rangeAfterFocus";
            minimum = (int)void_call_guest_this(0xA0046390u, slider, 0, NULL); read |= 1;
            if (minimum != 0 && minimum != -100) goto slider_release;
            maximum = (int)void_call_guest_this(0xA0145770u, slider, 0, NULL); read |= 2;
            if (maximum != 100) goto slider_release;
            stage = "current";
            current = (int)void_call_guest_this(0xA0957FA3u, slider, 0, NULL); read |= 4;
            if (current >= minimum && current <= maximum && touch_slider_ready(manager, hit, 1)) {
                *before = *after = current;
                int desired = current + delta;
                if (desired < minimum) desired = minimum;
                if (desired > maximum) desired = maximum;
                result = S2_SLIDER_LIMIT;
                if (desired != current) {
                    uint32_t arg = (uint32_t)desired;
                    result = S2_SLIDER_FAILED;
                    stage = "setter";
                    if (void_call_guest_this(0xA0958268u, slider, 1, &arg) & 0xFFu) {
                        uint32_t table = MEM32(hit);
                        uint32_t id = void_call_guest_this(MEM32(table + 0x138), hit, 0, NULL);
                        uint32_t args[5] = { MEM32(hit + 0x5C), 3, 0x887113A4u, id, 0 };
                        /* Finish the original edit even if A3 changed focus/context. */
                        void_call_guest_this(MEM32(table + 0x298), hit, 5, args);
                        *after = (int)void_call_guest_this(0xA0957FA3u, slider, 0, NULL);
                        stage = "readback";
                        if (*after == desired) result = S2_SLIDER_CHANGED;
                    }
                }
            }
        }
    }
slider_release:
    if ((result == S2_SLIDER_REJECTED || result == S2_SLIDER_FAILED) && slider_reject_logs < 16) {
        ++slider_reject_logs;
        uint32_t focus = manager && void_vm_readable(manager, 0x3C) ? MEM32(manager + 0x38) : 0;
        uint32_t capture = manager && void_vm_readable(manager, 0x38) ? MEM32(manager + 0x34) : 0;
        uint32_t manager_table = manager && void_vm_readable(manager, 4) ? MEM32(manager) : 0;
        uint32_t focus_fn = manager_table && void_vm_readable(manager_table, 0x9C) ? MEM32(manager_table + 0x98) : 0;
        uint32_t table = hit && void_vm_readable(hit, 4) ? MEM32(hit) : 0;
        uint32_t qi_fn = table && void_vm_readable(table, 4) ? MEM32(table) : 0;
        void_log("[touch-step] reject stage=%s status=%d context=%X manager=%08X managerVtable=%08X focusFn=%08X hit=%08X type=%s focus=%08X capture=%08X qiFn=%08X qi=%d iface=%08X ifaceVtable=%08X read=%X min=%d max=%d current=%d accept=%d focusReturn=%08X postPick=%08X",
            stage, result, s2_touch_context(), manager, manager_table, focus_fn, hit, touch_type(hit), focus, capture, qi_fn, retained, slider, iface_table,
            read, minimum, maximum, current, accept_value, focus_return, post_pick);
    }
    if (retained) void_call_guest_this(MEM32(MEM32(hit) + 8), hit, 0, NULL);
    return result;
}

static int touch_fill_type(uint32_t tool)
{
    if (!tool || !void_vm_readable(tool, 0xC0) || !void_vm_writable(tool, 0xC0)) return 0;
    if (MEM32(tool) == 0xA0DE8DA8u && !strcmp(touch_type(tool), ".?AVcTSUserToolFloorPattern@@")) return 1;
    if (MEM32(tool) == 0xA0DE9DCCu && !strcmp(touch_type(tool), ".?AVcTSUserToolWallpaper@@")) return 2;
    return 0;
}

static int touch_object_kind(uint32_t tool)
{
    if (!tool || !void_vm_readable(tool, 0x100) || MEM32(tool) != 0xA0DE83FCu ||
        strcmp(touch_type(tool), ".?AVcTSUserToolObject@@")) return 0;
    uint32_t selected = MEM32(tool + (MEM8(tool + 0xFC) ? 0x68 : 0x64));
    if (!selected || !void_vm_readable(selected, 0x60) || MEM32(selected) != 0xA0DEBB54u ||
        strcmp(touch_type(selected), ".?AVcTSUserToolUndoableObject@@")) return 0;
    uint32_t state = MEM32(selected + 0x54), object = MEM32(selected + 0x5C);
    /* Original constructor is empty0/NULL; state2/4 are not an empty Hand. */
    if (!state && !object) return 1;
    return (state == 1 || state == 3) && object ? 2 : 0;
}

int s2_touch_object_preview(uint64_t token)
{
    unsigned context=s2_touch_context();
    if ((context!=S2_TOUCH_BUY && context!=S2_TOUCH_BUILD) || (uint32_t)token!=2 || token!=s2_touch_tool_token()) return 0;
    uint32_t object=atomic_load(&shown_object);
    if (!object) return 0;
    atomic_store(&preview_object,object);return 1;
}

static int touch_object_preview_frozen(uint32_t tool)
{
    unsigned context=s2_touch_context();
    if ((context!=S2_TOUCH_BUY && context!=S2_TOUCH_BUILD) || touch_object_kind(tool)!=2) return 0;
    uint32_t selected=MEM32(tool+(MEM8(tool+0xFC)?0x68:0x64));
    return MEM32(selected+0x54)==3 && MEM32(selected+0x5C)!=atomic_load(&preview_object);
}

void sub_A06815D7_lifted(void);
void sub_A06815D7(void)
{
    /* Picking must not snap an owned object to the finger's ray. */
    if (touch_object_preview_frozen(g_ecx)) { g_eax=(g_eax&~255u)|1u;g_esp+=8;return; }
    sub_A06815D7_lifted();
}

void sub_A067D1BD_lifted(void);
void sub_A067D1BD(void)
{
    unsigned context = s2_touch_context();
    if ((context == S2_TOUCH_BUY || context == S2_TOUCH_BUILD) &&
        void_vm_readable(g_ecx, 0x100) && MEM32(g_ecx) == 0xA0DE83FCu) {
        /* A stationary mouse pickup still quantizes its ray; touch rotation is explicit. */
        g_eax = 1; g_esp += 8; return;
    }
    sub_A067D1BD_lifted();
}

void sub_A067D8A4_lifted(void);
void sub_A067D8A4(void)
{
    uint32_t slot=g_esp+4,argument=MEM32(slot);
    if(touch_object_preview_frozen(g_ecx)){
        uint32_t selected=MEM32(g_ecx+(MEM8(g_ecx+0xFC)?0x68:0x64));
        if(!void_vm_readable(selected,0x80)||!MEM8(selected+0x7D)||MEM32(selected+0x70)||!void_vm_writable(g_ecx+0x74,12)) { g_eax=(g_eax&~255u)|1u;g_esp+=8;return; }
        /* The undo record owns the exact original WorldLocation, including its floor. */
        memcpy(GPTR(g_ecx+0x74),GPTR(selected+0x60),12);MEM32(slot)=g_ecx+0x74;
    }
    sub_A067D8A4_lifted();MEM32(slot)=argument;
}

static uint32_t touch_fill_painter(uint32_t tool, int kind)
{
    uint32_t painter = MEM32(tool + (kind == 1 ? 0x64 : 0x50));
    return painter && void_vm_readable(painter, 0x64) &&
        MEM32(painter) == (kind == 1 ? 0xA0DEBC7Cu : 0xA0DEBFECu) ? painter : 0;
}

static int touch_town_navigation(void)
{
    unsigned context = s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT;
    uint32_t tool = tool_handler && void_vm_readable(tool_handler, 0x24) ? MEM32(tool_handler + 0x20) : 0;
    int town_tool = !strncmp(touch_type(tool), ".?AVcTSUserToolNeighborhood", sizeof ".?AVcTSUserToolNeighborhood" - 1);
    return last_world_context == S2_TOUCH_TOWN && (!context || context == S2_TOUCH_TOWN) &&
        (town_tool || (scene_owner && void_vm_readable(scene_owner, 4) && scene_window && void_vm_readable(scene_window, 0xEC) &&
        MEM32(scene_window) == 0xA0DD89FCu && MEM32(scene_window + 4) == win_manager &&
        !strcmp(touch_type(scene_window), ".?AVcTSWinSceneGraph@@"))) &&
        town_camera && void_vm_readable(town_camera, 0x458) && MEM32(town_camera) == 0xA0DBAABCu &&
        !strcmp(touch_type(town_camera), ".?AVcCameraNeighborhood@nTSSG@@");
}

void s2_touch_pump(void)
{
    if (!win_manager) { atomic_store(&shown_cost, -1); atomic_store(&shown_tool, 0); atomic_store(&shown_fill_kind, 0); s2_touch_input_context(0); return; }
    if (win_manager_thread != void_thread_id()) return;
    s2_touch_text_pump(win_manager);
    uint32_t focus = win_manager && void_vm_readable(win_manager, 0x3C) ? MEM32(win_manager + 0x38) : 0;
    uint32_t tool = tool_handler && void_vm_readable(tool_handler, 0x24) ? MEM32(tool_handler + 0x20) : 0;
    const char *type = touch_type(tool);
    unsigned input=touch_text(focus)?S2_TOUCH_FOCUSED_TEXT:0;
    unsigned world=s2_touch_context()&(S2_TOUCH_TOWN|S2_TOUCH_LIVE|S2_TOUCH_BUY|S2_TOUCH_BUILD);
    if(world)last_world_context=world;
    /* Reused world panels survive Options/Rewards; recover only from their exact active tool. */
    if(lot_camera&&!strcmp(type,".?AVcTSUserToolStandardAction@@")&&MEM32(tool)==0xA0DE9374u)input|=S2_TOUCH_LIVE;
    else if(!strcmp(type,".?AVcTSUserToolObject@@")&&MEM32(tool)==0xA0DE83FCu&&
        (last_world_context==S2_TOUCH_BUY||last_world_context==S2_TOUCH_BUILD))input|=last_world_context;
    else if(touch_town_navigation()) input |= S2_TOUCH_TOWN;
    s2_touch_input_context(input);
#ifdef __ANDROID__
    if (s2_touch_context() & (S2_TOUCH_TOWN | S2_TOUCH_LIVE | S2_TOUCH_BUY | S2_TOUCH_BUILD))
        s2_cheats_sample_testing();
#endif
    if (strncmp(type, ".?AVcTSUserTool", 15)) { touch_tool_clear(); return; }
    uint32_t vtable = MEM32(tool);
    if (tool != shown_pointer || vtable != shown_vtable) {
        shown_pointer = tool; shown_vtable = vtable;
        if (!++tool_generation) ++tool_generation;
    }
    /* Observed identity, not a semantic transaction or an unseen ABA guarantee. */
    int kind = !strcmp(type, ".?AVcTSUserToolObject@@") ? touch_object_kind(tool) : touch_tool_kind(type);
    uint32_t object=0;
    if (kind==2) { uint32_t selected=MEM32(tool+(MEM8(tool+0xFC)?0x68:0x64));if(selected&&void_vm_readable(selected,0x60))object=MEM32(selected+0x5C); }
    if (atomic_load(&shown_object)!=object) { atomic_store(&preview_object,0);atomic_store(&shown_object,object); }
    atomic_store(&shown_tool, (uint64_t)tool_generation << 32 | (unsigned)kind);
    atomic_store(&shown_fill_kind, touch_fill_type(tool));
    int cost = -1;
    if (kind == 2 && vtable == 0xA0DE83FCu) {
        void_regs saved;
        void_regs_save(&saved);
        /* The original affordability path queries this exact selected-object total. */
        cost = (int)void_call_guest_this(0xA067AD3Au, tool, 0, NULL);
        /* Unchanged previews have no transaction cost; show the object's value until placement. */
        uint32_t selected = MEM32(tool + (MEM8(tool + 0xFC) ? 0x68 : 0x64));
        if (!cost && selected && void_vm_readable(selected, 0x60) &&
            MEM32(selected) == 0xA0DEBB54u &&
            (MEM32(selected + 0x54) == 1 || MEM32(selected + 0x54) == 3)) {
            uint32_t object = MEM32(selected + 0x5C);
            if (object && void_vm_readable(object, 0x2F4) && void_vm_readable(MEM32(object), 0x218)) {
                uint32_t method=MEM32(MEM32(object)+0x1B8);
                if(method==0xA0547400u||method==0xA052F8D0u)
                    cost=(uint16_t)void_call_guest_this(method,object,0,NULL);
            }
        }
        void_regs_load(&saved);
        if (cost < 0 || cost > 1000000000) cost = -1;
    }
    atomic_store(&shown_cost, cost);
}

static int touch_fill_idle(void)
{
    static const uint32_t keys[] = { 1, 2, 4, 0x10, 0x11, 0x12 };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; ++i)
        if (void_input_key_state(keys[i]) & 0x8000) return 0;
    return 1;
}

static int touch_fill_current(uint32_t manager, uint32_t handler, uint32_t tool, uint32_t root,
    int kind, uint64_t expected, uint64_t size, unsigned epoch)
{
    return touch_layout_matches(S2_TOUCH_BUILD, size, epoch) &&
        s2_touch_owner_manager() == manager && tool_handler == handler &&
        void_vm_readable(handler, 0x2C) && MEM32(handler + 0x20) == tool &&
        expected == s2_touch_tool_token() && touch_fill_type(tool) == kind &&
        touch_fill_painter(tool, kind) && MEM32(manager + 0x44) == root;
}

static int touch_fill_owner(uint32_t manager, uint32_t tool, uint32_t owner, uint32_t viewport)
{
    return void_vm_readable(tool, 0xC0) && MEM32(tool + 4) == owner &&
        void_vm_readable(viewport, 0x100) && MEM32(viewport) == 0xA0DD89FCu &&
        MEM32(owner) == 0xA0DD8CB8u && MEM32(viewport + 4) == manager &&
        void_vm_readable(manager, 0x1E1) &&
        (MEM32(manager) == 0xA0E5B240u || MEM32(manager) == 0xA0E5B5B8u);
}

/* An explicit release gesture uses original mode/Down/Up, never host Shift/LMB.
 * Once Down starts, Up completes on the retained target even after a mode change. */
static int touch_fill_room_apply(uint64_t expected, int x, int y, unsigned context, uint64_t size, unsigned epoch)
{
    uint32_t manager = s2_touch_owner_manager(), handler = tool_handler;
    if (context != S2_TOUCH_BUILD || !touch_layout_matches(context, size, epoch) ||
        !manager || manager != win_manager || !void_vm_readable(manager, 0x1E1) ||
        !MEM8(manager + 0x1E0) || MEM32(manager + 0x34) || !touch_fill_idle() ||
        !expected || (uint32_t)expected != 3 || expected != s2_touch_tool_token() ||
        !handler || !void_vm_readable(handler, 0x2C)) return S2_SLIDER_REJECTED;
    uint32_t tool = MEM32(handler + 0x20), root = MEM32(manager + 0x44);
    int kind = touch_fill_type(tool);
    if (!kind || !root || !void_vm_readable(root, 4) || !void_vm_readable(MEM32(root), 0xC0) ||
        !touch_fill_painter(tool, kind) || MEM8(tool + 0x10) || MEM8(tool + 0x1C) ||
        !(MEM32(tool + 0x20) & 1)) return S2_SLIDER_REJECTED;
    uint32_t tag = MEM32(handler + 0x28), original_mode = (tag & 0xFFFFu) - 1;
    if ((tag >> 16) != 1 || original_mode > 3 ||
        MEM32(tool + (kind == 1 ? 0x8C : 0x58)) != original_mode ||
        (kind == 1 && MEM32(tool + 0x90) != original_mode)) return S2_SLIDER_REJECTED;
    uint32_t owner = MEM32(tool + 4), viewport = owner >= 0xF8 ? owner - 0xF8 : 0;
    if (!viewport || !touch_fill_owner(manager, tool, owner, viewport) ||
        !(MEM32(viewport + 0xFC) & 0x7FFFFFFFu) || !MEM32(manager + 0xC) ||
        (MEM32(viewport + 0xE8) & 3) != 3) return S2_SLIDER_REJECTED;

    void_regs saved;
    void_regs_save(&saved);
    int result = S2_SLIDER_REJECTED, armed = 0;
    uint32_t point = 0, painter = touch_fill_painter(tool, kind);
    uint32_t setter = kind == 1 ? 0xA0685954u : 0xA0693287u;
    /* The tool's owner can be detached by a callback. Keep both original
     * viewport and manager alive independently through completion/cleanup. */
    void_call_guest_this(0xA0145240u, manager, 0, NULL);
    void_call_guest_this(0xA05B84E8u, viewport, 0, NULL);
    void_call_guest_this(0xA078D60Au, tool, 0, NULL);
    if (!touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch) ||
        !touch_fill_owner(manager, tool, owner, viewport) || MEM32(manager + 0x34) || !touch_fill_idle()) goto done;
    if (s2_ui_pick(root, x, y) != viewport ||
        !touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch) ||
        touch_fill_painter(tool, kind) != painter || !touch_fill_owner(manager, tool, owner, viewport) || MEM32(handler + 0x28) != tag ||
        MEM32(manager + 0x34) || MEM8(tool + 0x10) || MEM8(tool + 0x1C) || !touch_fill_idle()) goto done;
    point = void_galloc(8);
    if (!point) goto done;
    MEM32(point) = (uint32_t)x; MEM32(point + 4) = (uint32_t)y;
    uint32_t fill = 0x10003u;
    armed = 1;
    void_call_guest_this(setter, tool, 1, &fill);
    if (!touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch) ||
        touch_fill_painter(tool, kind) != painter || MEM32(handler + 0x28) != tag ||
        MEM32(tool + (kind == 1 ? 0x8C : 0x58)) != 2 || !touch_fill_owner(manager, tool, owner, viewport) ||
        MEM32(manager + 0x34) || MEM8(tool + 0x10) || MEM8(tool + 0x1C) || !touch_fill_idle()) goto done;
    uint32_t args[4] = { 1, 4, point, 0 };
    int down = void_call_guest_this(0xA078DC91u, tool, 4, args) & 255;
    int stable = touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch);
    args[1] = 5;
    int up = 0;
    if (touch_fill_type(tool) == kind && touch_fill_owner(manager, tool, owner, viewport))
        up = void_call_guest_this(0xA078DC91u, tool, 4, args) & 255;
    else if (MEM32(manager) == 0xA0E5B240u || MEM32(manager) == 0xA0E5B5B8u)
        void_call_guest_this(0xA097340Au, manager, 1, &viewport);
    result = down && up && stable &&
        touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch) &&
        !MEM8(tool + 0x10) && !MEM8(tool + 0x1C) ? S2_SLIDER_CHANGED : S2_SLIDER_UNKNOWN;
done:
    /* Normal End may replace the painter. Restore only a fresh live current
     * target; original setters dereference a cleared painter after UnInit. */
    if (armed && tool_handler == handler && void_vm_readable(handler, 0x2C) &&
        MEM32(handler + 0x20) == tool && MEM32(handler + 0x28) == tag &&
        touch_fill_type(tool) == kind && touch_fill_painter(tool, kind) &&
        touch_fill_owner(manager, tool, owner, viewport) && !MEM8(tool + 0x1C)) {
        void_call_guest_this(setter, tool, 1, &tag);
        if (MEM32(tool + (kind == 1 ? 0x8C : 0x58)) != original_mode ||
            (result == S2_SLIDER_CHANGED && !touch_fill_current(manager, handler, tool, root, kind, expected, size, epoch)))
            result = S2_SLIDER_UNKNOWN;
    } else if (armed && result == S2_SLIDER_CHANGED) result = S2_SLIDER_UNKNOWN;
    if (point) void_gfree(point);
    void_call_guest_this(0xA078D8E9u, tool, 0, NULL);
    void_call_guest_this(0xA05BBE91u, viewport, 0, NULL);
    void_call_guest_this(0xA01A85B0u, manager, 0, NULL);
    void_regs_load(&saved);
    return result;
}

/* One original object START/STOP pair, retained across picker/handler callbacks. */
static int touch_rotate_object_apply(uint64_t expected, int x, int y, unsigned expected_context, uint64_t expected_size, unsigned expected_epoch, int direction)
{
    unsigned mode = s2_touch_context();
    uint32_t manager = s2_touch_owner_manager();
    if (!touch_layout_matches(expected_context, expected_size, expected_epoch) ||
        (mode != S2_TOUCH_BUY && mode != S2_TOUCH_BUILD) || !manager ||
        manager != win_manager || !void_vm_readable(manager, 0x48) || MEM32(manager + 0x34) ||
        !expected || (uint32_t)expected != 2 || expected != s2_touch_tool_token() ||
        !tool_handler || !void_vm_readable(tool_handler, 0x24)) return 0;
    static const uint32_t held_keys[] = { 1, 2, 4, 0x10, 0x11, 0x12, 0xBC, 0xBE };
    for (unsigned i = 0; i < sizeof held_keys / sizeof held_keys[0]; ++i)
        if (void_input_key_state(held_keys[i]) & 0x8000) return 0;
    uint32_t handler = tool_handler;
    uint32_t tool = MEM32(handler + 0x20), root = MEM32(manager + 0x44);
    if (!tool || !void_vm_readable(tool, 0x140) || !void_vm_writable(tool, 0x140) ||
        MEM32(tool) != 0xA0DE83FCu || strcmp(touch_type(tool), ".?AVcTSUserToolObject@@") ||
        MEM8(tool + 0x13E) || !root || !void_vm_readable(root, 0xEC) ||
        !void_vm_readable(MEM32(root), 0xC0)) return 0;
    uint32_t selected = MEM32(tool + (MEM8(tool + 0xFC) ? 0x68 : 0x64));
    if (!selected || !void_vm_readable(selected, 0x60)) return 0;

    void_regs saved;
    void_regs_save(&saved);
    int result = 0, retained = 0, constructed = 0;
    uint32_t message = 0;
    /* The capability chain is proved read-only. Hold the target before the
     * picker or original callbacks can replace its owning tool reference. */
    if (!(void_call_guest_this(0xA067BE89u, tool, 0, NULL) & 255)) goto done;
    void_call_guest_this(0xA078D60Au, tool, 0, NULL);
    retained = 1;
    if (!touch_layout_matches(expected_context, expected_size, expected_epoch) ||
        s2_touch_context() != mode || expected != s2_touch_tool_token() ||
        s2_touch_owner_manager() != manager || tool_handler != handler ||
        !void_vm_readable(handler, 0x24) || MEM32(handler + 0x20) != tool ||
        MEM32(manager + 0x34) || MEM32(manager + 0x44) != root) goto done;
    uint32_t hit = s2_ui_pick(root, x, y);
    if (!hit || !(hit == scene_window || !strcmp(touch_type(hit), ".?AVcTSWinSceneGraph@@")) ||
        !touch_layout_matches(expected_context, expected_size, expected_epoch) ||
        s2_touch_context() != mode || expected != s2_touch_tool_token() ||
        s2_touch_owner_manager() != manager || tool_handler != handler ||
        !void_vm_readable(handler, 0x24) || MEM32(handler + 0x20) != tool ||
        MEM32(manager + 0x44) != root || MEM32(manager + 0x34) ||
        MEM32(tool) != 0xA0DE83FCu || MEM8(tool + 0x13E)) goto done;
    /* The exact original ToolObject AddRef/Release preserve this target even
     * if START's original callbacks replace the handler's current tool. */
    if (!(void_call_guest_this(0xA067BE89u, tool, 0, NULL) & 255) ||
        MEM32(tool + (MEM8(tool + 0xFC) ? 0x68 : 0x64)) != selected ||
        !touch_layout_matches(expected_context, expected_size, expected_epoch) ||
        s2_touch_context() != mode || expected != s2_touch_tool_token() ||
        s2_touch_owner_manager() != manager || tool_handler != handler ||
        !void_vm_readable(handler, 0x24) || MEM32(handler + 0x20) != tool ||
        MEM32(manager + 0x44) != root || MEM32(manager + 0x34)) goto done;
    for (unsigned i = 0; i < sizeof held_keys / sizeof held_keys[0]; ++i)
        if (void_input_key_state(held_keys[i]) & 0x8000) goto done;
    message = void_galloc(44);
    if (!message) goto done;
    void_call_guest_this(0xA01C033Du, message, 0, NULL);
    constructed = 1;
    uint32_t id = 0xACB3A629u, turn = (uint32_t)direction;
    void_call_guest_this(0xA01C0915u, message, 1, &id);
    void_call_guest_this(0xA01C0874u, message, 1, &turn);
    uint32_t args[2] = { message, 0 };
    result = (void_call_guest_this(0xA067C356u, tool, 2, args) & 255) != 0;
    /* A067C56C is an interior branch, never a callable entry. START latches
     * tool+13E. Finish on the retained ORIGINAL target, never a replacement. */
    id = 0x2FE3C0CEu;
    void_call_guest_this(0xA01C0915u, message, 1, &id);
    void_call_guest_this(0xA067C356u, tool, 2, args);
done:
    /* Explicit original constructor/destructor pairing: the 44-byte object
     * is host-galloc-owned. Do not call deleting Release or guess a flag. */
    if (constructed) void_call_guest_this(0xA01C0388u, message, 0, NULL);
    if (message) void_gfree(message);
    if (retained) void_call_guest_this(0xA078D8E9u, tool, 0, NULL);
    void_regs_load(&saved);
    return result;
}

uint32_t s2_touch_owner_manager(void)
{
    return win_manager_thread == void_thread_id() ? win_manager : 0;
}

void s2_touch_barrier(unsigned serial)
{
    if (!win_manager || win_manager_thread != void_thread_id()) return;
    s2_touch_pump();
    uint32_t tool = tool_handler && void_vm_readable(tool_handler, 0x24) ? MEM32(tool_handler + 0x20) : 0;
    int x, y, diagnostic, radius, step = 0, delta = 0;
    uint64_t rotate_tool = 0, rotate_size = 0;
    unsigned rotate_context = 0, rotate_epoch = 0;
    unsigned command = 0;
    float camera_dx = 0, camera_dy = 0, camera_radians = 0, camera_zoom = 0, camera_pitch = 0, camera_pose[6] = {0};
    int camera_absolute = 0;
    unsigned camera_epoch = 0, camera_context = 0;
    uint64_t camera_size = 0;
    int catalog_x = 0, catalog_y = 0, catalog_amount = 0, catalog_info = 0;
    unsigned catalog_context = 0, catalog_epoch = 0;
    uint64_t catalog_size = 0, catalog_owner = 0;
    uint32_t command_value = 0;
    pthread_mutex_lock(&pick_lock);
    ui_command_expire();
    slider_expire();
    int pending = pick_pending && pick_serial == serial;
    uint64_t world_key = pick_world_key;
    x = pick_x; y = pick_y; diagnostic = pick_diagnostic; radius = pick_radius;
    if (pending && slider_serial == serial && slider_state == SLIDER_QUEUED) {
        slider_state = SLIDER_STARTED; step = 1; delta = slider_delta;
        rotate_tool = rotate_request.tool; rotate_context = rotate_request.context;
        rotate_size = rotate_request.size; rotate_epoch = rotate_request.epoch;
    }
    if (pending && ui_command_kind) {
        command = ui_command_kind;
        command_value = ui_command_value;
        if (command == S2_UI_CAMERA) {
            camera_dx = camera_request.dx; camera_dy = camera_request.dy; camera_radians = camera_request.radians; camera_zoom = camera_request.zoom;
            camera_pitch = camera_request.pitch;
            camera_epoch = camera_request.epoch; camera_context = camera_request.context; camera_size = camera_request.size;
            camera_absolute = camera_request.absolute;
            memcpy(camera_pose, camera_request.pose, sizeof camera_pose);
        }
        if (command == S2_UI_CATALOG) {
            catalog_x = catalog_request.x; catalog_y = catalog_request.y;
            catalog_amount = catalog_request.amount; catalog_info = catalog_request.info;
            catalog_context = catalog_request.context; catalog_size = catalog_request.size; catalog_epoch = catalog_request.epoch;
            catalog_owner = catalog_request.owner;
        }
        ui_command_kind = 0;
        ui_command_started = command;
    }
    if (pending) pick_pending = 0;
    pthread_mutex_unlock(&pick_lock);
    if (!pending) return;
    if (step && (delta == 0 || delta == 2 || delta == 3)) {
        int result = delta == 2 ? touch_fill_room_apply(rotate_tool, x, y, rotate_context, rotate_size, rotate_epoch) :
            touch_rotate_object_apply(rotate_tool, x, y, rotate_context, rotate_size, rotate_epoch, delta == 3 ? 1 : -1) ? S2_SLIDER_CHANGED : S2_SLIDER_REJECTED;
        pthread_mutex_lock(&pick_lock);
        if (slider_serial == serial && slider_state == SLIDER_STARTED) {
            slider_result = result; slider_state = SLIDER_DONE;
        }
        pthread_cond_broadcast(&pick_ready);
        pthread_mutex_unlock(&pick_lock);
        return;
    }
    if (command) {
        void_regs saved;
        void_regs_save(&saved);
        int applied = 0;
        if (command == S2_UI_SCALE) applied = s2_ui_scale_apply((int)command_value);
        else if (command == S2_UI_CAMERA && s2_touch_context() == camera_context && camera_absolute) {
            applied = touch_camera_pose_apply(camera_pose, camera_epoch, camera_size);
            void_log("[camera] camset applied=%d", applied);
        }
        else if (command == S2_UI_CAMERA && s2_touch_context() == camera_context) applied = touch_camera_apply(camera_dx, camera_dy, camera_radians, camera_zoom, camera_pitch, camera_epoch, camera_size);
        else if (command == S2_UI_CATALOG) applied = catalog_info == 3 ?
            touch_fling_apply(catalog_x, catalog_y, catalog_amount, catalog_context, catalog_size, catalog_epoch, catalog_owner) :
            touch_catalog_apply(catalog_x, catalog_y, catalog_amount, catalog_info, catalog_context, catalog_size, catalog_epoch);
        else if (command == S2_UI_TEXT_READ || command == S2_UI_TEXT_WRITE) {
            s2_touch_text_owner(command, command_value, win_manager);
            applied = 1;
        }
        else if (command == S2_UI_CHEAT) { s2_cheats_owner(command_value, win_manager); applied = 1; }
        else if (command == S2_UI_CLEAR_CATALOG) {
            uint32_t tool = tool_handler && void_vm_readable(tool_handler, 0x24) ? MEM32(tool_handler + 0x20) : 0;
            if (tool && void_vm_readable(tool, 0x100) && MEM32(tool) == 0xA0DE83FCu && touch_object_kind(tool) == 1)
                applied = s2_catalog_clear_owner(command_value);
        }
        else if (command == S2_UI_CAS_PAN) {
            cas_pan_sync();
            int height;
            if (cas_editor_ready(cas_editor_window, 1, &height) && height < 600) {
                s2_touch_cancel_text();
                float dy;
                memcpy(&dy, &command_value, 4);
                if (!cas_pan_selected) cas_pan_y = (float)(height - 600);
                cas_pan_y += dy * height;
                cas_pan_selected = 1;
                cas_pan_move(cas_editor_window, height);
                applied = 1;
            }
        }
        void_regs_load(&saved);
        pthread_mutex_lock(&pick_lock);
        ui_command_started = 0;
        pthread_cond_broadcast(&pick_ready);
        pthread_mutex_unlock(&pick_lock);
        if (command != S2_UI_CAMERA && command != S2_UI_CATALOG)
            void_log("[native-ui] command=%u serial=%u applied=%d scale=%d", command, serial, applied, s2_ui_scale());
        return;
    }
    uint32_t manager = win_manager;
    uint32_t root = manager && void_vm_readable(manager, 0x48) ? MEM32(manager + 0x44) : 0;
    uint32_t hit = 0;
    int result = 0;
    void_regs saved;
    void_regs_save(&saved);
    unsigned context = s2_touch_context();
    if (!(context & S2_TOUCH_LOADING) && root && void_vm_readable(root, 0x5C) && void_vm_readable(MEM32(root), 0xC0)) {
        hit = touch_input_pick(root, x, y);
        if (hit) {
            unsigned modes = S2_TOUCH_CAS | S2_TOUCH_TOWN | S2_TOUCH_LIVE | S2_TOUCH_BUY | S2_TOUCH_BUILD;
            unsigned blockers = S2_TOUCH_MODAL | S2_TOUCH_OPTIONS | S2_TOUCH_CHOOSER | S2_TOUCH_LOT_INFO;
            if ((context & modes) && !(context & blockers))
                result = hit == scene_window || !strcmp(touch_type(hit), ".?AVcTSWinSceneGraph@@") || touch_cas_viewport(hit) || touch_town_callout(hit, root)
                    ? 2 : 1 | (int)touch_widget(hit);
            else if (hit != root && touch_type(hit)[0] && (!touch_scene_ancestry(hit) || touch_ui_button(hit)))
                result = 1 | (int)touch_widget(hit);
        }
        if (context == S2_TOUCH_CHOOSER && touch_chooser_window(root)) result = 1 | 256;
        else if ((result & 3) == 1 || (!step && radius < 0 && result == 2)) {
            if (touch_vertical_band(root, x, y)) result = 1 | (result & ~3) | 512;
            if ((result & 3) == 1) {
                uint32_t clock = touch_time_band(root, x, y);
                if (clock) result |= 256 | touch_time_state(clock);
            }
        }
    }
    int target_x = x, target_y = y;
    /* An open pie menu takes every click (TouchLayer.PIE_ITEM / PIE_DISMISS). It runs as
     * a modal loop; in Live mode without mouse capture. */
    uint32_t pie = !step && radius < 0 && root && !(context & S2_TOUCH_LOADING) &&
        (context & S2_TOUCH_MODAL) && s2_touch_context() == context && manager == s2_touch_owner_manager() &&
        void_vm_readable(manager, 0x48) && MEM32(manager + 0x44) == root ? touch_open_pie(manager, root) : 0;
    if (pie) {
        touch_pie_describe(pie, manager);
        int item = touch_pie_pick(pie, manager, &target_x, &target_y, -radius);
        if (item) result = 1 | 32768;
        else if (touch_pie_outside(pie, root, &target_x, &target_y)) result = 1 | 16384;
        else result = 1 | 32768; /* no point outside the menu: the original resolves the finger */
        if (pie_pick_logs++ < 64) void_log("[touch-pie] tap %d,%d -> %d,%d %s hit=%08X", x, y, target_x, target_y,
            item == 1 ? "item" : item == 2 ? "label" : (result & 16384) ? "dismiss" : "none", hit);
    }
    else if (!step && radius < 0 && root && !(context & S2_TOUCH_LOADING)) {
        uint32_t target = touch_ui_target(root, hit, &target_x, &target_y, -radius);
        /* Enlarged tap targets must not change the gesture that starts at the finger. */
        const unsigned swipe_flags = 256 | 512 | 1024 | (3 << 11);
        unsigned gestures = result & swipe_flags;
        if (touch_pan_background(context, manager, root, hit, result))
            gestures |= 8192; /* TouchLayer.PAN_BACKGROUND */
        if (target) {
            hit = target;
            result = 1 | (touch_widget(target) & ~swipe_flags);
        }
        result |= gestures;
        if ((result & TOUCH_DRAG_FILTER) && void_vm_readable(MEM32(root), 0xC4)) {
            /* The drag manager needs a move of over three logical pixels inside the source. */
            double factor = s2_ui_factor();
            int start_x, start_y, width, height;
            int step = (int)(4 * factor);
            step += step < 4 * factor;
            touch_window_center(hit, factor, &start_x, &start_y);
            void_input_screen_size(&width, &height);
            if (step > 0 && step <= 15 && start_x >= 0 && start_x < width - step && start_y >= 0 && start_y < height &&
                s2_ui_mouse_pick(root, start_x, start_y) == hit && s2_ui_mouse_pick(root, start_x + step, start_y) == hit) {
                target_x = start_x;
                target_y = start_y;
                result |= step << TOUCH_DRAG_STEP_SHIFT;
            }
        }
        /* TouchLayer.HOLD_BUTTON: a held finger keeps an original button pressed, as a held mouse. */
        if ((result & 3) == 1)
            for (uint32_t window = hit, depth = 0; window && depth < 4 && void_vm_readable(window, 0xEC); ++depth) {
                if (touch_ui_button(window)) { result |= 65536; break; }
                window = MEM32(window + 0x58);
            }
    }
    /* A second finger on the HUD's transparent container is a world finger for a pinch. */
    else if (!step && !radius && touch_pan_background(context, manager, root, hit, result)) result |= 8192;
    uint32_t picked = hit;
    static s2_world_targets world;
    static _Atomic int world_busy;
    int scan_world = !step && radius > 0 && result == 2;
    if (scan_world && atomic_exchange(&world_busy, 1)) { scan_world = 0; result = 0; }
    if (scan_world) {
        world.count = 0;
        struct timespec started = {0}, finished = {0};
        clock_gettime(CLOCK_MONOTONIC, &started);
        if (!touch_world_scan(manager, tool, root, x, y, radius, world_key, &world)) result = 0;
        clock_gettime(CLOCK_MONOTONIC, &finished);
        if (target_logs++ < 32) void_log("[touch-target] %d,%d radius=%d choices=%u result=%d us=%lld", x, y, radius,
            world.count, result, (long long)(finished.tv_sec - started.tv_sec) * 1000000 + (finished.tv_nsec - started.tv_nsec) / 1000);
    }
    int before = -101, after = -101;
    if (step) result = touch_slider_apply(manager, hit, x, y, delta, &before, &after);
    if (diagnostic) void_log("[touch] pick %d,%d root=%08X hit=%08X scene=%08X area=%d context=%X",
        x, y, root, hit, scene_window, result, s2_touch_context());
    if (diagnostic) void_log("[touch] manager=%08X type=%s thread=%u/%u", win_manager,
        touch_type(win_manager), void_thread_id(), win_manager_thread);
    if (diagnostic) {
        uint32_t cx, cy;
        void_input_cursor(&cx, &cy);
        void_log("[touch] cursor=%u,%u", cx, cy);
        static const uint32_t offsets[] = { 0x34, 0x3C, 0x38 };
        static const char *names[] = { "capture", "hover", "focus" };
        for (unsigned i = 0; i < 3; ++i) {
            uint32_t window = win_manager && void_vm_readable(win_manager, 0x40) ? MEM32(win_manager + offsets[i]) : 0;
            void_log("[touch] %s=%08X id=%08X flags=%08X type=%s", names[i], window,
                window && void_vm_readable(window, 0xEC) ? MEM32(window + 0x10) : 0,
                window && void_vm_readable(window, 0xEC) ? MEM32(window + 0xE8) : 0, touch_type(window));
        }
    }
    if (diagnostic) void_log("[touch] scene-owner=%08X type=%s", scene_owner, touch_type(scene_owner));
    if (diagnostic) void_log("[touch] tool=%08X kind=%d type=%s", tool, s2_touch_tool(), touch_type(tool));
    if (diagnostic && tool && void_vm_readable(tool, 0x100) && MEM32(tool) == 0xA0DE83FCu) {
        uint32_t selected = MEM32(tool + (MEM8(tool + 0xFC) ? 0x68 : 0x64));
        int readable = selected && void_vm_readable(selected, 0x60);
        void_log("[touch] object activeSlot=%u wrapper=%08X vtable=%08X state=%u object=%08X kind=%d",
            MEM8(tool + 0xFC) != 0, selected, readable ? MEM32(selected) : 0,
            readable ? MEM32(selected + 0x54) : UINT32_MAX, readable ? MEM32(selected + 0x5C) : 0, touch_object_kind(tool));
    }
    if (diagnostic) touch_log_camera();
    if (diagnostic) touch_log_slider(manager, hit);
    for (unsigned depth = 0; diagnostic && hit && depth < 16 && void_vm_readable(hit, 0xEC); ++depth) {
        void_log("[touch] ancestor %u win=%08X vtable=%08X id=%08X rect=%d,%d,%d,%d parent=%08X flags=%08X",
            depth, hit, MEM32(hit), MEM32(hit + 0x10), (int)MEM32(hit + 0x24), (int)MEM32(hit + 0x28),
            (int)MEM32(hit + 0x2C), (int)MEM32(hit + 0x30), MEM32(hit + 0x58), MEM32(hit + 0xE8));
        void_log("[touch] type %s", touch_type(hit));
        touch_log_procs(hit);
        hit = MEM32(hit + 0x58);
    }
    void_regs_load(&saved);
    pthread_mutex_lock(&pick_lock);
    if (step && slider_serial == serial && slider_state == SLIDER_STARTED) {
        slider_before = before; slider_after = after; slider_result = result; slider_state = SLIDER_DONE;
    }
    if (pick_serial == serial) {
        pick_done = serial; pick_result = result;
        pick_point = (uint64_t)(uint32_t)target_y << 32 | (uint32_t)target_x;
        if (scan_world && !world_key && result == 2) {
            pick_world.count = world.count;
            memcpy(pick_world.items, world.items, world.count * sizeof world.items[0]);
        }
        pick_owner_context = context; pick_owner_root = root; pick_owner_hit = picked;
    }
    if (scan_world) atomic_store(&world_busy, 0);
    pthread_cond_broadcast(&pick_ready);
    pthread_mutex_unlock(&pick_lock);
}
