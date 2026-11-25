package port.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Handler;
import android.os.Looper;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

/** Worker publishes byte counts; the dialog samples them on the UI thread. */
final class ArchiveCheckDialog implements ArchiveMd5.Progress {
    private final Activity activity;
    private final Ui ui;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final WorkProgress meter = new WorkProgress();
    private final String language;
    private final Runnable stop;
    private volatile long done, total = -1;
    private boolean closed, cancelled;
    private AlertDialog dialog;
    private TextView amount;
    private ProgressBar bar;

    ArchiveCheckDialog(Activity activity, Ui ui, Runnable stop) {
        this.activity = activity;
        this.ui = ui;
        this.language = Texts.language(activity);
        this.stop = stop;
    }

    void start(String name) {
        handler.post(() -> {
            if (closed || activity.isFinishing() || activity.isDestroyed()) return;
            LinearLayout box = new LinearLayout(activity);
            box.setOrientation(LinearLayout.VERTICAL);
            box.setPadding(ui.dp(24), ui.dp(12), ui.dp(24), 0);
            TextView file = ui.label(name, 14, ui.text);
            box.addView(file);
            bar = ui.progress();
            LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, ui.dp(8));
            layout.topMargin = layout.bottomMargin = ui.dp(12);
            box.addView(bar, layout);
            amount = ui.label("", 13, ui.text);
            box.addView(amount);
            dialog = ui.show(ui.dialog().setTitle(Texts.t(activity, Texts.VERIFY_MD5)).setView(box)
                .setNegativeButton(Texts.t(activity, Texts.CANCEL), (d, which) -> cancel())
                .setOnCancelListener(d -> cancel()));
            dialog.setCanceledOnTouchOutside(false);
            refresh.run();
        });
    }

    @Override public void update(long bytes, long length) {
        total = length;
        done = bytes;
    }

    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            long bytes = done, length = total;
            bar.setIndeterminate(length < 0);
            if (length >= 0) bar.setProgress(length == 0 ? bar.getMax()
                : (int) Math.min(bar.getMax(), bytes * (double) bar.getMax() / length));
            amount.setText(meter.describe(language, false, bytes, length, 0, 0));
            handler.postDelayed(this, 150);
        }
    };

    private void cancel() {
        if (closed) return;
        cancelled = true;
        close();
        stop.run();
    }

    boolean cancelled() { return cancelled; }

    void close() {
        closed = true;
        handler.removeCallbacksAndMessages(null);
        if (dialog != null) dialog.dismiss();
    }
}
