#ifndef PORT_PANVK_IDENTITY_H
#define PORT_PANVK_IDENTITY_H
#include <stdio.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define PORT_PANVK_DRIVER "libvulkan_panfrost.so"

static inline const char *port_panvk_page_size_error(int bundled)
{
    /* The bundled kbase backend uses 4 KiB pages for its mmap offsets and allocations. */
    return bundled && sysconf(_SC_PAGESIZE) != 4096
        ? "Bundled PanVK requires 4 KiB kernel pages. Import a driver for this page size." : NULL;
}

static inline int port_vulkan_driver_mapped(const char *path)
{
    struct stat driver;
    if (stat(path, &driver)) return 0;
    FILE *maps = fopen("/proc/self/maps", "re");
    if (!maps) return 0;
    char line[4096];
    int found = 0;
    while (fgets(line, sizeof line, maps)) {
        /* Android may map the app's files under a different mount path. */
        unsigned int device_major, device_minor;
        unsigned long long inode;
        char perms[5];
        if (sscanf(line, "%*s %4s %*s %x:%x %llu", perms, &device_major, &device_minor, &inode) == 4 &&
            perms[2] == 'x' && inode == (unsigned long long)driver.st_ino &&
            device_major == major(driver.st_dev) && device_minor == minor(driver.st_dev)) {
            found = 1;
            break;
        }
    }
    fclose(maps);
    return found;
}

static inline int port_panvk_mapped(const char *path)
{
    return port_vulkan_driver_mapped(path);
}
#endif
