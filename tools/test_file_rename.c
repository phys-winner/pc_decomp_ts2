/* The game's two-stage DBPF compaction keeps handles open across renames. */
#include "../android/host/files.c"
#include <assert.h>

static uint32_t stack, buffer, output, image;
uint32_t __wrap_void_module_exe_base(void) { return image; }
static void args(uint32_t h, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    g_esp = stack; ARG(0) = h; ARG(1) = a; ARG(2) = b; ARG(3) = c; ARG(4) = d;
}
static void expect(uint32_t h, char value)
{
    assert(file_seek(file_get(h), 0, SEEK_SET) == 0);
    args(h, buffer, 32, output, 0);
    assert(ReadFile_impl() && MEM32(output) == 32);
    for (unsigned i = 0; i < 32; ++i) assert(MEM8(buffer + i) == (uint8_t)value);
}
static void put(uint32_t h, char value)
{
    assert(file_seek(file_get(h), 0, SEEK_SET) == 0);
    memset(GPTR(buffer), value, 32);
    args(h, buffer, 32, output, 0);
    assert(WriteFile_impl() && MEM32(output) == 32);
}
int main(void)
{
    setbuf(stdout, NULL);
    char root[] = "/data/local/tmp/sims2/rename-game-XXXXXX";
    assert(mkdtemp(root));
    char game[1024], profile[1024];
    snprintf(game, sizeof game, "%s/game", root);
    snprintf(profile, sizeof profile, "%s/profile", root);
    assert(void_guest_reserve());
    image = void_vm_alloc(0x400000, 0x2000, 0x3000u, 4u); assert(image == 0x400000);
    MEM32(image + 0x3C) = 0x80; MEM32(image + 0x80 + 0x28) = 0x1000; MEM32(image + 0x80 + 0x50) = 0x2000;
    void_threads_init(); void_dispatch_init(); void_machine_enter();
    assert(void_files_init(game, profile));
    stack = void_galloc(16384) + 8192; buffer = void_galloc(65536); output = void_galloc(32);
    const char *name = "C:\\Users\\Player\\Documents\\N002.package";
    const char *old = "C:\\Users\\Player\\Documents\\segment-old.tmp";
    const char *compact = "C:\\Users\\Player\\Documents\\segment-new.tmp";
    uint32_t a = create_file(name, GENERIC_READ_ | GENERIC_WRITE_, CREATE_ALWAYS_, 0);
    assert(a != WIN_INVALID_HANDLE); put(a, 'A'); expect(a, 'A');
    uint32_t lazy = create_file(name, GENERIC_READ_, OPEN_EXISTING_, 0);
    assert(lazy != WIN_INVALID_HANDLE && file_get(lazy)->lazy);
    uint32_t b = create_file(compact, GENERIC_READ_ | GENERIC_WRITE_, CREATE_ALWAYS_, 0);
    assert(b != WIN_INVALID_HANDLE); put(b, 'B');
    assert(move_file_ex(name, old, 2));
    assert(move_file_ex(compact, name, 2));
    uint32_t reader = create_file(name, GENERIC_READ_, OPEN_EXISTING_, 0);
    assert(reader != WIN_INVALID_HANDLE);
    expect(reader, 'B');
    expect(a, 'A');
    expect(lazy, 'A');
    put(b, 'C');
    expect(reader, 'C');
    expect(a, 'A');
    assert(file_get(a)->path_key == void_readcache_key(file_get(a)->host_path));
    assert(file_get(b)->path_key == file_get(reader)->path_key);
    uint64_t key = file_get(b)->path_key;
    assert(!move_file(name, old));
    assert(file_get(b)->path_key == key);
    expect(b, 'C'); expect(reader, 'C'); expect(a, 'A');
    void_handle_close(a); void_handle_close(b); void_handle_close(lazy); void_handle_close(reader);
    puts("PASS: DBPF compaction keeps old and new handles isolated; lazy handles and dirty writes follow renamed paths");
}
