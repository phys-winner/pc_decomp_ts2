/* Android application glue: JNI from GameActivity, the native window, the
 * game thread, touch input in DirectInput scan codes, and AAudio output for
 * the DirectSound mixer. */
#ifdef VOID_ANDROID_APP
#define _GNU_SOURCE
#include "readcache.h"
#include "host.h"
#include "hud.h"
#include "input.h"
#include "sims2_settings.h"
#include <aaudio/AAudio.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

int void_game_main(const char *game, const char *data, const char *images, int selftest);
void void_process_exit(uint32_t code, int run_detach);

static ANativeWindow *window;
static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t window_cond = PTHREAD_COND_INITIALIZER;
static pthread_t game_thread;
static int game_started;
static char game_dir[512], data_dir[512], images_dir[512];
static JavaVM *jvm;
static jobject activity;

/* The activity's surface comes and goes (Home, the app switcher, the screen
 * turning off): window_serial counts the changes, and the renderer takes a
 * new one up at its next present (d3d9_gl.c), acknowledging the serial it
 * has switched to; a destroyed surface waits (briefly) for that. */
static uint32_t window_serial, window_acked;
static int audio_paused;
static uint64_t audio_pause_state;

uint64_t void_audio_pause_state(void)
{
    return __atomic_load_n(&audio_pause_state, __ATOMIC_ACQUIRE);
}
extern void __llvm_profile_set_filename(const char *) __attribute__((weak));

/* d3d9_gl.c asks for the window when the game creates its device; wait for
 * the surface if the activity has not delivered it yet. */
void *void_android_window(void)
{
    pthread_mutex_lock(&window_lock);
    while (!window) pthread_cond_wait(&window_cond, &window_lock);
    ANativeWindow *w = window;
    pthread_mutex_unlock(&window_lock);
    return w;
}

/* The current window (NULL while the activity has none) and its serial; with
 * wait set, block until there is one. */
void *void_android_window_current(uint32_t *serial, int wait)
{
    pthread_mutex_lock(&window_lock);
    while (wait && !window) pthread_cond_wait(&window_cond, &window_lock);
    ANativeWindow *w = window;
    *serial = window_serial;
    pthread_mutex_unlock(&window_lock);
    return w;
}

/* The screen the game is shown on (sims2_settings.c turns it and the UI
 * scale into the game's resolution): the activity's surface, landscape. */
void void_android_screen_size(int *w, int *h)
{
    pthread_mutex_lock(&window_lock);
    while (!window) pthread_cond_wait(&window_cond, &window_lock);
    *w = ANativeWindow_getWidth(window);
    *h = ANativeWindow_getHeight(window);
    pthread_mutex_unlock(&window_lock);
}

void *void_android_window_acquire(uint32_t *serial)
{
    pthread_mutex_lock(&window_lock);
    ANativeWindow *current = window;
    *serial = window_serial;
    if (current) ANativeWindow_acquire(current);
    pthread_mutex_unlock(&window_lock);
    return current;
}

int void_android_display_size(int *width, int *height)
{
    pthread_mutex_lock(&window_lock);
    *width = window ? ANativeWindow_getWidth(window) : 0;
    *height = window ? ANativeWindow_getHeight(window) : 0;
    pthread_mutex_unlock(&window_lock);
    return *width > 0 && *height > 0;
}

void void_android_game_exited(int code)
{
    if (!jvm || !activity) return;
    JNIEnv *env = NULL;
    int attached = (*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK;
    if (attached && (*jvm)->AttachCurrentThread(jvm, &env, NULL) != JNI_OK) return;
    jclass cls = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetMethodID(env, cls, "nativeExited", "(I)V");
    if (method) (*env)->CallVoidMethod(env, activity, method, code);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, cls);
    if (attached) (*jvm)->DetachCurrentThread(jvm);
}

void void_android_window_ack(uint32_t serial)
{
    pthread_mutex_lock(&window_lock);
    window_acked = serial;
    pthread_cond_broadcast(&window_cond);
    pthread_mutex_unlock(&window_lock);
}

