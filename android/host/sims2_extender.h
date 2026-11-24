#ifndef SIMS2_EXTENDER_H
#define SIMS2_EXTENDER_H

enum {
    S2_EXTENDER_SEPARATES = 1u << 0,
    S2_EXTENDER_ZODIAC = 1u << 1,
    S2_EXTENDER_RNG = 1u << 2,
    S2_EXTENDER_PINK = 1u << 3,
    S2_EXTENDER_MAKEUP = 1u << 4,
    S2_EXTENDER_POOL = 1u << 5,
    S2_EXTENDER_OUTDOOR = 1u << 6,
    S2_EXTENDER_SUN = 1u << 7,
    S2_EXTENDER_THUMBNAILS = 1u << 8,
    S2_EXTENDER_OCEAN = 1u << 9,
    S2_EXTENDER_SIMANTICS = 1u << 10,
    S2_EXTENDER_GROUPS = 1u << 11,
    S2_EXTENDER_OFB = 1u << 12,
    S2_EXTENDER_LUA = 1u << 13,
};

extern unsigned s2_extender_flags;
void s2_extender_init(const char *profile);
void s2_extender_guest_init(void);
int s2_extender_skips_intro(void);
double s2_extender_ui_reference(void);
void s2_extender_camera_sensitivity(float *yaw, float *pitch);
void s2_extender_note_cas_tab(void);
extern const char s2_extender_downloads_dir[];
const char *s2_extender_download_name(unsigned index);
const char *s2_extender_download_path(const char *name);
void s2_extender_note_package_open(const char *path);
void s2_extender_custom_content(int allowed);
int s2_extender_hidden_clothes(unsigned window);

#endif
