package org.sims2.port;

import android.content.Context;
import android.content.res.AssetManager;
import android.os.Process;

import java.io.File;
import java.io.IOException;

/** The engine, libsims2.so. A full APK carries it as xz in assets/native (tools/build_game_apk.py):
 *  about half the size of the deflated library in lib/, which was most of the APK. It is unpacked
 *  into the app's files once per build by the small libs2xz.so (android/unpack/unpack.c: its chunks
 *  on all cores at once) and loaded from there. A development library pushed into files/
 *  (build_game_apk.py --push-lib) still comes first, and an APK with the library in its lib folder
 *  (the Android 9 test build) loads that one. (The Void port's NativeLibrary, adapted.) */
final class NativeLibrary {
    private static native long nativeUnpack(AssetManager assets, String name, String path, long total, int chunk,
                                            int[] sizes);
    private static native long nativeUnpacked();

    private static volatile boolean unpacker;
    /** The size of the library being unpacked now, 0 when none is. */
    private static volatile long unpacking;

    private NativeLibrary() { }

    /** How far the unpacking has come: {done, total} bytes, or null when nothing is unpacked. */
    static long[] progress() {
        long total = unpacking;
        return total > 0 && unpacker ? new long[] {Math.min(total, nativeUnpacked()), total} : null;
    }

    /** Whether lib<library>.so must be unpacked before it can load: the first start of a build. */
    static boolean needsUnpack(Context context, String library) {
        String file = "lib" + library + ".so";
        if (new File(context.getFilesDir(), file).isFile()) return false;
        String[] entry = entry(context.getAssets(), file);
        if (entry == null) return false;
        File dir = new File(context.getFilesDir(), "native");
        return new File(dir, file).length() != Long.parseLong(entry[1])
            || !entry[2].equals(read(new File(dir, file + ".sha256")).trim());
    }

    private static String assetDirectory() {
        return Process.is64Bit() ? "native/" : "native/armeabi-v7a/";
    }

    /** libs2xz.so's constructor holds the PE image VAs. A 32-bit process must do that before
     *  prepare maps the unpacked library and before System.load places libsims2.so. */
    private static void holdGuestImages() {
        if (Process.is64Bit() || unpacker) return;
        System.loadLibrary("s2xz");
        unpacker = true;
    }

    /** Loads lib<library>.so, unpacking it first if this build's copy is not in the files yet. */
    static void load(Context context, String library) throws IOException {
        holdGuestImages();
        File file = prepare(context, library);
        if (file == null) {
            System.loadLibrary(library);
            return;
        }
        if (!file.setReadOnly()) throw new IOException("cannot protect " + file);
        System.load(file.getAbsolutePath());
    }

    /** The file System.load will open, or null when the library is in the APK's lib folder. */
    static File prepare(Context context, String library) throws IOException {
        String file = "lib" + library + ".so";
        File dev = new File(context.getFilesDir(), file);
        if (dev.isFile()) return dev;
        String[] entry = entry(context.getAssets(), file);
        if (entry == null) return null;
        long size = Long.parseLong(entry[1]);
        File dir = new File(context.getFilesDir(), "native");
        File target = new File(dir, file), stamp = new File(dir, file + ".sha256");
        /* The stamp is written after the library is whole: it names the build's library. */
        if (target.length() != size || !entry[2].equals(read(stamp).trim())) {
            if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot make " + dir);
            if (stamp.exists() && !stamp.delete()) throw new IOException("cannot remove " + stamp);
            File part = new File(dir, file + ".part");
            if (part.exists() && !part.delete()) throw new IOException("cannot remove " + part);
            if (!unpacker) {
                System.loadLibrary("s2xz");
                unpacker = true;
            }
            String[] packed = entry[4].split(",");
            int[] sizes = new int[packed.length];
            for (int i = 0; i < sizes.length; ++i) sizes[i] = Integer.parseInt(packed[i]);
            long started = android.os.SystemClock.elapsedRealtime(), written;
            unpacking = size;
            try {
                written = nativeUnpack(context.getAssets(), assetDirectory() + file + ".xz", part.getAbsolutePath(), size,
                    Integer.parseInt(entry[3]), sizes);
            } finally {
                unpacking = 0;
            }
            if (written != size) {
                part.delete();
                throw new IOException("unpacking " + file + " failed (" + written + " of " + size + ")");
            }
            /* A read-only copy from an earlier build: setReadOnly keeps it from being written, not removed. */
            if (target.exists() && !target.delete() || !part.renameTo(target))
                throw new IOException("cannot replace " + target);
            try (java.io.FileOutputStream out = new java.io.FileOutputStream(stamp)) {
                out.write((entry[2] + "\n").getBytes(java.nio.charset.StandardCharsets.US_ASCII));
                out.getFD().sync();
            }
            android.util.Log.i("Sims2", "native: " + file + " unpacked (" + size + " bytes) in "
                + (android.os.SystemClock.elapsedRealtime() - started) + " ms");
        }
        return target;
    }

    /** The library's line of assets/native/index.txt: {name, size, sha256, chunk, packed sizes}, or null. */
    private static String[] entry(AssetManager assets, String file) {
        try (java.io.InputStream in = assets.open(assetDirectory() + "index.txt")) {
            String text = new String(readAll(in), java.nio.charset.StandardCharsets.US_ASCII);
            for (String line : text.split("\n")) {
                String[] fields = line.trim().split(" ");
                if (fields.length == 5 && fields[0].equals(file)) return fields;
            }
        } catch (IOException e) {
            /* No index: the library is in the APK's lib folder. */
        }
        return null;
    }

    private static String read(File file) {
        try (java.io.InputStream in = new java.io.FileInputStream(file)) {
            return new String(readAll(in), java.nio.charset.StandardCharsets.US_ASCII);
        } catch (IOException e) {
            return "";
        }
    }

    private static byte[] readAll(java.io.InputStream in) throws IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[4096];
        for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        return out.toByteArray();
    }
}
