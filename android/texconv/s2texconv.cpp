/* The Sims 2 texture converter (s2texconv.h). Portable C++17. */
#include "s2texconv.h"
#include "s2tc.h"
#include "../launcher/native/zipmount.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../reference/astcenc/Source/astcenc.h"
#include "../../reference/etcpak/ProcessRGB.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/* ------------------------------------------------------------- progress */

std::mutex progress_lock;
s2texconv_progress progress_state;
std::atomic<bool> cancelled{false};
std::atomic<int64_t> done_packages{0}, done_bytes{0}, done_levels{0};

void set_state(int state)
{
    std::lock_guard<std::mutex> lock(progress_lock);
    progress_state.state = state;
}

int fail(const char *format, ...)
{
    std::lock_guard<std::mutex> lock(progress_lock);
    va_list args;
    va_start(args, format);
    vsnprintf(progress_state.message, sizeof progress_state.message, format, args);
    va_end(args);
    progress_state.state = S2TEXCONV_FAILED;
    return -1;
}

void begin(int state)
{
    std::lock_guard<std::mutex> lock(progress_lock);
    progress_state = s2texconv_progress{};
    progress_state.state = state;
    done_packages = 0;
    done_bytes = 0;
    done_levels = 0;
}

void set_totals(int64_t packages, int64_t bytes)
{
    std::lock_guard<std::mutex> lock(progress_lock);
    progress_state.total = packages;
    progress_state.bytes_total = bytes;
}

/* ----------------------------------------------------------------- files */

std::string join(const std::string &dir, const std::string &name) { return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name; }

