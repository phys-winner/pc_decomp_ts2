#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>
using HRESULT = int32_t;
constexpr uint32_t D3DLOCK_READONLY = 16, D3DLOCK_DISCARD = 8192, D3DLOCK_NOOVERWRITE = 4096;
constexpr uint32_t D3DFMT_INDEX32 = 102, D3DPT_POINTLIST = 1, D3D_OK = 0, D3DERR_INVALIDCALL = 0x8876086c;
#define FAILED(hr) ((hr) < 0)
#define SUCCEEDED(hr) ((hr) >= 0)
enum class Kind { IDirect3DVertexBuffer9, IDirect3DIndexBuffer9, IDirect3DDevice9 };
struct Context { Kind kind; void *self; unsigned slot; const uint32_t *a; };
struct Desc { uint32_t Size, Format; };
using D3DVERTEXBUFFER_DESC = Desc;
using D3DINDEXBUFFER_DESC = Desc;
struct Buffer {
  uint32_t data[16]{};
  unsigned locks = 0, unlocks = 0;
  bool fail_lock = false, fail_unlock = false, null_data = false;
  HRESULT GetDesc(Desc *out) { *out = {sizeof data, 0}; return 0; }
  HRESULT Lock(uint32_t start, uint32_t count, void **out, uint32_t flags) {
    assert(start == 0 && count == sizeof data && flags == D3DLOCK_READONLY);
    ++locks;
    if (fail_lock) return HRESULT(D3DERR_INVALIDCALL);
    *out = null_data ? nullptr : data;
    return 0;
  }
  HRESULT Unlock() { ++unlocks; return fail_unlock ? HRESULT(D3DERR_INVALIDCALL) : 0; }
};
using IDirect3DVertexBuffer9 = Buffer;
using IDirect3DIndexBuffer9 = Buffer;
struct IDirect3DVertexDeclaration9 {};
static Buffer input, output, fresh_output;
static uint32_t heap[8192], next_guest = 64;
static uint32_t guest_alloc(uint32_t size) { uint32_t at = next_guest; next_guest += (size + 3) & ~3u; assert(next_guest < sizeof heap); return at; }
static void guest_free(uint32_t) {}
static void *unwrap_guest(uint32_t va) { return va == 100 ? &output : va == 104 ? &fresh_output : nullptr; }
static const struct { uint32_t (*guest_alloc)(uint32_t); void (*guest_free)(uint32_t); void *(*unwrap_guest)(uint32_t); }
  host_storage = {guest_alloc, guest_free, unwrap_guest}, *host = &host_storage;
template<class T> T *memory(uint32_t va, size_t count = 1, bool = false) {
  assert(va && uint64_t(va) + count * sizeof(T) <= sizeof heap);
  return reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(heap) + va);
}
struct IDirect3DDevice9 {
  bool fail = false;
  HRESULT ProcessVertices(uint32_t first, uint32_t dest, uint32_t count, Buffer *out, IDirect3DVertexDeclaration9*, uint32_t) {
    if (fail) return HRESULT(D3DERR_INVALIDCALL);
    for (uint32_t i = 0; i < count; ++i) out->data[dest + i] = input.data[first + i] * 2;
    return 0;
  }
};
#include "dxvk_shadow_types.h"
static std::unordered_map<void*, Shadow> shadows;
#include "dxvk_shadow_helpers.h"
static void shadow_upload(void *buffer, Shadow &s, uint32_t lo, uint32_t hi) {
  assert(lo == 0 && hi == s.size);
  if (!s.dirty_count) return;
  memcpy(static_cast<Buffer*>(buffer)->data, memory<uint8_t>(s.guest, s.size), s.size);
  for (auto &v : s.dirty) v = 0;
  s.dirty_count = 0;
}
static void upload_for_draw(unsigned slot, const uint32_t *args) {
  assert(slot == 81 && args[0] == D3DPT_POINTLIST && args[1] == 1 && args[2] == 2);
  Shadow &s = shadows[&input];
  shadow_upload(&input, s, 0, s.size);
}
#include "dxvk_shadow_operations.h"
static uint32_t lock(Buffer &buffer, uint32_t flags, uint32_t result_va) {
  uint32_t args[] = {0, sizeof buffer.data, result_va, flags};
  Context c{Kind::IDirect3DVertexBuffer9, &buffer, 11, args};
  return buffer_lock(c, false);
}
int main() {
  uint32_t result = guest_alloc(4);
  assert(lock(input, 0, result) == 0);
  uint32_t *in = memory<uint32_t>(*memory<uint32_t>(result), 16);
  in[1] = 6; in[2] = 8;
  assert(lock(output, 0, result) == 0);
  uint32_t *out = memory<uint32_t>(*memory<uint32_t>(result), 16);
  for (unsigned i = 0; i < 16; ++i) out[i] = 7;
  uint32_t args[] = {1, 2, 2, 100, 0, 0};
  IDirect3DDevice9 device;
  Context process{Kind::IDirect3DDevice9, &device, 85, args};
  assert(process_vertices(process) == 0 && shadows[&output].native_written);
  assert(output.data[0] == 7 && output.data[2] == 12 && output.data[3] == 16 && output.data[15] == 7);
  assert(out[2] == 7 && output.locks == 0); /* Download stays lazy until the guest locks. */
  assert(lock(output, D3DLOCK_READONLY, result) == 0);
  assert(out[2] == 12 && out[3] == 16 && out[0] == 7 && out[15] == 7);
  assert(lock(output, D3DLOCK_READONLY, result) == 0 && output.locks == 1);
  assert(process_vertices(process) == 0);
  output.fail_lock = true;
  assert(lock(output, D3DLOCK_READONLY, result) == D3DERR_INVALIDCALL && *memory<uint32_t>(result) == 0);
  assert(shadows[&output].native_written);
  output.fail_lock = false; output.fail_unlock = true;
  assert(lock(output, D3DLOCK_READONLY, result) == D3DERR_INVALIDCALL && shadows[&output].native_written);
  output.fail_unlock = false; output.null_data = true;
  unsigned unlocked = output.unlocks;
  try { lock(output, D3DLOCK_READONLY, result); assert(false); } catch (const std::runtime_error&) {}
  assert(output.unlocks == unlocked + 1 && shadows[&output].native_written);
  output.null_data = false;
  assert(lock(output, D3DLOCK_READONLY, result) == 0);
  assert(process_vertices(process) == 0);
  unsigned reads = output.locks;
  assert(lock(output, D3DLOCK_DISCARD, result) == 0 && output.locks == reads);
  assert(!shadows[&output].native_written && shadows[&output].discard);
  device.fail = true;
  assert(process_vertices(process) == D3DERR_INVALIDCALL && !shadows[&output].native_written);
  device.fail = false;
  args[3] = 104;
  assert(process_vertices(process) == 0 && !shadows[&fresh_output].guest);
  assert(lock(fresh_output, D3DLOCK_READONLY, result) == 0);
  assert(memory<uint32_t>(*memory<uint32_t>(result), 16)[2] == 12);
  args[3] = 0;
  assert(process_vertices(process) == D3DERR_INVALIDCALL);
  puts("PASS DXVK ProcessVertices: input/destination upload, lazy readback, first lock, untouched bytes, DISCARD, native failures and null data");
}
