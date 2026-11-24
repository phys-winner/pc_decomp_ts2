/* The port's settings (sims2_settings.c): UI scale, frame rate limit, render scale. */
#ifndef SIMS2_SETTINGS_H
#define SIMS2_SETTINGS_H
#include <stdint.h>

void s2_settings_init(const char *data_dir, int screen_w, int screen_h);
int s2_ui_scale(void);          /* percent, 100..300 */
int s2_fps_limit(void);         /* frames per second, 0 = off */
int s2_render_scale(void);      /* percent of the screen's pixels */
int s2_ui_fsr(void);
int s2_render_aa(void);
int s2_render_msaa(void);
int s2_render_upscaler(void);
int s2_ui_drawing(void);
void s2_physical_size(int *w, int *h);
void s2_game_resolution(int *w, int *h);
double s2_ui_factor(void);
uint32_t s2_ui_pick(uint32_t root, int x, int y);
uint32_t s2_ui_mouse_pick(uint32_t root, int x, int y);
void s2_ui_scale_requested(int scale);
void s2_ui_resized(void);
/* The main thread's message pump: a fit that closed returns the UI to its scale. */
void s2_fit_tick(void);
void s2_load_present(uint64_t work_ns);
int s2_loading_animations(void);
void s2_fit_scene_attached(void);
enum { S2_UI_SCALE = 1, S2_UI_CAS_PAN = 2, S2_UI_TEXT_READ = 3, S2_UI_TEXT_WRITE = 4, S2_UI_CAMERA = 5, S2_UI_CATALOG = 6, S2_UI_CHEAT = 7, S2_UI_CLEAR_CATALOG = 8 };
/* Explicit gesture settings; queued on the existing owner-thread input fence. */
int s2_touch_ui_command(unsigned kind, uint32_t value);
int s2_touch_camera(float dx, float dy, float radians, float zoom, float pitch);
int s2_touch_pan(float from_x, float from_y, float to_x, float to_y);
void s2_touch_camera_contact(int active);
void s2_touch_camera_pointer_intent(void);
int s2_touch_camera_hover_active(void);
void s2_touch_camera_hover_enable(int enabled);
/* Benchmarks (script camset/camget): an absolute camera pose; the current one logged. */
int s2_touch_camera_pose(const float pose[6]);
void s2_touch_camera_log(void);
int s2_touch_camera_projection(float projection[3]);
int s2_touch_scroll(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch);
uint64_t s2_touch_scroll_owner(void);
void s2_touch_cancel_fling(void);
int s2_touch_fling(int x, int y, int amount, unsigned context, uint64_t size, unsigned epoch, uint64_t owner);
int s2_touch_catalog_info(int x, int y, unsigned context, uint64_t size, unsigned epoch);
int s2_touch_tool_menu(int action, unsigned context, uint64_t size, unsigned epoch);
int s2_catalog_info(uint32_t proc);
void s2_catalog_track(uint32_t thumbnail);
uint32_t s2_catalog_clear_request(void);
int s2_catalog_clear_owner(uint32_t serial);
int s2_ui_scale_apply(int scale);
void s2_touch_reset_cas_pan(void);
int s2_touch_text_editable(uint32_t window);
int s2_touch_text_read(int x, int y, uint8_t value[513], unsigned *length);
uint64_t s2_touch_text_replace(const uint8_t *value, unsigned length);
uint64_t s2_touch_text_status(unsigned serial);
void s2_touch_cancel_text(void);
void s2_touch_text_pump(uint32_t manager);
void s2_touch_text_owner(unsigned kind, unsigned serial, uint32_t manager);
uint32_t s2_touch_owner_manager(void);

