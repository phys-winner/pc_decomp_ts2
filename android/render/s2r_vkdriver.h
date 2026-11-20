/* The Vulkan driver the native Vulkan renderer runs on (s2r_vkdriver.c). Include after
 * <vulkan/vulkan.h> (VK_NO_PROTOTYPES: every function comes from the driver's vkGetInstanceProcAddr). */
#pragma once
#include <stddef.h>

/* S2R_VK_SYSTEM, S2R_VK_TURNIP, S2R_VK_PANVK: s2r_internal.h. */
/* GameActivity's choice, before the device starts: the driver kind and the APK's library folder. */
void s2r_set_vk_driver(int kind, const char *native_dir);
int s2r_vk_driver(void);
const char *s2r_vk_driver_name(int kind);
/* The player's own driver (the launcher's import) rather than the APK's. */
int s2r_vk_custom(void);
PFN_vkGetInstanceProcAddr s2r_vk_open(int kind, char *error, size_t error_size);
int s2r_vk_panvk_mapped(void);
