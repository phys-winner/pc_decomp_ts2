package port.launcher;

import android.content.Context;
import android.os.CancellationSignal;
import java.io.File;

/** Driver downloads use the launcher's standard cancellable work panel. */
final class TurnipDownload extends Work.Job {
    private final Context context;
    private final String url;
    private final CancellationSignal cancellation = new CancellationSignal();
    String installedTitle;

    TurnipDownload(Context context, String name, String url) {
        super(Texts.RENDERER, Texts.fmt(context, Texts.DOWNLOADING, name),
              new File(context.getCacheDir(), "turnip-download.part"));
        this.context = context.getApplicationContext();
        this.url = url;
        file = name;
    }

    @Override void cancel() {
        super.cancel();
        cancellation.cancel();
    }

    @Override void work() throws Exception {
        try {
            check();
            String key = TurnipDriver.hex(java.security.MessageDigest.getInstance("SHA-256")
                .digest(url.getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            String name = State.get(context, "turnip-cache", key, "");
            File cached = new File(TurnipDriver.folder(context), name);
            // shortcut: release URLs key the cache; add asset digests if publishers replace files in place.
            if (name.matches("libturnip-[0-9a-f]{64}[.]so") && cached.isFile()
                    && name.equals("libturnip-" + TurnipDriver.sha(cached) + ".so")) {
                check();
                installedTitle = TurnipDriver.select(context, cached,
                    State.get(context, "turnip-cache", name + ".title", name));
                return;
            }
            TurnipReleases.download(context, url, target, TurnipReleases.MAX_BYTES, cancellation, this);
            check();
            File dir = TurnipDriver.folder(context);
            if (!dir.isDirectory() && !dir.mkdirs()) throw new java.io.IOException("cannot create " + dir);
            String[] driver = TurnipDriver.unpack(target, file, dir);
            check();
            installedTitle = TurnipDriver.select(context, new File(dir, driver[0]), driver[1]);
            String stored = TurnipDriver.chosen(context).getName();
            if (!State.update(context, "turnip-cache", values -> {
                values.setProperty(key, stored);
                values.setProperty(stored + ".title", installedTitle);
            })) throw new java.io.IOException("cannot save the driver cache");
        } finally {
            if (target.exists() && !target.delete()) target.deleteOnExit();
        }
    }
}
