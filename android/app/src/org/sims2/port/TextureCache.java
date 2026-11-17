package org.sims2.port;

import java.io.File;
import org.sims2.port.launcher.TextureBackend;

/** The launcher base's texture card (port.json "textures.backend") over the port's converter
 *  (android/texconv, libs2texconv.so): every DXT mip level of the installed game re-encoded once
 *  into ETC2 or ASTC 4x4 and filed by a hash of its original blocks in
 *  &lt;game&gt;/port_textures/textures.s2tc, which the renderer (android/host/d3d9_texcache.c)
 *  uploads instead of decoding DXT on the CPU. The game's own package files stay as they are;
 *  undoing the conversion deletes that folder. The base runs these on its background work and
 *  polls progress(); one operation at a time. */
public final class TextureCache implements TextureBackend {
    private static native long[] nativeScan(String game, String index, int archiveFd);
    private static native int nativeConvert(String game, int format, int threads, String index, int archiveFd);
    private static native int nativeRemove(String game);
    private static native void nativeCancel();
    private static native long[] nativeProgress();
    private static native String nativeMessage();

    private static Boolean loaded;
    private static final Object operation = new Object();
    private volatile String message = "";
    private volatile boolean running;

    static synchronized boolean available() {
        if (loaded == null) {
            try {
                System.loadLibrary("s2texconv");
                loaded = true;
            } catch (Throwable e) {
                android.util.Log.w("Sims2", "textures: no converter: " + e);
                loaded = false;
            }
        }
        return loaded;
    }

    @Override public Scan scan(File game) { return scan(game, null, -1); }

    @Override public Scan scan(File game, File index, int archiveFd) {
        Scan scan = new Scan();
        scan.sampled = true;
        if (!available()) {
            scan.failure = "libs2texconv.so";
            return scan;
        }
        long[] v;
        synchronized (operation) { v = nativeScan(game.getAbsolutePath(), index == null ? null : index.getAbsolutePath(), archiveFd); }
        if (v == null || v.length < 6) {
            scan.failure = nativeMessage();
            return scan;
        }
        scan.textures = v[0];
        scan.bytes = v[1];
        scan.converted = (int) v[2] == ASTC ? ASTC : (int) v[2] == ETC2 ? ETC2 : 0;
        scan.convertedTextures = v[3];
        scan.convertedBytes = v[4];
        return scan;
    }

    /** Blocking, on the base's work thread: 0 done, 1 failed (message() says why), 2 stopped. */
    @Override public int convert(File game, int format, int threads) {
        return convert(game, format, threads, null, -1);
    }

    @Override public int convert(File game, int format, int threads, File index, int archiveFd) {
        if (format != ETC2 && format != ASTC) {
            message = "Unknown texture format";
            return 1;
        }
        return run(game, format, threads, index, archiveFd);
    }

    @Override public int undo(File game) { return run(game, 0, 0, null, -1); }

    private int run(File game, int format, int threads, File index, int archiveFd) {
        message = "";
        if (!available()) {
            message = "libs2texconv.so";
            return 1;
        }
        int result;
        synchronized (operation) {
            running = true;
            try {
                result = format == 0 ? nativeRemove(game.getAbsolutePath())
                    : nativeConvert(game.getAbsolutePath(), format, Math.max(1, threads),
                        index == null ? null : index.getAbsolutePath(), archiveFd);
            } finally {
                running = false;
            }
        }
        if (result == 1) return 2;
        if (result != 0) {
            message = nativeMessage();
            return 1;
        }
        return 0;
    }

    @Override public void cancel() { if (available()) nativeCancel(); }

    /** {done, total, bytesDone, bytesTotal}: the packages read and their texture bytes. */
    @Override public long[] progress() {
        if (!running || !available()) return null;
        long[] p = nativeProgress();
        return p == null || p.length < 5 ? null : new long[] {p[1], p[2], p[3], p[4]};
    }

    @Override public String message() { return message; }
}
