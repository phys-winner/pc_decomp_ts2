#ifndef PORT_CRC32_IEEE_H
#define PORT_CRC32_IEEE_H
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <zlib.h>
#if defined(__ANDROID__) && defined(__aarch64__)
#include <arm_acle.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <string.h>

/* IEEE CRC-32, like ZIP/zlib. CRC32C uses a different polynomial. */
__attribute__((target("crc"), noinline))
static uint32_t port_crc32_arm(uint32_t seed, const unsigned char *data, size_t size)
{
    uint32_t crc = ~seed;
    while (size >= 8) {
        uint64_t word;
        memcpy(&word, data, sizeof word);
        crc = __crc32d(crc, word);
        data += 8;
        size -= 8;
    }
    while (size--) crc = __crc32b(crc, *data++);
    return ~crc;
}
#endif

static inline uint32_t port_crc32_ieee(uint32_t seed, const void *buffer, size_t size)
{
    const unsigned char *data = buffer;
#if defined(__ANDROID__) && defined(__aarch64__)
    if (getauxval(AT_HWCAP) & HWCAP_CRC32) return port_crc32_arm(seed, data, size);
#endif
    while (size) {
        uInt chunk = size > UINT_MAX ? UINT_MAX : (uInt)size;
        seed = (uint32_t)crc32(seed, data, chunk);
        data += chunk;
        size -= chunk;
    }
    return seed;
}
#endif
