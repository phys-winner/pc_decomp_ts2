/* The guest heaps (android/host/memory.c: the Win32 page map and dlmalloc
 * mspaces), on the device. memory.c is compiled into this file, so the checks
 * reach guest_sys_free and the page map directly:
 *   1. guest_sys_free on bare allocations, in the shapes dlmalloc unmaps: a
 *      top trimmed from inside one allocation across two neighbours, a head
 *      unmapped from its base after such a trim, a segment glued from three
 *      neighbours unmapped from its base, and a range running into free pages
 *      (refused, nothing changes);
 *   2. one heap grown by blocks that each need a new mapping, which dlmalloc
 *      glues into one segment: with its upper half freed (trims across
 *      allocation boundaries) nothing may stay committed or reserved beyond
 *      the heap's footprint, and HeapDestroy must return every page and
 *      reservation;
 *   3. a random HeapAlloc/HeapReAlloc/HeapFree load: every live block keeps
 *      its bytes, freed large blocks give their pages back (heap_release_pages),
 *      freeing everything leaves little resident, and destroying the heap
 *      returns every page and reservation.
 * dlmalloc's own corruption checks abort the run.
 * Build and run: tools/check_heap_release.py --serial SERIAL; --memory FILE
 * builds against another memory.c (one before a fix) to see the checks fail. */
#define _GNU_SOURCE
#ifndef MEMORY_C
#define MEMORY_C "../host/memory.c"
#endif
#include MEMORY_C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

