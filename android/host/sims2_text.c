/* Whole-value native text editing. JNI exchanges standard UTF-8; guest calls run only on the owner fence. */
#include "host.h"
#include "input.h"
#include "sims2_settings.h"
#include <stdatomic.h>
#include <time.h>

enum { TEXT_IDLE, TEXT_QUEUED, TEXT_STARTED, TEXT_DONE, TEXT_CANCELLED };
static pthread_mutex_t text_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t text_ready = PTHREAD_COND_INITIALIZER;
static unsigned text_serial, text_state, text_kind, text_result, text_epoch_request, text_abandoned;
static int text_x, text_y;
static uint8_t text_value[513], text_readback[513];
static unsigned text_length, text_readback_length;
static struct timespec text_expiry;
static _Atomic unsigned text_epoch;
static struct {
    uint32_t window, iface, root, vtable, release;
    unsigned epoch, context, length;
    int x, y, scale, width, height;
    uint8_t value[513];
} text_session;

static int valid_utf8(const uint8_t *s, unsigned n)
{
    unsigned units = 0;
    for (unsigned i = 0; i < n;) {
        unsigned c = s[i++], count, minimum;
        if (!c) return 0;
        if (c < 0x80) { if (++units > 128) return 0; continue; }
        if (c >= 0xC2 && c <= 0xDF) { c &= 31; count = 1; minimum = 0x80; }
        else if (c >= 0xE0 && c <= 0xEF) { c &= 15; count = 2; minimum = 0x800; }
        else if (c >= 0xF0 && c <= 0xF4) { c &= 7; count = 3; minimum = 0x10000; }
        else return 0;
        if (count > n - i) return 0;
        while (count--) {
            unsigned b = s[i++];
            if ((b & 0xC0) != 0x80) return 0;
            c = (c << 6) | (b & 63);
        }
        if (c < minimum || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return 0;
        units += c > 0xFFFF ? 2 : 1;
        if (units > 128) return 0;
    }
    return 1;
}

int s2_touch_text_editable(uint32_t window)
{
    if (!window || !void_vm_readable(window, 0x168) || !void_vm_writable(window, 0x168) ||
        (MEM32(window + 0xE8) & 0x8003u) != 0x8003u) return 0;
    if (MEM32(window) == 0xA0E56138u) return (MEM32(window + 0x160) & 1) != 0;
    if (MEM32(window) == 0xA0E583B8u) return MEM8(window + 0x165) != 0;
    return 0;
}

static void text_release(void)
{
    uint32_t window = text_session.window, release = text_session.release;
    text_session.iface = text_session.window = 0;
    if (window) void_call_guest_this(release, window, 0, NULL);
}

void s2_touch_cancel_text(void) { atomic_fetch_add(&text_epoch, 1); }

void s2_touch_text_pump(uint32_t manager)
{
    (void)manager;
    pthread_mutex_lock(&text_lock);
    int cleanup = text_state != TEXT_STARTED && text_session.iface && text_session.epoch != atomic_load(&text_epoch);
    pthread_mutex_unlock(&text_lock);
    if (cleanup) {
        void_regs saved;
        void_regs_save(&saved);
        text_release();
        void_regs_load(&saved);
    }
}

static uint32_t text_field(uint32_t manager, int x, int y, uint32_t *root)
{
    if (!manager || s2_touch_owner_manager() != manager || !void_vm_readable(manager, 0x48) || MEM32(manager + 0x34) ||
        (s2_touch_context() & S2_TOUCH_LOADING) || (void_input_key_state(1) & 0x8000) ||
        (void_input_key_state(2) & 0x8000) || (void_input_key_state(4) & 0x8000)) return 0;
    *root = MEM32(manager + 0x44);
    if (!*root || !void_vm_readable(*root, 0xEC) || !void_vm_readable(MEM32(*root), 0xC0)) return 0;
    uint32_t hit = s2_ui_pick(*root, x, y);
    uint32_t field = s2_touch_text_editable(hit) && MEM32(manager + 0x38) == hit ? hit : 0;
    if (!field) return 0;
    for (unsigned depth = 0; hit && hit != *root && depth < 16; ++depth) {
        if (!void_vm_readable(hit, 0xEC) || (MEM32(hit + 0xE8) & 3) != 3) return 0;
        hit = MEM32(hit + 0x58);
    }
    return hit == *root && (MEM32(*root + 0xE8) & 3) == 3 ? field : 0;
}

static int text_get(uint32_t iface, uint32_t vtable, uint8_t value[513], unsigned *length)
{
    uint32_t data, n;
    if (vtable == 0xA0E56138u) {
        if (!void_vm_readable(iface, 4) || MEM32(iface) != 0xA0E56404u) return 0;
        data = void_call_guest_this(0xA01C1C80u, iface, 0, NULL);
        n = void_call_guest_this(0xA0148BA0u, iface, 0, NULL);
    } else {
        uint32_t string = iface + 0x18;
        if (!void_vm_readable(string, 12) || MEM32(string) != 0xA0D9B470u) return 0;
        data = void_call_guest_this(0xA01ACD4Fu, string, 0, NULL);
        n = void_call_guest_this(0xA0149D00u, string, 0, NULL);
    }
    if (n > 512 || (n && (!data || !void_vm_readable(data, n + 1) || MEM8(data + n)))) return 0;
    if (n) memcpy(value, GPTR(data), n);
    value[n] = 0;
    if (!valid_utf8(value, n)) return 0;
    *length = n;
    return 1;
}

static int text_same(uint32_t manager, unsigned epoch)
{
    if (!text_session.iface || epoch != atomic_load(&text_epoch) || text_session.epoch != epoch ||
        s2_touch_context() != text_session.context || s2_ui_scale() != text_session.scale) return 0;
    int width, height;
    void_input_screen_size(&width, &height);
    if (width != text_session.width || height != text_session.height) return 0;
    uint32_t root = 0;
    return text_field(manager, text_session.x, text_session.y, &root) == text_session.window &&
        root == text_session.root && MEM32(text_session.window) == text_session.vtable;
}

static int text_begin(uint32_t manager, int x, int y, unsigned epoch)
{
    text_release();
    uint32_t root = 0, window = text_field(manager, x, y, &root);
    if (!window) return 0;
    uint32_t vtable = MEM32(window), release = MEM32(vtable + 8), slot = void_galloc(4);
    if (!slot) return 0;
    MEM32(slot) = 0;
    uint32_t args[2] = { vtable == 0xA0E56138u ? 0x231A1C57u : 0x21335C59u, slot };
    uint32_t ok = void_call_guest_this(vtable == 0xA0E56138u ? 0xA095079Au : 0xA095E9E7u, window, 2, args);
    uint32_t iface = MEM32(slot);
    void_gfree(slot);
    if (!(ok & 255) || iface != window + 0xF8 || !void_vm_readable(iface, 4) ||
        MEM32(iface) != (vtable == 0xA0E56138u ? 0xA0E56404u : 0xA0E58670u)) {
        /* Both exact original QIs retain this window, even if its secondary interface is unusable. */
        if (ok & 255) void_call_guest_this(release, window, 0, NULL);
        return 0;
    }
    text_session.window = window; text_session.iface = iface; text_session.root = root; text_session.vtable = vtable;
    text_session.release = release;
    text_session.x = x; text_session.y = y; text_session.epoch = epoch;
    text_session.context = s2_touch_context(); text_session.scale = s2_ui_scale();
    void_input_screen_size(&text_session.width, &text_session.height);
    if (!text_same(manager, epoch) || !text_get(iface, vtable, text_session.value, &text_session.length)) {
        text_release();
        return 0;
    }
    return 1;
}

static unsigned text_write(uint32_t manager, unsigned epoch, const uint8_t *value, unsigned length)
{
    if (!text_same(manager, epoch)) { text_release(); return S2_SLIDER_REJECTED; }
    uint8_t old[513]; unsigned old_length;
    if (!text_get(text_session.iface, text_session.vtable, old, &old_length) ||
        old_length != text_session.length || memcmp(old, text_session.value, old_length)) {
        text_release(); return S2_SLIDER_REJECTED;
    }
    if (old_length == length && !memcmp(old, value, length)) { text_release(); return S2_SLIDER_LIMIT; }
    uint32_t bytes = void_galloc(length + 1);
    if (!bytes) { text_release(); return S2_SLIDER_FAILED; }
    memcpy(GPTR(bytes), value, length); MEM8(bytes + length) = 0;
    if (text_session.vtable == 0xA0E56138u) {
        uint32_t args[3] = { bytes, length, 1 };
        void_call_guest_this(0xA09520EDu, text_session.iface, 3, args);
    } else void_call_guest_this(0xA095ED6Du, text_session.iface, 1, &bytes);
    void_gfree(bytes);
    unsigned result = S2_SLIDER_FAILED;
    /* A notification may replace the field: do not call getters on that stale UI target. */
    if (text_same(manager, epoch) && text_get(text_session.iface, text_session.vtable, old, &old_length) &&
        old_length == length && !memcmp(old, value, length)) result = S2_SLIDER_CHANGED;
    text_release();
    return result;
}

static int text_deadline(struct timespec *until, struct timespec *expiry)
{
    if (clock_gettime(CLOCK_REALTIME, until) || clock_gettime(CLOCK_MONOTONIC, expiry)) return 0;
    until->tv_nsec += 500000000; expiry->tv_nsec += 500000000;
    if (until->tv_nsec >= 1000000000) { ++until->tv_sec; until->tv_nsec -= 1000000000; }
    if (expiry->tv_nsec >= 1000000000) { ++expiry->tv_sec; expiry->tv_nsec -= 1000000000; }
    return 1;
}

static unsigned text_queue(unsigned kind, int x, int y, const uint8_t *value, unsigned length, const struct timespec *expiry)
{
    if (text_state == TEXT_QUEUED || text_state == TEXT_STARTED) return 0;
    unsigned serial = text_serial + 1;
    if (!serial) ++serial;
    text_state = TEXT_QUEUED; text_kind = kind; text_result = S2_SLIDER_IN_PROGRESS; text_abandoned = 0;
    text_serial = serial; text_x = x; text_y = y; text_length = length;
    if (length) memcpy(text_value, value, length);
    text_value[length] = 0;
    text_expiry = *expiry; text_epoch_request = atomic_load(&text_epoch);
    if (!s2_touch_ui_command(kind, serial)) { text_state = TEXT_CANCELLED; text_result = S2_SLIDER_CANCELLED; return 0; }
    return serial;
}

int s2_touch_text_read(int x, int y, uint8_t value[513], unsigned *length)
{
    struct timespec until, expiry;
    if (!text_deadline(&until, &expiry)) return 0;
    pthread_mutex_lock(&text_lock);
    if (text_state == TEXT_QUEUED || text_state == TEXT_STARTED) { pthread_mutex_unlock(&text_lock); return 0; }
    s2_touch_cancel_text();
    unsigned serial = text_queue(S2_UI_TEXT_READ, x, y, NULL, 0, &expiry);
    while (serial && text_serial == serial && (text_state == TEXT_QUEUED || text_state == TEXT_STARTED))
        if (pthread_cond_timedwait(&text_ready, &text_lock, &until)) break;
    int ok = serial && text_serial == serial && text_state == TEXT_DONE && text_result == S2_SLIDER_CHANGED;
    if (ok) { *length = text_readback_length; memcpy(value, text_readback, *length + 1); }
    else if (text_serial == serial && (text_state == TEXT_QUEUED || text_state == TEXT_STARTED)) {
        text_abandoned = 1;
        if (text_state == TEXT_QUEUED) text_state = TEXT_CANCELLED;
        s2_touch_cancel_text();
    }
    pthread_mutex_unlock(&text_lock);
    return ok;
}

uint64_t s2_touch_text_replace(const uint8_t *value, unsigned length)
{
    if ((!value && length) || length > 512 || !valid_utf8(value, length)) return S2_SLIDER_REJECTED;
    struct timespec until, expiry;
    if (!text_deadline(&until, &expiry)) return S2_SLIDER_REJECTED;
    pthread_mutex_lock(&text_lock);
    if (text_state == TEXT_QUEUED || text_state == TEXT_STARTED) { pthread_mutex_unlock(&text_lock); return S2_SLIDER_BUSY; }
    unsigned serial = text_queue(S2_UI_TEXT_WRITE, 0, 0, value, length, &expiry);
    while (serial && text_serial == serial && (text_state == TEXT_QUEUED || text_state == TEXT_STARTED))
        if (pthread_cond_timedwait(&text_ready, &text_lock, &until)) break;
    if (serial && text_serial == serial && text_state == TEXT_QUEUED) {
        text_state = TEXT_CANCELLED; text_result = S2_SLIDER_CANCELLED;
    }
    unsigned result = text_serial == serial ? (text_state == TEXT_STARTED ? S2_SLIDER_IN_PROGRESS : text_result) : S2_SLIDER_UNKNOWN;
    pthread_mutex_unlock(&text_lock);
    return serial ? (uint64_t)serial << 32 | result : S2_SLIDER_BUSY;
}

uint64_t s2_touch_text_status(unsigned serial)
{
    pthread_mutex_lock(&text_lock);
    uint64_t result = serial && serial == text_serial ? (uint64_t)serial << 32 |
        (text_state == TEXT_QUEUED || text_state == TEXT_STARTED ? S2_SLIDER_IN_PROGRESS : text_result) : (uint64_t)serial << 32;
    pthread_mutex_unlock(&text_lock);
    return result;
}

void s2_touch_text_owner(unsigned kind, unsigned serial, uint32_t manager)
{
    pthread_mutex_lock(&text_lock);
    struct timespec now;
    if (text_serial != serial || text_state != TEXT_QUEUED || text_kind != kind) { pthread_mutex_unlock(&text_lock); return; }
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec > text_expiry.tv_sec ||
        (now.tv_sec == text_expiry.tv_sec && now.tv_nsec >= text_expiry.tv_nsec)) {
        text_state = TEXT_CANCELLED; text_result = S2_SLIDER_CANCELLED;
        pthread_cond_broadcast(&text_ready); pthread_mutex_unlock(&text_lock); return;
    }
    text_state = TEXT_STARTED;
    int x = text_x, y = text_y; unsigned epoch = text_epoch_request, length = text_length;
    uint8_t value[513]; memcpy(value, text_value, length + 1);
    pthread_mutex_unlock(&text_lock);
    unsigned result;
    if (kind == S2_UI_TEXT_READ) result = text_begin(manager, x, y, epoch) ? S2_SLIDER_CHANGED : S2_SLIDER_REJECTED;
    else result = text_write(manager, epoch, value, length);
    pthread_mutex_lock(&text_lock);
    if (kind == S2_UI_TEXT_READ && (text_abandoned || epoch != atomic_load(&text_epoch))) {
        pthread_mutex_unlock(&text_lock); text_release(); pthread_mutex_lock(&text_lock);
        result = S2_SLIDER_REJECTED;
    }
    if (kind == S2_UI_TEXT_READ && result == S2_SLIDER_CHANGED) {
        text_readback_length = text_session.length;
        memcpy(text_readback, text_session.value, text_readback_length + 1);
    }
    if (text_serial == serial) { text_result = result; text_state = TEXT_DONE; }
    pthread_cond_broadcast(&text_ready);
    pthread_mutex_unlock(&text_lock);
}
