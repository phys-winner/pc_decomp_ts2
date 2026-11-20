/* Standalone actual-C check: tools/check_game_resolution.py builds a 16 KiB
 * aligned Android probe; only its isolated device exit status confirms PASS. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#define VOID_HOST_H
static uint32_t guest[64], g_eax, g_ecx, g_esp;
#define GPTR(a) ((uint8_t *)guest + ((a) & 255u))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)GPTR(a))
void void_log(const char *, ...);
typedef struct { uint32_t eax, ecx, esp; } void_regs;
static void void_regs_save(void_regs *r) { *r = (void_regs){g_eax, g_ecx, g_esp}; }
static void void_regs_load(const void_regs *r) { g_eax = r->eax; g_ecx = r->ecx; g_esp = r->esp; }
uint32_t void_galloc(uint32_t);
void void_gfree(uint32_t);
uint32_t void_call_guest_this(uint32_t, uint32_t, int, const uint32_t *);
int void_vm_readable(uint32_t, size_t);
void void_x87_push(double);
static uint64_t void_monotonic_ns(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}
static unsigned modal_message_depth;
static unsigned void_begin_modal_messages(void) { return modal_message_depth++; }
static void void_end_modal_messages(unsigned previous) {
    assert(modal_message_depth == previous + 1);
    modal_message_depth = previous;
}
#include "../android/host/sims2_settings.c"
void s2_touch_reset_camera(void) {}
void s2_touch_reset_cas_pan(void) {}

static int resize_count, resized_scale;
static int input_width, input_height;
uint32_t s2_touch_owner_manager(void) { return 0; }
void void_input_screen_size(int *w, int *h) { *w = input_width; *h = input_height; }
static int modal_calls, nest_modal, fp_pushes, resize_category;
static int lot_show_ok = 1, lot_visible, lot_modal_hide;
static int lot_resize_callbacks, lot_refresh_reenter;
static int property_calls;
static uint32_t loaded_property_value;
static uint32_t load_step_result = 1;
static uint32_t loading_screen_result = 1;
static double fp_value;
void sub_A0619442_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A06194E1_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A0619A89_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A0619C09_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A061A29D_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A061A2CB_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A061A67F_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A061A796_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void void_log(const char *format, ...) { (void)format; }
void void_x87_push(double value) { fp_value = value; ++fp_pushes; }
int void_vm_readable(uint32_t address, size_t bytes) { return address < sizeof guest && bytes <= sizeof guest - address; }
void void_input_set_screen_size(int w, int h) { input_width = w; input_height = h; }
void sub_A01BF515_lifted(void)
{
    loaded_property_value = MEM32(MEM32(g_esp + 8) + 4);
    ++property_calls;
    g_eax = 1;
    g_esp += 12;
}
void s2_ui_scale_requested(int scale)
{
    ++resize_count;
    resized_scale = scale;
    assert(s2_ui_scale() == scale);
}

void sub_A0972F48_lifted(void)
{
    int w, h;
    s2_game_resolution(&w, &h);
    assert(ui_fit_depth && w >= 800 && h >= 600);
    assert(s2_touch_context() & S2_TOUCH_MODAL);
    ++modal_calls;
    if (nest_modal) {
        nest_modal = 0;
        sub_A0972F48();
    }
    g_eax = 0x12345678u;
}

void sub_A0648ADF_lifted(void)
{
    if (MEM32(g_esp + 4)) assert(ui_fit_depth);
    MEM32(g_ecx + 0x1C) = MEM32(g_esp + 4);
    g_eax = 0x87654321u;
    g_esp += 8;
}

void sub_A06491B9_lifted(void) { g_eax = 0x2468ACE0u; g_esp += 8; }
void sub_A0647521_lifted(void) { g_eax = 0x10203040u; g_esp += 4; }
void sub_A06195A1_lifted(void) { assert(ui_fit_depth); g_eax = g_ecx; g_esp += 4; }
void sub_A061A2F9_lifted(void) { assert(ui_fit_depth); g_eax = g_ecx; g_esp += 4; }
void sub_A0769278_lifted(void) { assert(ui_fit_depth); g_eax = g_ecx; g_esp += 12; }
void sub_A0769B01_lifted(void) { if (ui_fit_chooser == g_ecx) assert(ui_fit_depth); g_eax = g_ecx; g_esp += 12; }
void sub_A076662A_lifted(void) { g_eax = g_ecx; g_esp += 4; }
void sub_A05C0B82_lifted(void) { assert(ui_fit_depth); g_eax = g_ecx; g_esp += 12; }
void sub_A05C0C89_lifted(void) { if (ui_fit_loading == g_ecx) assert(ui_fit_depth); g_eax = g_ecx; g_esp += 12; }
void sub_A05BE74D_lifted(void) {
    if (MEM32(MEM32(g_esp + 8)) == 0xA2BF8AD5u)
        assert(ui_fit_loading_screen == g_ecx && ui_fit_height_depth && (s2_touch_context() & S2_TOUCH_LOADING));
    g_eax = loading_screen_result;
    g_esp += 12;
}
void sub_A0C47EFC_lifted(void) { assert(ui_fit_depth && ui_fit_height_depth && (s2_touch_context() & S2_TOUCH_LOADING));g_eax=0xABCDEF01;g_esp+=20; }
void sub_A0C471DA_lifted(void) { g_eax=load_step_result;g_esp+=4; }
void sub_A0C46C58_lifted(void) { g_eax=0x10203040;g_esp+=4; }
void sub_A0C44B8C_lifted(void) { assert(ui_fit_depth && ui_fit_height_depth && (s2_touch_context() & S2_TOUCH_LOADING));g_eax=0xABCDEF01;g_esp+=24; }
void sub_A0C42D84_lifted(void) { g_eax=load_step_result;g_esp+=4; }
void sub_A0C42483_lifted(void) { g_eax=0x10203040;g_esp+=4; }

void sub_A07B6484_lifted(void) { lot_visible = 0; g_eax = 1; g_esp += 4; }
void sub_A07B7B27_lifted(void)
{
    assert(ui_fit_depth);
    uint32_t sp = g_esp;
    lot_visible = lot_show_ok;
    if (lot_modal_hide) sub_A07B6484();
    g_eax = (uint32_t)lot_show_ok;
    g_esp = sp + 16;
}
void sub_A07B6F59_lifted(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    if (lot_refresh_reenter) s2_ui_resized();
    if (lot_visible) {
        sub_A07B6484();
        g_ecx = self; g_esp = 128;
        MEM32(g_esp + 12) = 3;
        sub_A07B7B27();
    }
    g_eax = 1;
    g_esp = sp + 4;
}

uint32_t void_call_guest_this(uint32_t va, uint32_t self, int nargs, const uint32_t *args)
{
    assert(va == 0xA07B6F59u && !nargs && !args);
    ++lot_resize_callbacks;
    uint32_t sp = g_esp;
    g_ecx = self;
    g_esp -= 4;
    sub_A07B6F59();
    assert(g_esp == sp);
    return g_eax;
}
void sub_A07B2A6F_lifted(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    g_ecx = self + 0xC;
    sub_A07B6484();
    g_eax = self;
    g_esp = sp + 4;
}

static void lot_card_call(void (*hook)(void), uint32_t base, uint32_t pop, uint32_t mode)
{
    g_ecx = base + 0xC; g_esp = 128;
    MEM32(g_esp + 12) = mode;
    hook();
    assert(g_esp == 128 + pop);
}

static void screen_hook(void (*hook)(void), uint32_t self, uint32_t stack_pop)
{
    g_ecx = self; g_esp = 128;
    hook();
    assert(g_esp == 128 + stack_pop && g_eax == self);
}

static void option_visibility(uint32_t self, int show)
{
    g_ecx = self;
    g_esp = 128;
    MEM32(g_esp + 4) = (uint32_t)show;
    sub_A0648ADF();
    assert(g_esp == 136 && g_eax == 0x87654321u);
}

static void whole_option_visibility(uint32_t self, int show)
{
    g_ecx = self + 8;
    g_esp = 128;
    MEM32(g_esp + 4) = (uint32_t)show;
    sub_A06491B9();
    assert(g_esp == 136 && g_eax == 0x2468ACE0u);
}

void sub_A064785E_lifted(void)
{
    uint32_t self = g_ecx;
    option_visibility(self, 0);
    option_visibility(self, resize_category);
}

static void check(int screen_w, int screen_h, int scale, int expected_w, int expected_h)
{
    phys_w = screen_w;
    phys_h = screen_h;
    ui_scale = scale;
    int w, h;
    s2_game_resolution(&w, &h);
    assert(w == expected_w && h == expected_h);
    assert(ui_scale == scale && phys_w == screen_w && phys_h == screen_h);
}

static void check_saved_scale_migration(void)
{
    char directory[] = "/data/local/tmp/s2-ui-scale-XXXXXX", path[1400];
    static const char *const subdirs[] = {
        "/Documents", "/Documents/EA Games", "/Documents/EA Games/The Sims 2 Legacy",
        "/Documents/EA Games/The Sims 2 Legacy/Config"
    };
    assert(mkdtemp(directory));
    for (unsigned i = 0; i < sizeof subdirs / sizeof subdirs[0]; ++i) {
        snprintf(path, sizeof path, "%s%s", directory, subdirs[i]);
        assert(!mkdir(path, 0700));
    }
    snprintf(path, sizeof path, "%s%s/userProps.xml", directory, subdirs[3]);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputs("<cGZPropertySetString>\n<AnyUint32 key=\"s2UIScale\" type=\"0xeb61e4f7\">400</AnyUint32>\n"
        "<AnyUint32 key=\"s2FrameRateLimit\" type=\"0xeb61e4f7\">75</AnyUint32>\n"
        "<AnyUint32 key=\"optionSoundQuality\" type=\"0xeb61e4f7\">3</AnyUint32>\n"
        "<AnyString key=\"probeUnicode\" type=\"0x0b8bea18\">Семья Ёж</AnyString>\n</cGZPropertySetString>\n", file) >= 0);
    assert(!fclose(file));
    unsetenv("S2_UI_SCALE");
    unsetenv("S2_FPS_LIMIT");
    unsetenv("S2_RENDER_SCALE");
    unsetenv("PORT_OPTION_SCREEN_RESOLUTION");
    s2_settings_init(directory, 2400, 1080);
    assert(ui_scale == 300 && ui_scale_choice == 300 && input_width == 2400 && input_height == 1080);
    int previous_resizes = resize_count;
    g_ecx = 32; sub_A05C0B82();
    assert(!ui_startup_fit && ui_fit_loading == 32 && resize_count == previous_resizes);
    ui_owner_fit(&ui_fit_loading, 32, 0);
    assert(!ui_fit_depth && !ui_fit_height_depth);
    assert(s2_fps_limit() == 75);
    char *text = read_text(path, NULL);
    assert(text && xml_value(text, "s2UIScale") == 300 && xml_value(text, "s2FrameRateLimit") == 75);
    free(text);
    /* This checks disk persistence only; the separate native fixture checks the live guest setter. */
    assert(persist_ui_scale(175));
    text = read_text(path, NULL);
    assert(text && xml_value(text, "s2UIScale") == 175 && xml_value(text, "s2FrameRateLimit") == 75);
    assert(xml_value(text, "optionSoundQuality") == 3 && strstr(text, ">Семья Ёж</AnyString>"));
    char temporary[1600];
    snprintf(temporary, sizeof temporary, "%s.native-ui.tmp", path);
    assert(access(temporary, F_OK) && !mkdir(temporary, 0700));
    assert(!persist_ui_scale(225)); /* An unwritable temporary target must not truncate the original. */
    char *unchanged = read_text(path, NULL);
    assert(unchanged && !strcmp(text, unchanged));
    free(unchanged); free(text);
    assert(!rmdir(temporary));
    assert(persist_ui_scale(300));
    setenv("S2_UI_SCALE", "400", 1);
    s2_settings_init(directory, 2400, 1080);
    assert(ui_scale == 300 && ui_scale_choice == 300 && input_width == 2400 && input_height == 1080);
    ui_startup_fit = 0;
    assert(s2_fps_limit() == 75);
    changed("s2FrameRateLimit", 60);
    assert(s2_fps_limit() == 60);
    changed("s2FrameRateLimit", 75);
    assert(s2_fps_limit() == 75);
    /* The actual native SetProperty hook also keeps 75 supplied by the guest. */
    fps_limit = 60;
    memcpy(GPTR(64), "s2FrameRateLimit", 16);
    MEM32(32) = 64; MEM32(48) = 16; MEM32(52) = 31;
    MEM32(92) = 75;
    g_esp = 128; MEM32(132) = 32; MEM32(136) = 88;
    sub_A01BF515();
    assert(s2_fps_limit() == 75 && loaded_property_value == 75 && property_calls == 1 && g_esp == 140);
    unsetenv("S2_UI_SCALE");
    /* Launcher resolution controls GPU pixels and never overwrites UI preference. */
    setenv("PORT_OPTION_SCREEN_RESOLUTION", "33", 1);
    s2_settings_init(directory, 2400, 1080);
    assert(s2_render_scale() == 33 && ui_scale == 300 && input_width == 2400 && input_height == 1080);
    text = read_text(path, NULL);
    assert(text && xml_value(text, "s2UIScale") == 300 && xml_value(text, "s2RenderScale") == 33);
    free(text);
    setenv("S2_RENDER_SCALE", "150", 1);
    s2_settings_init(directory, 2400, 1080);
    assert(s2_render_scale() == 150 && ui_scale == 300);
    text = read_text(path, NULL);
    assert(text && xml_value(text, "s2RenderScale") == 33);
    free(text);
    unsetenv("S2_RENDER_SCALE");
    unsetenv("PORT_OPTION_SCREEN_RESOLUTION");
    render_scale = 100;
    file = fopen(path, "wb");
    assert(file && fputs("malformed config; preserve unchanged", file) >= 0 && !fclose(file));
    assert(!persist_ui_scale(225));
    text = read_text(path, NULL);
    assert(text && !strcmp(text, "malformed config; preserve unchanged"));
    free(text);
    assert(!remove(path));
    for (unsigned i = sizeof subdirs / sizeof subdirs[0]; i; --i) {
        snprintf(path, sizeof path, "%s%s", directory, subdirs[i - 1]);
        assert(!rmdir(path));
    }
    assert(!rmdir(directory));
}

