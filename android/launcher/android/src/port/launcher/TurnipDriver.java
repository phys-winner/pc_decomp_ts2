package port.launcher;

import android.content.Context;
import android.net.Uri;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.Enumeration;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** The player's own Turnip driver in place of the port's (renderer modes of kind turnip): a driver
 *  package for adrenotools (a ZIP whose meta.json names the library, as Turnip builds are published)
 *  or the library itself. It is copied into files/launcher/turnip, checked by Probe like the port's
 *  driver (the check's key includes it) and handed to the game as PORT_TURNIP_DRIVER, the library's
 *  full path, which the game gives adrenotools instead of its own driver. */
final class TurnipDriver {
    static final String KEY = "turnip.driver", TITLE = "turnip.title";
    private static final long MAX_SIZE = 256L << 20;
    private static final Pattern LIBRARY = Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"\\\\]+)\"");
    private static final Pattern NAME = Pattern.compile("\"name\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");

    private TurnipDriver() { }

    static File folder(Context context) { return new File(State.folder(context), "turnip"); }

    /** The player's driver; null for the port's own (or when its file has gone). */
    static File chosen(Context context) {
        String name = State.get(context, State.CHOICES, KEY, "");
        File file = name.isEmpty() ? null : new File(folder(context), name);
        return file != null && file.isFile() ? file : null;
    }

    /** The driver's name for the player: its package's own, else the library's. */
    static String title(Context context) {
        File file = chosen(context);
        return file == null ? "" : State.get(context, State.CHOICES, TITLE, file.getName());
    }

    /** What the driver check depends on: empty for the port's own driver. */
    static String identity(Context context) {
        File file = chosen(context);
        return file == null ? "" : file.getName() + ":" + file.length() + ":" + file.lastModified();
    }

    /** Copies the picked file (a content Uri, or a path of the built-in browser) and makes the driver in
     *  it the player's; returns its title. IOException with the reason when it is no driver. */
    static String install(Context context, Uri uri, File path) throws IOException {
        File dir = folder(context);
        if (!dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
        File picked = new File(dir, "picked.part");
        try {
            try (InputStream in = path != null ? new FileInputStream(path) : context.getContentResolver().openInputStream(uri);
                 OutputStream out = new FileOutputStream(picked)) {
                if (in == null) throw new IOException("the file cannot be read");
                copy(in, out, MAX_SIZE);
            }
            String name = path != null ? path.getName() : displayName(context, uri);
            String[] driver = unpack(picked, name, dir);
            return select(context, new File(dir, driver[0]), driver[1]);
        } finally {
            picked.delete();
        }
    }

    private static String displayName(Context context, Uri uri) {
        try (android.database.Cursor c = context.getContentResolver().query(uri,
                new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) return c.getString(0);
        } catch (RuntimeException e) {
            android.util.Log.w("Launcher", "driver name: " + e);
        }
        return uri.getLastPathSegment();
    }

    /** Back to the port's own driver; external libraries stay cached until cleanup. */
    static void reset(Context context) throws IOException {
        if (!State.update(context, State.CHOICES, values -> {
            values.remove(KEY);
            values.remove(TITLE);
        })) throw new IOException("cannot save the selected driver");
    }

    /** The driver of a picked file into dir: {library file name, title}. A ZIP is a driver package
     *  (meta.json's libraryName, else its only .so); anything else must be the library itself. */
    static String[] unpack(File picked, String pickedName, File dir) throws IOException {
        byte[] magic = new byte[4];
        try (RandomAccessFile in = new RandomAccessFile(picked, "r")) {
            if (in.read(magic) != 4) throw new IOException("the file is empty");
        }
        if (magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4) {
            try (ZipFile zip = new ZipFile(picked)) {
                ZipEntry meta = zip.getEntry("meta.json");
                String text = meta != null ? new String(read(zip.getInputStream(meta), 1 << 16), StandardCharsets.UTF_8) : "";
                Matcher library = LIBRARY.matcher(text), title = NAME.matcher(text);
                ZipEntry entry = library.find() ? zip.getEntry(library.group(1)) : only(zip);
                if (entry == null || entry.isDirectory())
                    throw new IOException(meta != null ? "the package lacks its library" : "no meta.json and no single .so in the archive");
                String file = safe(entry.getName());
                File part = new File(dir, file + ".part");
                try {
                    try (InputStream in = zip.getInputStream(entry); OutputStream out = new FileOutputStream(part)) {
                        copy(in, out, MAX_SIZE);
                    }
                    return settle(part, new File(dir, file), title.find() ? unescape(title.group(1)) : file);
                } finally {
                    part.delete();
                }
            }
        }
        String file = safe(pickedName != null ? pickedName : "");
        File part = new File(dir, file + ".part");
        try {
            try (InputStream in = new FileInputStream(picked); OutputStream out = new FileOutputStream(part)) {
                copy(in, out, MAX_SIZE);
            }
            return settle(part, new File(dir, file), file);
        } finally {
            part.delete();
        }
    }

    /** A checked library takes its final name. */
    private static String[] settle(File part, File target, String title) throws IOException {
        try {
            check(part);
            target = new File(target.getParentFile(), "libturnip-" + sha(part) + ".so");
            java.nio.file.Files.move(part.toPath(), target.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            return new String[] {target.getName(), title.trim().isEmpty() ? target.getName() : title.trim()};
        } finally {
            part.delete();
        }
    }

    /** An Android library for AArch64: ELF64, little-endian, shared object, EM_AARCH64. */
    static void check(File library) throws IOException {
        byte[] header = new byte[20];
        try (RandomAccessFile in = new RandomAccessFile(library, "r")) {
            if (in.read(header) != header.length) throw new IOException("too short for a library");
        }
        boolean elf = header[0] == 0x7F && header[1] == 'E' && header[2] == 'L' && header[3] == 'F';
        if (!elf || header[4] != 2 || header[5] != 1) throw new IOException("not a 64-bit library");
        int type = (header[16] & 0xFF) | (header[17] & 0xFF) << 8, machine = (header[18] & 0xFF) | (header[19] & 0xFF) << 8;
        if (type != 3 || machine != 183) throw new IOException("not an AArch64 shared library");
    }

    /** The archive's only .so file, else null. */
    private static ZipEntry only(ZipFile zip) {
        ZipEntry found = null;
        for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
            ZipEntry entry = e.nextElement();
            if (entry.isDirectory() || !entry.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".so")) continue;
            if (found != null) return null;
            found = entry;
        }
        return found;
    }

    /** A file name the driver folder can hold: the last path component, other characters than ASCII
     *  letters, digits and ._- as '_'. */
    static String safe(String name) {
        String base = name.substring(Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
        StringBuilder out = new StringBuilder();
        for (char c : base.toCharArray()) out.append(c < 128 && (Character.isLetterOrDigit(c) || c == '.' || c == '_' || c == '-') ? c : '_');
        String result = out.toString().replaceAll("^\\.+", "");
        if (result.isEmpty() || result.equals("picked.part")) result = "libvulkan_turnip.so";
        return result.endsWith(".so") ? result : result + ".so";
    }

    private static String unescape(String text) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length(); ++i) {
            char c = text.charAt(i);
            if (c != '\\' || i + 1 == text.length()) { out.append(c); continue; }
            char next = text.charAt(++i);
            if (next == 'u' && i + 4 < text.length()) {
                try { out.append((char) Integer.parseInt(text.substring(i + 1, i + 5), 16)); i += 4; }
                catch (NumberFormatException bad) { out.append(next); }
            } else out.append(next == 'n' || next == 't' || next == 'r' ? ' ' : next);
        }
        return out.toString();
    }

    static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }

    static String sha(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            java.security.MessageDigest digest = java.security.MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[65536];
            for (int n; (n = in.read(buffer)) > 0;) digest.update(buffer, 0, n);
            return hex(digest.digest());
        } catch (java.security.NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
    }

    static String select(Context context, File file, String title) throws IOException {
        if (!file.getCanonicalFile().getParentFile().equals(folder(context).getCanonicalFile()))
            throw new IOException("driver leaves its folder");
        check(file);
        if (!State.update(context, State.CHOICES, values -> {
            values.setProperty(KEY, file.getName());
            values.setProperty(TITLE, title);
        })) throw new IOException("cannot save the selected driver");
        return title;
    }

    static java.util.List<File> unused(Context context) {
        java.util.List<File> result = new java.util.ArrayList<>();
        File active = chosen(context), dir = folder(context);
        File[] files = dir.listFiles();
        if (files != null) for (File file : files)
            if (file.isFile() && file.getName().endsWith(".so") && !file.equals(active)) result.add(file);
        return result;
    }

    static void removeUnused(Context context) throws IOException {
        for (File file : unused(context))
            if (!file.equals(chosen(context)) && !file.delete() && file.exists())
                throw new IOException("cannot delete " + file.getName());
        State.update(context, "turnip-cache", values -> values.entrySet().removeIf(entry -> {
            String key = (String)entry.getKey();
            String file = key.endsWith(".title") ? key.substring(0, key.length() - 6) : (String)entry.getValue();
            return !new File(folder(context), file).isFile();
        }));
    }

    private static void copy(InputStream in, OutputStream out, long limit) throws IOException {
        byte[] buffer = new byte[1 << 16];
        long total = 0;
        for (int n; (n = in.read(buffer)) > 0;) {
            total += n;
            if (total > limit) throw new IOException("larger than a driver can be");
            out.write(buffer, 0, n);
        }
    }

    private static byte[] read(InputStream stream, int limit) throws IOException {
        try (InputStream in = stream; java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
            copy(in, out, limit);
            return out.toByteArray();
        }
    }
}