bool ends_with_ci(const std::string &text, const char *suffix)
{
    size_t n = strlen(suffix);
    if (text.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (tolower(static_cast<unsigned char>(text[text.size() - n + i])) != tolower(static_cast<unsigned char>(suffix[i]))) return false;
    return true;
}

bool read_exact(int fd, void *buffer, size_t size, uint64_t offset)
{
    uint8_t *p = static_cast<uint8_t *>(buffer);
    while (size) {
        ssize_t n = pread(fd, p, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

bool write_exact(int fd, const void *buffer, size_t size, uint64_t offset)
{
    const uint8_t *p = static_cast<const uint8_t *>(buffer);
    while (size) {
        ssize_t n = pwrite(fd, p, size, static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
        offset += static_cast<uint64_t>(n);
    }
    return true;
}

struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    ~Fd() { if (fd >= 0) ::close(fd); }
};

/* One bounded package view; archive descriptors are borrowed and shared via pread. */
struct Source {
    std::string game;
    zm_mount *mount;
    Source(const char *dir, const char *index, int fd) : game(dir), mount(index ? zm_open(fd, index, dir) : nullptr) {}
    ~Source() { zm_close(mount); }
    const zm_entry *entry(const std::string &path) const {
        return mount ? zm_find(mount, path.c_str() + game.size() + 1) : nullptr;
    }
};

struct PackageFile {
    const Source &source;
    const zm_entry *entry;
    Fd file;
    uint64_t size = 0;
    bool valid = false;
    PackageFile(const Source &s, const std::string &path) : source(s), entry(s.entry(path)) {
        file.fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (file.fd >= 0) {
            valid = !fstat(file.fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 0;
            if (valid) size = st.st_size;
        } else if (entry && !(entry->flags & ZM_SIDECAR)) {
            size = entry->size;
            valid = true;
        }
    }
    bool read(void *buffer, size_t bytes, uint64_t at) const {
        if (!valid || at > size || bytes > size - at) return false;
        if (file.fd >= 0) return read_exact(file.fd, buffer, bytes, at);
        auto *p = static_cast<uint8_t *>(buffer);
        while (bytes) {
            long long n = zm_pread(source.mount, entry, p, bytes, at);
            if (n <= 0) return false;
            p += n; bytes -= n; at += n;
        }
        return true;
    }
};

/* shortcut: sample this texture bank for availability and freshness; conversion alone scans all packages. */
const char *probe_package = "Base/TSData/Res/Sims3D/Objects06.package";

/* The game's package files: every *.package below a pack's TSData folder
 * (<game>/<pack>/TSData/...), sorted. */
void collect_packages(const std::string &dir, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (!cancelled) {
        dirent *e = readdir(d);
        if (!e) break;
        if (e->d_name[0] == '.') continue;
        std::string path = join(dir, e->d_name);
        struct stat st;
        bool is_dir = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && !stat(path.c_str(), &st) && S_ISDIR(st.st_mode));
        if (is_dir) subdirs.push_back(path);
        else if (ends_with_ci(path, ".package")) out.push_back(path);
    }
    closedir(d);
    for (const std::string &s : subdirs) collect_packages(s, out);
}

std::vector<std::string> game_packages(const Source &source)
{
    const std::string &game = source.game;
    std::vector<std::string> out;
    DIR *d = opendir(game.c_str());
    std::vector<std::string> packs;
    while (d && !cancelled) {
        dirent *e = readdir(d);
        if (!e) break;
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, S2TC_DIR)) continue;
        packs.push_back(e->d_name);
    }
    if (d) closedir(d);
    for (const std::string &pack : packs) {
        std::string pack_dir = join(game, pack);
        DIR *p = opendir(pack_dir.c_str());
        if (!p) continue;
        std::string tsdata;
        while (dirent *e = readdir(p))
            if (!strcasecmp(e->d_name, "TSData")) tsdata = join(pack_dir, e->d_name);
        closedir(p);
        if (!tsdata.empty()) collect_packages(tsdata, out);
    }
    if (source.mount) {
        struct Visit { const Source &source; std::vector<std::string> &out; } visit{source, out};
        zm_list(source.mount, "", [](const zm_entry *entry, void *user) {
            if (cancelled) return 1;
            auto &v = *static_cast<Visit *>(user);
            std::string path(entry->path, entry->length);
            size_t slash = path.find('/');
            if (slash == std::string::npos || strncasecmp(path.c_str() + slash + 1, "TSData/", 7)
                    || !ends_with_ci(path, ".package")) return 0;
            v.out.push_back(join(v.source.game, path));
            return 0;
        }, &visit);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint64_t packages_signature(const Source &source)
{
    PackageFile file(source, join(source.game, probe_package));
    uint64_t crc = file.file.fd < 0 && file.entry ? file.entry->crc : 0;
    return s2tc_xxh64(probe_package, strlen(probe_package), file.size ^ (crc << 32));
}

/* --------------------------------------------------------------- QFS */

bool qfs_decompress(const uint8_t *in, size_t n, std::vector<uint8_t> &out)
{
    if (n < 9 || in[4] != 0x10 || in[5] != 0xFB) return false;
    size_t size = (static_cast<size_t>(in[6]) << 16) | (static_cast<size_t>(in[7]) << 8) | in[8];
    out.resize(size);
    size_t i = 9, o = 0;
    while (i < n) {
        uint32_t c0 = in[i], plain, count, offset;
        if (c0 < 0x80) {
            if (i + 2 > n) return false;
            uint32_t c1 = in[i + 1];
            plain = c0 & 3; count = ((c0 & 0x1C) >> 2) + 3; offset = ((c0 & 0x60) << 3) + c1 + 1;
            i += 2;
        } else if (c0 < 0xC0) {
            if (i + 3 > n) return false;
            uint32_t c1 = in[i + 1], c2 = in[i + 2];
            plain = (c1 & 0xC0) >> 6; count = (c0 & 0x3F) + 4; offset = ((c1 & 0x3F) << 8) + c2 + 1;
            i += 3;
        } else if (c0 < 0xE0) {
            if (i + 4 > n) return false;
            uint32_t c1 = in[i + 1], c2 = in[i + 2], c3 = in[i + 3];
            plain = c0 & 3; count = ((c0 & 0x0C) << 6) + c3 + 5; offset = ((c0 & 0x10) << 12) + (c1 << 8) + c2 + 1;
            i += 4;
        } else if (c0 < 0xFC) {
            plain = ((c0 & 0x1F) << 2) + 4; count = 0; offset = 0;
            i += 1;
        } else {
            plain = c0 & 3; count = 0; offset = 0;
            i += 1;
        }
        if (i + plain > n || o + plain > size) return false;
        memcpy(&out[o], in + i, plain);
        i += plain;
        o += plain;
        if (count) {
            if (offset > o || o + count > size) return false;
            for (uint32_t k = 0; k < count; ++k, ++o) out[o] = out[o - offset];
        }
        if (c0 >= 0xFC) break;
    }
    return o == size;
}

/* --------------------------------------------------------------- DBPF */

constexpr uint32_t TYPE_TXTR = 0x1C4A276Cu, TYPE_LIFO = 0xED534136u, TYPE_DIR = 0xE86B1EEFu;

uint32_t le32(const uint8_t *p) { return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)); }

struct Entry {
    uint32_t type, group, instance, resource, offset, size;
    bool compressed;
};

struct PackageIndex {
    std::string path;
    std::vector<Entry> textures;      /* TXTR and LIFO entries, by offset */
    int64_t bytes = 0;
};

/* The package's TXTR and LIFO entries, compressed ones marked from its directory. */
bool read_index(const Source &source, const std::string &path, PackageIndex &out, std::string &why)
{
    PackageFile f(source, path);
    if (!f.valid) { why = "open"; return false; }
    uint8_t header[96];
    if (!f.read(header, sizeof header, 0) || memcmp(header, "DBPF", 4)) { why = "not a DBPF package"; return false; }
    uint32_t count = le32(header + 36), offset = le32(header + 40), size = le32(header + 44);
    if (!count) return true;
    if (size / count < 20 || count > 10000000u || offset > f.size || size > f.size - offset) { why = "index"; return false; }
    uint32_t step = size / count;
    std::vector<uint8_t> index(size);
    if (!f.read(index.data(), size, offset)) { why = "index read"; return false; }
    std::vector<Entry> all;
    all.reserve(count);
    const Entry *dir = nullptr;
    for (uint32_t k = 0; k < count; ++k) {
        const uint8_t *e = &index[static_cast<size_t>(k) * step];
        Entry x{};
        x.type = le32(e); x.group = le32(e + 4); x.instance = le32(e + 8);
        if (step >= 24) { x.resource = le32(e + 12); x.offset = le32(e + 16); x.size = le32(e + 20); }
        else { x.offset = le32(e + 12); x.size = le32(e + 16); }
        if (x.offset > f.size || x.size > f.size - x.offset) { why = "resource bounds"; return false; }
        all.push_back(x);
    }
    for (const Entry &x : all) if (x.type == TYPE_DIR) dir = &x;
    std::unordered_set<uint64_t> packed;
    auto pack_key = [](uint32_t t, uint32_t g, uint32_t i, uint32_t r) {
        return s2tc_xxh64(&t, 4, (static_cast<uint64_t>(g) << 32 | i) ^ (static_cast<uint64_t>(r) * 0x9E3779B97F4A7C15ull));
    };
    if (dir && dir->size < (64u << 20)) {
        std::vector<uint8_t> raw(dir->size);
        if (f.read(raw.data(), raw.size(), dir->offset)) {
            uint32_t dstep = step >= 24 ? 20 : 16;
            for (size_t at = 0; at + dstep <= raw.size(); at += dstep) {
                const uint8_t *d = &raw[at];
                uint32_t r = dstep == 20 ? le32(d + 12) : 0;
                packed.insert(pack_key(le32(d), le32(d + 4), le32(d + 8), r));
            }
        }
    }
    for (Entry &x : all) {
        if (x.type != TYPE_TXTR && x.type != TYPE_LIFO) continue;
        x.compressed = packed.count(pack_key(x.type, x.group, x.instance, x.resource)) != 0;
        out.textures.push_back(x);
        out.bytes += x.size;
    }
    std::sort(out.textures.begin(), out.textures.end(), [](const Entry &a, const Entry &b) { return a.offset < b.offset; });
    out.path = path;
    return true;
}

bool read_resource(const PackageFile &file, const Entry &e, std::vector<uint8_t> &raw, std::vector<uint8_t> &data)
{
    raw.resize(e.size);
    if (!e.size || !file.read(raw.data(), e.size, e.offset)) return false;
    if (e.compressed && e.size >= 9 && raw[4] == 0x10 && raw[5] == 0xFB) return qfs_decompress(raw.data(), raw.size(), data);
    data.swap(raw);
    return true;
}

/* --------------------------------------------------------------- RCOL */

struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    Reader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
    bool has(size_t n) { if (!ok || static_cast<size_t>(end - p) < n) ok = false; return ok; }
    uint32_t u8() { if (!has(1)) return 0; return *p++; }
    uint32_t u32() { if (!has(4)) return 0; uint32_t v = le32(p); p += 4; return v; }
    std::string str()
    {
        uint32_t n = 0, shift = 0;
        for (;;) {
            uint32_t b = u8();
            if (!ok) return std::string();
            n |= (b & 0x7F) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
            if (shift > 28) { ok = false; return std::string(); }
        }
        if (!has(n)) return std::string();
        std::string s(reinterpret_cast<const char *>(p), n);
        p += n;
        return s;
    }
    const uint8_t *take(size_t n) { if (!has(n)) return nullptr; const uint8_t *r = p; p += n; return r; }
};

/* Past the RCOL header (links, block ids) to the first block's name. */
bool rcol_open(Reader &r)
{
    uint32_t first = r.u32();
    uint32_t links = first == 0xFFFF0001u ? r.u32() : first;
    uint32_t link_size = first == 0xFFFF0001u ? 16 : 12;
    if (!r.ok || links > 4096 || !r.take(static_cast<size_t>(links) * link_size)) return false;
    uint32_t blocks = r.u32();
    if (!r.ok || blocks == 0 || blocks > 64 || !r.take(static_cast<size_t>(blocks) * 4)) return false;
    return r.ok;
}

std::string lower(std::string s)
{
    for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

int source_of(uint32_t txtr_format)
{
    switch (txtr_format) {
    case 4: return S2TC_SRC_DXT1;
    case 5: return S2TC_SRC_DXT3;
    case 8: return S2TC_SRC_DXT5;
    default: return 0;
    }
}

struct Level {
    int source;
    uint32_t width, height;
    const uint8_t *data;
    uint32_t bytes;
};

/* A TXTR's inline DXT levels, and its LIFO references (name -> source). */
bool parse_txtr(const std::vector<uint8_t> &data, std::vector<Level> &levels, std::vector<std::pair<std::string, int>> &lifos,
                int &source)
{
    Reader r(data.data(), data.size());
    if (!rcol_open(r)) return false;
    std::string block = r.str();
    r.u32();
    uint32_t version = r.u32();
    if (!r.ok || block != "cImageData" || version < 7 || version > 9) return false;
    r.str(); r.u32(); r.u32();          /* cSGResource, its id and version */
    r.str();                            /* the resource name */
    uint32_t width = r.u32(), height = r.u32(), format = r.u32(), mips = r.u32();
    r.u32();                            /* purpose */
    uint32_t outer = r.u32();
    r.u32();
    if (version == 9) r.str();
    if (!r.ok || !width || !height || width > 8192 || height > 8192 || outer > 64 || mips > 16) return false;
    source = source_of(format);
    for (uint32_t t = 0; t < outer && r.ok; ++t) {
        uint32_t inner = version == 7 ? mips : r.u32();
        if (!r.ok || inner > 16) return false;
        for (uint32_t j = 0; j < inner && r.ok; ++j) {
            uint32_t kind = r.u8();
            uint32_t level = inner - 1 - j;
            uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
            if (kind == 0) {
                uint32_t size = r.u32();
                const uint8_t *p = r.take(size);
                if (p && source && size == s2tc_dxt_bytes(source, w, h)) levels.push_back(Level{source, w, h, p, size});
            } else {
                std::string name = r.str();
                if (r.ok && source) lifos.emplace_back(lower(name), source);
            }
        }
    }
    return r.ok;
}

/* A LIFO's level: its name and pixels (the format comes from the TXTR naming it). */
bool parse_lifo(const std::vector<uint8_t> &data, std::string &name, uint32_t &w, uint32_t &h, const uint8_t *&pixels, uint32_t &bytes)
{
    Reader r(data.data(), data.size());
    if (!rcol_open(r)) return false;
    std::string block = r.str();
    r.u32();
    r.u32();
    if (!r.ok || block != "cLevelInfo") return false;
    r.str(); r.u32(); r.u32();
    name = lower(r.str());
    w = r.u32(); h = r.u32();
    r.u32();                            /* z level */
    bytes = r.u32();
    pixels = r.take(bytes);
    return r.ok && pixels && w && h && w <= 8192 && h <= 8192;
}

/* ------------------------------------------------------------ DXT decode */

void rgb565(uint32_t c, uint8_t *out)
{
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

/* As the renderer decodes them without a cache (android/host/d3d9_format.c). */
void color_block(const uint8_t *block, uint8_t out[16][4], bool four)
{
    uint32_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8), bits = le32(block + 4);
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (c0 > c1 || four) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k] + 1) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (int i = 0; i < 16; ++i) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

void alpha_block(const uint8_t *block, uint8_t out[16])
{
    uint32_t a0 = block[0], a1 = block[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
    uint8_t pal[8];
    pal[0] = static_cast<uint8_t>(a0);
    pal[1] = static_cast<uint8_t>(a1);
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i) pal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1 + 3) / 7);
    } else {
        for (uint32_t i = 1; i < 5; ++i) pal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1 + 2) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

