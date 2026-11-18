"""Exercise production lot targeting without an Android or scene-graph dependency."""
from pathlib import Path

from audio_probe import CLANG, compile_probe
from check_virtual_query import function


PRELUDE = r"""
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "sims2_settings.h"

static uint32_t memory[16384], g_esp = 0x9000, win_manager = 0x1000, tool_handler = 0x3000, scene_window = 0x2100;
static uint8_t *guest_ptr(uint32_t p) {
    return (uint8_t *)memory + (p == 0xA10CC288u ? 0x1100 : p);
}
#define GPTR(a) guest_ptr(a)
#define MEM32(a) (*(uint32_t *)GPTR(a))
static unsigned context, queries, stale_after, ground, overlay;
static unsigned unavailable, tested;
static uint32_t handset_base;
static uint16_t handset_base_id;
static long elapsed_ns, test_cost_ns, pick_cost_ns;
static int pick_radius;
static int center_offscreen;
static int boxes[3][4];
static int void_vm_readable(uint32_t p, size_t n) {
    return (p == 0xA10CC288u && n == 4) || (p >= 0x1000 && (uint64_t)p + n <= sizeof memory);
}
static int void_vm_writable(uint32_t p, size_t n) { return void_vm_readable(p, n); }
#define CLOCK_MONOTONIC 1
static int clock_gettime(int clock, struct timespec *out) {
    assert(clock == CLOCK_MONOTONIC);
    *out = (struct timespec){ .tv_nsec = elapsed_ns };
    return 0;
}
unsigned s2_touch_context(void) { return context; }
unsigned s2_touch_layout_epoch(void) { return 1; }
static const char *touch_type(uint32_t p) { (void)p; return ""; }
static void void_input_screen_size(int *w, int *h) { *w = *h = 200; }
uint32_t s2_ui_pick(uint32_t root, int x, int y) {
    assert(root == 0x2000 && x >= 0 && x < 200 && y >= 0 && y < 200);
    elapsed_ns += pick_cost_ns;
    if (++queries == stale_after) context = S2_TOUCH_OPTIONS;
    return overlay && x > 104 ? 0x2200 : scene_window;
}
static int touch_query(int x, int y, int radius, uint64_t *point, uint64_t key, s2_world_targets *out) {
    assert(x == 100 && y == 100 && !point && !key && out);
    pick_radius = radius;
    return 2;
}
static void mock_method(void) {}
static void (*void_dispatch_find(uint32_t fn))(void) { return fn >= 6 && fn <= 18 ? mock_method : NULL; }
static uint32_t void_call_guest_this(uint32_t fn, uint32_t self, int n, const uint32_t *args) {
    if (fn == 0xA078D789u) {
        assert(self == 0x4000 && n == 5 && !args[2] && args[3] == 0x8fc0 && !args[4]);
        int x = (int)args[0], y = (int)args[1];
        for (unsigned i = 0; i < 3; ++i)
            if (x >= boxes[i][0] && x <= boxes[i][2] && y >= boxes[i][1] && y <= boxes[i][3])
                return 0x7000 + 16 * i;
        return 0;
    }
    if (fn == 0xA068C4C1u) {
        assert(self == 0x4000 && n == 3 && args[2] == 0x8fc0);
        return ground;
    }
    if (fn == 1) {
        assert(n == 1 && args[0] == 0x8fc0);
        return 0xa000 + 16 * ((self - 0x7000) / 16);
    }
    if (fn == 2) return 0;
    if (fn == 3) return self == 0xa020 ? 0xa000 : self;
    if (fn == 4) return (self - 0xa000) / 16 + 1;
    if (fn == 0xA0144F5Bu || fn == 0xA0141455u) return 0;
    if (fn == 5) {
        assert(n == 1 && args[0] == 0x8fd0);
        memcpy(GPTR(args[0] + 4), self == 0xa000 ? "One" : "Two", 4);
        return 1;
    }
    if (fn == 6) {
        assert(n == 1 && args[0] == 0x8fc0 && self != 0x7120);
        const float empty[] = {0, 0, 0, 0, 0, 0};
        const float first[] = {15, 18, 2, 25, 26, 6};
        const float second[] = {33, 38, 1, 39, 44, 9};
        const float extension[] = {23, 20, 2, 29, 28, 8};
        const float *bounds = self == 0x7100 ? first : self == 0x7110 ? second : self == 0x7130 ? extension : empty;
        memcpy(GPTR(args[0]), bounds, sizeof empty);
        return args[0];
    }
    if (fn == 7) {
        assert(n == 0);
        if (self == 0x7000 || self == 0x7020) return 0x7200;
        if (self == 0x7010) return 0x7110;
        return self == 0x7200 ? 0x7100 : 0;
    }
    if (fn == 8) {
        assert(n == 0);
        return self == 0x7100 ? 0x7120 : self == 0x7120 ? 0x7130 : 0;
    }
    if (fn == 9) {
        assert(n == 1);
        if (args[0] == 0x1C585E30u) return self >= 0x7100 && self <= 0x7130;
        assert(args[0] == 0xFA1C39EDu);
        return self == 0x7120;
    }
    if (fn == 11) { assert(self == 0xB800 && n == 0); return 0xB810; }
    if (fn == 12) { assert(self == 0xB810 && n == 0); return 0xB820; }
    if (fn == 13) { assert(self == 0xB800 && n == 0); return 0xB830; }
    if (fn == 14) {
        assert(self == 0xB830 && n == 2 && args[0] == 0xB820);
        ++tested;
        elapsed_ns += test_cost_ns;
        return !(unavailable & (1u << ((args[1] - 0xA000) / 16)));
    }
    if (fn == 15) { assert(n == 0); return 0xA100 + self - 0xA000; }
    if (fn == 16) { assert(n == 0 && self == 0xA010); return handset_base; }
    if (fn == 18) { assert(n == 1 && self == 0xA010 && !args[0]); return handset_base_id; }
    if (fn == 0xA041B670u) {
        assert(n == 1 && args[0] == 201);
        return self == 0xA110 ? 0x6C734DB3u : 0x6C734D8Cu;
    }
    if (fn == 17) return self;
    if (fn == 0xA05BD6B6u) {
        assert(n == 2 && self == 0x6200 && args[0] == 0x8fc0 && args[1] == 0x8fd8);
        float center[3];
        memcpy(center, GPTR(args[1]), sizeof center);
        assert((center[0] == 22 && center[1] == 23 && center[2] == 5) ||
               (center[0] == 36 && center[1] == 41 && center[2] == 5));
        MEM32(args[0]) = center_offscreen ? 200 : 100 + (int)center[0];
        MEM32(args[0] + 4) = 20 + (int)center[1];
        return args[0];
    }
    if (fn == 0xA0149D00u) return 3;
    assert(fn == 0xA01ACD4Fu);
    return self + 4;
}
"""


