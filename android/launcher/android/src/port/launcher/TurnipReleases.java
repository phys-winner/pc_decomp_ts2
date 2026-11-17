package port.launcher;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import org.json.JSONArray;
import org.json.JSONException;

/** GitHub release lists, cached for six hours. One request per repository, not per frame. */
final class TurnipReleases {
    static final String[] REPOSITORIES = {
        "MrPurple666/purple-turnip", "StevenMXZ/Adreno-Tools-Drivers", "The412Banner/Banners-Turnip",
        "Vauzi-17/710", "whitebelyash/AdrenoToolsDrivers", "aydar-kamaltdinov/mesa-unified", "K11MCH1/AdrenoToolsDrivers"
    };
    static final long MAX_BYTES = 256L << 20;
    private static final long FRESH_MS = 6L * 60L * 60L * 1000L;
    private static final int TIMEOUT_MS = 20000;

    private TurnipReleases() { }

    static JSONArray releases(Context context, String repo) throws IOException {
        if (repo == null || !repo.matches("[\\w.-]+/[\\w.-]+")) throw new IOException("invalid repository");
        String safe = repo.replace('/', '_');
        File body = new File(State.folder(context), "github-" + safe + ".json");
        File stamp = new File(State.folder(context), "github-" + safe + ".stamp");
        if (body.isFile() && stamp.isFile() && Math.abs(System.currentTimeMillis() - stamp.lastModified()) < FRESH_MS) {
            try { return new JSONArray(read(body, 2 << 20)); }
            catch (JSONException | IOException ignored) { body.delete(); stamp.delete(); }
        }
        try {
            String text = http(context, "https://api.github.com/repos/" + repo + "/releases?per_page=10", 2 << 20);
            JSONArray parsed = new JSONArray(text);
            write(body, text);
            if (!stamp.exists() && !stamp.createNewFile() && !stamp.isFile()) throw new IOException("cannot stamp " + repo);
            if (!stamp.setLastModified(System.currentTimeMillis())) stamp.delete();
            return parsed;
        } catch (JSONException failure) {
            throw new IOException("GitHub releases", failure);
        } catch (IOException failure) {
            if (!body.isFile()) throw failure;
            try { return new JSONArray(read(body, 2 << 20)); }
            catch (JSONException stale) { throw failure; }
        }
    }

    static void download(Context context, String address, File out, long maxBytes,
                         android.os.CancellationSignal cancel, Work.Job progress) throws IOException {
        if (cancel != null && cancel.isCanceled()) throw new Failure(Failure.CANCELLED, null);
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        if (cancel != null) cancel.setOnCancelListener(() ->
            new Thread(connection::disconnect, "turnip-cancel").start());
        try {
            connection.setConnectTimeout(TIMEOUT_MS);
            connection.setReadTimeout(TIMEOUT_MS);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("User-Agent", context.getPackageName());
            connection.setRequestProperty("Accept", "application/octet-stream");
            int code = connection.getResponseCode();
            if (cancel != null && cancel.isCanceled()) throw new Failure(Failure.CANCELLED, null);
            if (code != 200) throw new IOException("HTTP " + code);
            long advertised = connection.getContentLengthLong();
            if (progress != null) progress.total = advertised > 0 ? advertised : 0;
            if (advertised > maxBytes) throw new IOException("file is too large");
            File parent = out.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
            try (InputStream in = connection.getInputStream(); OutputStream sink = new FileOutputStream(out)) {
                byte[] buffer = new byte[65536];
                long total = 0;
                for (int n; (n = in.read(buffer)) > 0; ) {
                    if (cancel != null && cancel.isCanceled()) throw new Failure(Failure.CANCELLED, null);
                    total += n;
                    if (total > maxBytes) throw new IOException("file is too large");
                    sink.write(buffer, 0, n);
                    if (progress != null) progress.done = total;
                }
                if (advertised >= 0 && total != advertised) throw new IOException("incomplete download");
            }
            if (cancel != null && cancel.isCanceled()) throw new Failure(Failure.CANCELLED, null);
        } catch (IOException failure) {
            if (cancel != null && cancel.isCanceled()) throw new Failure(Failure.CANCELLED, null);
            throw failure;
        } finally {
            if (cancel != null) cancel.setOnCancelListener(null);
            connection.disconnect();
        }
    }

    private static String http(Context context, String address, int limit) throws IOException {
        HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
        try {
            connection.setConnectTimeout(TIMEOUT_MS);
            connection.setReadTimeout(TIMEOUT_MS);
            connection.setInstanceFollowRedirects(true);
            connection.setRequestProperty("Accept", "application/vnd.github+json");
            connection.setRequestProperty("User-Agent", context.getPackageName());
            int code = connection.getResponseCode();
            if (code != 200) throw new IOException("HTTP " + code);
            try (InputStream in = connection.getInputStream(); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buffer = new byte[8192];
                for (int n; (n = in.read(buffer)) > 0; ) {
                    if (out.size() + n > limit) throw new IOException("response is too large");
                    out.write(buffer, 0, n);
                }
                return new String(out.toByteArray(), StandardCharsets.UTF_8);
            }
        } finally {
            connection.disconnect();
        }
    }

    private static String read(File file, int limit) throws IOException {
        try (InputStream in = new FileInputStream(file); ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[8192];
            for (int n; (n = in.read(buffer)) > 0; ) {
                if (out.size() + n > limit) throw new IOException(file.getName() + " is too large");
                out.write(buffer, 0, n);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        }
    }

    private static void write(File file, String text) throws IOException {
        File temporary = new File(file.getParentFile(), file.getName() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(temporary)) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
            out.getFD().sync();
        }
        java.nio.file.Files.move(temporary.toPath(), file.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
    }
}