void decode_dxt(int source, const uint8_t *src, uint32_t width, uint32_t height, uint8_t *rgba)
{
    uint32_t bw = (width + 3) / 4, bh = (height + 3) / 4, step = source == S2TC_SRC_DXT1 ? 8 : 16;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t *block = src + (static_cast<size_t>(by) * bw + bx) * step;
            uint8_t texels[16][4], alpha[16];
            if (source == S2TC_SRC_DXT1) {
                color_block(block, texels, false);
            } else if (source == S2TC_SRC_DXT3) {
                color_block(block + 8, texels, true);
                for (int i = 0; i < 16; ++i) texels[i][3] = static_cast<uint8_t>(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
            } else {
                color_block(block + 8, texels, true);
                alpha_block(block, alpha);
                for (int i = 0; i < 16; ++i) texels[i][3] = alpha[i];
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
                    memcpy(rgba + ((static_cast<size_t>(by) * 4 + y) * width + bx * 4 + x) * 4, texels[y * 4 + x], 4);
        }
}

/* -------------------------------------------------------------- encoders */

struct AstcContext {
    astcenc_context *context = nullptr;
    ~AstcContext() { if (context) astcenc_context_free(context); }
    astcenc_context *get()
    {
        if (context) return context;
        astcenc_config config;
        unsigned flags = ASTCENC_FLG_USE_DECODE_UNORM8 | ASTCENC_FLG_USE_ALPHA_WEIGHT;
        if (astcenc_config_init(ASTCENC_PRF_LDR, 4, 4, 1, ASTCENC_PRE_FAST, flags, &config) != ASTCENC_SUCCESS) return nullptr;
        if (astcenc_context_alloc(&config, 1, &context, nullptr) != ASTCENC_SUCCESS) context = nullptr;
        return context;
    }
};

thread_local AstcContext astc_context;

int encode_rgba(int encoding, const uint8_t *rgba, uint32_t w, uint32_t h, uint8_t *out)
{
    if (encoding == S2TC_ENC_ETC2_RGB || encoding == S2TC_ENC_ETC2_RGBA) {
        /* etcpak: whole 4x4 blocks of BGRA, rows of the padded width. */
        uint32_t pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
        std::vector<uint32_t> bgra(static_cast<size_t>(pw) * ph);
        for (uint32_t y = 0; y < ph; ++y)
            for (uint32_t x = 0; x < pw; ++x) {
                const uint8_t *p = &rgba[(static_cast<size_t>(std::min(y, h - 1)) * w + std::min(x, w - 1)) * 4];
                bgra[static_cast<size_t>(y) * pw + x] = static_cast<uint32_t>(p[2]) | (static_cast<uint32_t>(p[1]) << 8) |
                                                      (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            }
        uint32_t blocks = (pw / 4) * (ph / 4);
        if (encoding == S2TC_ENC_ETC2_RGB) CompressEtc2Rgb(bgra.data(), reinterpret_cast<uint64_t *>(out), blocks, pw, true);
        else CompressEtc2Rgba(bgra.data(), reinterpret_cast<uint64_t *>(out), blocks, pw, true);
        return 0;
    }
    if (encoding != S2TC_ENC_ASTC_4x4) return -1;
    astcenc_context *context = astc_context.get();
    if (!context) return -1;
    bool opaque = true;
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n && opaque; ++i) opaque = rgba[i * 4 + 3] == 255;
    astcenc_image image{};
    image.dim_x = w;
    image.dim_y = h;
    image.dim_z = 1;
    image.data_type = ASTCENC_TYPE_U8;
    void *slices[] = {const_cast<uint8_t *>(rgba)};
    image.data = slices;
    const astcenc_swizzle swizzle{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, opaque ? ASTCENC_SWZ_1 : ASTCENC_SWZ_A};
    astcenc_error status = astcenc_compress_image(context, &image, &swizzle, out, s2tc_encoded_bytes(S2TC_ENC_ASTC_4x4, w, h), 0);
    astcenc_compress_reset(context);
    return status == ASTCENC_SUCCESS ? 0 : -1;
}

/* ------------------------------------------------------------ the cache */

struct Cache {
    int fd = -1;
    int format = 0;
    std::mutex lock;
    uint64_t data_end = sizeof(s2tc_header);
    std::vector<s2tc_entry> index;
    std::unordered_set<uint64_t> keys;      /* converted or being converted */
    std::atomic<uint64_t> levels{0}, source_bytes{0}, textures{0};
    std::atomic<bool> failed{false};

    /* Claims a level for conversion: false when an equal one is in. */
    bool claim(uint64_t key)
    {
        std::lock_guard<std::mutex> l(lock);
        return keys.insert(key).second;
    }

    bool add(uint64_t key, const Level &level, int encoding, const uint8_t *blocks, uint32_t bytes)
    {
        uint64_t at;
        {
            std::lock_guard<std::mutex> l(lock);
            at = (data_end + 15) & ~15ull;
            data_end = at + bytes;
            s2tc_entry e{};
            e.key = key;
            e.offset = at;
            e.source_bytes = level.bytes;
            e.bytes = bytes;
            e.width = static_cast<uint16_t>(level.width);
            e.height = static_cast<uint16_t>(level.height);
            e.source = static_cast<uint8_t>(level.source);
            e.encoding = static_cast<uint8_t>(encoding);
            index.push_back(e);
        }
        levels++;
        source_bytes += level.bytes;
        if (!write_exact(fd, blocks, bytes, at)) { failed = true; return false; }
        return true;
    }
};

/* Decodes and encodes one level into the cache (each distinct level once). */
bool convert_level(Cache &cache, const Level &level, std::vector<uint8_t> &rgba, std::vector<uint8_t> &out)
{
    uint64_t key = s2tc_key(level.data, level.bytes, level.source);
    if (!cache.claim(key)) return true;
    rgba.resize(static_cast<size_t>(level.width) * level.height * 4);
    decode_dxt(level.source, level.data, level.width, level.height, rgba.data());
    int encoding = S2TC_ENC_ASTC_4x4;
    if (cache.format == S2TC_FORMAT_ETC2) {
        bool opaque = true;
        for (size_t i = 3; i < rgba.size() && opaque; i += 4) opaque = rgba[i] == 255;
        encoding = opaque ? S2TC_ENC_ETC2_RGB : S2TC_ENC_ETC2_RGBA;
    }
    uint32_t bytes = s2tc_encoded_bytes(encoding, level.width, level.height);
    out.resize(bytes);
    if (encode_rgba(encoding, rgba.data(), level.width, level.height, out.data())) return false;
    done_levels++;
    return cache.add(key, level, encoding, out.data(), bytes);
}

std::string cache_dir(const std::string &game) { return join(game, S2TC_DIR); }
std::string cache_path(const std::string &game) { return join(cache_dir(game), S2TC_FILE); }

bool read_header(const std::string &path, s2tc_header &h)
{
    Fd f(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (f.fd < 0 || !read_exact(f.fd, &h, sizeof h, 0)) return false;
    return h.magic == S2TC_MAGIC && h.version == S2TC_VERSION && (h.flags & 1);
}

int hardware_threads()
{
    unsigned n = std::thread::hardware_concurrency();
    return n ? static_cast<int>(n) : 4;
}

/* Workers over the packages: each takes the next one and handles its TXTR (pass 1) or LIFO (pass 2) entries. */
template <typename Work>
void for_each_package(const std::vector<PackageIndex> &packages, int threads, Work work)
{
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&]() {
            std::vector<uint8_t> raw, data, rgba, out;
            for (;;) {
                size_t k = next++;
                if (k >= packages.size() || cancelled) break;
                work(packages[k], raw, data, rgba, out);
            }
        });
    for (std::thread &t : pool) t.join();
}