CHECK = r"""
static void reset(void) {
    memset(memory, 0, sizeof memory);
    memset(boxes, -1, sizeof boxes);
    queries = stale_after = overlay = 0; ground = 1; context = S2_TOUCH_LIVE; center_offscreen = 0;
    unavailable = tested = 0; handset_base = 0;
    handset_base_id = 0;
    elapsed_ns = test_cost_ns = pick_cost_ns = 0;
    MEM32(0x1044) = 0x2000; MEM32(0x3020) = 0x4000; MEM32(0x4000) = 0xA0DE9374u;
    MEM32(0x4004) = 0x6200; MEM32(0x6200) = 0xA0DD8CB8u; MEM32(0x620c) = 0x6300;
    for (unsigned i = 0; i < 3; ++i) {
        MEM32(0x7000 + 16 * i) = 0x5000;
        MEM32(0x7100 + 16 * i) = 0x5000;
        MEM32(0xa000 + 16 * i) = 0xb000;
    }
    MEM32(0x7130) = 0x5000;
    MEM32(0x51e8) = 1; MEM32(0xb188) = 2; MEM32(0xb1e0) = 3;
    MEM32(0xb0bc) = 4; MEM32(0xb4a8) = 5;
    MEM32(0x5120) = 6; MEM32(0x509c) = 7; MEM32(0x5098) = 8; MEM32(0x5064) = 9;
    /* This resource node has a different method in the bounds slot. */
    MEM32(0x7200) = 0x5400;
    MEM32(0x5520) = 10; MEM32(0x549c) = 7; MEM32(0x5498) = 8; MEM32(0x5464) = 9;
}
static void box(unsigned i, int x, int y) {
    boxes[i][0] = boxes[i][2] = x;
    boxes[i][1] = boxes[i][3] = y;
}
static int scan(s2_world_targets *out, int x, int y, int radius, uint64_t key) {
    out->count = 0;
    int ok = touch_world_scan(win_manager, 0x4000, 0x2000, x, y, radius, key, out);
    assert(g_esp == 0x9000);
    return ok;
}
int main(void) {
    s2_world_targets out;
    reset();
    assert(s2_touch_world_targets(100, 100, 50, &out) && pick_radius == S2_WORLD_PICK_RADIUS);
    assert(s2_touch_world_targets(100, 100, 0, &out) && pick_radius == 1);
    assert(scan(&out, 100, 100, 16, 0) && queries < 220);
    queries = 0;
    box(0, 124, 100);
    box(1, 124, 124);
    assert(scan(&out, 100, 100, 32, 0) && queries < 390);
    assert(out.count == 1 && out.items[0].x == 124 && out.items[0].y == 100);
    reset();
    box(0, 148, 100);
    box(1, 140, 140);
    assert(scan(&out, 100, 100, S2_WORLD_PICK_RADIUS, 0) && queries < 475);
    assert(out.count == 1 && out.items[0].x == 148 && out.items[0].y == 100);
    reset();
    box(0, 106, 100);
    assert(scan(&out, 100, 100, 4, 0) && out.count == 1 && out.items[0].key == UINT64_MAX);
    reset();

    assert(scan(&out, 100, 100, 12, 0) && out.count == 1);
    assert(out.items[0].key == UINT64_MAX && out.items[0].x == 100 && out.items[0].y == 100);
    assert(scan(&out, 100, 100, 12, UINT64_MAX));
    box(0, 106, 100);
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1);
    uint64_t first = out.items[0].key;
    assert(first && first != UINT64_MAX && out.items[0].x == 106);
    box(2, 104, 100);
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].key == first && out.items[0].x == 104);

    box(1, 100, 100);
    assert(scan(&out, 100, 100, 12, 0) && out.count == 3);
    assert(out.items[2].key == UINT64_MAX);
    assert(out.items[2].anchor_x == out.items[2].x && out.items[2].anchor_y == out.items[2].y);
    assert(out.items[0].x == 100 && out.items[1].x == 104);
    assert(out.items[0].anchor_x == 136 && out.items[0].anchor_y == 61);
    assert(out.items[1].anchor_x == 122 && out.items[1].anchor_y == 43);
    assert(out.items[0].key != out.items[1].key);
    assert(!strcmp(out.items[0].name, out.items[0].key == first ? "One" : "Two"));
    assert(!strcmp(out.items[1].name, out.items[1].key == first ? "One" : "Two"));
    assert(scan(&out, 104, 100, 12, first));
    assert(!scan(&out, 100, 100, 12, first));
    center_offscreen = 1;
    assert(scan(&out, 100, 100, 12, 0) && out.items[0].anchor_x == 100 && out.items[1].anchor_x == 104);

    ground = 0;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 2);
    overlay = 1; box(2, 106, 103);
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].x == 100);
    reset(); box(0, 0, 0);
    assert(scan(&out, 4, 4, 12, 0) && out.count == 1 && out.items[0].x == 0 && out.items[0].y == 0);
    reset(); ground = 0;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && !out.items[0].key);
    reset(); stale_after = 3;
    assert(!scan(&out, 100, 100, 12, 0) && !out.count && queries == 3);
    reset(); box(0, 106, 100); box(1, 100, 100); box(2, 104, 100);
    MEM32(0xA10CC288u) = 0xB800;
    MEM32(0xB800) = 0xBA00; MEM32(0xB810) = 0xBB00; MEM32(0xB830) = 0xBC00;
    MEM32(0xBA00 + 104) = 11; MEM32(0xBB00 + 156) = 12;
    MEM32(0xBA00 + 60) = 13; MEM32(0xBC00 + 72) = 14;
    unavailable = 2; pick_cost_ns = 2000000;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].key == first && tested == 2);
    pick_cost_ns = 0;
    unavailable = 3;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].key == UINT64_MAX);
    unavailable = 0;
    MEM32(0xB100) = 15; MEM32(0xB11C) = 16;
    MEM32(0xA100) = MEM32(0xA110) = 0xA0DC3B30u;
    handset_base = 0xA000;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].key == first && out.items[0].x == 104);
    handset_base = 0;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 3);
    MEM32(0xB03C) = 18; handset_base_id = 1;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].key == first);
    handset_base_id = 9;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 3);
    handset_base_id = 0;
    handset_base = 0xA000; unavailable = 1;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 1 && out.items[0].x == 100);
    unavailable = 0;
    MEM32(0xB1E0) = 17; MEM32(0xA120) = 0xA0DC3B30u;
    assert(scan(&out, 100, 100, 12, 0) && out.count == 3);
    assert(out.items[0].x == 104 && out.items[1].x == 106 && out.items[2].key == UINT64_MAX);
    test_cost_ns = 120000000; tested = 0;
    assert(scan(&out, 100, 100, 12, 0) && tested == 1 && out.count == 1 && out.items[0].x == 100);
    puts("PASS live tap radius, native action filtering and budget, paired handsets, independent phones, ground order, model centers and stale selection");
    return 0;
}
"""


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    source = (root / "android/host/sims2_touch.c").read_text(encoding="utf-8")
    actual = "\n".join(function(source, sig) for sig in (
        "int s2_touch_world_targets(", "static int touch_world_owner(",
        "static uint64_t touch_world_object(", "static void touch_world_name(", "static uint32_t touch_scene_method(",
        "static uint32_t touch_world_member(", "static uint32_t touch_world_guid(", "static int touch_world_phone_base(",
        "static int touch_world_phone_duplicate(",
        "static void touch_world_bounds(", "static void touch_world_anchor(", "static int touch_world_scan(",
    ))
    compile_probe("small-targets", PRELUDE + actual + CHECK, {
        "sims2_settings.h": (root / "android/host/sims2_settings.h").read_text(encoding="utf-8"),
    }, compiler_override=CLANG)
