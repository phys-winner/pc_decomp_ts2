/* adb-shell front end of the texture converter (s2texconv.h):
 *   s2texconv scan GAME
 *   s2texconv convert GAME etc2|astc [THREADS [dxt1,dxt3,dxt5]]   (those formats only: a selective conversion,
 *                                                                  extending a cache of the same target format)
 *   s2texconv remove GAME
 *   s2texconv index CACHE_FILE      (the levels a cache holds per source format)
 * Prints the progress every second; exit status 0 done, 1 cancelled, 2 failed. */
#include "s2texconv.h"
#include "s2tc.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

/* The header and the index's entries counted by source and encoding. */
static int index_report(const char *path)
{
    FILE *f = fopen(path, "rb");
    s2tc_header h;
    if (!f || fread(&h, sizeof h, 1, f) != 1 || h.magic != S2TC_MAGIC || h.entry_count > (1u << 26)) {
        fprintf(stderr, "%s: not a texture cache\n", path);
        if (f) fclose(f);
        return 2;
    }
    std::vector<s2tc_entry> entries(static_cast<size_t>(h.entry_count));
    bool ok = !fseeko(f, static_cast<off_t>(h.index_offset), SEEK_SET) &&
              (entries.empty() || fread(entries.data(), sizeof(s2tc_entry), entries.size(), f) == entries.size());
    fclose(f);
    if (!ok) { fprintf(stderr, "%s: index unreadable\n", path); return 2; }
    unsigned long long by_source[8] = {}, by_encoding[4] = {}, bytes = 0;
    for (const s2tc_entry &e : entries) {
        ++by_source[e.source & 7];
        ++by_encoding[e.encoding & 3];
        bytes += e.bytes;
    }
    uint32_t covered = s2tc_covered(h.sources);
    printf("format %u, complete %u, entries %llu, textures %llu, file %llu bytes, header sources %#x (%s%s%s)\n", h.format, h.flags & 1,
           static_cast<unsigned long long>(h.entry_count), static_cast<unsigned long long>(h.textures),
           static_cast<unsigned long long>(h.file_size), h.sources, (covered & 2u) ? " DXT1" : "", (covered & 8u) ? " DXT3" : "",
           (covered & 32u) ? " DXT5" : "");
    printf("levels by source: DXT1 %llu, DXT3 %llu, DXT5 %llu, other %llu; by encoding: ETC2 RGB %llu, ETC2 RGBA %llu, ASTC 4x4 %llu; "
           "blocks %llu bytes\n", by_source[1], by_source[3], by_source[5],
           static_cast<unsigned long long>(entries.size()) - by_source[1] - by_source[3] - by_source[5], by_encoding[1], by_encoding[2],
           by_encoding[3], bytes);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s scan|convert|remove|index GAME|CACHE_FILE [etc2|astc] [THREADS] [dxt1,dxt3,dxt5]\n", argv[0]);
        return 2;
    }
    const char *command = argv[1], *game = argv[2];
    if (!strcmp(command, "index")) return index_report(game);
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
        printf("sample texture resources %lld (%.1f MB); converted format %d, %lld textures, %.1f MB, sources%s%s%s%s\n",
               static_cast<long long>(s.textures), s.bytes / 1e6, s.converted_format,
               static_cast<long long>(s.converted_textures), s.converted_bytes / 1e6,
               (s.converted_sources & 2u) ? " DXT1" : "", (s.converted_sources & 8u) ? " DXT3" : "",
               (s.converted_sources & 32u) ? " DXT5" : "", s.stale ? " (stale)" : "");
        return 0;
    }
    if (!strcmp(command, "remove")) return s2texconv_remove(game) ? 2 : 0;
    if (strcmp(command, "convert") || argc < 4) {
        fprintf(stderr, "unknown command\n");
        return 2;
    }
    int format = !strcmp(argv[3], "astc") ? 2 : 1;
    int threads = argc > 4 ? atoi(argv[4]) : 0;
    unsigned sources = 0;      /* S2TC_SOURCE_BIT (s2tc.h): DXT1 2, DXT3 8, DXT5 32 */
    if (argc > 5) {
        if (strstr(argv[5], "dxt1")) sources |= 2u;
        if (strstr(argv[5], "dxt3")) sources |= 8u;
        if (strstr(argv[5], "dxt5")) sources |= 32u;
        if (!sources) { fprintf(stderr, "unknown source formats %s\n", argv[5]); return 2; }
    }
    int result = 0;
    std::atomic<bool> finished{false};
    auto started = std::chrono::steady_clock::now();
    std::thread worker([&]() { result = s2texconv_convert_sources(game, game, format, threads, index, archive_fd, sources); finished = true; });
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
