/* Standalone assert check of the native window lookup; compile with the NDK
 * clang for Android and run on the target. No game data is touched. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define VOID_HOST_H
static uint32_t memory[1u << 18];
#define GPTR(a) ((uint8_t *)memory + ((uint32_t)(a) & 0xFFFFFu))
#define MEM32(a) (*(uint32_t *)GPTR(a))
#define MEM8(a) (*(uint8_t *)GPTR(a))
static uint32_t g_eax, g_ecx, g_edx, g_esp, g_flag_k, g_flag_a, g_flag_b, g_flag_cf;
enum { FK_CMP = 2, FK_TEST = 3 };
#define FK_NARROW(k, shift) ((uint32_t)(k) | ((uint32_t)(shift) << 8))
static uint32_t void_call_guest_this(uint32_t, uint32_t, int, const uint32_t *);
static void void_log(const char *format, ...) { (void)format; }
void sub_A01BEFFF_lifted(void) { assert(0); }
void sub_A01BF191_lifted(void) { assert(0); }
void sub_A0845799_lifted(void) { assert(0); }
#include "../android/host/sims2_fast.c"

static uint32_t cursor = 0x30000;
static uint32_t custom_calls;

static uint32_t alloc(void)
{
    uint32_t p = cursor;
    cursor += 0x200;
    return p;
}

static void window(uint32_t p, uint32_t id)
{
    MEM32(p) = 0xA0000100u;
    MEM32(p + 0x10) = id;
    uint32_t list = alloc(), head = alloc();
    MEM32(p + 0x54) = list;
    MEM32(list) = head;
    MEM32(head) = MEM32(head + 4) = head;
}

static void prepend(uint32_t parent, uint32_t child)
{
    uint32_t head = MEM32(MEM32(parent + 0x54)), first = MEM32(head), node = alloc();
    MEM32(node) = first;
    MEM32(node + 4) = head;
    MEM32(node + 8) = child;
    MEM32(first + 4) = node;
    MEM32(head) = node;
    MEM32(child + 0x58) = parent;
}

static uint32_t lookup(uint32_t root, uint32_t id)
{
    g_esp = 0x80000;
    g_ecx = root;
    MEM32(g_esp + 4) = id;
    sub_A0931546();
    assert(g_esp == 0x80008);
    for (uint32_t p = 0x10000; p <= 0x13000; p += 0x1000)
        assert(MEM32(MEM32(p + 0x54) + 8) == 0);
    return g_eax;
}

static uint32_t void_call_guest_this(uint32_t fn, uint32_t self, int count, const uint32_t *args)
{
    ++custom_calls;
    if (fn == 0xA0000001u) {
        assert(count == 1 && args[0] == 42);
        return self;
    }
    assert(fn == 0xA0000002u && count == 0);
    return MEM32(self + 0x14);
}

int main(void)
{
    MEM32(0xA0000100u + 0x9C) = FIND_CHILD_VA;
    MEM32(0xA0000100u + 0x138) = GET_ID_VA;
    for (uint32_t p = 0x10000; p <= 0x13000; p += 0x1000) window(p, 0);
    MEM32(0x11010) = 42;
    prepend(0x10000, 0x11000);
    assert(lookup(0x10000, 42) == 0x11000);
    /* A new earlier duplicate must take precedence over a remembered match. */
    MEM32(0x12010) = 42;
    prepend(0x10000, 0x12000);
    assert(lookup(0x10000, 42) == 0x12000);
    MEM32(0x12010) = 7;
    assert(lookup(0x10000, 42) == 0x11000);
    MEM32(0x13010) = 42;
    prepend(0x12000, 0x13000);
    assert(lookup(0x10000, 42) == 0x13000);
    MEM32(0x13010) = 7;
    MEM32(0x11010) = 7;
    MEM32(0x10010) = 42;
    assert(lookup(0x10000, 42) == 0x10000);
    assert(lookup(0x10000, 99) == 0);
    MEM32(0x13000) = 0xA0000400u;
    MEM32(0xA0000400u + 0x9C) = 0xA0000001u;
    assert(lookup(0x10000, 42) == 0x13000 && custom_calls == 1);
    MEM32(0xA0000400u + 0x9C) = FIND_CHILD_VA;
    MEM32(0xA0000400u + 0x138) = 0xA0000002u;
    MEM32(0x13014) = 42;
    assert(lookup(0x10000, 42) == 0x13000 && custom_calls == 2);
    puts("window search PASS (duplicate insertion, ID mutation, DFS order, misses, custom methods, list references)");
    return 0;
}
