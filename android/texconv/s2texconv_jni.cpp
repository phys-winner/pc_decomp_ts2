/* JNI for org.sims2.port.TextureCache (the launcher's texture card). */
#include <jni.h>

#include "s2texconv.h"

namespace {

struct Utf {
    JNIEnv *env;
    jstring text;
    const char *chars;
    Utf(JNIEnv *e, jstring t) : env(e), text(t), chars(t ? e->GetStringUTFChars(t, nullptr) : nullptr) {}
    ~Utf() { if (chars) env->ReleaseStringUTFChars(text, chars); }
};

}  // namespace

extern "C" {

/* {textures, bytes, convertedFormat, convertedTextures, convertedBytes, stale}, or null with the
 * reason in nativeMessage(). */
JNIEXPORT jlongArray JNICALL Java_org_sims2_port_TextureCache_nativeScan(JNIEnv *env, jclass, jstring game, jstring cache, jstring index, jint fd)
{
    Utf dir(env, game), output(env, cache), mount(env, index);
    s2texconv_status s;
    if (s2texconv_scan_at(dir.chars, output.chars, &s, mount.chars, fd)) return nullptr;
    jlong values[] = {s.textures, s.bytes, s.converted_format, s.converted_textures, s.converted_bytes, s.stale};
    jlongArray out = env->NewLongArray(6);
    if (out) env->SetLongArrayRegion(out, 0, 6, values);
    return out;
}

JNIEXPORT jint JNICALL Java_org_sims2_port_TextureCache_nativeConvert(JNIEnv *env, jclass, jstring game, jstring cache, jint format, jint threads, jstring index, jint fd)
{
    Utf dir(env, game), output(env, cache), mount(env, index);
    return s2texconv_convert_at(dir.chars, output.chars, format, threads, mount.chars, fd);
}

JNIEXPORT jint JNICALL Java_org_sims2_port_TextureCache_nativeRemove(JNIEnv *env, jclass, jstring game)
{
    Utf dir(env, game);
    return dir.chars ? s2texconv_remove(dir.chars) : -1;
}

JNIEXPORT void JNICALL Java_org_sims2_port_TextureCache_nativeCancel(JNIEnv *, jclass) { s2texconv_cancel(); }

/* {state, done, total, bytesDone, bytesTotal, levels}. */
JNIEXPORT jlongArray JNICALL Java_org_sims2_port_TextureCache_nativeProgress(JNIEnv *env, jclass)
{
    s2texconv_progress p;
    s2texconv_get_progress(&p);
    jlong values[] = {p.state, p.done, p.total, p.bytes_done, p.bytes_total, p.levels};
    jlongArray out = env->NewLongArray(6);
    if (out) env->SetLongArrayRegion(out, 0, 6, values);
    return out;
}

JNIEXPORT jstring JNICALL Java_org_sims2_port_TextureCache_nativeMessage(JNIEnv *env, jclass)
{
    s2texconv_progress p;
    s2texconv_get_progress(&p);
    return env->NewStringUTF(p.message);
}

}  // extern "C"
