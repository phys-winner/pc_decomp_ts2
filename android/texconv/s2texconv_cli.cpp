/* adb-shell front end of the texture converter (s2texconv.h):
 *   s2texconv scan GAME
 *   s2texconv convert GAME etc2|astc [THREADS]
 *   s2texconv remove GAME
 * Prints the progress every second; exit status 0 done, 1 cancelled, 2 failed. */
#include "s2texconv.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s scan|convert|remove GAME [etc2|astc] [THREADS]\n", argv[0]);
        return 2;
    }
    const char *command = argv[1], *game = argv[2];
    const char *index = getenv("PORT_MOUNT_GAME"), *fd_text = getenv("PORT_MOUNT_FD_GAME");
    int archive_fd = fd_text ? atoi(fd_text) : -1;
    if (!strcmp(command, "scan")) {
        s2texconv_status s;
        if (s2texconv_scan_zip(game, &s, index, archive_fd)) {
            s2texconv_progress p;
            s2texconv_get_progress(&p);
            fprintf(stderr, "scan failed: %s\n", p.message);
            return 2;
        }
        printf("sample texture resources %lld (%.1f MB); converted format %d, %lld textures, %.1f MB%s\n",
               static_cast<long long>(s.textures), s.bytes / 1e6, s.converted_format,
               static_cast<long long>(s.converted_textures), s.converted_bytes / 1e6, s.stale ? " (stale)" : "");
        return 0;
    }
    if (!strcmp(command, "remove")) return s2texconv_remove(game) ? 2 : 0;
    if (strcmp(command, "convert") || argc < 4) {
        fprintf(stderr, "unknown command\n");
        return 2;
    }
    int format = !strcmp(argv[3], "astc") ? 2 : 1;
    int threads = argc > 4 ? atoi(argv[4]) : 0;
    int result = 0;
    std::atomic<bool> finished{false};
    auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() { result = s2texconv_convert_zip(game, format, threads, index, archive_fd); finished = true; });
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        s2texconv_progress p;
        s2texconv_get_progress(&p);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        printf("%6.0f s  state %d  packages %lld/%lld  bytes %.0f/%.0f MB  levels %lld\n", seconds, p.state,
               static_cast<long long>(p.done), static_cast<long long>(p.total), p.bytes_done / 1e6, p.bytes_total / 1e6,
               static_cast<long long>(p.levels));
        fflush(stdout);
    }
    worker.join();
    s2texconv_progress p;
    s2texconv_get_progress(&p);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (result < 0) fprintf(stderr, "failed: %s\n", p.message);
    else printf("%s in %.0f s, %lld levels\n", result ? "cancelled" : "done", seconds, static_cast<long long>(p.levels));
    return result < 0 ? 2 : result;
}
