package port.launcher;

import android.content.Context;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.Properties;

/** The renderer failure policy (owner's decision, 24 November 2026). When the selected renderer fails the
 *  start stops: no game switches to another driver or renderer by itself. The game reports the failure with
 *  Launch.rendererFailed (Java) or port_renderer_failed (native/replacements/renderer_failure.h); both write
 *  this record, files/launcher/renderer-failure-<distribution>.txt (PORT_RENDERER_FAILURE), and end the game.
 *  The launcher then offers the next available renderer of its list once (LauncherActivity.rendererFailureOffer).
 *
 *  A failure counts when the renderer did not start (kind "start": driver library missing or refused, wrong
 *  driver identity, instance/device/surface/swapchain or EGL context creation failed, a required feature
 *  missing at run time) before the first presented frame, or when the GPU device was lost (kind "lost":
 *  VK_ERROR_DEVICE_LOST, an EGL context loss or a GL robustness reset) before the first presented frame or
 *  within renderers.failureWindowSeconds (default 30) after it. Anything else is an ordinary crash: the
 *  launcher shows it as "renderer failure: …" in the last game failure line, without the offer.
 *
 *  The record is a few lines of text, written in one piece:
 *  <pre>
 *  port-renderer-failure 1
 *  pid=12345
 *  kind=lost
 *  renderer=vulkan-turnip
 *  time_ms=1764012345678
 *  since_present_ms=1520
 *  reason=vkQueueSubmit: VK_ERROR_DEVICE_LOST
 *  </pre>
 *  since_present_ms is -1 when the writer saw no presented frame; the reason is one line, the last field. */
final class RendererFailure {
    static final String HEADER = "port-renderer-failure 1";
    static final String START = "start", LOST = "lost";
    static final int DEFAULT_WINDOW_SECONDS = 30;
    /** since(): the game reached its first frame, but neither record says when. */
    static final long UNKNOWN = Long.MIN_VALUE;
    /** The launch record's mark that the player answered the offer for this start. */
    static final String ANSWERED = "rendererFailureAnswered";
    static final int MAX_REASON = 480;
    private static final long SLACK_MS = 2000;

    final long pid, time, sincePresent;
    final String kind, renderer, reason;
    /** Set by last(): within the rules above (the offer applies); answered: the player saw it. */
    boolean inRules, answered;

    RendererFailure(long pid, String kind, String renderer, long time, long sincePresent, String reason) {
        this.pid = pid;
        this.kind = LOST.equals(kind) ? LOST : START;
        this.renderer = renderer == null ? "" : renderer;
        this.time = time;
        this.sincePresent = sincePresent < 0 ? -1 : sincePresent;
        this.reason = reason == null ? "" : reason;
    }

    static File file(File dir, String distribution) { return new File(dir, "renderer-failure-" + distribution + ".txt"); }

