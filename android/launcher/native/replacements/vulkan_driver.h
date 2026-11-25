#ifndef PORT_VULKAN_DRIVER_H
#define PORT_VULKAN_DRIVER_H
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

typedef void *(*port_adrenotools_open)(int, int, const char *, const char *, const char *, const char *, const char *, void **);

static inline PFN_vkGetInstanceProcAddr port_open_system_vulkan(void)
{
    void *loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : NULL;
}

/* The port's Mesa driver through adrenotools from a real folder (adrenotools requires one): the native
 * library directory (extractNativeLibs), or the folder where a game that packs its Turnip set
 * unpacked it (PortHooks.checkLibraries). Its hooks load the driver in a linker namespace of its own, so the
 * system driver stays untouched. 1 = ADRENOTOOLS_DRIVER_CUSTOM; no temporary directory (API 29+
 * uses memfd), no file redirection, no GPU mapping import. A driver given as a full path (the
 * player's own, TurnipDriver.java) is loaded from its folder with the same hooks. */
static inline PFN_vkGetInstanceProcAddr port_open_vulkan_driver(const char *directory, const char *driver, char *error, size_t size)
{
    char path[1024], hooks[1024], folder[1024];
    const char *slash = strrchr(driver, '/');
    if (!directory || snprintf(path, sizeof path, "%s/libadrenotools.so", directory) >= (int)sizeof path ||
        snprintf(hooks, sizeof hooks, "%s/", directory) >= (int)sizeof hooks ||
        snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - driver + 1) : 0, driver) >= (int)sizeof folder) {
        snprintf(error, size, "Vulkan driver path unavailable or too long");
        return NULL;
    }
    /* Custom drivers use the platform loader's Android WSI. */
    dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
    void *helper = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    port_adrenotools_open open = helper ? (port_adrenotools_open)dlsym(helper, "adrenotools_open_libvulkan") : NULL;
    void *loader = open ? open(RTLD_NOW | RTLD_LOCAL, 1, NULL, hooks, slash ? folder : hooks, slash ? slash + 1 : driver, NULL, NULL) : NULL;
    PFN_vkGetInstanceProcAddr get = loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : NULL;
    if (!get) {
        const char *detail = dlerror();
        snprintf(error, size, "adrenotools returned no driver (%s)",
                 detail ? detail : helper ? "no custom driver" : "libadrenotools.so missing");
    }
    return get;
}

static inline int port_vulkan_is_turnip(const VkPhysicalDeviceProperties *properties, VkDriverId id)
{
    return properties->vendorID == 0x5143 &&
        (id == VK_DRIVER_ID_MESA_TURNIP || (!id && strstr(properties->deviceName, "Turnip")));
}

static inline VkPhysicalDevice port_vulkan_physical_device(VkInstance instance,
    PFN_vkEnumeratePhysicalDevices enumerate, PFN_vkGetPhysicalDeviceProperties properties,
    VkPhysicalDeviceProperties *result)
{
    uint32_t count = 8;
    VkPhysicalDevice devices[8], chosen = VK_NULL_HANDLE;
    if (!enumerate || !properties || enumerate(instance, &count, devices) < VK_SUCCESS) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties candidate;
        properties(devices[i], &candidate);
        if (!chosen || candidate.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
            chosen = devices[i];
            *result = candidate;
        }
        if (candidate.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) break;
    }
    return chosen;
}

static inline uint32_t port_vulkan_graphics_queue(VkPhysicalDevice device,
    PFN_vkGetPhysicalDeviceQueueFamilyProperties properties, uint32_t *timestamp_bits)
{
    uint32_t count = 16;
    VkQueueFamilyProperties queues[16];
    if (!properties) return UINT32_MAX;
    properties(device, &count, queues);
    for (uint32_t i = 0; i < count; ++i) {
        if (!(queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !queues[i].queueCount) continue;
        if (timestamp_bits) *timestamp_bits = queues[i].timestampValidBits;
        return i;
    }
    return UINT32_MAX;
}

static inline int port_vulkan_samples(VkFormatProperties properties)
{
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (properties.optimalTilingFeatures & need) == need;
}

#endif