int convert(const Source &source, int format, int threads)
{
    const std::string &game = source.game;
    begin(S2TEXCONV_SCANNING);
    cancelled = false;
    if (format != S2TC_FORMAT_ETC2 && format != S2TC_FORMAT_ASTC) return fail("Unknown texture format %d", format);
    std::vector<std::string> files = game_packages(source);
    if (cancelled) { set_state(S2TEXCONV_CANCELLED); return 1; }
    if (files.empty()) return fail("No package files under %s/*/TSData", game.c_str());
    std::vector<PackageIndex> packages;
    int64_t total_bytes = 0;
    for (const std::string &path : files) {
        if (cancelled) { set_state(S2TEXCONV_CANCELLED); return 1; }
        PackageIndex p;
        std::string why;
        if (!read_index(source, path, p, why)) continue;          /* not every .package is DBPF; the game skips them too */
        if (p.textures.empty()) continue;
        total_bytes += p.bytes;
        packages.push_back(std::move(p));
    }
    set_totals(static_cast<int64_t>(packages.size()) * 2, total_bytes);
    uint64_t signature = packages_signature(source);
    mkdir(cache_dir(game).c_str(), 0775);
    std::string final_path = cache_path(game), temporary = final_path + ".tmp";
    Cache cache;
    cache.format = format;
    cache.fd = ::open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (cache.fd < 0) return fail("Cannot create %s: %s", temporary.c_str(), strerror(errno));
    if (threads <= 0) threads = hardware_threads();
    set_state(S2TEXCONV_CONVERTING);

    /* Pass 1: TXTR resources; their inline (small) levels and the names of the LIFO levels. */
    std::mutex refs_lock;
    std::unordered_map<std::string, int> lifo_source;
    for_each_package(packages, threads, [&](const PackageIndex &p, std::vector<uint8_t> &raw, std::vector<uint8_t> &data,
                                            std::vector<uint8_t> &rgba, std::vector<uint8_t> &out) {
        PackageFile f(source, p.path);
        std::vector<std::pair<std::string, int>> refs;
        for (const Entry &e : p.textures) {
            if (cancelled || !f.valid) break;
            if (e.type != TYPE_TXTR) continue;
            done_bytes += e.size;
            if (!read_resource(f, e, raw, data)) continue;
            std::vector<Level> levels;
            int source = 0;
            size_t before = refs.size();
            if (!parse_txtr(data, levels, refs, source) || !source) continue;
            if (!levels.empty() || refs.size() > before) cache.textures++;
            for (const Level &level : levels) convert_level(cache, level, rgba, out);
        }
        std::lock_guard<std::mutex> l(refs_lock);
        for (auto &r : refs) {
            auto it = lifo_source.find(r.first);
            if (it == lifo_source.end()) lifo_source.emplace(r.first, r.second);
            else if (it->second != r.second) it->second = 0;      /* named with two formats: leave it */
        }
        done_packages++;
    });

    /* Pass 2: LIFO resources (the large levels), in the format their TXTR gave. */
    for_each_package(packages, threads, [&](const PackageIndex &p, std::vector<uint8_t> &raw, std::vector<uint8_t> &data,
                                            std::vector<uint8_t> &rgba, std::vector<uint8_t> &out) {
        PackageFile f(source, p.path);
        for (const Entry &e : p.textures) {
            if (cancelled || !f.valid) break;
            if (e.type != TYPE_LIFO) continue;
            done_bytes += e.size;
            if (!read_resource(f, e, raw, data)) continue;
            std::string name;
            uint32_t w = 0, h = 0, bytes = 0;
            const uint8_t *pixels = nullptr;
            if (!parse_lifo(data, name, w, h, pixels, bytes)) continue;
            auto it = lifo_source.find(name);
            if (it == lifo_source.end() || !it->second || bytes != s2tc_dxt_bytes(it->second, w, h)) continue;
            convert_level(cache, Level{it->second, w, h, pixels, bytes}, rgba, out);
        }
        done_packages++;
    });

    if (cancelled) {
        ::close(cache.fd);
        unlink(temporary.c_str());
        set_state(S2TEXCONV_CANCELLED);
        return 1;
    }
    if (cache.failed) {
        ::close(cache.fd);
        unlink(temporary.c_str());
        return fail("Writing %s failed (is the storage full?)", temporary.c_str());
    }
    set_state(S2TEXCONV_FINISHING);
    std::sort(cache.index.begin(), cache.index.end(), [](const s2tc_entry &a, const s2tc_entry &b) { return a.key < b.key; });
    uint64_t index_at = (cache.data_end + 15) & ~15ull;
    size_t index_bytes = cache.index.size() * sizeof(s2tc_entry);
    s2tc_header h{};
    h.magic = S2TC_MAGIC;
    h.version = S2TC_VERSION;
    h.format = static_cast<uint32_t>(format);
    h.flags = 1;
    h.entry_count = cache.index.size();
    h.index_offset = index_at;
    h.data_offset = sizeof(s2tc_header);
    h.file_size = index_at + index_bytes;
    h.textures = cache.textures;
    h.levels = cache.levels;
    h.source_bytes = cache.source_bytes;
    h.converted_bytes = cache.data_end - sizeof(s2tc_header);
    h.signature = signature;
    bool ok = (index_bytes == 0 || write_exact(cache.fd, cache.index.data(), index_bytes, index_at)) &&
              write_exact(cache.fd, &h, sizeof h, 0) && fsync(cache.fd) == 0;
    ok = ::close(cache.fd) == 0 && ok;
    if (!ok || rename(temporary.c_str(), final_path.c_str())) {
        int e = errno;
        unlink(temporary.c_str());
        return fail("Finishing %s failed: %s", final_path.c_str(), strerror(e));
    }
    set_state(S2TEXCONV_DONE);
    return 0;
}

