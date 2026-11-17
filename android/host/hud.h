/* What the game shows that the touch layer reacts to: the paint cursor (any
 * screen with a cursor: menus, the body, the map, painting) and the helper
 * icons the HUD draws at the centre of the screen when the hero can interact
 * with something. Recognised by content: the game loads them as DDS files
 * through D3DXCreateTextureFromFileInMemoryEx (hud.c). */
#ifndef VOID_HUD_H
#define VOID_HUD_H
#include <stdint.h>

enum {
    HUD_CURSOR = 1u << 0,       /* cursor_paint */
    HUD_USE = 1u << 1,          /* ui/helper/ui_helper_use */
    HUD_USE_ARROW = 1u << 2,    /* ui/helper/ui_helper_use_arrow */
    HUD_PAINT = 1u << 3,        /* ui/helper/ui_helper_paint */
    HUD_TRAP = 1u << 4,         /* ui/helper/ui_helper_trap */
    HUD_WAIT = 1u << 5,         /* ui/helper/ui_helper_wait, _wait_wheel */
    HUD_DEATH = 1u << 6,        /* ui/helper/ui_helper_death */
    HUD_NO_LYMPHA = 1u << 7,    /* ui/helper/ui_helper_not_enough_limfa, _col */
    HUD_QUESTLOG = 1u << 8,     /* ui/helper/ui_helper_questlog */
    HUD_CURSOR_LYMPHA = 1u << 9,   /* cursor_paint_with_limfa: the brush carries a colour */
};

/* The tag of a texture created from these file bytes (0: none). */
uint32_t void_hud_tag_for(const uint8_t *data, uint32_t size);
/* A draw sampled textures with these tags. */
void void_hud_draw(uint32_t tags);
/* A frame was presented: what it drew becomes the state. */
void void_hud_present(void);
/* Tags drawn in the last presented frame; *frame (if set) counts presents. */
uint32_t void_hud_state(uint32_t *frame);

#endif
