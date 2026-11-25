#ifndef PORT_CRASH_MARKER_H
#define PORT_CRASH_MARKER_H
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

/* Open at game startup, then pass the descriptor to the game's existing fatal-signal handler. */
static inline int port_crash_marker_open(void)
{
    const char *path = getenv("PORT_CRASH_JOURNAL");
    return path && *path ? open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600) : -1;
}

static inline void port_crash_marker_write(int fd, uint8_t event, off_t offset)
{
    if (fd < 0) return;
    int saved = errno;
    uint32_t pid = (uint32_t)getpid();
    uint8_t bytes[5] = {event, (uint8_t)pid, (uint8_t)(pid >> 8), (uint8_t)(pid >> 16), (uint8_t)(pid >> 24)};
    size_t pos = 0;
    while (pos < sizeof bytes) {
        ssize_t n = pwrite(fd, bytes + pos, sizeof bytes - pos, offset + (off_t)pos);
        if (n > 0) pos += (size_t)n;
        else if (n >= 0 || errno != EINTR) break;
    }
    errno = saved;
}

static inline void port_crash_marker_signal(int fd, int sig)
{
    if (sig > 0 && sig <= 255) port_crash_marker_write(fd, (uint8_t)sig, 0);
}

static inline void port_crash_marker_present(int fd)
{
    port_crash_marker_write(fd, 1, 5);
}
#endif