int main(void)
{
    ui_fit_linger_ns = 0;
    screen_hook(sub_A0619442, 48, 4);
    assert(s2_touch_context() == S2_TOUCH_BUILD);
    screen_hook(sub_A06194E1, 64, 4);
    screen_hook(sub_A061A29D, 48, 4);
    assert(s2_touch_context() == S2_TOUCH_BUY);
    screen_hook(sub_A061A2CB, 64, 4);
    assert(!s2_touch_context());
    screen_hook(sub_A0619A89, 48, 4);
    assert(s2_touch_context() == S2_TOUCH_LIVE);
    screen_hook(sub_A061A67F, 48, 4);
    screen_hook(sub_A0619C09, 64, 4);
    assert(s2_touch_context() == S2_TOUCH_TOWN);
    screen_hook(sub_A061A796, 64, 4);
    assert(!s2_touch_context());
    check_saved_scale_migration();
    render_scale = 100;
    check(2400, 1080, 100, 2400, 1080);
    check(2400, 1080, 150, 2400, 1080);
    check(2400, 1080, 200, 2400, 1080);
    check(2400, 1080, 300, 2400, 1080);
    check(2780, 1264, 200, 2780, 1264);
    check(2944, 1840, 200, 2944, 1840);
    check(1920, 1080, 200, 1920, 1080);
    check(1280, 720, 200, 1280, 720);
    check(800, 600, 200, 800, 600);
    check(1280, 1024, 200, 1280, 1024);
    check(1080, 1080, 200, 1080, 1080);
    render_scale = 50;
    check(2400, 1080, 100, 2400, 1080);
    render_scale = 200;
    check(2400, 1080, 200, 2400, 1080);
    for (int sw = 320; sw <= 4096; sw += 127) {
        for (int sh = 240; sh <= 2160; sh += 113) {
            for (int scale = 100; scale <= 300; scale += 25) {
                for (int render = 25; render <= 200; render += 25) {
                    phys_w = sw; phys_h = sh; ui_scale = scale; render_scale = render;
                    int w, h, chosen_w = sw & ~1, chosen_h = sh & ~1;
                    s2_game_resolution(&w, &h);
                    assert(w >= 800 && h >= 600 && !(w & 1) && !(h & 1));
                    if (chosen_w >= 800 && chosen_h >= 600) assert(w == chosen_w && h == chosen_h);
                    else assert(abs(w * sh - h * sw) <= 2 * (sw > sh ? sw : sh));
                    /* Context, nested modals and delayed teardown never change geometry. */
                    ui_fit_depth = ui_fit_height_depth = 3; ui_fit_linger = 2; ui_startup_fit = 1;
                    int modal_w, modal_h;
                    s2_game_resolution(&modal_w, &modal_h);
                    assert(modal_w == w && modal_h == h);
                    ui_fit_depth = ui_fit_height_depth = ui_fit_linger = ui_startup_fit = 0;
                }
            }
        }
    }
    phys_w = 2400; phys_h = 1080; render_scale = 100;
    ui_scale = ui_scale_choice = 200; resize_count = 0;
    changed("s2UIScale", 100);
    assert(ui_scale == 100 && ui_scale_choice == 100 && resized_scale == 100 && resize_count == 1);
    changed("s2UIScale", 100); assert(resize_count == 1);
    changed("s2UIScale", 150); assert(ui_scale == 150 && resize_count == 2);
    changed("s2UIScale", 300); assert(ui_scale == 300 && resize_count == 3);
    changed("s2UIScale", 400); assert(ui_scale == 300 && resize_count == 3);
    changed("s2UIScale", 1); assert(ui_scale == 100 && resize_count == 4);
    setenv("S2_UI_SCALE", "175", 1);
    changed("s2UIScale", 200); assert(ui_scale == 100 && resize_count == 4);
    unsetenv("S2_UI_SCALE");
    changed("s2RenderScale", 50); assert(render_scale == 50 && resize_count == 5);
    changed("s2RenderScale", 50); assert(resize_count == 5);
    changed("s2RenderScale", 100); assert(render_scale == 100 && resize_count == 6);
    ui_scale = ui_scale_choice = 300; resize_count = 0;
    nest_modal = 1; sub_A0972F48();
    assert(modal_calls == 2 && !ui_fit_depth && !resize_count && g_eax == 0x12345678u);
    g_esp = 100; g_eax = 42; g_ecx = 43;
    sub_A08B8390(); sub_A09713BE();
    assert(g_esp == 116 && g_eax == 42 && g_ecx == 43 && fp_value == ui_applied_factor && fp_pushes == 2);
    option_visibility(16, 1); option_visibility(16, 1); option_visibility(32, 1); option_visibility(16, 0);
    assert(ui_fit_depth == 1 && ui_fit_option == 32 && !resize_count);
    resize_category = 1; g_ecx = 32;
    for (int i = 0; i < 10; ++i) sub_A064785E();
    assert(ui_fit_depth == 1 && !ui_option_resize_depth && !resize_count);
    whole_option_visibility(32, 0); whole_option_visibility(32, 0);
    assert(!ui_fit_depth && !ui_fit_option && MEM32(32 + 0x1C) == 1 && !resize_count);
    whole_option_visibility(32, 1); whole_option_visibility(32, 1);
    assert(ui_fit_depth == 1 && ui_fit_option == 32);
    g_ecx = 32; g_esp = 128; sub_A0647521();
    assert(!ui_fit_depth && !ui_fit_option && g_esp == 132 && g_eax == 0x10203040u);
    MEM32(32 + 0x1C) = 0; whole_option_visibility(32, 1); assert(!ui_fit_depth);
    screen_hook(sub_A06195A1, 48, 4);
    assert(ui_fit_cas == 48 && ui_fit_depth == 1 && ui_fit_height_depth == 1);
    nest_modal = 1; sub_A0972F48();
    assert(ui_fit_depth == 1 && ui_fit_height_depth == 1 && !resize_count);
    screen_hook(sub_A061A2F9, 48, 4); assert(!ui_fit_cas && !ui_fit_depth);
    screen_hook(sub_A05C0B82, 64, 12);
    screen_hook(sub_A0769278, 80, 12); screen_hook(sub_A0769278, 80, 12);
    assert(ui_fit_depth == 2 && ui_fit_loading == 64 && ui_fit_chooser == 80);
    screen_hook(sub_A05C0C89, 64, 12); screen_hook(sub_A05C0C89, 64, 12);
    assert(ui_fit_depth == 1 && !ui_fit_loading);
    screen_hook(sub_A0769278, 96, 12); screen_hook(sub_A0769B01, 80, 12);
    assert(ui_fit_depth == 1 && ui_fit_chooser == 96);
    screen_hook(sub_A076662A, 96, 4); assert(!ui_fit_depth && !ui_fit_chooser);
    lot_card_call(sub_A07B6484, 112, 4, 0); lot_card_call(sub_A07B6F59, 112, 4, 0); s2_ui_resized();
    assert(!lot_resize_callbacks && !ui_fit_lot_info);
    lot_card_call(sub_A07B7B27, 112, 16, 0); lot_card_call(sub_A07B7B27, 112, 16, 0);
    assert(ui_fit_depth == 1 && ui_fit_lot_info == 112 && g_eax == 1);
    lot_refresh_reenter = 1; s2_ui_resized(); lot_refresh_reenter = 0;
    assert(lot_resize_callbacks == 1 && ui_fit_depth == 1 && ui_fit_lot_info == 112);
    for (int i = 0; i < 10; ++i) lot_card_call(sub_A07B6F59, 112, 4, 0);
    lot_card_call(sub_A07B6484, 112, 4, 0); lot_card_call(sub_A07B6484, 112, 4, 0);
    assert(!ui_fit_depth && !ui_fit_lot_info && !ui_lot_info_refresh_depth);
    lot_show_ok = 0; lot_card_call(sub_A07B7B27, 112, 16, 0);
    assert(!ui_fit_depth && !ui_fit_lot_info && g_eax == 0);
    lot_show_ok = 1; lot_card_call(sub_A07B7B27, 112, 16, 0); screen_hook(sub_A07B2A6F, 112, 4);
    assert(!ui_fit_depth && !ui_fit_lot_info);
    lot_modal_hide = 1; lot_card_call(sub_A07B7B27, 112, 16, 1); lot_modal_hide = 0;
    assert(!ui_fit_depth && !ui_fit_lot_info && !resize_count);
    g_ecx=64;g_esp=128;sub_A0C47EFC();
    assert(ui_fit_town_loading==64 && ui_fit_depth==1 && g_esp==148 && g_eax==0xABCDEF01);
    load_step_result=0xABCDEF01;g_ecx=64;g_esp=128;sub_A0C471DA();assert(ui_fit_town_loading==64);
    g_ecx=80;g_esp=128;sub_A0C44B8C();assert(ui_fit_lot_loading==80 && ui_fit_depth==2 && g_esp==152);
    load_step_result=0xABCDEF00;g_ecx=64;g_esp=128;sub_A0C471DA();assert(!ui_fit_town_loading && ui_fit_depth==1);
    g_ecx=81;g_esp=128;sub_A0C42483();assert(ui_fit_lot_loading==80);
    g_ecx=80;g_esp=128;sub_A0C42D84();assert(!ui_fit_lot_loading && !ui_fit_depth);
    g_ecx=64;g_esp=128;sub_A0C47EFC();g_ecx=80;g_esp=128;sub_A0C44B8C();assert(ui_fit_depth==2);
    g_ecx=64;g_esp=128;sub_A0C46C58();g_ecx=80;g_esp=128;sub_A0C42483();
    assert(!ui_fit_depth && !(s2_touch_context() & S2_TOUCH_LOADING) && !resize_count);
    ui_fit_linger_ns = 400000000ull;ui_fit(1,1);ui_fit(0,1);
    assert(ui_fit_linger == 2 && !resize_count);
    ui_fit_linger_until = 0;s2_fit_tick();assert(!ui_fit_linger && !resize_count);
    s2_fit_scene_attached();
    ui_fit_linger_ns = 0;
    MEM32(132) = 120; MEM32(136) = 144; MEM32(144) = 0xA2BF8AD5u;
    g_ecx = 88; g_esp = 128; sub_A05BE74D();
    assert(g_esp == 140 && g_eax == 1 && ui_fit_loading_screen == 88);
    g_ecx = 64; g_esp = 128; sub_A0C47EFC();
    g_ecx = 64; g_esp = 128; sub_A0C471DA();
    assert(!ui_fit_town_loading && ui_fit_height_depth == 1 && s2_touch_context() & S2_TOUCH_LOADING);
    MEM32(144) = 0xA2BF8AD6u; g_ecx = 88; g_esp = 128; sub_A05BE74D();
    assert(!ui_fit_loading_screen && !ui_fit_depth && !(s2_touch_context() & S2_TOUCH_LOADING));
    MEM32(144) = 0xA2BF8AD5u; loading_screen_result = 0;
    g_ecx = 88; g_esp = 128; sub_A05BE74D();
    assert(!ui_fit_loading_screen && !ui_fit_depth && g_eax == 0 && g_esp == 140);
    puts("game resolution PASS (fixed canvas across UI/render choices, nested ownership/ABI, atomic persistence)");
    return 0;
}
