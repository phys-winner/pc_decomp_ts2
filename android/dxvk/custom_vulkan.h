/* App-local Vulkan through adrenotools, with UT3's Turnip and Frostpunk's PanVK checks. */
#pragma once
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>
#include "../launcher/native/panvk_identity.h"

#define SIMS2_TURNIP_DRIVER "libs2turnip.so"

inline PFN_vkGetInstanceProcAddr sims2_open_custom_vulkan(const char *directory, bool panvk, char *error, size_t error_size) {
  const char *label = panvk ? "PanVK" : "Turnip";
  if (!directory || !*directory) { snprintf(error, error_size, "%s: no native library directory", label); return nullptr; }
  const char *custom = std::getenv(panvk ? "PORT_PANVK_DRIVER" : "PORT_TURNIP_DRIVER");
  if (panvk) {
    if (const char *page_error = port_panvk_page_size_error(!custom || !*custom)) {
      snprintf(error, error_size, "%s", page_error);
      return nullptr;
    }
  }
  static PFN_vkGetInstanceProcAddr opened;
  if (opened) return opened;
  std::string path = std::string(directory) + "/";
  void *helper = dlopen((path + "libadrenotools.so").c_str(), RTLD_NOW | RTLD_LOCAL);
  using Open = void *(*)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
  auto open = helper ? reinterpret_cast<Open>(dlsym(helper, "adrenotools_open_libvulkan")) : nullptr;
  /* 1 = ADRENOTOOLS_DRIVER_CUSTOM; hooks and driver both from nativeLibraryDir; no temporary
   * directory (API 29+ uses memfd), no file redirection, no GPU mapping import. */
  std::string driver = panvk ? PORT_PANVK_DRIVER : SIMS2_TURNIP_DRIVER, driver_path = path;
  if (custom && *custom) {
    std::string full(custom); auto slash = full.find_last_of('/');
    if (full.front() != '/' || slash == full.size() - 1) {
      snprintf(error, error_size, "%s: driver must be an absolute file path", label); return nullptr;
    }
    driver_path = full.substr(0, slash + 1); driver = full.substr(slash + 1);
  }
  void *loader = open ? open(RTLD_NOW | RTLD_LOCAL, 1, nullptr, path.c_str(), driver_path.c_str(), driver.c_str(), nullptr, nullptr) : nullptr;
  auto get = loader ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr")) : nullptr;
  if (!get) {
    const char *detail = dlerror();
    snprintf(error, error_size, "%s: adrenotools returned no driver (%s)", label,
        detail ? detail : helper ? "no custom driver" : "libadrenotools.so missing");
    return nullptr;
  }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "The Sims 2 driver check";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  VkInstance instance{};
  VkResult result = create ? create(&info, nullptr, &instance) : VK_ERROR_INITIALIZATION_FAILED;
  if (result != VK_SUCCESS) { snprintf(error, error_size, "%s: vkCreateInstance(1.3) failed %d", label, result); return nullptr; }
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
  auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
  auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(instance, "vkGetPhysicalDeviceProperties2"));
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  bool supported = false;
  uint32_t count = 8;
  VkPhysicalDevice devices[8];
  char seen[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = "no device";
  if (enumerate && (panvk ? properties2 != nullptr : properties != nullptr) && destroy &&
      enumerate(instance, &count, devices) >= VK_SUCCESS)
    for (unsigned i = 0; i < count && i < 8; ++i) {
      VkPhysicalDeviceDriverProperties driverInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
      VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driverInfo};
      if (panvk) properties2(devices[i], &p);
      else properties(devices[i], &p.properties);
      const auto &gpu = p.properties;
      snprintf(seen, sizeof seen, "%s", gpu.deviceName);
      if (gpu.apiVersion >= VK_API_VERSION_1_3)
        supported |= panvk ? gpu.vendorID == 0x13b5 && driverInfo.driverID == VK_DRIVER_ID_MESA_PANVK
                          : gpu.vendorID == 0x5143 && strstr(gpu.deviceName, "Turnip");
    }
  if (panvk && supported) supported = port_panvk_mapped((driver_path + driver).c_str());
  if (destroy) destroy(instance, nullptr);
  if (!supported) { snprintf(error, error_size, "%s: selected driver did not provide a Vulkan 1.3 device (%s)", label, seen); return nullptr; }
  opened = get;
  return get;
}
