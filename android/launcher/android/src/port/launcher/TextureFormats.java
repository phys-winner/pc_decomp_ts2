package port.launcher;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

/** Bit n denotes VkFormat (VK_FORMAT_BC1_RGB_UNORM_BLOCK + n) in both probes. */
final class TextureFormats {
    static final List<String> DXT = Arrays.asList("DXT1", "DXT3", "DXT5");
    static final List<String> ETC2 = Arrays.asList("ETC2_RGB", "ETC2_RGBA");
    static final List<String> ASTC = Arrays.asList("ASTC_4x4");
    private static final List<String> NAMES = new ArrayList<>(Arrays.asList(
        "BC1_RGB", "BC1_RGB_SRGB", "BC1_RGBA", "BC1_RGBA_SRGB", "BC2", "BC2_SRGB", "BC3", "BC3_SRGB",
        "BC4", "BC4_SNORM", "BC5", "BC5_SNORM", "BC6H_UFLOAT", "BC6H_SFLOAT", "BC7", "BC7_SRGB",
        "ETC2_RGB", "ETC2_RGB_SRGB", "ETC2_RGB_A1", "ETC2_RGB_A1_SRGB", "ETC2_RGBA", "ETC2_RGBA_SRGB",
        "EAC_R11", "EAC_R11_SNORM", "EAC_RG11", "EAC_RG11_SNORM"));
    private static final int[] GL = {0x83f0,0x8c4c,0x83f1,0x8c4d,0x83f2,0x8c4e,0x83f3,0x8c4f,
        0x8dbb,0x8dbc,0x8dbd,0x8dbe,0x8e8f,0x8e8e,0x8e8c,0x8e8d,
        0x9274,0x9275,0x9276,0x9277,0x9278,0x9279,0x9270,0x9271,0x9272,0x9273};
    static {
        for (String size : "4x4 5x4 5x5 6x5 6x6 8x5 8x6 8x8 10x5 10x6 10x8 10x10 12x10 12x12".split(" ")) {
            NAMES.add("ASTC_" + size);
            NAMES.add("ASTC_" + size + "_SRGB");
        }
    }

    static long gles(int[] compressed) {
        long mask = 0;
        for (int format : compressed) {
            for (int i = 0; i < GL.length; i++) if (GL[i] == format) mask |= 1L << i;
            for (int i = 0; i < 14; i++) {
                if (format == 0x93b0 + i) mask |= 1L << (GL.length + 2*i);
                if (format == 0x93d0 + i) mask |= 1L << (GL.length + 2*i + 1);
            }
        }
        return mask;
    }

    static int index(String name) {
        switch (name) {
        case "DXT1": return 2;
        case "DXT1_SRGB": return 3;
        case "DXT2": case "DXT3": return 4;
        case "DXT3_SRGB": return 5;
        case "DXT4": case "DXT5": return 6;
        case "DXT5_SRGB": return 7;
        default: return NAMES.indexOf(name);
        }
    }

    /** A null mask means that this driver has no usable report. Zero is a measured result. */
    static Long vulkan(Map<String, String> report, String driver) {
        if (report == null) return null;
        String status = report.get(driver + ".status");
        if (!"ok".equals(status) && !"missing".equals(status)) return null;
        if ("turnip".equals(driver) && Arrays.asList(report.getOrDefault(driver + ".missing", "").split(",")).contains("notTurnip")) return null;
        try { return Long.parseLong(report.getOrDefault(driver + ".formats", ""), 16); }
        catch (NumberFormatException e) { return null; }
    }

    static List<String> missing(long mask, List<String> required) {
        List<String> missing = new ArrayList<>();
        for (String name : required) {
            int bit = index(name);
            if (bit < 0 || (mask & (1L << bit)) == 0) missing.add(name);
        }
        return missing;
    }

    static Boolean supports(Long mask, List<String> required) {
        return mask == null || required.isEmpty() ? null : missing(mask, required).isEmpty();
    }

    /** What a conversion for a renderer that lacks `missing` would convert: all of it when there is no cache
     *  (converted 0); with a cache, the missing formats it does not cover (covers null: it covers every format,
     *  the conversion of a backend that re-encodes the whole data set). */
    static List<String> uncovered(List<String> missing, int converted, List<String> covers) {
        List<String> left = new ArrayList<>(missing);
        if (converted == 0) return left;
        if (covers == null) left.clear();
        else left.removeAll(covers);
        return left;
    }

    /** The launcher's record of what a cache covers after a conversion job: "*" (every format: a backend that
     *  converts everything, or a job without sources), else the earlier formats and the job's (comma list). */
    static String covered(List<String> before, List<String> sources, boolean selective) {
        if (!selective || sources == null || before == null) return "*";
        List<String> all = new ArrayList<>(before);
        for (String format : sources) if (!all.contains(format)) all.add(format);
        return String.join(",", all);
    }

    /** The record of covered(): null for every format, else its formats; an empty record covers nothing. */
    static List<String> covers(String record) {
        if ("*".equals(record)) return null;
        List<String> formats = new ArrayList<>();
        for (String name : record.split(",")) if (!name.trim().isEmpty()) formats.add(name.trim());
        return formats;
    }
}
