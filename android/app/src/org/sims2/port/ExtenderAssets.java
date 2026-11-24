package org.sims2.port;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Stages the Legacy release's companion data in the app cache for this profile. */
final class ExtenderAssets {
    static final String ZIP_NAME = "TS2Extender.zip";
    static final String INI_NAME = "TS2Extender.ini";
    private static final String ZIP_INI = "TSBin/" + INI_NAME;
    private static final String[] PACKAGES = {
        "ld_dateStoodUpFix.package", "ld_timingFix.package", "ld_WallTopFix.package",
        "ld_TS2Extender_uniformFix.package"
    };

    private ExtenderAssets() {}

    static File prepare(Context context, File profile) throws IOException {
        File archive = new File(profile, ZIP_NAME);
        if (!archive.isFile()) return null;
        File directory = new File(context.getCacheDir(), "ts2extender-" + profile.getName());
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("Cannot stage TS2 Extender data");
        try (ZipFile zip = legacyZip(archive)) {
            extract(zip, ZIP_INI, new File(directory, INI_NAME), 65536);
            for (String name : PACKAGES)
                extract(zip, "Documents/Downloads/" + name, new File(directory, name), 2 * 1024 * 1024);
            extract(zip, "TSBin/Splash/Legacy.png", new File(directory, "Splash.png"), 8 * 1024 * 1024);
        }
        Files.createDirectories(new File(profile, "Documents/EA Games/The Sims 2 Legacy/Downloads").toPath());
        return directory;
    }

    static ZipFile legacyZip(File archive) throws IOException {
        ZipFile zip = new ZipFile(archive);
        if (entry(zip, "TSBin/TS2Extender.asi") != null && entry(zip, ZIP_INI) != null) return zip;
        zip.close();
        throw new IOException("Expected a TS2 Extender Legacy Collection ZIP");
    }

    static String zipIni(File archive) throws IOException {
        try (ZipFile zip = legacyZip(archive)) {
            ZipEntry entry = entry(zip, ZIP_INI);
            long size = entry.getSize();
            if (size < 0 || size > 65536 || entry.isDirectory()) throw new IOException("Invalid TS2 Extender INI");
            byte[] bytes = new byte[(int)size];
            try (java.io.DataInputStream input = new java.io.DataInputStream(zip.getInputStream(entry))) {
                input.readFully(bytes);
                if (input.read() != -1) throw new IOException("Invalid TS2 Extender INI size");
            }
            return new String(bytes, StandardCharsets.UTF_8);
        }
    }

    static final class Splash {
        final Bitmap image;
        final float coverage;
        Splash(Bitmap image, float coverage) { this.image = image; this.coverage = coverage; }
    }

    static Splash splash(File directory, File profile) throws IOException {
        if (directory == null) return null;
        File ini = new File(profile, INI_NAME);
        if (!ini.isFile()) ini = new File(directory, INI_NAME);
        boolean inSplash = false, enabled = true;
        float coverage = 0.5f;
        if (ini.isFile() && ini.length() <= 65536) {
            for (String source : Files.readAllLines(ini.toPath(), StandardCharsets.UTF_8)) {
                String line = source.replaceFirst("^[\\uFEFF]|[;#].*$", "").trim();
                if (line.startsWith("[")) inSplash = "[Splash]".equalsIgnoreCase(line);
                else if (inSplash && line.contains("=")) {
                    String[] option = line.split("=", 2);
                    String key = option[0].trim(), value = option[1].trim();
                    if ("Enabled".equalsIgnoreCase(key)) {
                        if ("true".equalsIgnoreCase(value) || "1".equals(value)) enabled = true;
                        else if ("false".equalsIgnoreCase(value) || "0".equals(value)) enabled = false;
                    } else if ("VerticalCoverage".equalsIgnoreCase(key)) {
                        try {
                            float fraction = Float.parseFloat(value);
                            if (fraction > 0 && fraction <= 1) coverage = fraction;
                        } catch (NumberFormatException ignored) { }
                    }
                }
            }
        }
        if (!enabled) return null;
        String path = new File(directory, "Splash.png").getAbsolutePath();
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(path, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || (long)bounds.outWidth * bounds.outHeight > 8_000_000)
            return null;
        Bitmap bitmap = BitmapFactory.decodeFile(path);
        return bitmap == null ? null : new Splash(bitmap, coverage);
    }

    private static void extract(ZipFile zip, String name, File file, int limit) throws IOException {
        ZipEntry entry = entry(zip, name);
        if (entry == null) {
            Files.deleteIfExists(file.toPath());
            return;
        }
        if (entry.isDirectory() || entry.getSize() < 0 || entry.getSize() > limit)
            throw new IOException("Invalid TS2 Extender entry " + name);
        int total = 0;
        byte[] buffer = new byte[8192];
        try (InputStream input = zip.getInputStream(entry); OutputStream output = Files.newOutputStream(file.toPath())) {
            int count;
            while ((count = input.read(buffer)) != -1) {
                total += count;
                if (total > limit) throw new IOException("TS2 Extender entry exceeds size limit " + name);
                output.write(buffer, 0, count);
            }
            if (total != entry.getSize()) throw new IOException("Truncated TS2 Extender entry " + name);
        }
        if (file.getName().endsWith(".package")) {
            try (java.io.DataInputStream input = new java.io.DataInputStream(Files.newInputStream(file.toPath()))) {
                if (input.readInt() != 0x44425046) throw new IOException("Invalid TS2 Extender package " + name);
            }
        }
    }

    private static ZipEntry entry(ZipFile zip, String name) {
        ZipEntry unix = zip.getEntry(name);
        // The official LC ZIP mixes separators in its central directory.
        return unix != null ? unix : zip.getEntry(name.replace('/', '\\'));
    }
}