int scan(const Source &source, s2texconv_status *out)
{
    const std::string &game = source.game;
    begin(S2TEXCONV_SCANNING);
    *out = s2texconv_status{};
    PackageIndex p;
    std::string why;
    if (!read_index(source, join(game, probe_package), p, why))
        return fail("Cannot read %s: %s", probe_package, why.c_str());
    for (const Entry &e : p.textures) out->textures += e.type == TYPE_TXTR;
    out->bytes = p.bytes;
    s2tc_header h;
    if (read_header(cache_path(game), h)) {
        if (h.signature == packages_signature(source)) {
            out->converted_format = static_cast<int>(h.format);
            out->converted_textures = static_cast<int64_t>(h.textures);
            out->converted_bytes = static_cast<int64_t>(h.file_size);
        } else {
            out->stale = 1;
        }
    }
    set_state(S2TEXCONV_IDLE);
    return 0;
}

int remove_conversion(const std::string &game)
{
    begin(S2TEXCONV_REMOVING);
    std::string path = cache_path(game);
    if (unlink(path.c_str()) && errno != ENOENT) return fail("Cannot remove %s: %s", path.c_str(), strerror(errno));
    unlink((path + ".tmp").c_str());
    rmdir(cache_dir(game).c_str());
    set_state(S2TEXCONV_DONE);
    return 0;
}

}  // namespace

