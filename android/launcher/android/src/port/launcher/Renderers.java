package port.launcher;

import android.content.Context;
import android.os.Build;
import java.io.File;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.Set;

/** The renderers of port.json and whether each can run here: the Android version, the libraries
 *  this build carries, the system Vulkan version the platform declares, the GPU model (Turnip drives
 *  Adreno GPUs only) and, for the DXVK modes, the driver check of Probe (Vulkan 1.3, robustness2,
 *  BC1-3 textures...). A mode of the port's own Vulkan renderer ("probe": "version") asks the same
 *  check only for a driver that loads and offers the mode's Vulkan version (BC1-3 if needsBC says so).
 *  A Vulkan mode the check has not confirmed yet is "not checked": it can be
 *  chosen by hand, never automatically. A start that never reached the game's first frame is the
 *  last word: the launcher then names the renderer and offers the default one. */
final class Renderers {
    static final class Status {
        final Spec.Mode mode;
        /** available: can be chosen; verified: confirmed to run here (the check passed, or OpenGL ES). */
        final boolean available, verified;
        final String reason;

        Status(Spec.Mode mode, boolean available, boolean verified, String reason) {
            this.mode = mode;
            this.available = available;
            this.verified = verified;
            this.reason = reason;
        }
    }

    private Renderers() { }

    /** The libraries this build carries: nativeLibraryDir and what the game packs (PortHooks.packedLibraries). */
    static Set<String> libraries(Context context) {
        Set<String> names = new HashSet<>();
        String[] list = new File(context.getApplicationInfo().nativeLibraryDir).list();
        if (list != null) for (String name : list) names.add(name);
        try {
            names.addAll(PortHooks.of(context).packedLibraries(context));
        } catch (RuntimeException e) {
            android.util.Log.w("Launcher", "hooks packedLibraries: " + e);
        }
        return names;
    }

    static List<Status> evaluate(Context context, Spec spec, Spec.Distribution d, Gpu gpu, boolean convertedTextures) {
        return evaluate(context, spec, d, gpu, convertedTextures, false);
    }

    static List<Status> evaluate(Context context, Spec spec, Spec.Distribution d, Gpu gpu, boolean convertedTextures, boolean originalsRemoved) {
        Set<String> present = libraries(context);
        Map<String, String> report = Probe.needed(spec) ? Probe.report(context) : null;
        boolean probing = report == null && Probe.running(context);
        boolean carried = present.contains(Probe.FILE);
        PortHooks hooks = PortHooks.of(context);
        List<Status> result = new ArrayList<>();
        for (String id : d.renderers) {
            Spec.Mode mode = spec.mode(id);
            if (mode == null) continue;
            String driver = mode.driverKey();
            String device = report != null && mode.vulkanKind() ? report.getOrDefault(driver + ".device", gpu.renderer) : gpu.renderer;
            String version = mode.vulkanKind() ? report == null ? "" : report.getOrDefault(driver + ".driver", "") : gpu.glesDriver;
            String problem = hooks.rendererProblem(context, mode.id, device, version);
            if (problem != null) {
                result.add(new Status(mode, false, false, problem));
                continue;
            }
            result.add(evaluate(context, mode, gpu, present, convertedTextures, originalsRemoved, carried, report, probing,
                d.textureFormats != null ? d.textureFormats : mode.originalFormats, d.originalTextureRenderers.contains(mode.id), spec));
        }
        return result;
    }

    static boolean on(String value) { return "1".equals(value) || "true".equals(value); }

