/* Run by check_texconv_zip.py on Android against the production converter. */
#include "../android/texconv/s2texconv.cpp"
#include <cassert>
#include <set>

static bool watching, stop_on_open, stop_on_cache;
static std::set<std::string> opened;
extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...)
{
    if (watching && ends_with_ci(path, ".package")) {
        opened.insert(path);
        if (stop_on_open) s2texconv_cancel();
    }
    if (stop_on_cache && ends_with_ci(path, ".tmp")) s2texconv_cancel();
    int mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, mode);
}

static std::vector<uint8_t> bytes(const std::string &path)
{
    Fd f(::open(path.c_str(), O_RDONLY));
    struct stat st;
    assert(f.fd >= 0 && !fstat(f.fd, &st));
    std::vector<uint8_t> data(st.st_size);
    assert(read_exact(f.fd, data.data(), data.size(), 0));
    return data;
}

int main(int argc, char **argv)
{
    setbuf(stdout, nullptr);
    assert(argc == 2);
    std::string root = argv[1], plain = join(root, "plain"), side = join(root, "side");
    std::string archive = join(root, "archive.bin"), index = join(root, "archive.zmi");
    auto original = bytes(archive);
    /* A sparse archive tests unaligned positional reads beyond 4 GiB. */
    uint64_t base = (1ull << 32) + 103;
    Fd writable(::open(archive.c_str(), O_RDWR));
    assert(writable.fd >= 0 && !ftruncate(writable.fd, base + original.size()));
    assert(write_exact(writable.fd, original.data(), original.size(), base));
    auto records = bytes(index);
    uint64_t size = base + original.size();
    memcpy(records.data() + 8, &size, 8);
    for (size_t at = 48; at < records.size();) {
        uint64_t offset;
        memcpy(&offset, records.data() + at, 8);
        offset += base;
        memcpy(records.data() + at, &offset, 8);
        uint16_t length;
        memcpy(&length, records.data() + at + 32, 2);
        at += 34 + length;
    }
    Fd idx(::open(index.c_str(), O_WRONLY | O_TRUNC));
    assert(idx.fd >= 0 && write_exact(idx.fd, records.data(), records.size(), 0));
    Fd fd(::open(archive.c_str(), O_RDONLY));
    assert(fd.fd >= 0);

    s2texconv_status status;
    watching = true;
    assert(!s2texconv_scan_zip(side.c_str(), &status, index.c_str(), fd.fd));
    watching = false;
    assert(status.textures == 2 && opened.size() == 1);
    assert(*opened.begin() == join(side, probe_package));
    puts("PASS: launcher probe opens only Objects06.package");

    {
        Source source(side.c_str(), index.c_str(), fd.fd);
        PackageFile f(source, join(side, probe_package));
        uint8_t b[4];
        assert(f.file.fd < 0 && f.entry && f.entry->offset > (1ull << 32));
        assert(f.valid && f.read(b, 4, 0) && !memcmp(b, "DBPF", 4));
        assert(!f.read(b, 1, f.size) && !f.read(b, 4, UINT64_MAX));
        std::vector<std::thread> readers;
        for (int i = 0; i < 4; ++i) readers.emplace_back([&]() {
            for (int n = 0; n < 1000; ++n) {
                uint8_t h[4];
                assert(f.read(h, 4, 0) && !memcmp(h, "DBPF", 4));
            }
        });
        for (auto &t : readers) t.join();
    }
    puts("PASS: bounded concurrent reads at unaligned offsets beyond 4 GiB");

    for (int format : {S2TC_FORMAT_ETC2, S2TC_FORMAT_ASTC}) {
        assert(!s2texconv_convert(plain.c_str(), format, 2));
        assert(!s2texconv_convert_zip(side.c_str(), format, 2, index.c_str(), fd.fd));
        auto a = bytes(cache_path(plain)), b = bytes(cache_path(side));
        const auto *ha = reinterpret_cast<const s2tc_header *>(a.data());
        const auto *hb = reinterpret_cast<const s2tc_header *>(b.data());
        assert(ha->entry_count == 3 && hb->entry_count == ha->entry_count);
        assert(ha->textures == 3 && hb->textures == ha->textures);
        const auto *ia = reinterpret_cast<const s2tc_entry *>(a.data() + ha->index_offset);
        const auto *ib = reinterpret_cast<const s2tc_entry *>(b.data() + hb->index_offset);
        for (size_t i = 0; i < ha->entry_count; ++i) {
            assert(ia[i].key == ib[i].key && ia[i].bytes == ib[i].bytes);
            assert(!memcmp(a.data() + ia[i].offset, b.data() + ib[i].offset, ia[i].bytes));
        }
        assert(!s2texconv_scan_zip(side.c_str(), &status, index.c_str(), fd.fd));
        assert(status.converted_format == format && status.converted_textures == 3);
        printf("PASS: format %d ZIP and folder caches match (inline TXTR and referenced LIFO)\n", format);
    }
    /* Mixed ZIPs and writable overrides use the launcher's existing sidecar. */
    for (const char *dir : {"Base", "Base/TSData", "Base/TSData/Res", "Base/TSData/Res/Sims3D"})
        assert(!mkdir(join(side, dir).c_str(), 0775) || errno == EEXIST);
    auto probe = bytes(join(plain, probe_package));
    Fd override_file(::open(join(side, probe_package).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0664));
    assert(override_file.fd >= 0 && write_exact(override_file.fd, probe.data(), probe.size(), 0));
    uint32_t flags = ZM_SIDECAR;
    memcpy(records.data() + 48 + 20, &flags, 4);
    assert(write_exact(idx.fd, records.data(), records.size(), 0));
    assert(!s2texconv_convert_zip(side.c_str(), 1, 2, index.c_str(), fd.fd));
    assert(!s2texconv_scan_zip(side.c_str(), &status, index.c_str(), fd.fd));
    assert(status.converted_format == 1 && status.converted_textures == 3);
    puts("PASS: mixed sidecar and stored ZIP packages");
    auto previous = bytes(cache_path(side));
    watching = stop_on_open = true;
    assert(s2texconv_convert_zip(side.c_str(), 1, 2, index.c_str(), fd.fd) == 1);
    watching = stop_on_open = false;
    assert(bytes(cache_path(side)) == previous);
    stop_on_cache = true;
    assert(s2texconv_convert_zip(side.c_str(), 1, 2, index.c_str(), fd.fd) == 1);
    stop_on_cache = false;
    assert(access((cache_path(side) + ".tmp").c_str(), F_OK));
    assert(bytes(cache_path(side)) == previous);
    assert(s2texconv_scan_zip(side.c_str(), &status, "/no-such-index", fd.fd) == -1);
    assert(bytes(cache_path(side)) == previous);
    assert(!s2texconv_remove(side.c_str()));
    assert(access(cache_path(side).c_str(), F_OK));
    std::vector<uint8_t> after(original.size());
    assert(read_exact(fd.fd, after.data(), after.size(), base) && after == original);
    puts("PASS: cancellation/failure preserve old cache; undo leaves ZIP data unchanged");
}
