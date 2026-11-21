/* Build-time Direct3D 9 shader translator for the native renderers.
 *
 * tools/build_shaderlib.py runs this host program once per build over every
 * game shader the build knows (the bytecode captured from the game's own
 * material text on a device, debug.s2.shaderdump=1, kept in the private
 * generated/shaders/capture), and the sampler-type variants of its ps_1_x
 * shaders, then packs the results into the APK's shader library. The
 * translation itself is android/render/s2r_translate.c, the same code the
 * renderers run on the device for a shader the library lacks (a mod's, a
 * material the capture never saw). This file only writes its results as JSON.
 * (Heroes V's tools/shaderlib/h5_shaderc.c with The Sims 2's translator.)
 *
 * It also applies the UI FSR 1 rewrite to a pixel shader's Vulkan text
 * (android/render/s2r_uifilter.c, the device's code), so the library carries
 * that SPIR-V as well.
 *
 * Usage: s2_shaderc <list> <out.json>; each list line is
 * "vs|ps <sampler types, hex> <path>" (translate bytecode) or
 * "ui 0 <path>" (the UI FSR 1 form of a GLSL text: "glsl"). */
#include "s2r_translate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *s2r_ui_filter_source(const char *src);

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 32) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static uint8_t *read_file(const char *path, long *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)size + 4);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(data); fprintf(stderr, "cannot read %s\n", path); return NULL; }
    fclose(f);
    data[size] = 0;
    *size_out = size;
    return data;
}

static int ui_filter(FILE *out, const char *path, int first)
{
    long size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data) return 0;
    if (!first) fputs(",\n", out);
    fputs("{\"path\": ", out); json_string(out, path);
    char *filtered = s2r_ui_filter_source((const char *)data);
    if (filtered) { fputs(", \"glsl\": ", out); json_string(out, filtered); }
    else fputs(", \"error\": \"no port block\"", out);
    fputs("}", out);
    free(filtered);
    free(data);
    return 1;
}

static int translate(FILE *out, int pixel, unsigned types, const char *path, int first)
{
    long size = 0;
    uint8_t *code = read_file(path, &size);
    if (!code) return 0;
    s2r_translation t;
    char error[512];
    if (!first) fputs(",\n", out);
    fputs("{\"path\": ", out); json_string(out, path);
    if (!s2r_translate(pixel, code, (uint32_t)size, types, &t, error, sizeof error)) {
        fputs(", \"error\": ", out); json_string(out, error); fputs("}", out);
        free(code);
        return 1;
    }
    fprintf(out, ", \"pixel\": %d, \"types\": %u, \"major\": %d, \"minor\": %d, \"synthetic_ctab\": %d, \"shadow_pcf\": %d",
        pixel, types, t.major, t.minor, t.synthetic_ctab, t.shadow_pcf);
    fprintf(out, ", \"f_total\": %d, \"i_total\": %d, \"b_total\": %d", t.f_total, t.i_total, t.b_total);
    static const char *const keys[3] = { "fmap", "imap", "bmap" };
    for (int set = 0; set < 3; ++set) {
        fprintf(out, ", \"%s\": [", keys[set]);
        for (int k = 0; k < t.map_count[set]; ++k)
            fprintf(out, "%s[%d, %d, %d]", k ? ", " : "", t.map[set][k][0], t.map[set][k][1], t.map[set][k][2]);
        fputs("]", out);
    }
    fputs(", \"texbem\": [", out);
    for (int k = 0; k < t.texbem_count; ++k) fprintf(out, "%s[%d, %d]", k ? ", " : "", t.texbem[k][0], t.texbem[k][1]);
    fputs("], \"attributes\": [", out);
    for (int a = 0; a < t.attribute_count; ++a) fprintf(out, "%s[%d, %d]", a ? ", " : "", t.attributes[a][0], t.attributes[a][1]);
    fputs("], \"samplers\": [", out);
    for (int k = 0; k < t.sampler_count; ++k) fprintf(out, "%s[%d, %d]", k ? ", " : "", t.samplers[k][0], t.samplers[k][1]);
    fputs("]", out);
    fprintf(out, ", \"ps_colors\": %u, \"ps_tex\": %u, \"ps_fog\": %u", t.ps_colors, t.ps_tex, t.ps_fog);
    fprintf(out, ", \"writes_fog\": %d, \"input_mask\": %u", t.writes_fog, t.input_mask);
    fputs(", \"glsl\": ", out);
    json_string(out, t.glsl);
    fputs("}", out);
    s2r_translation_free(&t);
    free(code);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: s2_shaderc <list> <out.json>\n"); return 2; }
    FILE *list = fopen(argv[1], "r");
    if (!list) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    FILE *out = fopen(argv[2], "w");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[2]); fclose(list); return 1; }
    fputs("[\n", out);
    char line[4096];
    int first = 1, ok = 1;
    while (fgets(line, sizeof line, list)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char stage[4];
        unsigned types = 0;
        int consumed = 0;
        if (sscanf(line, "%3s %x %n", stage, &types, &consumed) != 2 || !consumed) continue;
        int done = !strcmp(stage, "ui") ? ui_filter(out, line + consumed, first)
                 : translate(out, !strcmp(stage, "ps"), types, line + consumed, first);
        if (!done) ok = 0;
        first = 0;
    }
    fputs("\n]\n", out);
    fclose(out);
    fclose(list);
    return ok ? 0 : 1;
}