    private static Status evaluate(Context context, Spec.Mode mode, Gpu gpu, Set<String> present, boolean converted,
                                   boolean originalsRemoved, boolean carried, Map<String, String> report, boolean probing,
                                   List<String> originals, boolean originalOnly, Spec spec) {
        boolean turnip = "turnip".equals(mode.kind);
        if (turnip) {
            int vendor = report == null ? 0 : Probe.vendor(report, "stock");
            String here = gpu.renderer.isEmpty() && report != null ? report.getOrDefault("stock.device", "?") : gpu.renderer;
            if (vendor != 0 ? vendor != Probe.QUALCOMM : gpu.probed && !gpu.qualcomm())
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_QUALCOMM, here));
            if (Probe.external(context, mode) == null && mode.adrenoMin > 0 && gpu.adreno > 0
                    && (gpu.adreno < mode.adrenoMin || gpu.adreno > mode.adrenoMax))
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_ADRENO, mode.adrenoMin, mode.adrenoMax, here));
        } else if ("panvk".equals(mode.kind)) {
            if ((gpu.probed || (report != null && Probe.vendor(report, "stock") != 0)) && !Probe.mali(context, report)) {
                String here = gpu.renderer.isEmpty() ? "?" : gpu.renderer;
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_MALI, here));
            }
        }
        if (Build.VERSION.SDK_INT < mode.minSdk)
            return new Status(mode, false, false, Texts.fmt(context, Texts.R_ANDROID, version(mode.minSdk), Build.VERSION.SDK_INT));
        Spec.Option needed = mode.option.isEmpty() ? null : spec.option(mode.option);
        if (needed != null && !on(Launch.option(context, needed)))
            return new Status(mode, false, false, Texts.fmt(context, Texts.R_OPTION, Texts.of(context, needed.title)));
        List<String> missing = new ArrayList<>();
        for (String name : Probe.requiredLibraries(mode, Probe.external(context, mode)))
            if (!present.contains(name)) missing.add(name);
        if (!missing.isEmpty()) return new Status(mode, false, false, Texts.fmt(context, Texts.R_LIBRARIES, String.join(", ", missing)));
        if (converted && (originalOnly || !mode.readsCache()) && (originalsRemoved || (!originalOnly && !"originals".equals(mode.textureCache))))
            return new Status(mode, false, false, Texts.t(context, Texts.R_CONVERTED));
        if ("gles".equals(mode.kind)) {
            if (mode.gles > 0 && gpu.gles < mode.gles)
                return new Status(mode, false, false, Texts.fmt(context, Texts.GL_REQUIRED, glVersion(mode.gles)));
            return new Status(mode, true, true, null);
        }
        if (!turnip && !"panvk".equals(mode.kind) && (report == null || !report.containsKey("stock.api"))) {
            /* The system Vulkan driver, as the platform declares it. */
            if (gpu.vulkan == 0) return new Status(mode, false, false, Texts.t(context, Texts.R_NO_VULKAN));
            if (mode.vulkan != 0 && Integer.compareUnsigned(gpu.vulkan, mode.vulkan) < 0)
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_VULKAN, Gpu.version(gpu.vulkan), Gpu.version(mode.vulkan)));
        }
        if (!carried || report == null)
            return new Status(mode, !"panvk".equals(mode.kind), false, Texts.t(context, probing ? Texts.R_CHECKING : Texts.R_UNCHECKED));
        return judge(context, mode, report, originals);
    }

    /** A Vulkan mode by the driver check's report (Probe). */
    static Status judge(Context context, Spec.Mode mode, Map<String, String> report) {
        return judge(context, mode, report, mode.originalFormats);
    }

    private static Status judge(Context context, Spec.Mode mode, Map<String, String> report, List<String> originals) {
        String driver = mode.driverKey();
        String status = report.getOrDefault(driver + ".status", "");
        if ("turnip".equals(driver) && java.util.Arrays.asList(report.getOrDefault("turnip.missing", "").split(",")).contains("notTurnip"))
            return new Status(mode, false, false, Texts.t(context, Texts.R_NOT_TURNIP));
        if ("panvk".equals(driver)) {
            if (status.isEmpty() || "skipped".equals(status))
                return new Status(mode, false, false, Texts.t(context, Texts.R_UNCHECKED));
            /* ARM's vendor and VK_DRIVER_ID_MESA_PANVK = 20. */
            if (("ok".equals(status) || "missing".equals(status)) && (Probe.vendor(report, driver) != 0x13b5
                    || !"20".equals(report.get(driver + ".driverId"))))
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_ERROR, "PanVK driver not active"));
        }
        boolean partialBc = false;
        if (("ok".equals(status) || "missing".equals(status)) && mode.needsBC) {
            Long mask = TextureFormats.vulkan(report, driver);
            if (mask == null) return new Status(mode, false, false, Texts.t(context, Texts.R_UNCHECKED));
            if (originals.isEmpty() && (mask & 0xffffL) == 0)
                return new Status(mode, false, false, Texts.fmt(context, Texts.TEX_MISSING, "BC1–BC7"));
            partialBc = originals.isEmpty() && (mask & 0xffffL) != 0xffffL;
            List<String> absent = TextureFormats.missing(mask, originals);
            if (!absent.isEmpty()) return new Status(mode, false, false,
                Texts.fmt(context, Texts.TEX_MISSING, String.join(", ", absent)));
        }
        Status compatible = new Status(mode, true, !partialBc, partialBc ? Texts.t(context, Texts.TEX_UNKNOWN_SOURCE) : null);
        if (mode.versionProbe()) {
            Status version = versionCheck(context, mode, report, driver, status);
            return version.verified ? compatible : version;
        }
        switch (status) {
        case "ok": return compatible;
        case "crash": return new Status(mode, false, false, Texts.t(context, Texts.R_CRASH));
        case "error": return new Status(mode, false, false, Texts.fmt(context, Texts.R_ERROR, report.getOrDefault(driver + ".error", "?")));
        case "missing": {
            Set<String> tokens = new LinkedHashSet<>();
            for (String token : report.getOrDefault(driver + ".missing", "").split(",")) if (!token.trim().isEmpty()) tokens.add(token.trim());
            tokens.remove("bcTextures"); // Exact original formats were checked above.
            if (tokens.isEmpty()) return compatible;
            return new Status(mode, false, false, reasons(context, tokens, report.getOrDefault(driver + ".api", "?")));
        }
        case "skipped":
            if ("turnip".equals(driver) && Probe.vendor(report, "stock") != Probe.QUALCOMM && Probe.vendor(report, "stock") != 0) {
                String gpu = report.getOrDefault("stock.device", "?");
                return new Status(mode, false, false, Texts.fmt(context, Texts.R_QUALCOMM, gpu));
            }
            return new Status(mode, true, false, Texts.t(context, Texts.R_UNCHECKED));
        default: return new Status(mode, true, false, Texts.t(context, Texts.R_UNCHECKED));
        }
    }

    /** A mode of the port's own Vulkan renderer ("probe": "version"): the driver loaded in the check and
     *  offers the mode's Vulkan version; BC1-3 count only when the mode needs them. What the report's
     *  status and missing tokens say about DXVK (Vulkan 1.3, robustness2, its features) does not apply. */
    static Status versionCheck(Context context, Spec.Mode mode, Map<String, String> report, String driver, String status) {
        switch (status) {
        case "crash": return new Status(mode, false, false, Texts.t(context, Texts.R_CRASH));
        case "error": return new Status(mode, false, false, Texts.fmt(context, Texts.R_ERROR, report.getOrDefault(driver + ".error", "?")));
        case "ok": case "missing": break;
        default: return new Status(mode, true, false, Texts.t(context, Texts.R_UNCHECKED));
        }
        String api = report.getOrDefault(driver + ".api", "");
        int offered = apiVersion(api);
        if (offered == 0) return new Status(mode, true, false, Texts.t(context, Texts.R_UNCHECKED));
        if (Integer.compareUnsigned(offered, mode.vulkan) < 0)
            return new Status(mode, false, false, Texts.fmt(context, Texts.R_VULKAN, api, Gpu.version(mode.vulkan)));
        if (mode.maintenance1 && !"1".equals(report.get(driver + ".maintenance1"))
                && (offered < apiVersion("1.1") || apiVersion(report.getOrDefault(driver + ".loader", api)) < apiVersion("1.1")))
            return new Status(mode, false, false, Texts.fmt(context, Texts.R_FEATURES, "VK_KHR_maintenance1"));
        Set<String> missing = new LinkedHashSet<>();
        for (String capability : mode.vulkanRequires)
            if (!"1".equals(report.get(driver + "." + capability))) missing.add(capability);
        if (!missing.isEmpty()) return new Status(mode, false, false, reasons(context, missing, api));
        return new Status(mode, true, true, null);
    }

    static String glVersion(int gl) { return (gl >>> 16) + "." + (gl & 0xffff); }

    /** "1.1.128" (the check's apiVersion) → VK_MAKE_API_VERSION(0, 1, 1, 128); 0 if it is no version. */
    static int apiVersion(String text) {
        String[] parts = text.trim().split("\\.");
        try {
            int major = Integer.parseInt(parts[0]), minor = parts.length > 1 ? Integer.parseInt(parts[1]) : 0;
            int patch = parts.length > 2 ? Integer.parseInt(parts[2]) : 0;
            if (major <= 0 || major > 127 || minor < 0 || minor > 1023 || patch < 0) return 0;
            return (major << 22) | (minor << 12) | Math.min(patch, 4095);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    /** The check's missing-requirement tokens (native/vulkan_requirements.h) as the player reads them. */
    static String reasons(Context context, Set<String> tokens, String api) {
        Set<String> parts = new LinkedHashSet<>(), features = new LinkedHashSet<>();
        for (String token : tokens) {
            switch (token) {
            case "vulkan13": parts.add(Texts.fmt(context, Texts.R_VULKAN13, api)); break;
            case "robustness2": parts.add(Texts.t(context, Texts.R_ROBUSTNESS)); break;
            case "bcTextures": parts.add(Texts.t(context, Texts.R_BC)); break;
            case "swapchain": case "androidSurface": parts.add(Texts.t(context, Texts.R_PRESENT)); break;
            case "notTurnip": parts.add(Texts.t(context, Texts.R_NOT_TURNIP)); break;
            default: features.add(token);
            }
        }
        if (!features.isEmpty()) parts.add(Texts.fmt(context, Texts.R_FEATURES, String.join(", ", features)));
        return parts.isEmpty() ? Texts.t(context, Texts.R_UNFIT) : String.join("; ", parts);
    }

    /** "Android 11" for API 30 and so on (the versions a port can require). */
    static int version(int sdk) {
        if (sdk >= 33) return sdk - 20;
        if (sdk >= 31) return 12;
        return sdk >= 30 ? 11 : sdk >= 29 ? 10 : sdk >= 28 ? 9 : 8;
    }

    static Status status(List<Status> statuses, String id) {
        for (Status s : statuses) if (s.mode.id.equals(id)) return s;
        return null;
    }

    /** The renderer the launcher offers after `failed` failed (RendererFailure; owner's decision of 24 November
     *  2026): the next row after it in the list's order (the renderer card's), wrapping around, skipping rows that
     *  are unavailable here (disabled with a reason) and the failed one itself; null when none is left. An
     *  earlier id of the failed mode (its aliases) names its current row; an id of no row starts at the first. */
    static Status next(List<Status> statuses, String failed) {
        int at = -1;
        for (int i = 0; i < statuses.size(); ++i) {
            Spec.Mode mode = statuses.get(i).mode;
            if (mode.id.equals(failed) || mode.aliases.contains(failed)) { at = i; break; }
        }
        String id = at >= 0 ? statuses.get(at).mode.id : failed;
        for (int step = 1; step <= statuses.size(); ++step) {
            Status s = statuses.get((at + step) % statuses.size());
            if (s.available && !s.mode.id.equals(id)) return s;
        }
        return null;
    }

    /** The renderer a saved choice is switched to once that choice can no longer run here — a driver update,
     *  a removed driver, a new requirement (owner's decision of 24 November 2026: switch to a working renderer and
     *  say so in a toast): the default renderer when it is confirmed here, else the first confirmed row of the list.
     *  null while the choice still runs, while the verdict is not final (settled: the driver check has reported and
     *  is not running — PanVK is unavailable until then), when the choice is no row of this list, or when nothing
     *  confirmed is left (the card's note then names the problem, as before). */
    static Status replacement(List<Status> statuses, String chosen, String defaultMode, boolean settled) {
        Status current = status(statuses, chosen);
        if (!settled || current == null || current.available) return null;
        Status fallback = status(statuses, defaultMode);
        if (fallback != null && fallback != current && fallback.available && fallback.verified) return fallback;
        for (Status other : statuses) if (other != current && other.available && other.verified) return other;
        return null;
    }

    static String key(Spec.Distribution d) { return "renderer." + d.id; }

    /** The player's renderer: the game's own setting (hooks), else the launcher's, else the default. */
    static String chosen(Context context, Spec spec, Spec.Distribution d, PortHooks hooks) {
        String mode = explicitChoice(context, spec, d, hooks);
        return mode != null ? mode : spec.defaultMode;
    }

    /** The mode the player chose (in the game's settings or the launcher); null when nobody did. */
    static String explicitChoice(Context context, Spec spec, Spec.Distribution d, PortHooks hooks) {
        String mode = null;
        try { mode = hooks.renderer(context); } catch (RuntimeException e) { android.util.Log.w("Launcher", "hooks renderer: " + e); }
        if (mode == null || spec.mode(mode) == null) mode = State.get(context, State.CHOICES, key(d), "");
        /* The mode's current id: the saved one may be an alias from before a rename. */
        return spec.mode(mode) != null ? spec.mode(mode).id : null;
    }

    static void choose(Context context, Spec.Distribution d, PortHooks hooks, String mode) {
        State.put(context, State.CHOICES, key(d), mode);
        try { hooks.setRenderer(context, mode); } catch (RuntimeException e) { android.util.Log.w("Launcher", "hooks renderer: " + e); }
    }

    /** The renderer a start uses: the chosen one if it can run here (one the driver check has not
     *  confirmed only when the player chose it), else the default or the first confirmed one. */
    static String effective(String defaultMode, List<Status> statuses, String chosen, boolean explicit) {
        Status s = status(statuses, chosen);
        if (s != null && s.available && (s.verified || explicit)) return chosen;
        Status fallback = status(statuses, defaultMode);
        if (fallback != null && fallback.available && fallback.verified) return defaultMode;
        for (Status other : statuses) if (other.available && other.verified) return other.mode.id;
        return chosen;
    }

    /** The renderer of the last start that never showed the game's first frame; null if none, or
     *  if the game itself refused the start (Launch.failed: files, access), which is no renderer's fault. */
    static String failed(Context context, Spec.Distribution d) {
        Properties launch = State.read(context, State.launch(d.id));
        return Launch.reachedFrame(context, d, launch) ? null : failed(launch, Launch.build(context));
    }

    static String failed(Properties launch, String build) {
        if (!"1".equals(launch.getProperty("started")) || "1".equals(launch.getProperty("reached"))) return null;
        if (!launch.getProperty("failed", "").isEmpty()) return null;
        if (!build.equals(launch.getProperty("build"))) return null;
        return launch.getProperty("renderer", "");
    }
}
