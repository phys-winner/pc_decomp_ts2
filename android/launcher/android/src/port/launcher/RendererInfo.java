package port.launcher;

import java.util.Map;

final class RendererInfo {
    /** The renderer samples every texture format the game needs (its real formats for the data set): the
     *  launcher then says so in one short line in its green. Unknown caps or unknown formats are not "all". */
    static boolean allSupported(Long caps, java.util.List<String> originals) {
        return Boolean.TRUE.equals(TextureFormats.supports(caps, originals));
    }

    /** A renderer row's texture line: one short message when it supports all of the game's own formats,
     *  else what it supports and lacks of those and of the converter's output formats (formats). */
    static String textures(android.content.Context context, Long caps, java.util.List<String> formats, java.util.List<String> originals) {
        if (allSupported(caps, originals)) return Texts.t(context, Texts.TEXTURES) + " · " + Texts.t(context, Texts.TEX_ALL_SUPPORTED);
        return textures(context, caps, formats);
    }

    static String textures(android.content.Context context, Long caps, java.util.List<String> formats) {
        String label = Texts.t(context, Texts.TEXTURES) + " · ";
        if (formats.isEmpty()) return label + Texts.t(context, Texts.TEX_UNKNOWN_SOURCE);
        if (caps == null) return label + Texts.fmt(context, Texts.TEX_UNCHECKED, String.join(", ", formats));
        java.util.List<String> absent = TextureFormats.missing(caps, formats);
        java.util.List<String> supported = new java.util.ArrayList<>(formats);
        supported.removeAll(absent);
        String yes = supported.isEmpty() ? "" : Texts.fmt(context, Texts.TEX_SUPPORTED, String.join(", ", supported));
        String no = absent.isEmpty() ? "" : Texts.fmt(context, Texts.TEX_MISSING, String.join(", ", absent));
        return label + yes + (yes.isEmpty() || no.isEmpty() ? "" : "\n") + no;
    }

    /** A custom driver has its own API version. The platform's Vulkan declaration describes stock only. */
    static String title(String kind, String configured, String systemTitle, int gl, int stock, Map<String, String> report) {
        if ("gles".equals(kind)) return gl > 0 ? "OpenGL ES " + (gl >>> 16) + "." + (gl & 0xffff) : "OpenGL ES";
        boolean turnip = "turnip".equals(kind), panvk = "panvk".equals(kind);
        if (!turnip && !panvk && !"vulkan".equals(kind)) return configured;
        String driver = turnip || panvk ? kind : "stock";
        String status = report == null ? null : report.get(driver + ".status");
        String api = "";
        if ("ok".equals(status) || "missing".equals(status)) {
            String missing = report.getOrDefault(driver + ".missing", "");
            if (!missing.contains("notTurnip") && !missing.contains("notPanvk"))
                api = report.getOrDefault(driver + ".api", "");
        }
        if (Renderers.apiVersion(api) == 0) api = "";
        if (api.isEmpty() && !turnip && !panvk && stock != 0) api = Gpu.version(stock);
        String backend = turnip ? "Vulkan (Turnip)" : panvk ? "Vulkan (PanVK)" : systemTitle;
        if (configured.contains("DXVK")) backend += " · DXVK";
        return api.isEmpty() ? backend : backend.replace("Vulkan", "Vulkan " + api);
    }
}
