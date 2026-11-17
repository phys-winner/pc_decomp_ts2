"""The DXVK renderer's native boundary: generated tables, the Vulkan requirement check, and strict
NDK compiles of the bridge (android/dxvk) and of the host glue in every edition build."""
import re
import subprocess
import sys
import unittest

from audio_probe import ROOT, compile_probe

sys.path.insert(0, str(ROOT / "tools"))
import build_android
import generate_dxvk_bridge
from build_android import TOOLCHAIN
from build_game_apk import launcher_root

HEADER = ROOT / "reference/dxvk/include/native/directx/d3d9.h"
VULKAN = TOOLCHAIN / "sysroot/usr/include/vulkan"


class DxvkBridgeTest(unittest.TestCase):
    def test_native_process_vertices_keeps_guest_shadows_coherent(self):
        source = (ROOT / "android/dxvk/bridge.cpp").read_text(encoding="utf-8")
        function = lambda name: re.search(r"(?:uint32_t |void |Shadow \*)" + name + r"\(.*?^}", source, re.S | re.M)[0]
        shadow = re.search(r"constexpr uint32_t kShadowPage.*?^};", source, re.S | re.M)[0]
        compile_probe("dxvk-process-vertices", (ROOT / "tools/fixtures/dxvk_process_vertices.cpp").read_text(encoding="utf-8"), {
            "dxvk_shadow_types.h": shadow,
            "dxvk_shadow_helpers.h": function("shadow_dirty") + function("shadow_of"),
            "dxvk_shadow_operations.h": function("buffer_lock") + function("process_vertices"),
        }, ["/TP", "/EHsc"])

    def test_bound_texture_proxies_survive_borrowed_references(self):
        glue = (ROOT / "android/host/d3d9_dxvk.c").read_text(encoding="utf-8")
        enums = "\n".join(re.findall(r"enum \{.*?\};", glue, re.S))
        types = re.search(r"typedef struct dxvk_texture_refs.*?} dxvk_object;", glue, re.S)[0]
        compile_probe("dxvk-texture-refs", (ROOT / "tools/fixtures/dxvk_texture_refs.c").read_text(encoding="utf-8"), {
            "dxvk_types.h": "typedef struct { uint32_t guest; int refs, native_refs; } void_com_object;\n" + enums + types,
            "d3d9_dxvk_refs.inc": (ROOT / "android/host/d3d9_dxvk_refs.inc").read_text(encoding="utf-8"),
        })

    def test_zero_backbuffer_dimensions_use_guest_client(self):
        glue = (ROOT / "android/host/d3d9_dxvk.c").read_text(encoding="utf-8")
        function = re.search(r"static void dxvk_backbuffer_size\(.*?^}", glue, re.S | re.M)[0]
        compile_probe("dxvk-zero-backbuffer", r'''
#include <assert.h>
#include <stdint.h>
#define void_log(...) ((void)0)
static uint32_t words[32], client_window;
static int client_w = 1320, client_h = 600;
#define MEM32(a) words[(a)/4]
static void void_window_client_size(uint32_t window, int *w, int *h) {
    client_window = window; *w = client_w; *h = client_h;
}
''' + function + r'''
int main(void) {
    uint32_t pp = 4;
    dxvk_backbuffer_size(pp, 7);
    assert(MEM32(pp) == 1320 && MEM32(pp+4) == 600 && client_window == 7);
    MEM32(pp+28) = 9;
    MEM32(pp) = 0; MEM32(pp+4) = 480;
    dxvk_backbuffer_size(pp, 7);
    assert(MEM32(pp) == 1320 && MEM32(pp+4) == 480 && client_window == 9);
    MEM32(pp) = 800; MEM32(pp+4) = 0;
    dxvk_backbuffer_size(pp, 7);
    assert(MEM32(pp) == 800 && MEM32(pp+4) == 600);
    client_w = client_h = 0;
    dxvk_backbuffer_size(pp, 7);
    assert(MEM32(pp) == 800 && MEM32(pp+4) == 600);
    MEM32(pp) = MEM32(pp+4) = 0;
    dxvk_backbuffer_size(pp, 7);
    assert(MEM32(pp) == 1280 && MEM32(pp+4) == 720);
    dxvk_backbuffer_size(0, 7);
    return 0;
}
''', {})

    def test_monitor_tracks_fitted_canvas_not_probe_backbuffer(self):
        glue = (ROOT / "android/host/d3d9_dxvk.c").read_text(encoding="utf-8")
        function = re.search(r"static int host_display_size\(.*?^}", glue, re.S | re.M)[0]
        self.assertIn("void_android_window_ack, host_display_size, void_input_set_screen_size", glue)
        compile_probe("dxvk-logical-monitor", r'''
#include <assert.h>
static int canvas_w, canvas_h;
static void s2_game_resolution(int *w, int *h) { *w = canvas_w; *h = canvas_h; }
''' + function + r'''
int main(void) {
    const int canvases[][2] = {{1320,600}, {926,420}, {1544,702}, {2780,1264}};
    for (unsigned i = 0; i < sizeof canvases / sizeof *canvases; ++i) {
        int w = 800, h = 600; /* A temporary capability-probe device's backbuffer. */
        canvas_w = canvases[i][0]; canvas_h = canvases[i][1];
        assert(host_display_size(&w, &h) == 1);
        assert(w == canvas_w && h == canvas_h);
    }
    canvas_w = 0;
    int w, h;
    assert(host_display_size(&w, &h) == 0);
    return 0;
}
''', {})

    def test_committed_tables_are_generated_from_the_pinned_header(self):
        if not HEADER.is_file():
            self.skipTest("DXVK headers are not imported (tools/build_dxvk.py)")
        generated, methods = generate_dxvk_bridge.outputs(HEADER)
        for relative, data in generated.items():
            self.assertEqual((ROOT / relative).read_bytes().replace(b"\r\n", b"\n"), data, relative)
        self.assertEqual(sum(len(items) for items in methods.values()), 328)

    def test_host_and_bridge_agree_on_kinds_slots_and_digest(self):
        host = (ROOT / "android/host/dxvk_methods.h").read_text(encoding="utf-8")
        metadata = (ROOT / "android/dxvk/bridge_metadata.h").read_text(encoding="utf-8")
        digest = re.search(r"#define DXVK_SLOTS_DIGEST (0x[0-9A-F]{8})u", host)[1]
        self.assertIn(f"kSlotsDigest = {digest}u", metadata)
        classes = re.findall(r'\{"DXVK::(\w+)", (\d+), dxvk_methods_(\d+), 0\}', host)
        kinds = metadata[metadata.index("enum class Kind : unsigned {") + 28:metadata.index(",Count};")].split(",")
        self.assertEqual([name for name, _, _ in classes], kinds)
        self.assertEqual([int(kind) for _, _, kind in classes], list(range(len(kinds))))
        glue = (ROOT / "android/host/d3d9_dxvk.c").read_text(encoding="utf-8")
        enum = dict(re.findall(r"KIND_(\w+) = (\d+)", glue))
        names = {"D3D9": "IDirect3D9", "DEVICE": "IDirect3DDevice9", "TEXTURE": "IDirect3DTexture9",
                 "CUBE": "IDirect3DCubeTexture9", "SURFACE": "IDirect3DSurface9", "QUERY": "IDirect3DQuery9",
                 "SWAPCHAIN": "IDirect3DSwapChain9", "STATEBLOCK": "IDirect3DStateBlock9"}
        self.assertEqual({key: kinds[int(value)] for key, value in enum.items()}, names)
        # The slots d3d9_dxvk.c hooks: as d3d9.h numbers them, with their stack sizes.
        thunk = lambda kind, slot: re.search(rf"static void dxvk_{kind}_{slot}\(void\) \{{ uint32_t a\[(\d+)\].*?g_esp \+= (\d+)u; \}}", host)
        for kind, slot, popped in ((1, 16, 32), (2, 13, 16), (2, 16, 12), (2, 17, 24), (2, 41, 8), (2, 42, 8),
                                   (2, 59, 16), (2, 60, 8), (2, 61, 12), (2, 65, 16), (15, 4, 8), (15, 5, 8),
                                   (17, 3, 28), (16, 7, 20)):
            self.assertEqual(int(thunk(kind, slot)[2]), popped, (kind, slot))

    def test_vulkan_requirements_name_what_dxvk_refuses(self):
        out = ROOT / "build/tests/dxvk-requirements"
        (out / "vulkan").mkdir(parents=True, exist_ok=True)
        for name in ("vulkan_core.h", "vk_platform.h"):
            (out / "vulkan" / name).write_bytes((VULKAN / name).read_bytes())
        (out / "vk_video").mkdir(exist_ok=True)
        for path in (VULKAN.parent / "vk_video").glob("*.h"):
            (out / "vk_video" / path.name).write_bytes(path.read_bytes())
        compile_probe("dxvk-requirements", r'''
#include <assert.h>
#include <stdio.h>
#include "vulkan_requirements.h"
static port_vk_caps complete(void) {
    port_vk_caps c; memset(&c, 0, sizeof c);
    c.api_version = VK_API_VERSION_1_3; c.swapchain = c.robustness2 = c.android_surface = 1;
    VkBool32 *core = (VkBool32 *)&c.core;
    for (size_t i = 0; i < sizeof c.core / sizeof *core; ++i) core[i] = VK_TRUE;
    c.sampler_mirror_clamp_to_edge = c.vulkan_memory_model = c.host_query_reset = c.timeline_semaphore = VK_TRUE;
    c.shader_demote_to_helper_invocation = c.synchronization2 = c.dynamic_rendering = c.maintenance4 = VK_TRUE;
    c.robust_buffer_access2 = c.null_descriptor = VK_TRUE; c.bc1 = c.bc2 = c.bc3 = 1;
    return c;
}
int main(void) {
    char out[256];
    port_vk_caps c = complete();
    assert(port_vk_missing(&c, 1, out, sizeof out) == 0 && !out[0]);
    /* The Adreno 650 test phone's system driver: Vulkan 1.1, no robustness2; the 1.2/1.3 structures were not queried. */
    c = complete(); c.api_version = VK_MAKE_API_VERSION(0, 1, 1, 128); c.robustness2 = 0;
    c.vulkan_memory_model = c.synchronization2 = VK_FALSE;
    assert(port_vk_missing(&c, 1, out, sizeof out) == 2 && !strcmp(out, "vulkan13,robustness2"));
    /* Without BC formats (Mali) and with a missing D3D9 feature. */
    c = complete(); c.bc2 = 0; c.core.geometryShader = VK_FALSE;
    assert(port_vk_missing(&c, 1, out, sizeof out) == 2 && !strcmp(out, "geometryShader,bcTextures"));
    c = complete(); c.null_descriptor = VK_FALSE; c.maintenance4 = VK_FALSE;
    assert(port_vk_missing(&c, 1, out, sizeof out) == 2 && !strcmp(out, "nullDescriptor,maintenance4"));
    /* A short buffer keeps counting and stays terminated. */
    c = complete(); c.api_version = VK_API_VERSION_1_0; c.android_surface = c.swapchain = 0;
    assert(port_vk_missing(&c, 1, out, 12) == 3 && strlen(out) < 12 && !strncmp(out, "vulkan13,an", 11));
    assert(port_vk_missing(&c, 1, out, 0) == 3);
    puts("PASS Vulkan requirements: complete device, Vulkan 1.1 without robustness2, BC and D3D9 features, robustness2 parts, short buffers");
    return 0;
}
''', {"vulkan_requirements.h": (launcher_root() / "native/vulkan_requirements.h").read_text(encoding="utf-8")}, ["/I."])

    def test_bridge_compiles_strictly_for_android(self):
        if not HEADER.is_file():
            self.skipTest("DXVK headers are not imported (tools/build_dxvk.py)")
        import build_dxvk
        out = ROOT / "build/tests/dxvk-bridge"
        out.mkdir(parents=True, exist_ok=True)
        flags = ["--target=" + build_dxvk.TARGET, "-std=c++17", "-O2", "-fPIC", "-fvisibility=hidden", "-Wall", "-Wextra", "-Werror",
                 "-Wno-missing-field-initializers", "-Wno-unused-parameter",
                 "-I" + str(ROOT / "reference/dxvk/include/native/windows"), "-I" + str(ROOT / "reference/dxvk/include/native/directx"),
                 "-I" + str(ROOT / "android/dxvk")]
        for source in build_dxvk.BRIDGE_SOURCES:
            result = subprocess.run([str(TOOLCHAIN / "bin/clang++.exe"), *flags, "-c", str(ROOT / source), "-o", str(out / "bridge.o")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, source + "\n" + result.stderr[-4000:])

    def test_host_glue_compiles_for_app_and_shell(self):
        out = ROOT / "build/tests/dxvk-host"
        out.mkdir(parents=True, exist_ok=True)
        for name in ("d3d9_dxvk.c", "d3d9_device.c", "d3dx9.c", "android_main.c"):
            for app in (True, False):
                command = [str(build_android.CLANG), *build_android.HOST_FLAGS,
                           *(["-DVOID_ANDROID_APP"] if app else []), "-Werror", "-c",
                           str(ROOT / "android/host" / name), "-o", str(out / "host.o")]
                result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, f"{name} app={app}\n{result.stderr[-4000:]}")


if __name__ == "__main__":
    unittest.main()