static void *game_main(void *arg)
{
    (void)arg;
    /* APKs have no writable working directory for LLVM's default.profraw. */
    static char profile_path[sizeof data_dir + sizeof "/sims2.profraw"];
    const char *profile_env = getenv("LLVM_PROFILE_FILE");
    if (__llvm_profile_set_filename && (!profile_env || !*profile_env)) {
        snprintf(profile_path, sizeof profile_path, "%s/sims2.profraw", data_dir);
        __llvm_profile_set_filename(profile_path);
    }
    int code = void_game_main(game_dir, data_dir, images_dir, 0);
    void_log("[android] game thread finished with %d", code);
    void_process_exit((uint32_t)code, 0);
    return NULL;
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    jvm = vm;
    return JNI_VERSION_1_6;
}

extern const char *void_command_line;
static char command_line[2048];

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeStart(JNIEnv *env, jobject self, jstring game,
    jstring data, jstring images, jstring args, jboolean verbose)
{
    if (game_started) return;
    const char *g = (*env)->GetStringUTFChars(env, game, NULL);
    const char *d = (*env)->GetStringUTFChars(env, data, NULL);
    const char *i = (*env)->GetStringUTFChars(env, images, NULL);
    const char *a = (*env)->GetStringUTFChars(env, args, NULL);
    snprintf(game_dir, sizeof game_dir, "%s", g);
    snprintf(data_dir, sizeof data_dir, "%s", d);
    snprintf(images_dir, sizeof images_dir, "%s", i);
    /* <sdcard>/Sims2/args.txt: the game's own command-line switches. */
    if (a && *a) {
        snprintf(command_line, sizeof command_line, "\"C:\\Games\\The Sims 2 Legacy\\EP9\\TSBin\\Sims2EP9.exe\" %s", a);
        void_command_line = command_line;
    }
    (*env)->ReleaseStringUTFChars(env, game, g);
    (*env)->ReleaseStringUTFChars(env, data, d);
    (*env)->ReleaseStringUTFChars(env, images, i);
    (*env)->ReleaseStringUTFChars(env, args, a);
    activity = (*env)->NewGlobalRef(env, self);
    void_verbose = verbose ? 1 : 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);
    game_started = pthread_create(&game_thread, &attr, game_main, NULL) == 0;
    pthread_attr_destroy(&attr);
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeSurface(JNIEnv *env, jobject self, jobject surface)
{
    (void)self;
    s2_touch_reset_cas_pan();
    pthread_mutex_lock(&window_lock);
    ANativeWindow *old = window;
    window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    uint32_t serial = ++window_serial;
    pthread_cond_broadcast(&window_cond);
    if (!surface && old) {
        /* Android wants the EGL surface gone before surfaceDestroyed returns;
         * the game thread lets it go at its next present. A game busy
         * loading may not present for a while: do not hold the UI thread
         * long (EGL keeps its own reference to the window meanwhile). */
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 1;
        while (window_acked != serial && pthread_cond_timedwait(&window_cond, &window_lock, &until) == 0) {}
    }
    pthread_mutex_unlock(&window_lock);
    if (old) ANativeWindow_release(old);
}