recomp_machine_t g_m;
static uint32_t last_error;
void void_set_last_error(uint32_t e) { last_error = e; }
void void_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
void void_fatal(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\nFATAL\n"); exit(3); }

static uint32_t rng = 0x9E3779B9u;
static uint32_t next(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

typedef struct { uint32_t va, size, seed; } live_t;
#define SLOTS 1500

static void fill(uint32_t va, uint32_t size, uint32_t seed)
{
    uint8_t *p = (uint8_t *)GPTR(va);
    /* Every byte for small blocks; first, last and one per 4 KiB for large ones. */
    if (size <= 8192) { for (uint32_t i = 0; i < size; ++i) p[i] = (uint8_t)(seed + i * 131u); return; }
    for (uint32_t i = 0; i < size; i += 4096) p[i] = (uint8_t)(seed + i * 131u);
    for (uint32_t i = size - 64; i < size; ++i) p[i] = (uint8_t)(seed + i * 131u);
}

static int intact(uint32_t va, uint32_t size, uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)GPTR(va);
    if (size <= 8192) { for (uint32_t i = 0; i < size; ++i) if (p[i] != (uint8_t)(seed + i * 131u)) return 0; return 1; }
    for (uint32_t i = 0; i < size; i += 4096) if (p[i] != (uint8_t)(seed + i * 131u)) return 0;
    for (uint32_t i = size - 64; i < size; ++i) if (p[i] != (uint8_t)(seed + i * 131u)) return 0;
    return 1;
}

static uint32_t random_size(void)
{
    uint32_t r = next() % 100;
    if (r < 60) return 16 + next() % 2048;             /* small objects */
    if (r < 85) return 4096 + next() % 65536;          /* medium */
    if (r < 97) return 200000 + next() % 900000;       /* large: released on free */
    return 4u * 1024 * 1024 + next() % (2u * 1024 * 1024);   /* direct mmap */
}

static double mb(int64_t bytes) { return bytes / 1048576.0; }

/* ------------------------------------------------ 1. bare allocations */
#define UNIT (256u * 1024u)                /* dlmalloc's mapping granularity here */

static uint32_t pages_in(uint32_t va, uint32_t bytes, uint8_t state)
{
    uint32_t n = 0;
    for (uint32_t p = va >> 12; p < (va + bytes) >> 12; ++p) n += page_state[p] == state;
    return n;
}

static int expect_pages(const char *what, uint32_t va, uint32_t bytes, uint8_t state)
{
    uint32_t n = pages_in(va, bytes, state), all = bytes >> 12;
    if (n == all) return 0;
    printf("FAIL: %s: %u of %u pages at %08X are %s\n", what, n, all, va,
        state == ST_FREE ? "free" : state == ST_RESERVE ? "reserved" : "committed");
    return 1;
}

/* `count` committed UNIT allocations; 1 when find_free handed them out back to back. */
static int neighbours(uint32_t count, uint32_t *va)
{
    for (uint32_t i = 0; i < count; ++i) {
        va[i] = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
        if (!va[i]) return 0;
        memset(GPTR(va[i]), 0xA5, UNIT);
    }
    for (uint32_t i = 1; i < count; ++i) if (va[i] != va[i - 1] + UNIT) return 0;
    return 1;
}

static int unmap_shapes(void)
{
    int failures = 0;
    uint64_t c0, r0, x0, c, r, x;
    void_vm_stats(&c0, &r0, &x0);
    uint32_t va[3];
    if (!neighbours(3, va)) { printf("FAIL: three adjacent allocations were not handed out\n"); return 1; }
    /* A trim from inside the first allocation to the end of the third: the
     * first keeps its head, the other two go. */
    if (guest_sys_free(GPTR(va[0] + UNIT / 2), 2 * UNIT + UNIT / 2)) { printf("FAIL: trim across allocations refused\n"); ++failures; }
    failures += expect_pages("head of the trimmed allocation", va[0], UNIT / 2, ST_COMMIT);
    failures += expect_pages("tail of the trimmed allocation", va[0] + UNIT / 2, UNIT / 2, ST_RESERVE);
    failures += expect_pages("allocations after the trim point", va[1], 2 * UNIT, ST_FREE);
    /* The rest of that segment, from its base: the allocation goes whole. */
    if (guest_sys_free(GPTR(va[0]), UNIT / 2)) { printf("FAIL: unmap of the head refused\n"); ++failures; }
    failures += expect_pages("allocation whose head was unmapped", va[0], UNIT, ST_FREE);
    /* A segment glued from three neighbours, from its base: all three go. */
    if (!neighbours(3, va)) { printf("FAIL: three adjacent allocations were not handed out again\n"); return failures + 1; }
    if (guest_sys_free(GPTR(va[0]), 3 * UNIT)) { printf("FAIL: unmap of a glued segment refused\n"); ++failures; }
    failures += expect_pages("glued segment", va[0], 3 * UNIT, ST_FREE);
    /* A range running past its allocation into free pages: refused, nothing changes. */
    uint32_t one = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    if (!one || pages_in(one + UNIT, UNIT, ST_FREE) != UNIT >> 12) {
        printf("FAIL: no free pages after a lone allocation\n");
        ++failures;
    } else {
        if (guest_sys_free(GPTR(one), 2 * UNIT) != -1) { printf("FAIL: a range over free pages was accepted\n"); ++failures; }
        failures += expect_pages("allocation of the refused range", one, UNIT, ST_COMMIT);
    }
    if (one) void_vm_free(one, 0, MEM_RELEASE_);
    void_vm_stats(&c, &r, &x);
    if (c != c0 || r != r0) {
        printf("FAIL: bare allocations left %.2f MB committed and %.2f MB reserved\n", mb((int64_t)(c - c0)), mb((int64_t)(r - r0)));
        ++failures;
    }
    printf("unmap shapes: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

/* ------------------------------------------------ 2. a glued heap */
/* A heap's first segment holds the heap's own record, so dlmalloc never trims
 * it (has_segment_link). A spacer right after that segment sends the heap's
 * next mapping elsewhere: it starts a segment of its own, and the mappings
 * after it are glued onto that one. The blocks: four of 200 KiB, one of
 * 600 KiB (a 768 KiB mapping), then 200 KiB ones; freeing everything after the
 * big block trims the segment across the mappings after it, and a last trim
 * cuts inside the big mapping. */
#define GLUE_SMALL (200u * 1024u)
#define GLUE_BIG (600u * 1024u)
#define GLUE_BEFORE 4
#define GLUE_BLOCKS 32

static uint32_t glue_size(int i) { return i == GLUE_BEFORE ? GLUE_BIG : GLUE_SMALL; }

/* Allocations in [va, end) that have committed and reserved pages both: trimmed inside. */
static int split_allocations(uint32_t va, uint32_t end)
{
    int split = 0;
    for (uint32_t p = va >> 12; p < end >> 12;) {
        if (page_state[p] == ST_FREE) { ++p; continue; }
        /* The allocation's pages, read straight from the page map (memory.c before a fix has no helper for it). */
        uint32_t first = (uint32_t)page_alloc[p] << 4, limit = first, committed = 0, reserved = 0;
        while (limit < PAGES && page_state[limit] != ST_FREE && page_alloc[limit] == (first >> 4)) ++limit;
        for (uint32_t q = first; q < limit; ++q) { committed += page_state[q] == ST_COMMIT; reserved += page_state[q] == ST_RESERVE; }
        split += committed && reserved;
        p = limit > p ? limit : p + 1;
    }
    return split;
}

static int glued_heap(uint32_t info)
{
    int failures = 0, straddle = 0, heaps;
    uint64_t c0, r0, x0, c, r, x, footprint, peak, released, grown;
    void_vm_stats(&c0, &r0, &x0);
    uint32_t heap = void_heap_create(0, 0);
    uint32_t spacer = void_vm_alloc(0, UNIT, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_), block[GLUE_BLOCKS];
    for (int i = 0; i < GLUE_BLOCKS; ++i) {
        block[i] = void_heap_alloc(heap, glue_size(i), 0);
        if (!block[i]) { printf("FAIL: glued heap: alloc %d\n", i); return failures + 1; }
        fill(block[i], glue_size(i), 0x5Au + (uint32_t)i);
        /* A block whose first and last bytes lie in different allocations sits in a glued segment. */
        void_vm_query(block[i], info);
        uint32_t first = MEM32(info + 4);
        void_vm_query(block[i] + glue_size(i) - 1, info);
        straddle += MEM32(info + 4) != first;
    }
    void_heap_stats(&grown, &peak, &heaps, &released);
    if (!straddle) { printf("FAIL: glued heap: no block spans two allocations, dlmalloc glued nothing\n"); ++failures; }
    for (int i = GLUE_BLOCKS - 1; i > GLUE_BEFORE; --i) void_heap_free(heap, block[i]);
    /* dlmalloc trims on its own whenever the top passes 2 MiB; trim once more now, so the
     * last cut falls right behind the big block, inside its mapping. */
    mspace_trim(heap_find(heap)->space, 0);
    for (int i = 0; i <= GLUE_BEFORE; ++i)
        if (!intact(block[i], glue_size(i), 0x5Au + (uint32_t)i)) { printf("FAIL: glued heap: block %d changed\n", i); ++failures; }
    void_vm_stats(&c, &r, &x);
    void_heap_stats(&footprint, &peak, &heaps, &released);
    int64_t held = (int64_t)(c + r) - (int64_t)(c0 + r0) - UNIT;      /* without the spacer */
    int split = split_allocations(block[1] & ~(GUEST_GRANULARITY - 1), block[GLUE_BLOCKS - 1] + GLUE_SMALL);
    printf("glued heap: %d of %d blocks span two allocations; footprint %.2f MB, after freeing the blocks behind the big one "
        "%.2f MB, held %.2f MB, %d allocation(s) trimmed inside\n", straddle, GLUE_BLOCKS, mb((int64_t)grown),
        mb((int64_t)footprint), mb(held), split);
    if (grown - footprint < (2u << 20)) { printf("FAIL: glued heap: dlmalloc trimmed nothing, the trim path was not exercised\n"); ++failures; }
    if (!split) { printf("FAIL: glued heap: no trim ended inside the big block's mapping (block sizes no longer fit dlmalloc)\n"); ++failures; }
    /* A trim that ends inside an allocation leaves that allocation's tail reserved: at most one per segment. */
    if (held > (int64_t)(footprint + UNIT)) {
        printf("FAIL: glued heap: %.2f MB committed or reserved beyond the footprint after trims\n", mb(held - (int64_t)footprint));
        ++failures;
    }
    void_heap_destroy(heap);
    void_vm_free(spacer, 0, MEM_RELEASE_);
    void_vm_stats(&c, &r, &x);
    if (c != c0 || r != r0) {
        printf("FAIL: glued heap: HeapDestroy left %.2f MB committed and %.2f MB reserved\n", mb((int64_t)(c - c0)), mb((int64_t)(r - r0)));
        ++failures;
    } else {
        printf("glued heap destroyed: every page and reservation returned\n");
    }
    return failures;
}

/* ------------------------------------------------ 3. a random load */
int main(void)
{
    if (!void_guest_reserve()) { printf("FAIL: guest window\n"); return 2; }
    uint32_t info = void_vm_alloc(0, 4096, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    int failures = unmap_shapes();
    failures += glued_heap(info);
    uint64_t committed_before, reserved_before, resident_before;
    void_vm_stats(&committed_before, &reserved_before, &resident_before);
    uint32_t heap = void_heap_create(0, 0);
    uint64_t committed0, reserved0, resident0;
    void_vm_stats(&committed0, &reserved0, &resident0);
    static live_t live[SLOTS];
    uint64_t peak_resident = 0;
    for (int round = 0; round < 6; ++round) {
        for (int op = 0; op < 20000; ++op) {
            live_t *s = &live[next() % SLOTS];
            uint32_t what = next() % 10;
            if (s->va && !intact(s->va, s->size, s->seed)) { printf("FAIL: block %08X (%u bytes) changed\n", s->va, s->size); ++failures; }
            if (s->va && what < 4) {
                if (!void_heap_free(heap, s->va)) { printf("FAIL: free %08X\n", s->va); ++failures; }
                s->va = 0;
            } else if (s->va && what < 6) {
                /* HeapReAlloc keeps the common prefix. */
                uint32_t size = random_size(), keep = size < s->size ? size : s->size;
                uint32_t moved = void_heap_realloc(heap, s->va, size, 0, 0);
                if (!moved) { printf("FAIL: realloc %u\n", size); ++failures; continue; }
                /* The bytes fill() wrote: all of a small block, every 4 KiB of a large one. */
                const uint8_t *p = (const uint8_t *)GPTR(moved);
                uint32_t step = s->size > 8192 ? 4096 : 1;
                for (uint32_t i = 0; i < keep; i += step)
                    if (p[i] != (uint8_t)(s->seed + i * 131u)) { printf("FAIL: realloc lost byte %u\n", i); ++failures; break; }
                s->va = moved; s->size = size; s->seed = next();
                fill(s->va, s->size, s->seed);
            } else if (!s->va) {
                s->size = random_size(); s->seed = next();
                s->va = void_heap_alloc(heap, s->size, next() & 1);
                if (!s->va) { printf("FAIL: alloc %u\n", s->size); ++failures; continue; }
                fill(s->va, s->size, s->seed);
            }
        }
        uint64_t committed, reserved, resident, footprint, peak, released;
        int heaps;
        void_vm_stats(&committed, &reserved, &resident);
        void_heap_stats(&footprint, &peak, &heaps, &released);
        if (resident > peak_resident) peak_resident = resident;
        uint64_t live_bytes = 0;
        for (int i = 0; i < SLOTS; ++i) if (live[i].va) live_bytes += live[i].size;
        printf("round %d: live %.1f MB, committed %.1f MB, resident %.1f MB, footprint %.1f MB, returned so far %.1f MB\n",
            round, live_bytes / 1048576.0, committed / 1048576.0, resident / 1048576.0, footprint / 1048576.0, released / 1048576.0);
    }
    for (int i = 0; i < SLOTS; ++i) {
        if (live[i].va && !intact(live[i].va, live[i].size, live[i].seed)) { printf("FAIL: block %08X changed at the end\n", live[i].va); ++failures; }
        if (live[i].va) void_heap_free(heap, live[i].va);
        live[i].va = 0;
    }
    uint64_t committed, reserved, resident, footprint, peak, released;
    int heaps;
    void_vm_stats(&committed, &reserved, &resident);
    void_heap_stats(&footprint, &peak, &heaps, &released);
    int64_t held = (int64_t)(committed + reserved) - (int64_t)(committed_before + reserved_before);
    printf("all freed: committed %.1f MB (start %.1f), resident %.1f MB (peak %.1f), footprint %.1f MB, held beyond it %.1f MB, "
        "pages returned %.1f MB\n", committed / 1048576.0, committed0 / 1048576.0, resident / 1048576.0, peak_resident / 1048576.0,
        footprint / 1048576.0, mb(held - (int64_t)footprint), released / 1048576.0);
    /* With everything freed the heap keeps at most a few segments of free memory resident. */
    if (resident > resident0 + (48u << 20)) { printf("FAIL: %.1f MB still resident after freeing everything\n", (resident - resident0) / 1048576.0); ++failures; }
    if (!released) { printf("FAIL: no freed pages were returned\n"); ++failures; }
    void_heap_destroy(heap);
    void_vm_stats(&committed, &reserved, &resident);
    if (committed != committed_before || reserved != reserved_before) {
        printf("FAIL: HeapDestroy after the random load left %.2f MB committed and %.2f MB reserved\n",
            mb((int64_t)(committed - committed_before)), mb((int64_t)(reserved - reserved_before)));
        ++failures;
    } else {
        printf("random-load heap destroyed: every page and reservation returned\n");
    }
    printf(failures ? "FAILED %d\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