    /** One line without control characters, at most `limit` characters. */
    static String line(String text, int limit) {
        if (text == null) return "";
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < text.length() && out.length() < limit; ++i) {
            char c = text.charAt(i);
            out.append(c < 0x20 || c == 0x7f ? ' ' : c);
        }
        return out.toString().trim();
    }

    /** The record's text, as the native header writes it. */
    static String text(long pid, String kind, String renderer, long time, long sincePresent, String reason) {
        return HEADER + "\npid=" + pid + "\nkind=" + (LOST.equals(kind) ? LOST : START) + "\nrenderer=" + line(renderer, 64)
            + "\ntime_ms=" + time + "\nsince_present_ms=" + (sincePresent < 0 ? -1 : sincePresent)
            + "\nreason=" + line(reason, MAX_REASON) + "\n";
    }

    static void write(File file, long pid, String kind, String renderer, long time, long sincePresent, String reason) throws IOException {
        byte[] bytes = text(pid, kind, renderer, time, sincePresent, reason).getBytes(StandardCharsets.UTF_8);
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(bytes);
            out.getFD().sync();
        }
    }

    /** null when the text is no record of this format. */
    static RendererFailure parse(String text) {
        if (text == null) return null;
        String[] lines = text.split("\n", -1);
        if (lines.length < 2 || !HEADER.equals(lines[0].trim())) return null;
        Properties values = new Properties();
        for (int i = 1; i < lines.length; ++i) {
            int at = lines[i].indexOf('=');
            if (at > 0) values.setProperty(lines[i].substring(0, at), lines[i].substring(at + 1).replace("\r", ""));
        }
        String kind = values.getProperty("kind", "");
        if (!START.equals(kind) && !LOST.equals(kind)) return null;
        return new RendererFailure(State.getLong(values, "pid", 0), kind, values.getProperty("renderer", "").trim(),
            State.getLong(values, "time_ms", 0), State.getLong(values, "since_present_ms", -1), values.getProperty("reason", "").trim());
    }

    static RendererFailure read(File file) {
        if (!file.isFile() || file.length() > 4096) return null;
        try (InputStream in = new FileInputStream(file)) {
            byte[] bytes = new byte[(int) file.length()];
            int n = 0;
            for (int r; n < bytes.length && (r = in.read(bytes, n, bytes.length - n)) > 0;) n += r;
            return parse(new String(bytes, 0, n, StandardCharsets.UTF_8));
        } catch (IOException e) {
            return null;
        }
    }

    RendererFailure withRenderer(String id) {
        RendererFailure copy = new RendererFailure(pid, kind, id, time, sincePresent, reason);
        copy.inRules = inRules;
        copy.answered = answered;
        return copy;
    }

    /** Milliseconds from the first presented frame to the failure; -1 when it came before any presented frame;
     *  UNKNOWN when the game reached its first frame (reached: the launch record or the native journal) and
     *  neither record gives the time. The writer's own measure comes first, then the wall time of the first
     *  frame that Launch.firstFrame wrote (reachedTime; 0: none). */
    long since(long reachedTime, boolean reached) {
        if (sincePresent >= 0) return sincePresent;
        if (reachedTime > 0 && time > 0) return time >= reachedTime ? time - reachedTime : -1;
        return reached ? UNKNOWN : -1;
    }

    /** The owner's rules: "start" before the first presented frame; "lost" before it or within the window. */
    boolean inRules(long since, long windowMs) {
        if (since == UNKNOWN) return false;
        if (LOST.equals(kind)) return since < 0 || since <= windowMs;
        return since < 0;
    }

    /** The record of the distribution's last start of this build (null: none), judged by the rules. A record of
     *  another process (pid) or older than the start is not this start's. */
    static RendererFailure last(File dir, String distribution, Properties launch, String build, boolean reached, long windowMs) {
        if (!build.equals(launch.getProperty("build")) || !"1".equals(launch.getProperty("started"))) return null;
        File file = file(dir, distribution);
        /* A start removes the record (Launch.recordStart); the times only guard against a removal that failed.
           File times are coarse (a kernel tick behind the clock), hence the slack. */
        long started = State.getLong(launch, "time", 0);
        if (!file.isFile() || file.lastModified() + SLACK_MS < started) return null;
        RendererFailure failure = read(file);
        if (failure == null || (failure.time > 0 && failure.time + SLACK_MS < started)) return null;
        long pid = State.getLong(launch, "pid", 0);
        if (pid > 0 && failure.pid != pid) return null;
        if (failure.renderer.isEmpty()) failure = failure.withRenderer(launch.getProperty("renderer", ""));
        failure.inRules = failure.inRules(failure.since(State.getLong(launch, "reachedTime", 0), reached), windowMs);
        failure.answered = "1".equals(launch.getProperty(ANSWERED));
        return failure;
    }

    static RendererFailure last(Context context, Spec spec, Spec.Distribution d) {
        java.util.Properties launch = State.read(context, State.launch(d.id));
        return last(State.folder(context), d.id, launch, Launch.build(context), Launch.reachedFrame(context, d, launch),
            spec.rendererFailureWindowMs);
    }

    /** The failure the launcher still has to offer: this start's, within the rules, not answered yet. */
    static RendererFailure pending(Context context, Spec spec, Spec.Distribution d) {
        RendererFailure failure = last(context, spec, d);
        return failure != null && failure.inRules && !failure.answered ? failure : null;
    }

    static void answered(Context context, Spec.Distribution d) {
        State.put(context, State.launch(d.id), ANSWERED, "1");
    }

    /** For the crash line and the diagnostic report: what the game said. */
    String summary() { return "renderer failure: " + (reason.isEmpty() ? kind : reason); }
}
