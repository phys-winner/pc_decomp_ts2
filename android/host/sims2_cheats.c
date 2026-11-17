/* The original cheat registry/parser, reached only through the UI owner's input fence. */
#include "host.h"
#include "input.h"
#include "sims2_settings.h"
#include "sims2_cheats.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

enum { CHEAT_IDLE, CHEAT_QUEUED, CHEAT_STARTED, CHEAT_DONE };
static pthread_mutex_t cheat_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned cheat_serial, cheat_state, cheat_result, cheat_context, cheat_epoch;
static int cheat_width, cheat_height;
static uint32_t cheat_manager;
static struct timespec cheat_expiry;
static char cheat_command[513];
static char **cheat_rows;
static unsigned cheat_count;

static int cheat_expired(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec > cheat_expiry.tv_sec ||
        (now.tv_sec == cheat_expiry.tv_sec && now.tv_nsec >= cheat_expiry.tv_nsec);
}

static void cheat_clear_rows(void)
{
    for (unsigned i = 0; i < cheat_count; ++i) free(cheat_rows[i]);
    free(cheat_rows); cheat_rows = NULL; cheat_count = 0;
}

static int cheat_world(unsigned context)
{
    unsigned world = context & ~S2_TOUCH_OPTIONS;
    return context == S2_TOUCH_OPTIONS || world == S2_TOUCH_TOWN || world == S2_TOUCH_LIVE ||
        world == S2_TOUCH_BUY || world == S2_TOUCH_BUILD;
}

static uint64_t cheat_request(const char *command)
{
    unsigned context = s2_touch_context();
    if (!cheat_world(context) || (command && (!*command || strlen(command) > 512 ||
        strpbrk(command, "\r\n\t")))) return S2_SLIDER_REJECTED;
    pthread_mutex_lock(&cheat_lock);
    if (cheat_state == CHEAT_QUEUED && cheat_expired()) { cheat_state = CHEAT_DONE; cheat_result = S2_SLIDER_CANCELLED; }
    if (cheat_state == CHEAT_QUEUED || cheat_state == CHEAT_STARTED) {
        pthread_mutex_unlock(&cheat_lock); return S2_SLIDER_BUSY;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &cheat_expiry)) { pthread_mutex_unlock(&cheat_lock); return S2_SLIDER_FAILED; }
    cheat_expiry.tv_sec += 2;
    if (!++cheat_serial) ++cheat_serial;
    cheat_context = context; cheat_epoch = s2_touch_layout_epoch();
    /* The owner binds catalog reads; execution retains that catalog's manager. */
    if (!command) cheat_manager = 0;
    void_input_screen_size(&cheat_width, &cheat_height);
    cheat_command[0] = 0; if (command) strcpy(cheat_command, command);
    cheat_clear_rows(); cheat_state = CHEAT_QUEUED; cheat_result = S2_SLIDER_IN_PROGRESS;
    if (!s2_touch_ui_command(S2_UI_CHEAT, cheat_serial)) { cheat_state = CHEAT_DONE; cheat_result = S2_SLIDER_BUSY; }
    uint64_t result = (uint64_t)cheat_serial << 32 | cheat_result;
    pthread_mutex_unlock(&cheat_lock);
    return result;
}

static uint64_t cheat_status(unsigned serial)
{
    pthread_mutex_lock(&cheat_lock);
    if (serial == cheat_serial && cheat_state == CHEAT_QUEUED && cheat_expired()) {
        cheat_state = CHEAT_DONE; cheat_result = S2_SLIDER_CANCELLED;
    }
    uint64_t result = (uint64_t)serial << 32 | (serial == cheat_serial ? cheat_result : S2_SLIDER_UNKNOWN);
    pthread_mutex_unlock(&cheat_lock);
    return result;
}

static void cheat_cancel(void)
{
    pthread_mutex_lock(&cheat_lock);
    if (cheat_state == CHEAT_QUEUED) { cheat_state = CHEAT_DONE; cheat_result = S2_SLIDER_CANCELLED; }
    pthread_mutex_unlock(&cheat_lock);
}