enum {
    S2_TOUCH_MODAL = 1, S2_TOUCH_OPTIONS = 2, S2_TOUCH_CAS = 4,
    S2_TOUCH_CHOOSER = 8, S2_TOUCH_LOADING = 16, S2_TOUCH_LOT_INFO = 32,
    S2_TOUCH_TOWN = 64, S2_TOUCH_LIVE = 128, S2_TOUCH_BUY = 256, S2_TOUCH_BUILD = 512,
    S2_TOUCH_FOCUSED_TEXT = 1024
};
/* Published by the guest's real UI lifecycle hooks; zero means unknown. */
unsigned s2_touch_context(void);
/* Innermost original RunModal window; read on the window owner's thread. */
uint32_t s2_touch_modal_window(void);
int s2_touch_rewards(void);
int s2_touch_photo(void);
void s2_touch_input_context(unsigned flags);
/* Confirmed native tool: 0 unknown, 1 navigation, 2 object, 3 geometry,
 * 4 immediate brush, 5 deletion. This does not assert transactional safety. */
int s2_touch_tool(void);
/* Opaque observed identity in the high word, category in the low word; 0 unavailable. */
uint64_t s2_touch_tool_token(void);
void s2_touch_reset_camera(void);
void s2_touch_request_pick(int x, int y);
/* Low two bits: 2 scene, 1 UI, 0 unavailable;4 slider,8 scrollable,16 text,
 *32 original Grid2 page command,64 catalog thumbnail,256 horizontal time swipe,
 *512 vertical widget swipe,1024 valid time speed (bits 11-12: pause,1x,2x,3x). */
int s2_touch_pick(int x, int y);
int s2_touch_ui_pick(int x, int y, int side, uint64_t *point);
/* The sparse scan and its densely sampled center fit within this bound. */
enum { S2_WORLD_PICK_RADIUS = 48, S2_WORLD_TARGET_LIMIT = 1089 };
typedef struct {
    uint64_t key;
    int x, y;
    int anchor_x, anchor_y;
    char name[256];
} s2_world_target;
typedef struct {
    unsigned count;
    s2_world_target items[S2_WORLD_TARGET_LIMIT];
} s2_world_targets;
int s2_touch_world_targets(int x, int y, int radius, s2_world_targets *targets);
int s2_touch_world_confirm(int x, int y, uint64_t key);
enum {
    S2_SLIDER_UNKNOWN, S2_SLIDER_IN_PROGRESS, S2_SLIDER_CHANGED, S2_SLIDER_LIMIT,
    S2_SLIDER_REJECTED, S2_SLIDER_CANCELLED, S2_SLIDER_BUSY, S2_SLIDER_FAILED
};
/* High word serial; low byte status, then before+101 and after+101 bytes (0 unknown).
 * CANCELLED has no effect; IN_PROGRESS must be polled, never retried. */
uint64_t s2_touch_slider_step(int x, int y, int delta);
uint64_t s2_touch_slider_status(unsigned serial);
/* Explicit object rotation shares the existing command receipt; no purchase/save assertion. */
unsigned s2_touch_layout_epoch(void);
uint64_t s2_touch_swipe_feedback(void);
void s2_touch_note_floor_button(void);
int s2_touch_floor_button_used(void);
uint64_t s2_touch_rotate_object(int x, int y, uint64_t tool, unsigned context, uint64_t size, unsigned epoch, int direction);
/* Advisory exact native type: 1 FloorPattern, 2 Wallpaper; mutations recheck it. */
int s2_touch_fill_kind(void);
/* One original mode2 Down/Up on release. UNKNOWN may have a partial effect after STARTED. */
uint64_t s2_touch_fill_room(int x, int y, uint64_t tool, unsigned context, uint64_t size, unsigned epoch);
int s2_touch_object_cost(void);
int s2_touch_object_preview(uint64_t token);
void s2_touch_pump(void);
int s2_ui_only_resize(void);
void s2_ui_device_reset(int dimensions_only);
int s2_ui_take_material_resize(void);
void s2_touch_barrier(unsigned serial);

int s2_ui_layout_set(void);  /* 0 not yet, 1 the 800x600 layouts, 2 the regular ones */

#endif