extern "C" int s2texconv_scan(const char *game_dir, s2texconv_status *out)
{
    return s2texconv_scan_zip(game_dir, out, nullptr, -1);
}

extern "C" int s2texconv_convert(const char *game_dir, int format, int threads)
{
    return s2texconv_convert_zip(game_dir, format, threads, nullptr, -1);
}

extern "C" int s2texconv_scan_zip(const char *game_dir, s2texconv_status *out, const char *index, int fd)
{
    if (!game_dir || !out) return -1;
    Source source(game_dir, index, fd);
    if (index && !source.mount) return fail("Cannot open game ZIP index");
    return scan(source, out);
}

extern "C" int s2texconv_convert_zip(const char *game_dir, int format, int threads, const char *index, int fd)
{
    if (!game_dir) return -1;
    Source source(game_dir, index, fd);
    if (index && !source.mount) return fail("Cannot open game ZIP index");
    return convert(source, format, threads);
}

extern "C" int s2texconv_remove(const char *game_dir)
{
    return game_dir ? remove_conversion(game_dir) : -1;
}

extern "C" void s2texconv_cancel(void) { cancelled = true; }

extern "C" void s2texconv_get_progress(s2texconv_progress *out)
{
    std::lock_guard<std::mutex> lock(progress_lock);
    *out = progress_state;
    out->done = done_packages;
    out->bytes_done = done_bytes;
    out->levels = done_levels;
}

extern "C" int s2texconv_encode_rgba(int encoding, const uint8_t *rgba, uint32_t width, uint32_t height, uint8_t *out)
{
    return rgba && out && width && height ? encode_rgba(encoding, rgba, width, height, out) : -1;
}

extern "C" void s2texconv_decode_dxt(int source, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *rgba)
{
    decode_dxt(source, blocks, width, height, rgba);
}