static int cheat_encode(const uint16_t *units, unsigned length, char text[513])
{
    if (!units || !length || length > 128) return 0;
    size_t used = 0;
    for (unsigned i = 0; i < length; ++i) {
        uint32_t c = units[i];
        if (!c) return 0;
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (++i == length || units[i] < 0xDC00 || units[i] > 0xDFFF) return 0;
            c = 0x10000u + ((c - 0xD800u) << 10) + (units[i] - 0xDC00u);
        } else if (c >= 0xDC00 && c <= 0xDFFF) return 0;
        used += void_utf8_encode(c, text + used);
    }
    text[used] = 0;
    return 1;
}

static int cheat_valid(uint32_t manager)
{
    int width, height;
    void_input_screen_size(&width, &height);
    return manager && (!cheat_manager || manager == cheat_manager) && manager == s2_touch_owner_manager() &&
        void_vm_readable(manager, 0x48) && !MEM32(manager + 0x34) &&
        s2_touch_context() == cheat_context && cheat_world(cheat_context) &&
        s2_touch_layout_epoch() == cheat_epoch && width == cheat_width && height == cheat_height;
}

static uint32_t cheat_method(uint32_t object, unsigned slot)
{
    if (!object || !void_vm_readable(object, 4) || !void_vm_readable(MEM32(object), slot + 4)) return 0;
    uint32_t method = MEM32(MEM32(object) + slot);
    return method >= 0xA0001000u && method < 0xA0D99800u ? method : 0;
}

static char *cheat_string(uint32_t address, unsigned limit)
{
    if (!address) return NULL;
    unsigned n = 0;
    while (n < limit && void_vm_readable(address + n, 1) && MEM8(address + n)) ++n;
    if (n == limit || !void_vm_readable(address + n, 1)) return NULL;
    char *text = malloc(n * 3 + 1);
    if (!text) return NULL;
    size_t used = 0;
    for (unsigned i = 0; i < n; ++i) {
        uint8_t c = MEM8(address + i);
        used += void_utf8_encode(void_cp1252_to_u16(c), text + used);
    }
    text[used] = 0;
    return text;
}

static unsigned cheat_run(uint32_t manager, const char *command, char ***rows, unsigned *count)
{
    if (!void_vm_readable(0xA10C9378u, 4)) return S2_SLIDER_REJECTED;
    uint32_t service = MEM32(0xA10C9378u), query = cheat_method(service, 0x14);
    if (!query || !cheat_valid(manager)) return S2_SLIDER_REJECTED;
    uint32_t slot = void_galloc(517);
    if (!slot) return S2_SLIDER_FAILED;
    MEM32(slot) = 0;
    uint32_t args[3] = { 0x2A4915EAu, 0x4A490E00u, slot };
    uint32_t queried = void_call_guest_this(query, service, 3, args) & 255u;
    uint32_t iface = MEM32(slot), release = queried ? cheat_method(iface, 8) : 0;
    unsigned result = S2_SLIDER_REJECTED;
    if (!queried || !release || !void_vm_readable(iface, 4) || MEM32(iface) != 0xA0EB8188u || !cheat_valid(manager)) goto done;
    if (*command) {
        memcpy(GPTR(slot + 4), command, strlen(command) + 1);
        uint32_t text = slot + 4;
        /* The original console uses this exact Execute(const char*) entry at A0CAA70D. */
        result = void_call_guest_this(0xA0CBE460u, iface, 1, &text) & 255u ? S2_SLIDER_CHANGED : S2_SLIDER_REJECTED;
    } else {
        uint32_t parser = void_call_guest_this(0xA0CBE190u, iface, 0, NULL);
        uint32_t length_fn = cheat_method(parser, 0xA0), item_fn = cheat_method(parser, 0xA4);
        if (!length_fn || !item_fn || !cheat_valid(manager)) goto done;
        unsigned n = void_call_guest_this(length_fn, parser, 0, NULL);
        if (!n || n > 8192) goto done;
        *rows = calloc(n, sizeof **rows);
        if (!*rows) { result = S2_SLIDER_FAILED; goto done; }
        for (unsigned i = 0; i < n; ++i) {
            if (!cheat_valid(manager)) goto done;
            uint32_t item = void_call_guest_this(item_fn, parser, 1, &i);
            uint32_t name_fn = cheat_method(item, 0x14), help_fn = cheat_method(item, 0x18);
            if (!name_fn || !help_fn) goto done;
            char *name = cheat_string(void_call_guest_this(name_fn, item, 0, NULL), 129);
            uint32_t brief = 0;
            uint32_t help_address = void_call_guest_this(help_fn, item, 1, &brief);
            char *help = help_address ? cheat_string(help_address, 4096) : calloc(1, 1);
            if (!name || !*name || !help || strpbrk(name, " \r\n\t")) { free(name); free(help); goto done; }
            size_t size = strlen(name) + strlen(help) + 2;
            char *row = malloc(size);
            if (row) snprintf(row, size, "%s\t%s", name, help);
            free(name); free(help);
            if (!row) { result = S2_SLIDER_FAILED; goto done; }
            (*rows)[(*count)++] = row;
        }
        result = S2_SLIDER_CHANGED;
    }
done:
    if (release) void_call_guest_this(release, iface, 0, NULL);
    void_gfree(slot);
    return result;
}