/* onPause/onResume: the game itself waits in its present while it has no
 * surface; the audio stream pauses with the activity. */
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativePause(JNIEnv *env, jobject self, jboolean paused)
{
    (void)env; (void)self;
    void_clock_pause(paused);
    uint64_t state = void_audio_pause_state(), next;
    /* Keep transitions even when Home and resume both fall between worker ticks. */
    do {
        if ((state & 1u) == (paused ? 1u : 0u)) break;
        next = ((state & ~UINT64_C(1)) + 2u) | (paused ? 1u : 0u);
    } while (!__atomic_compare_exchange_n(&audio_pause_state, &state, next, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
    __atomic_store_n(&audio_paused, (int)(void_audio_pause_state() & 1u), __ATOMIC_RELAXED);
    if (paused) s2_touch_reset_cas_pan();
    if (paused) void_readcache_flush_all();        /* Android may end a paused app */
}

/* Scan code (DIK) based input from the touch layer. */
static uint32_t dik_to_vk(uint32_t dik)
{
    for (uint32_t vk = 1; vk < 256; ++vk) if (void_vk_to_dik(vk) == dik) return vk;
    return 0;
}

/* The touch layer sends Windows virtual-key codes (the game reads window messages). */
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeKey(JNIEnv *env, jobject self, jint vk, jboolean down)
{
    (void)env; (void)self;
    if (vk > 0 && vk < 256) void_input_key((uint32_t)vk, down);
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeText(JNIEnv *env, jclass cls, jstring text)
{
    (void)cls;
    if (!text) return JNI_FALSE;
    jsize length = (*env)->GetStringLength(env, text);
    if (length > 128) return JNI_FALSE;
    const jchar *chars = (*env)->GetStringChars(env, text, NULL);
    if (!chars) return JNI_FALSE;
    int accepted = void_input_text(chars, (uint32_t)length);
    (*env)->ReleaseStringChars(env, text, chars);
    return accepted ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseButton(JNIEnv *env, jobject self, jint button, jboolean down)
{
    (void)env; (void)self;
    if (down && button >= 0 && button <= 7) s2_touch_camera_pointer_intent();
    void_input_mouse_button(button, down);
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchContext(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jint)s2_touch_context();
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchRewards(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_touch_rewards() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchUiScale(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_ui_scale();
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchSetUiScale(JNIEnv *env, jclass cls, jint scale)
{
    (void)env; (void)cls;
    return s2_touch_ui_command(S2_UI_SCALE, (uint32_t)scale) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchPanCas(JNIEnv *env, jclass cls, jfloat dy)
{
    (void)env; (void)cls;
    uint32_t value;
    memcpy(&value, &dy, 4);
    return s2_touch_ui_command(S2_UI_CAS_PAN, value) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchCamera(JNIEnv *env, jclass cls, jfloat dx, jfloat dy, jfloat radians, jfloat zoom, jfloat pitch)
{
    (void)env; (void)cls;
    return s2_touch_camera(dx, dy, radians, zoom, pitch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchPan(JNIEnv *env, jclass cls, jfloat from_x, jfloat from_y, jfloat to_x, jfloat to_y)
{
    (void)env; (void)cls;
    return s2_touch_pan(from_x, from_y, to_x, to_y) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeTouchCameraContact(JNIEnv *env, jclass cls, jboolean active)
{
    (void)env; (void)cls;
    s2_touch_camera_contact(active != JNI_FALSE);
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchObjectPreview(JNIEnv *env,jclass cls,jlong token)
{
    (void)env;(void)cls;return s2_touch_object_preview((uint64_t)token)?JNI_TRUE:JNI_FALSE;
}

unsigned d3d_fsr3_status(void);
JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeUpscaleStatus(JNIEnv *env,jclass cls)
{
    (void)env;(void)cls;return (jint)d3d_fsr3_status();
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchTool(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_touch_tool();
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchObjectCost(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_touch_object_cost();
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchToolToken(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_tool_token();
}

/* A point of the view in [0,1] as game pixels, undoing the present's
 * aspect-preserving fit (d3d9_gl.c). */
static int view_to_game(float x, float y, int *gx, int *gy)
{
    int w, h;
    *gx = *gy = -1;
    if (!isfinite(x) || !isfinite(y)) return 0;
    void_input_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return 0;
    pthread_mutex_lock(&window_lock);
    float sw = window ? (float)ANativeWindow_getWidth(window) : (float)w;
    float sh = window ? (float)ANativeWindow_getHeight(window) : (float)h;
    pthread_mutex_unlock(&window_lock);
    if (sw <= 0 || sh <= 0) return 0;
    float scale = sw / (float)w < sh / (float)h ? sw / (float)w : sh / (float)h;
    float dx = (sw - (float)w * scale) / 2.0f, dy = (sh - (float)h * scale) / 2.0f;
    float fx = (x * sw - dx) / scale, fy = (y * sh - dy) / scale;
    int inside = fx >= 0 && fy >= 0 && fx < w && fy < h;
    *gx = (int)lroundf(fminf(fmaxf(fx, 0), (float)(w - 1)));
    *gy = (int)lroundf(fminf(fmaxf(fy, 0), (float)(h - 1)));
    return inside;
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchHitKind(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)env; (void)cls;
    if (!isfinite(x) || !isfinite(y)) return 1;
    if (s2_touch_context() & S2_TOUCH_LOADING) return 1;
    int gx, gy;
    if (!view_to_game(x, y, &gx, &gy)) return 1;
    return s2_touch_pick(gx, gy);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchTarget(JNIEnv *env, jclass cls, jfloat x, jfloat y, jfloat radius)
{
    (void)env; (void)cls;
    int gx, gy, edge_x, edge_y;
    if (!isfinite(radius) || radius <= 0 || radius > .1f || !view_to_game(x, y, &gx, &gy)) return -1;
    view_to_game(x < .5f ? x + radius : x - radius, y, &edge_x, &edge_y);
    return (jlong)s2_touch_target(gx, gy, abs(edge_x - gx));
}

JNIEXPORT jfloatArray JNICALL Java_org_sims2_port_GameActivity_nativeTouchUiPick(JNIEnv *env, jclass cls, jfloat x, jfloat y, jfloat diameter)
{
    (void)cls;
    int gx, gy, edge_x, edge_y, kind = (s2_touch_context() & S2_TOUCH_LOADING) ? 1 : 0;
    float values[3] = {0, x, y};
    if (!kind && isfinite(diameter) && diameter > 0 && diameter <= .25f && view_to_game(x, y, &gx, &gy)) {
        view_to_game(x < .5f ? x + diameter : x - diameter, y, &edge_x, &edge_y);
        uint64_t point = (uint64_t)(uint32_t)gy << 32 | (uint32_t)gx;
        kind = s2_touch_ui_pick(gx, gy, abs(edge_x - gx), &point);
        int w, h;
        void_input_screen_size(&w, &h);
        pthread_mutex_lock(&window_lock);
        float sw = window ? (float)ANativeWindow_getWidth(window) : (float)w;
        float sh = window ? (float)ANativeWindow_getHeight(window) : (float)h;
        pthread_mutex_unlock(&window_lock);
        if (w > 0 && h > 0 && sw > 0 && sh > 0) {
            float scale = fminf(sw / w, sh / h);
            values[1] = ((uint32_t)point * scale + (sw - w * scale) * .5f) / sw;
            values[2] = ((uint32_t)(point >> 32) * scale + (sh - h * scale) * .5f) / sh;
        }
    }
    values[0] = kind;
    jfloatArray result = (*env)->NewFloatArray(env, 3);
    if (result) (*env)->SetFloatArrayRegion(env, result, 0, 3, values);
    return result;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchScroll(JNIEnv *env, jclass cls,
    jfloat x, jfloat y, jint amount, jint context, jlong size, jlong epoch)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((uint64_t)epoch > UINT32_MAX || !view_to_game(x, y, &gx, &gy)) return JNI_FALSE;
    return s2_touch_scroll(gx, gy, amount, (unsigned)context, (uint64_t)size, (unsigned)epoch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchScrollOwner(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_scroll_owner();
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeTouchStopFling(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    s2_touch_cancel_fling();
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchFling(JNIEnv *env, jclass cls,
    jfloat x, jfloat y, jint amount, jint context, jlong size, jlong epoch, jlong owner)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((uint64_t)epoch > UINT32_MAX || !view_to_game(x, y, &gx, &gy)) return JNI_FALSE;
    return s2_touch_fling(gx, gy, amount, (unsigned)context, (uint64_t)size, (unsigned)epoch, (uint64_t)owner) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchCatalogInfo(JNIEnv *env, jclass cls,
    jfloat x, jfloat y, jint context, jlong size, jlong epoch)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((uint64_t)epoch > UINT32_MAX || !view_to_game(x, y, &gx, &gy)) return JNI_FALSE;
    return s2_touch_catalog_info(gx, gy, (unsigned)context, (uint64_t)size, (unsigned)epoch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchClearCatalog(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_touch_ui_command(S2_UI_CLEAR_CATALOG, 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_org_sims2_port_GameActivity_nativeTouchToolMenu(JNIEnv *env, jclass cls,
    jint action, jint context, jlong size, jlong epoch)
{
    (void)env; (void)cls;
    if ((uint64_t)epoch > UINT32_MAX) return JNI_FALSE;
    return s2_touch_tool_menu(action, (unsigned)context, (uint64_t)size, (unsigned)epoch) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jbyteArray JNICALL Java_org_sims2_port_GameActivity_nativeTouchReadText(JNIEnv *env, jclass cls, jfloat x, jfloat y)
{
    (void)cls;
    int gx, gy;
    uint8_t value[513];
    unsigned length;
    if (!view_to_game(x, y, &gx, &gy) || !s2_touch_text_read(gx, gy, value, &length)) return NULL;
    jbyteArray result = (*env)->NewByteArray(env, (jsize)length);
    if (!result) { s2_touch_cancel_text(); return NULL; }
    if (length) (*env)->SetByteArrayRegion(env, result, 0, (jsize)length, (const jbyte *)value);
    if ((*env)->ExceptionCheck(env)) { s2_touch_cancel_text(); return NULL; }
    return result;
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchReplaceText(JNIEnv *env, jclass cls, jbyteArray value)
{
    (void)cls;
    if (!value) return S2_SLIDER_REJECTED;
    jsize length = (*env)->GetArrayLength(env, value);
    if (length > 512) return S2_SLIDER_REJECTED;
    uint8_t bytes[513];
    if (length) (*env)->GetByteArrayRegion(env, value, 0, length, (jbyte *)bytes);
    if ((*env)->ExceptionCheck(env)) return S2_SLIDER_REJECTED;
    return (jlong)s2_touch_text_replace(bytes, (unsigned)length);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchReplaceTextStatus(JNIEnv *env, jclass cls, jint serial)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_text_status((unsigned)serial);
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeTouchCancelText(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    s2_touch_cancel_text();
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchSliderStep(JNIEnv *env, jclass cls, jfloat x, jfloat y, jint delta)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((delta != -1 && delta != 1) || !view_to_game(x, y, &gx, &gy)) return S2_SLIDER_REJECTED;
    return (jlong)s2_touch_slider_step(gx, gy, delta);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchSliderStepStatus(JNIEnv *env, jclass cls, jint serial)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_slider_status((unsigned)serial);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchLayoutEpoch(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_layout_epoch();
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchSwipeFeedback(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return (jlong)s2_touch_swipe_feedback();
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchRotateObject(JNIEnv *env, jclass cls,
    jfloat x, jfloat y, jlong tool, jint context, jlong size, jlong epoch, jint direction)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((uint64_t)epoch > UINT32_MAX || !view_to_game(x, y, &gx, &gy)) return S2_SLIDER_REJECTED;
    return (jlong)s2_touch_rotate_object(gx, gy, (uint64_t)tool, (unsigned)context, (uint64_t)size, (unsigned)epoch, direction);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchRotateObjectStatus(JNIEnv *env, jclass cls, jint serial)
{
    (void)env; (void)cls;
    uint64_t result = s2_touch_slider_status((unsigned)serial);
    return (jlong)(result ? result : (uint64_t)(unsigned)serial << 32);
}

JNIEXPORT jint JNICALL Java_org_sims2_port_GameActivity_nativeTouchFillKind(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return s2_touch_fill_kind();
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchFillRoom(JNIEnv *env, jclass cls,
    jfloat x, jfloat y, jlong tool, jint context, jlong size, jlong epoch)
{
    (void)env; (void)cls;
    int gx, gy;
    if ((uint64_t)epoch > UINT32_MAX || !view_to_game(x, y, &gx, &gy)) return S2_SLIDER_REJECTED;
    return (jlong)s2_touch_fill_room(gx, gy, (uint64_t)tool, (unsigned)context, (uint64_t)size, (unsigned)epoch);
}

JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchFillRoomStatus(JNIEnv *env, jclass cls, jint serial)
{
    (void)env; (void)cls;
    uint64_t result = s2_touch_slider_status((unsigned)serial);
    return (jlong)(result ? result : (uint64_t)(unsigned)serial << 32);
}

/* Logical cursor dimensions for Java's fractional relative-motion remainder. */
JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchGameSize(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    int w, h;
    void_input_screen_size(&w, &h);
    return w > 0 && h > 0 ? (jlong)((uint64_t)(uint32_t)h << 32 | (uint32_t)w) : 0;
}

/* Actual clamped game cursor projected back through the presentation fit.
 * Low/high words are view-normalized x/y in millionths. */
JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeTouchCursor(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    int w, h;
    uint32_t x, y;
    void_input_screen_size(&w, &h);
    if (w <= 0 || h <= 0) return 0;
    void_input_cursor(&x, &y);
    pthread_mutex_lock(&window_lock);
    float sw = window ? (float)ANativeWindow_getWidth(window) : (float)w;
    float sh = window ? (float)ANativeWindow_getHeight(window) : (float)h;
    pthread_mutex_unlock(&window_lock);
    if (sw <= 0 || sh <= 0) return 0;
    float scale = fminf(sw / (float)w, sh / (float)h);
    uint32_t nx = (uint32_t)lroundf(((float)x * scale + (sw - (float)w * scale) * 0.5f) / sw * 1000000.0f);
    uint32_t ny = (uint32_t)lroundf(((float)y * scale + (sh - (float)h * scale) * 0.5f) / sh * 1000000.0f);
    return (jlong)(((uint64_t)ny << 32) | nx);
}

/* Relative motion in game pixels, or an absolute position in [0,1]. */
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseMove(JNIEnv *env, jobject self, jfloat x, jfloat y, jboolean relative)
{
    (void)env; (void)self;
    if (!isfinite(x) || !isfinite(y)) return;
    if (relative) {
        if (x < -2147483648.0f || x >= 2147483648.0f || y < -2147483648.0f || y >= 2147483648.0f) return;
        int dx = (int)x, dy = (int)y;
        if (dx || dy) s2_touch_camera_pointer_intent();
        void_input_mouse_move(dx, dy);
    } else {
        int nx, ny;
        uint32_t cx, cy;
        view_to_game(x, y, &nx, &ny);
        if (nx < 0 || ny < 0) return;
        s2_touch_camera_pointer_intent();
        void_input_cursor(&cx, &cy);
        void_input_mouse_move(nx - (int)cx, ny - (int)cy);
    }
}

JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseWheel(JNIEnv *env, jobject self, jint delta)
{
    (void)env; (void)self;
    if (delta) s2_touch_camera_pointer_intent();
    void_input_mouse_wheel(delta);
}

/* What the last presented frame showed (hud.h): the cursor and the helper
 * icons, in the low 32 bits; the count of presented frames in the high ones. */
JNIEXPORT jlong JNICALL Java_org_sims2_port_GameActivity_nativeHudState(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    uint32_t frame = 0, tags = void_hud_state(&frame);
    return (jlong)(((uint64_t)frame << 32) | tags);
}

/* Put the cursor at a point of the view in [0,1] without mouse motion (a
 * paint stroke starts under the finger instead of turning the camera). */
JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_nativeMouseWarp(JNIEnv *env, jobject self, jfloat x, jfloat y)
{
    (void)env; (void)self;
    int nx, ny;
    view_to_game(x, y, &nx, &ny);
    if (nx < 0 || ny < 0) return;
    s2_touch_camera_pointer_intent();
    void_input_mouse_warp(nx, ny);
}

/* ------------------------------------------------------------- audio */
static AAudioStream *stream;
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t stream_pause_state;
static uint64_t audio_short_writes, audio_dropped_frames, audio_errors;
static unsigned audio_zero_chunks;
static int32_t audio_last_xruns;
static unsigned audio_bad_chunks;
/* Server-mixed output avoids the MMAP stalls observed on several devices.
 * Tune its queue too: Android's initial size can add unnecessary latency. */
static int audio_normal_output = 1;

static void audio_close_stream(void)
{
    if (stream) {
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
    }
    stream = NULL;
    audio_zero_chunks = 0;
    audio_last_xruns = 0;
    audio_bad_chunks = 0;
}

static void audio_close_failed(aaudio_result_t error)
{
    if (++audio_errors <= 4) void_log("[audio] stream error %d; reopening on next chunk", (int)error);
    audio_close_stream();
}

/* A timed-out write can accept a prefix. Never resend it or silently discard
 * the suffix; retry within a bounded budget, with no guest VM lock held. */
static uint32_t audio_write_frames(const int16_t *frames, uint32_t count)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t deadline = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec + 100000000LL;
    uint32_t written = 0;
    uint64_t pause = stream_pause_state;
    while (written < count && void_audio_pause_state() == pause) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = deadline - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
        if (remaining <= 0) break;
        aaudio_result_t result = AAudioStream_write(stream, frames + written * 2,
            (int32_t)(count - written), remaining < 20000000LL ? remaining : 20000000LL);
        if (result < 0) { audio_close_failed(result); break; }
        if ((uint32_t)result < count - written) ++audio_short_writes;
        written += (uint32_t)result;
        if (!result) usleep(1000);
    }
    if (written < count && void_audio_pause_state() == pause) {
        if (audio_dropped_frames < 1920)
            void_log("[audio] discarded %u unwritten frames after timeout/error", count - written);
        audio_dropped_frames += count - written;
    }
    return written;
}

uint32_t void_audio_output(const int16_t *frames, uint32_t count)
{
    pthread_mutex_lock(&audio_lock);
    uint64_t pause = void_audio_pause_state();
    /* Pause retains queued PCM; a fresh foreground epoch needs a fresh stream. */
    if (stream && pause != stream_pause_state) audio_close_stream();
    if (pause & 1u) {
        /* mixer_main already paces every chunk, including in the background. */
        pthread_mutex_unlock(&audio_lock);
        return 0;
    }
    if (!stream) {
        AAudioStreamBuilder *b = NULL;
        aaudio_result_t created = AAudio_createStreamBuilder(&b);
        if (created == AAUDIO_OK) {
            AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
            AAudioStreamBuilder_setChannelCount(b, 2);
            AAudioStreamBuilder_setSampleRate(b, 48000);
            AAudioStreamBuilder_setBufferCapacityInFrames(b, 1920);
            AAudioStreamBuilder_setPerformanceMode(b, audio_normal_output ?
                AAUDIO_PERFORMANCE_MODE_NONE : AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
            aaudio_result_t result = AAudioStreamBuilder_openStream(b, &stream);
            if (result == AAUDIO_OK) {
                stream_pause_state = pause;
                /* The producer wakes every 10 ms; two tiny MMAP bursts cannot cover it. */
                int32_t burst = AAudioStream_getFramesPerBurst(stream);
                int32_t target = burst > 0 ? (int32_t)(((int64_t)960 + burst - 1) / burst * burst) : 960;
                int32_t before = AAudioStream_getBufferSizeInFrames(stream);
                if (before != target) AAudioStream_setBufferSizeInFrames(stream, target);
                void_log("[audio] output: burst %d, capacity %d, buffer %d -> %d frames, normal %d", (int)burst,
                    (int)AAudioStream_getBufferCapacityInFrames(stream), (int)before,
                    (int)AAudioStream_getBufferSizeInFrames(stream), audio_normal_output);
                /* A partially primed queue can underrun before the first real chunk. */
                static const int16_t silence[2 * 1920] = {0};
                int32_t prefill = AAudioStream_getBufferSizeInFrames(stream);
                if (prefill <= 0) prefill = 960;
                for (int32_t filled = 0; filled < prefill && void_audio_pause_state() == pause;) {
                    int32_t remaining = prefill - filled;
                    result = AAudioStream_write(stream, silence, remaining < 1920 ? remaining : 1920, 0);
                    if (result <= 0) break;
                    filled += result;
                }
                if (result >= 0) {
                    if (void_audio_pause_state() == pause) result = AAudioStream_requestStart(stream);
                    else audio_close_stream();
                }
                if (result < 0) audio_close_failed(result);
            } else { stream = NULL; audio_close_failed(result); }
            AAudioStreamBuilder_delete(b);
        } else audio_close_failed(created);
        if (!stream) {
            if (void_audio_pause_state() == pause) audio_dropped_frames += count;
            pthread_mutex_unlock(&audio_lock);
            return 0;
        }
    }
    uint32_t written = audio_write_frames(frames, count);
    if (written || void_audio_pause_state() != pause) audio_zero_chunks = 0;
    else if (stream && count && ++audio_zero_chunks == 3) {
        ++audio_errors;
        if (!audio_normal_output) {
            audio_normal_output = 1;
            void_log("[audio] low latency stream stalled for 3 chunks; using normal output until process exit");
        } else void_log("[audio] normal stream stalled for 3 chunks; reopening on next chunk");
        audio_close_stream();
    }
    if (stream) {
        int32_t xruns = AAudioStream_getXRunCount(stream);
        if (xruns > audio_last_xruns) {
            int32_t size = AAudioStream_getBufferSizeInFrames(stream);
            int32_t capacity = AAudioStream_getBufferCapacityInFrames(stream);
            int32_t burst = AAudioStream_getFramesPerBurst(stream);
            int32_t grown = size;
            if (size > 0 && capacity > size) {
                int64_t target = (int64_t)size + (burst > 480 ? burst : 480);
                if (burst > 0) target = (target + burst - 1) / burst * burst;
                grown = AAudioStream_setBufferSizeInFrames(stream, (int32_t)(target < capacity ? target : capacity));
            }
            if (grown > size) {
                audio_bad_chunks = 0;
                void_log("[audio] underruns %d: buffer %d -> %d frames", (int)xruns, (int)size, (int)grown);
            } else if (!audio_normal_output && ++audio_bad_chunks == 3) {
                audio_normal_output = 1;
                ++audio_errors;
                void_log("[audio] low latency underruns persist at buffer limit; using normal output until process exit");
                audio_close_stream();
            }
        } else audio_bad_chunks = 0;
        if (stream && xruns >= 0) audio_last_xruns = xruns;
    }
    /* Level of what reached the device, every 10 s of audio: silence or
     * underruns show up in the log rather than only by ear. */
    static uint32_t level_frames;
    static double level_sum;
    static int level_peak;
    for (uint32_t i = 0; i < written * 2; ++i) {
        int v = frames[i] < 0 ? -frames[i] : frames[i];
        if (v > level_peak) level_peak = v;
        level_sum += (double)frames[i] * frames[i];
    }
    level_frames += written;
    if (level_frames >= 48000u * 10u) {
        void_log("[audio] last 10 s: peak %d, rms %.0f, underruns %d, short writes %llu, dropped frames %llu, errors %llu", level_peak,
                 sqrt(level_sum / (2.0 * level_frames)), stream ? (int)AAudioStream_getXRunCount(stream) : -1,
                 (unsigned long long)audio_short_writes, (unsigned long long)audio_dropped_frames,
                 (unsigned long long)audio_errors);
        level_frames = 0;
        level_sum = 0;
        level_peak = 0;
    }
    /* The mixer only paces chunks, so a queue drained by a late wake or an
     * underrun (MMAP restarts writing at the read position) never refills
     * by itself and every later hiccup underruns: ask for the shortfall. */
    uint32_t wanted = 0;
    if (stream && written == count && void_audio_pause_state() == pause) {
        int64_t queued = AAudioStream_getFramesWritten(stream) - AAudioStream_getFramesRead(stream);
        int32_t size = AAudioStream_getBufferSizeInFrames(stream);
        if (queued >= 0 && size > 0 && queued < size) wanted = (uint32_t)(size - queued);
    }
    pthread_mutex_unlock(&audio_lock);
    return wanted;
}
#endif