void s2_cheats_owner(unsigned serial, uint32_t manager)
{
    char command[513];
    pthread_mutex_lock(&cheat_lock);
    if (serial != cheat_serial || cheat_state != CHEAT_QUEUED) { pthread_mutex_unlock(&cheat_lock); return; }
    if (cheat_expired() || !cheat_valid(manager)) {
        cheat_state = CHEAT_DONE; cheat_result = S2_SLIDER_CANCELLED;
        pthread_mutex_unlock(&cheat_lock); return;
    }
    if (!cheat_manager) cheat_manager = manager;
    cheat_state = CHEAT_STARTED; strcpy(command, cheat_command);
    pthread_mutex_unlock(&cheat_lock);
    char **rows = NULL; unsigned count = 0;
    unsigned result = cheat_run(manager, command, &rows, &count);
    pthread_mutex_lock(&cheat_lock);
    cheat_rows = rows; cheat_count = count; cheat_result = result; cheat_state = CHEAT_DONE;
    pthread_mutex_unlock(&cheat_lock);
    void_log("[cheats] serial=%u kind=%s result=%u count=%u", serial, *command ? "execute" : "catalog", result, count);
}

#ifdef __ANDROID__
#include <jni.h>
JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeCheatsRequest(JNIEnv *env, jclass cls, jstring command)
{
    (void)cls;
    if (!command) return (jlong)cheat_request(NULL);
    jsize units = (*env)->GetStringLength(env, command);
    if (units <= 0 || units > 128) return S2_SLIDER_REJECTED;
    const jchar *chars = (*env)->GetStringChars(env, command, NULL);
    if (!chars) return S2_SLIDER_FAILED;
    char text[513]; int valid = cheat_encode(chars, (unsigned)units, text);
    (*env)->ReleaseStringChars(env, command, chars);
    uint64_t result = valid ? cheat_request(text) : S2_SLIDER_REJECTED;
    return (jlong)result;
}
JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeCheatsStatus(JNIEnv *env, jclass cls, jint serial)
{
    (void)env; (void)cls; return (jlong)cheat_status((unsigned)serial);
}
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeCheatsCancel(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls; cheat_cancel();
}
JNIEXPORT jobjectArray JNICALL Java_org_sims2_port_GameActivity_nativeCheatsRows(JNIEnv *env, jclass cls, jint serial)
{
    (void)cls;
    pthread_mutex_lock(&cheat_lock);
    jobjectArray result = NULL;
    if ((unsigned)serial == cheat_serial && cheat_state == CHEAT_DONE && cheat_result == S2_SLIDER_CHANGED && !*cheat_command) {
        jclass type = (*env)->FindClass(env, "java/lang/String");
        if (type) result = (*env)->NewObjectArray(env, (jsize)cheat_count, type, NULL);
        for (unsigned i = 0; result && i < cheat_count; ++i) {
            jstring row = (*env)->NewStringUTF(env, cheat_rows[i]);
            if (!row) break;
            (*env)->SetObjectArrayElement(env, result, (jsize)i, row); (*env)->DeleteLocalRef(env, row);
        }
    }
    pthread_mutex_unlock(&cheat_lock);
    return result;
}
#endif
