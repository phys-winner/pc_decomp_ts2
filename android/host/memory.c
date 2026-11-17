/* The guest address space: one 4 GiB reservation, a Win32-style page map
 * (64 KiB allocation granularity, reserve/commit, protection) and Win32 heaps
 * built on dlmalloc mspaces whose system memory comes from the same map. */
#define _GNU_SOURCE
#include "host.h"
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES (1u << 20)               /* 4 GiB / 4 KiB */
#define GRANULES (1u << 16)            /* 4 GiB / 64 KiB */

enum { ST_FREE = 0, ST_RESERVE = 1, ST_COMMIT = 2 };
#define MEM_COMMIT_ 0x1000u
#define MEM_RESERVE_ 0x2000u
#define MEM_DECOMMIT_ 0x4000u
#define MEM_RELEASE_ 0x8000u
#define MEM_TOP_DOWN_ 0x100000u
#define MEM_PRIVATE_ 0x20000u
#define MEM_IMAGE_ 0x1000000u

#define PAGE_NOACCESS_ 0x01u
#define PAGE_READONLY_ 0x02u
#define PAGE_READWRITE_ 0x04u
#define PAGE_WRITECOPY_ 0x08u
#define PAGE_EXECUTE_ 0x10u
#define PAGE_EXECUTE_READ_ 0x20u
#define PAGE_EXECUTE_READWRITE_ 0x40u
#define PAGE_EXECUTE_WRITECOPY_ 0x80u
#define PAGE_GUARD_ 0x100u

static uint8_t page_state[PAGES];
static uint16_t page_protect[PAGES];   /* Win32 PAGE_* of committed pages */
/* Every reservation base is 64 KiB aligned, so its granule fits in 16 bits. */
static uint16_t page_alloc[PAGES];     /* allocation base (VA >> 16), 0 if free */
static uint16_t alloc_protect[GRANULES]; /* initial protection by allocation base */
static uint8_t granule_used[GRANULES];
static pthread_mutex_t vm_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t host_page = GUEST_PAGE;
/* Reserved pages expose protection 0; reuse their unused field so fresh
 * anonymous commits remain demand-zero without touching their data pages. */
#define RESERVED_DIRTY 0xFFFFu

int void_guest_reserve(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page < GUEST_PAGE || page > GUEST_GRANULARITY || (page & (page - 1))) {
        void_log("[void] unsupported host page size %ld", page);
        return 0;
    }
    host_page = (uint32_t)page;
    void *want = (void *)(uintptr_t)VOID_GUEST_BASE;
    void *got = mmap(want, VOID_GUEST_SPAN, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (got == MAP_FAILED) {
        void_log("[void] guest window reservation failed: %s", strerror(errno));
        return 0;
    }
    if (got != want) {
        void_log("[void] guest window landed at %p instead of %p", got, want);
        munmap(got, VOID_GUEST_SPAN);
        return 0;
    }
    /* Null guard: the first 64 KiB are never allocatable, as on Windows. */
    granule_used[0] = 1;
    /* Synthetic thunk VAs: reserved, never accessible. */
    for (uint32_t g = GUEST_THUNK_BASE >> 16; g < (GUEST_THUNK_LIMIT >> 16); ++g) granule_used[g] = 1;
    /* RECOMP_RETADDR (0xDEAD0000) marks lifted return addresses: keep it unmapped. */
    granule_used[0xDEAD0000u >> 16] = 1;
    /* Everything above the user top stays unavailable. */
    for (uint32_t g = GUEST_USER_TOP >> 16; g < GRANULES; ++g) granule_used[g] = 1;
    void_log("[void] guest window %p..%p reserved (host page %u, guest page %u)", want,
        (void *)((uintptr_t)want + VOID_GUEST_SPAN), host_page, GUEST_PAGE);
    return 1;
}

static int host_prot(uint32_t protect)
{
    if (protect & PAGE_GUARD_) return PROT_NONE;
    switch (protect & 0xFFu) {
    case PAGE_NOACCESS_: return PROT_NONE;
    case PAGE_READONLY_: case PAGE_EXECUTE_: case PAGE_EXECUTE_READ_: return PROT_READ;
    default: return PROT_READ | PROT_WRITE;
    }
}

static int valid_protect(uint32_t protect)
{
    switch (protect & ~PAGE_GUARD_ & 0xFFFu) {
    case PAGE_NOACCESS_: case PAGE_READONLY_: case PAGE_READWRITE_: case PAGE_WRITECOPY_:
    case PAGE_EXECUTE_: case PAGE_EXECUTE_READ_: case PAGE_EXECUTE_READWRITE_: case PAGE_EXECUTE_WRITECOPY_:
        return 1;
    default:
        return 0;
    }
}

static uint32_t find_free(uint32_t size, int top_down)
{
    uint32_t need = (size + GUEST_GRANULARITY - 1) / GUEST_GRANULARITY;
    if (!need) return 0;
    uint32_t limit = GUEST_USER_TOP >> 16;
    if (top_down) {
        uint32_t run = 0;
        for (uint32_t g = limit; g-- > 1;) {
            run = granule_used[g] ? 0 : run + 1;
            if (run == need) return g << 16;
        }
    } else {
        uint32_t run = 0;
        for (uint32_t g = 1; g < limit; ++g) {
            run = granule_used[g] ? 0 : run + 1;
            if (run == need) return (g - need + 1) << 16;
        }
    }
    return 0;
}

static void set_pages(uint32_t first, uint32_t count, uint8_t state, uint16_t protect, uint32_t base_page)
{
    uint16_t base_granule = (uint16_t)(base_page >> 4);
    for (uint32_t p = first; p < first + count; ++p) {
        page_state[p] = state;
        page_protect[p] = protect;
        page_alloc[p] = base_granule;
    }
}

/* shortcut: mixed 4 KiB protections share a host PTE; metadata checks remain
 * exact, but enforcing subpage faults requires checked guest memory accesses. */
static int host_page_protection(uint32_t page, uint32_t first, uint32_t last, uint8_t state, uint16_t protect)
{
    int prot = PROT_NONE;
    for (uint32_t p = page; p < page + host_page / GUEST_PAGE; ++p) {
        int changed = state <= ST_COMMIT && p >= first && p <= last;
        uint8_t s = changed ? state : page_state[p];
        uint16_t access = changed ? protect : page_protect[p];
        if (s == ST_COMMIT) prot |= host_prot(access);
    }
    return prot;
}

static int host_protect_pages(uint32_t first, uint32_t last, uint8_t state, uint16_t protect)
{
    uint32_t step = host_page / GUEST_PAGE;
    uint32_t begin = first & ~(step - 1), end = last & ~(step - 1), run = begin;
    int held = host_page_protection(begin, first, last, state, protect);
    for (uint32_t p = begin + step; p <= end; p += step) {
        int prot = host_page_protection(p, first, last, state, protect);
        if (prot == held) continue;
        if (mprotect(GPTR(run << 12), (size_t)(p - run) << 12, held)) return 0;
        run = p;
        held = prot;
    }
    return mprotect(GPTR(run << 12), (size_t)(end + step - run) << 12, held) == 0;
}

static int host_protect_range(uint32_t first, uint32_t last, uint8_t state, uint16_t protect)
{
    if (host_protect_pages(first, last, state, protect)) return 1;
    int error = errno;
    /* Metadata still describes the old state, including every sibling. */
    if (!host_protect_pages(first, last, UINT8_MAX, 0)) {
        void_log("[void] host page protection rollback failed: %s", strerror(errno));
    }
    errno = error;
    return 0;
}

static int commit_range(uint32_t address, uint32_t size, uint32_t protect)
{
    if (host_page > GUEST_PAGE) {
        uint32_t first = address >> 12, last = (address + size - 1) >> 12;
        int dirty = 0;
        for (uint32_t p = first; p <= last; ++p)
            if (page_state[p] == ST_RESERVE && page_protect[p] == RESERVED_DIRTY) { dirty = 1; break; }
        if (dirty) {
            if (!host_protect_range(first, last, ST_COMMIT, PAGE_READWRITE_)) return 0;
            for (uint32_t p = first; p <= last; ++p)
                if (page_state[p] == ST_RESERVE && page_protect[p] == RESERVED_DIRTY) memset(GPTR(p << 12), 0, GUEST_PAGE);
        }
        return host_protect_range(first, last, ST_COMMIT, (uint16_t)protect);
    }
    void *host = GPTR(address);
    if (mprotect(host, size, host_prot(protect))) {
        void_log("[void] mprotect commit %08X+%08X failed: %s", address, size, strerror(errno));
        return 0;
    }
    return 1;
}

/* Reserve [address, address+size) as one allocation. Caller holds vm_lock. */
static int reserve_exact(uint32_t address, uint32_t size, uint32_t protect)
{
    uint32_t g0 = address >> 16, g1 = (uint32_t)(((uint64_t)address + size + GUEST_GRANULARITY - 1) >> 16);
    if ((address & (GUEST_GRANULARITY - 1)) || g1 > (GUEST_USER_TOP >> 16) || g1 <= g0) return 0;
    for (uint32_t g = g0; g < g1; ++g) if (granule_used[g]) return 0;
    for (uint32_t g = g0; g < g1; ++g) granule_used[g] = 1;
    uint32_t pages = (size + GUEST_PAGE - 1) / GUEST_PAGE;
    set_pages(address >> 12, pages, ST_RESERVE, 0, address >> 12);
    alloc_protect[address >> 16] = (uint16_t)protect;
    return 1;
}

uint32_t void_vm_alloc(uint32_t address, uint32_t size, uint32_t type, uint32_t protect)
{
    if (!size || !(type & (MEM_COMMIT_ | MEM_RESERVE_)) || !valid_protect(protect)) {
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    /* MEM_COMMIT alone with no address reserves too, as Windows does (the
     * game's small-block allocator asks for 512 KiB that way). */
    if (!address && (type & MEM_COMMIT_)) type |= MEM_RESERVE_;
    pthread_mutex_lock(&vm_lock);
    uint32_t result = 0;
    if (type & MEM_RESERVE_) {
        uint32_t base = address & ~(GUEST_GRANULARITY - 1);
        uint32_t span = (uint32_t)((((uint64_t)address + size + GUEST_PAGE - 1) & ~(uint64_t)(GUEST_PAGE - 1)) - base);
        if (!address) {
            span = (size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
            base = find_free(span, (type & MEM_TOP_DOWN_) != 0);
            if (!base) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_); return 0; }
        }
        if (!reserve_exact(base, span, protect)) {
            pthread_mutex_unlock(&vm_lock);
            void_set_last_error(ERROR_INVALID_ADDRESS_);
            return 0;
        }
        if (type & MEM_COMMIT_) {
            if (!commit_range(base, span, protect)) { pthread_mutex_unlock(&vm_lock); return 0; }
            set_pages(base >> 12, span >> 12, ST_COMMIT, (uint16_t)protect, base >> 12);
        }
        result = address ? address & ~(GUEST_PAGE - 1) : base;
        if (!address) result = base;
        else result = base;
    } else {
        /* Commit inside an existing reservation. */
        uint32_t first = address >> 12;
        uint32_t last = (uint32_t)(((uint64_t)address + size - 1) >> 12);
        for (uint32_t p = first; p <= last; ++p) {
            if (page_state[p] == ST_FREE) {
                pthread_mutex_unlock(&vm_lock);
                void_set_last_error(ERROR_INVALID_ADDRESS_);
                return 0;
            }
        }
        uint32_t start = first << 12, span = (last - first + 1) << 12;
        if (!commit_range(start, span, protect)) { pthread_mutex_unlock(&vm_lock); return 0; }
        for (uint32_t p = first; p <= last; ++p) { page_state[p] = ST_COMMIT; page_protect[p] = (uint16_t)protect; }
        result = start;
    }
    pthread_mutex_unlock(&vm_lock);
    return result;
}

int void_vm_claim(uint32_t address, uint32_t size, uint32_t protect)
{
    pthread_mutex_lock(&vm_lock);
    uint32_t span = (size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    int ok = reserve_exact(address, span, protect) && commit_range(address, span, protect);
    if (ok) set_pages(address >> 12, span >> 12, ST_COMMIT, (uint16_t)protect, address >> 12);
    pthread_mutex_unlock(&vm_lock);
    return ok;
}

int void_vm_free(uint32_t address, uint32_t size, uint32_t type)
{
    pthread_mutex_lock(&vm_lock);
    uint32_t page = address >> 12;
    if (type & MEM_RELEASE_) {
        if (size || (address & 0xFFFu) || page_state[page] == ST_FREE || ((uint32_t)page_alloc[page] << 4) != page) {
            pthread_mutex_unlock(&vm_lock);
            void_set_last_error(ERROR_INVALID_PARAMETER_);
            return 0;
        }
        uint32_t p = page;
        while (p < PAGES && page_state[p] != ST_FREE && page_alloc[p] == (page >> 4)) ++p;
        uint32_t span = (p - page) << 12;
        void *host = GPTR(address);
        if (mmap(host, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) == MAP_FAILED) {
            pthread_mutex_unlock(&vm_lock);
            void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
            return 0;
        }
        set_pages(page, p - page, ST_FREE, 0, 0);
        uint32_t g1 = (uint32_t)(((uint64_t)address + span + GUEST_GRANULARITY - 1) >> 16);
        for (uint32_t g = address >> 16; g < g1; ++g) granule_used[g] = 0;
    } else if (type & MEM_DECOMMIT_) {
        uint32_t first = page;
        uint32_t last = size ? (uint32_t)(((uint64_t)address + size - 1) >> 12) : first;
        if (!size) {
            uint32_t base = (uint32_t)page_alloc[page] << 4;
            first = base;
            last = base;
            while (last + 1 < PAGES && page_state[last + 1] != ST_FREE && page_alloc[last + 1] == (base >> 4)) ++last;
        }
        for (uint32_t p = first; p <= last; ++p) {
            if (page_state[p] == ST_FREE) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
        }
        if (host_page > GUEST_PAGE) {
            /* A mapped view is released by UnmapViewOfFile; decommitting it
             * cannot be emulated by zeroing a shared file on recommit. */
            for (uint32_t p = first; p <= last; ++p) if (granule_used[page_alloc[p]] == 2) {
                pthread_mutex_unlock(&vm_lock);
                void_set_last_error(ERROR_INVALID_ADDRESS_);
                return 0;
            }
            if (!host_protect_range(first, last, ST_RESERVE, 0)) {
                pthread_mutex_unlock(&vm_lock);
                void_set_last_error(ERROR_INVALID_PARAMETER_);
                return 0;
            }
            for (uint32_t p = first; p <= last; ++p) {
                if (page_state[p] == ST_COMMIT) page_protect[p] = RESERVED_DIRTY;
                page_state[p] = ST_RESERVE;
            }
            uint32_t step = host_page / GUEST_PAGE;
            for (uint32_t p = first & ~(step - 1); p <= last; p += step) {
                int live = 0;
                for (uint32_t q = p; q < p + step; ++q)
                    if (page_state[q] == ST_COMMIT) live = 1;
                if (!live && !madvise(GPTR(p << 12), host_page, MADV_DONTNEED))
                    for (uint32_t q = p; q < p + step; ++q)
                        if (page_state[q] == ST_RESERVE) page_protect[q] = 0;
            }
        } else {
            void *host = GPTR(first << 12);
            size_t span = (size_t)(last - first + 1) << 12;
            madvise(host, span, MADV_DONTNEED);
            mprotect(host, span, PROT_NONE);
            for (uint32_t p = first; p <= last; ++p) { page_state[p] = ST_RESERVE; page_protect[p] = 0; }
        }
    } else {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

int void_vm_protect(uint32_t address, uint32_t size, uint32_t protect, uint32_t *old)
{
    if (!size || !valid_protect(protect)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t first = address >> 12, last = (uint32_t)(((uint64_t)address + size - 1) >> 12);
    for (uint32_t p = first; p <= last; ++p) {
        if (page_state[p] != ST_COMMIT) { pthread_mutex_unlock(&vm_lock); void_set_last_error(ERROR_INVALID_ADDRESS_); return 0; }
    }
    if (old) *old = page_protect[first];
    if (host_page > GUEST_PAGE ? !host_protect_range(first, last, ST_COMMIT, (uint16_t)protect) :
        mprotect(GPTR(first << 12), (size_t)(last - first + 1) << 12, host_prot(protect))) {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_INVALID_PARAMETER_);
        return 0;
    }
    for (uint32_t p = first; p <= last; ++p) page_protect[p] = (uint16_t)protect;
    pthread_mutex_unlock(&vm_lock);
    return 1;
}

uint32_t void_vm_query(uint32_t address, uint32_t info)
{
    /* Stop address-space walks at the exclusive user top. Extending the
     * final free region to 4 GiB makes a 32-bit BaseAddress + RegionSize
     * wrap to zero; the game's memory-statistics fallback then loops forever. */
    if (address >= GUEST_USER_TOP) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t limit = GUEST_USER_TOP >> 12;
    uint32_t page = address >> 12;
    uint8_t state = page_state[page];
    uint32_t base_page = (uint32_t)page_alloc[page] << 4;
    uint16_t protect = page_protect[page];
    uint32_t p = page;
    while (p < limit && page_state[p] == state && page_alloc[p] == (base_page >> 4) &&
           (state != ST_COMMIT || page_protect[p] == protect)) ++p;
    uint32_t region = (p - page) << 12;
    if (state == ST_FREE) {
        /* A free region extends to the next allocated page. */
        p = page;
        while (p < limit && page_state[p] == ST_FREE) ++p;
        region = (p - page) << 12;
    }
    MEM32(info + 0) = page << 12;                                    /* BaseAddress */
    MEM32(info + 4) = state == ST_FREE ? 0 : base_page << 12;        /* AllocationBase */
    MEM32(info + 8) = state == ST_FREE ? 0 : alloc_protect[base_page >> 4]; /* AllocationProtect */
    MEM32(info + 12) = region;                                       /* RegionSize */
    MEM32(info + 16) = state == ST_FREE ? 0x10000u : state == ST_RESERVE ? MEM_RESERVE_ : MEM_COMMIT_;
    MEM32(info + 20) = state == ST_COMMIT ? protect : 0;             /* Protect */
    MEM32(info + 24) = state == ST_FREE ? 0 : MEM_PRIVATE_;          /* Type */
    pthread_mutex_unlock(&vm_lock);
    return 28;
}

/* MapViewOfFile: a fresh reservation with the file mapped over it. */
uint32_t void_vm_map_file(int fd, uint64_t offset, uint32_t size, int writable, int shared)
{
    if (!size) return 0;
    if (offset & (GUEST_GRANULARITY - 1)) { void_set_last_error(ERROR_INVALID_PARAMETER_); return 0; }
    pthread_mutex_lock(&vm_lock);
    uint32_t span = (size + GUEST_PAGE - 1) & ~(GUEST_PAGE - 1);
    uint32_t base = find_free(span, 0);
    if (!base || !reserve_exact(base, span, writable ? PAGE_READWRITE_ : PAGE_READONLY_)) {
        pthread_mutex_unlock(&vm_lock);
        void_set_last_error(ERROR_NOT_ENOUGH_MEMORY_);
        return 0;
    }
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *host = mmap(GPTR(base), span, prot, (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_FIXED, fd, (off_t)offset);
    if (host == MAP_FAILED) {
        int e = errno;
        set_pages(base >> 12, span >> 12, ST_FREE, 0, 0);
        uint32_t g1 = (uint32_t)(((uint64_t)base + span + GUEST_GRANULARITY - 1) >> 16);
        for (uint32_t g = base >> 16; g < g1; ++g) granule_used[g] = 0;
        pthread_mutex_unlock(&vm_lock);
        void_log("[void] mmap view failed: %s", strerror(e));
        void_set_last_error(ERROR_ACCESS_DENIED_);
        return 0;
    }
    set_pages(base >> 12, span >> 12, ST_COMMIT, (uint16_t)(writable ? PAGE_READWRITE_ : PAGE_READONLY_), base >> 12);
    if (host_page > GUEST_PAGE) granule_used[base >> 16] = 2;
    pthread_mutex_unlock(&vm_lock);
    return base;
}

int void_vm_unmap_file(uint32_t address)
{
    return void_vm_free(address & ~(GUEST_PAGE - 1), 0, MEM_RELEASE_);
}

static int range_access(uint32_t address, uint32_t size, int write)
{
    if (!size) return 1;
    uint64_t end = (uint64_t)address + size;
    if (end > 0x100000000ull) return 0;
    for (uint32_t p = address >> 12; p <= (uint32_t)((end - 1) >> 12); ++p) {
        if (page_state[p] != ST_COMMIT) return 0;
        int prot = host_prot(page_protect[p]);
        if (!(prot & PROT_READ) || (write && !(prot & PROT_WRITE))) return 0;
    }
    return 1;
}

int void_vm_readable(uint32_t address, uint32_t size) { return range_access(address, size, 0); }
int void_vm_writable(uint32_t address, uint32_t size) { return range_access(address, size, 1); }

/* Debugging (script "memfind"): every committed, readable guest address
 * holding the pattern, up to max hits. Patterns crossing a page into an
 * unreadable one are missed. */
int void_vm_find(const uint8_t *pattern, uint32_t length, uint32_t *hits, int max)
{
    int found = 0;
    if (!length) return 0;
    for (uint32_t page = 0; page < PAGES && found < max; ++page) {
        if (page_state[page] != ST_COMMIT || !(host_prot(page_protect[page]) & PROT_READ)) continue;
        uint32_t base = page << 12;
        const uint8_t *bytes = (const uint8_t *)GPTR(base);
        uint32_t span = 4096;
        if (page + 1 < PAGES && page_state[page + 1] == ST_COMMIT && (host_prot(page_protect[page + 1]) & PROT_READ))
            span += length - 1;
        for (uint32_t i = 0; i + length <= span && found < max; ++i)
            if (bytes[i] == pattern[0] && !memcmp(bytes + i, pattern, length)) hits[found++] = base + i;
    }
    return found;
}

/* ---------------------------------------------------------------- heaps */
#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MORECORE 0
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define USE_LOCKS 1
#define DEFAULT_GRANULARITY (256u * 1024u)
#define DEFAULT_MMAP_THRESHOLD (4u * 1024u * 1024u)
#define MALLOC_INSPECT_ALL 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
static void *guest_sys_alloc(size_t size);
static int guest_sys_free(void *p, size_t size);
#define MMAP(s) guest_sys_alloc(s)
#define DIRECT_MMAP(s) guest_sys_alloc(s)
#define MUNMAP(a, s) guest_sys_free((a), (s))
#define PROCEED_ON_ERROR 0
#define ABORT void_fatal("dlmalloc detected heap corruption")
#include "../../reference/dlmalloc/doc/dlalloc-2.8.6/malloc.c"

static void *guest_sys_alloc(size_t size)
{
    if (size > 0x7FFF0000u) return (void *)MFAIL;
    uint32_t va = void_vm_alloc(0, (uint32_t)size, MEM_RESERVE_ | MEM_COMMIT_, PAGE_READWRITE_);
    return va ? GPTR(va) : (void *)MFAIL;
}

/* The allocation holding `page`: its first page and one past its last.
 * Caller holds vm_lock. */
static void allocation_extent(uint32_t page, uint32_t *first, uint32_t *limit)
{
    uint32_t base = (uint32_t)page_alloc[page] << 4, p = base;
    while (p < PAGES && page_state[p] != ST_FREE && page_alloc[p] == (base >> 4)) ++p;
    *first = base;
    *limit = p;
}

/* dlmalloc hands system memory back (MUNMAP) as a whole segment from its base
 * or as a segment's top (a trim, from inside the segment to its end), and a
 * segment can span several of our allocations: a new mapping that starts
 * where the segment holding top ends, or ends at another segment's base, is
 * glued onto it, and find_free hands out the lowest free granules, so
 * neighbours are common. Each allocation the range touches is handled by
 * itself: covered whole, it is released; covered in part, those pages are
 * decommitted, and the allocation is released once none of its pages stay
 * committed (the head dlmalloc keeps after a trim is committed; a tail
 * trimmed earlier is not). Releasing only a base made every trim inside an
 * allocation fail, and left a glued segment's other allocations committed,
 * owned by nobody, for the rest of the session. A range that is not made of
 * live pages is refused before anything changes; after that the range is
 * always reported returned, since dlmalloc must not reuse pages that are
 * already gone, and a failed release or decommit only leaks. */
static int guest_sys_free(void *p, size_t size)
{
    uint32_t va = GVA(p);
    if (!size || (va & (GUEST_PAGE - 1)) || (uint64_t)va + size > GUEST_USER_TOP) return -1;
    uint32_t start = va >> 12, end = (uint32_t)(((uint64_t)va + size + GUEST_PAGE - 1) >> 12);
    pthread_mutex_lock(&vm_lock);
    int live = 1;
    for (uint32_t q = start; q < end && live; ++q) live = page_state[q] != ST_FREE;
    pthread_mutex_unlock(&vm_lock);
    if (!live) return -1;
    static int failures;
    for (uint32_t at = start; at < end;) {
        uint32_t first, limit;
        pthread_mutex_lock(&vm_lock);
        allocation_extent(at, &first, &limit);
        pthread_mutex_unlock(&vm_lock);
        uint32_t stop = limit < end ? limit : end;
        int ok = 1;
        if (at == first && stop == limit) {
            ok = void_vm_free(first << 12, 0, MEM_RELEASE_);
        } else {
            ok = void_vm_free(at << 12, (stop - at) << 12, MEM_DECOMMIT_);
            int committed = 0;
            pthread_mutex_lock(&vm_lock);
            for (uint32_t q = first; q < limit && !committed; ++q) committed = page_state[q] == ST_COMMIT;
            pthread_mutex_unlock(&vm_lock);
            if (ok && !committed) ok = void_vm_free(first << 12, 0, MEM_RELEASE_);
        }
        if (!ok && failures++ < 8)
            void_log("[void] heap unmap of %08X+%X: allocation %08X kept (%u failure(s))", at << 12, (stop - at) << 12,
                first << 12, (unsigned)failures);
        at = limit;
    }
    return 0;
}

/* A heap handle is the guest VA of a small descriptor, so it looks like the
 * pointer-valued HANDLE Windows returns. */
#define MAX_HEAPS 256
#define BLOCK_MAGIC 0x48454150u      /* 'HEAP' */
typedef struct { uint32_t handle; mspace space; } heap_t;
static heap_t heaps[MAX_HEAPS];
static int heap_count;
static uint32_t process_heap;
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

static heap_t *heap_find(uint32_t handle)
{
    for (int i = 0; i < heap_count; ++i) if (heaps[i].handle == handle && heaps[i].space) return &heaps[i];
    return NULL;
}

uint32_t void_heap_create(uint32_t initial, uint32_t maximum)
{
    (void)initial; (void)maximum;
    pthread_mutex_lock(&heap_lock);
    if (heap_count >= MAX_HEAPS) { pthread_mutex_unlock(&heap_lock); return 0; }
    mspace space = create_mspace(0, 1);
    if (!space) { pthread_mutex_unlock(&heap_lock); return 0; }
    /* The descriptor lives in the new heap itself: a real, readable address. */
    void *descriptor = mspace_calloc(space, 1, 64);
    heap_t *h = &heaps[heap_count++];
    h->space = space;
    h->handle = GVA(descriptor);
    MEM32(h->handle) = 0xEEFFEEFFu;   /* the Windows heap signature field */
    pthread_mutex_unlock(&heap_lock);
    return h->handle;
}

int void_heap_destroy(uint32_t handle)
{
    pthread_mutex_lock(&heap_lock);
    heap_t *h = heap_find(handle);
    if (!h || handle == process_heap) { pthread_mutex_unlock(&heap_lock); return 0; }
    destroy_mspace(h->space);
    h->space = NULL;
    pthread_mutex_unlock(&heap_lock);
    return 1;
}

uint32_t void_process_heap(void)
{
    if (!process_heap) process_heap = void_heap_create(0, 0);
    return process_heap;
}

uint32_t void_heap_alloc(uint32_t handle, uint32_t size, int zero)
{
    heap_t *h = heap_find(handle);
    if (!h || size > 0x7FF00000u) return 0;
    uint8_t *p = (uint8_t *)(zero ? mspace_calloc(h->space, 1, (size_t)size + 8) : mspace_malloc(h->space, (size_t)size + 8));
    if (!p) return 0;
    uint32_t block = GVA(p) + 8;
    MEM32(block - 8) = size;
    MEM32(block - 4) = BLOCK_MAGIC ^ handle;
    return block;
}

static int block_valid(uint32_t handle, uint32_t block)
{
    return block >= 0x10008u && (block & 7u) == 0 && void_vm_readable(block - 8, 8) &&
        MEM32(block - 4) == (BLOCK_MAGIC ^ handle);
}

/* A large block the game frees gives its pages back at once, as the Windows
 * heap decommits big free blocks: dlmalloc keeps free memory in its segments,
 * resident and dirty, until a whole segment empties. Only whole host pages
 * strictly inside the block go, clear of the links dlmalloc writes at the
 * start of a free chunk and of the next chunk's header after it; the pages
 * stay committed and read as zero when reused (HeapAlloc promises no content). */
#define HEAP_RELEASE_MIN (256u * 1024u)
static uint64_t heap_released_bytes;

static void heap_release_pages(uint32_t block, uint32_t size)
{
    if (size < HEAP_RELEASE_MIN) return;
    uint64_t lo = ((uint64_t)block + 64u + host_page - 1u) & ~(uint64_t)(host_page - 1u);
    uint64_t hi = ((uint64_t)block + size - 64u) & ~(uint64_t)(host_page - 1u);
    if (hi > lo && !madvise(GPTR((uint32_t)lo), (size_t)(hi - lo), MADV_DONTNEED)) heap_released_bytes += hi - lo;
}

int void_heap_free(uint32_t handle, uint32_t block)
{
    if (!block) return 1;
    heap_t *h = heap_find(handle);
    if (!h || !block_valid(handle, block)) {
        void_log("[void] HeapFree(%08X, %08X): not a block of this heap (caller %08X)", handle, block, g_cur_func);
        return 0;
    }
    heap_release_pages(block, MEM32(block - 8));
    MEM32(block - 4) = 0;
    mspace_free(h->space, GPTR(block - 8));
    return 1;
}

uint32_t void_heap_size(uint32_t handle, uint32_t block)
{
    if (!heap_find(handle) || !block_valid(handle, block)) return 0xFFFFFFFFu;
    return MEM32(block - 8);
}

uint32_t void_heap_realloc(uint32_t handle, uint32_t block, uint32_t size, int zero, int in_place_only)
{
    heap_t *h = heap_find(handle);
    if (!h || !block_valid(handle, block) || size > 0x7FF00000u) return 0;
    uint32_t old = MEM32(block - 8);
    uint8_t *p;
    if (in_place_only) {
        p = (uint8_t *)mspace_realloc_in_place(h->space, GPTR(block - 8), (size_t)size + 8);
        if (!p) return 0;
    } else {
        p = (uint8_t *)mspace_realloc(h->space, GPTR(block - 8), (size_t)size + 8);
        if (!p) return 0;
    }
    uint32_t moved = GVA(p) + 8;
    MEM32(moved - 8) = size;
    MEM32(moved - 4) = BLOCK_MAGIC ^ handle;
    if (zero && size > old) memset(GPTR(moved + old), 0, size - old);
    return moved;
}

/* The [mem] report: the system memory the Win32 heaps hold (dlmalloc
 * footprint, now and at its peak) and how many heaps are alive. */
void void_heap_stats(uint64_t *footprint, uint64_t *peak, int *count, uint64_t *released)
{
    *released = heap_released_bytes;
    uint64_t now = 0, high = 0;
    int live = 0;
    pthread_mutex_lock(&heap_lock);
    for (int i = 0; i < heap_count; ++i) {
        if (!heaps[i].space) continue;
        now += mspace_footprint(heaps[i].space);
        high += mspace_max_footprint(heaps[i].space);
        ++live;
    }
    pthread_mutex_unlock(&heap_lock);
    *footprint = now;
    *peak = high;
    *count = live;
}

/* The [mem] report (iostat.c): guest pages committed and reserved, and how
 * many of the committed ones are resident (mincore over the committed runs).
 * A diagnostic every few seconds: the scan holds vm_lock for well under a
 * millisecond and mincore only reads the page tables. */
void void_vm_stats(uint64_t *committed, uint64_t *reserved, uint64_t *resident)
{
    static unsigned char *vec;
    uint64_t c = 0, r = 0, in_core = 0;
    uint32_t step = host_page / GUEST_PAGE;
    if (!vec) vec = (unsigned char *)malloc(PAGES / (step ? step : 1) + 1);
    pthread_mutex_lock(&vm_lock);
    for (uint32_t p = 0; p < PAGES; ++p) { c += page_state[p] == ST_COMMIT; r += page_state[p] == ST_RESERVE; }
    for (uint32_t p = 0; vec && p < PAGES;) {
        if (page_state[p] != ST_COMMIT) { ++p; continue; }
        uint32_t first = p & ~(step - 1), last = p;
        while (last + 1 < PAGES && page_state[last + 1] == ST_COMMIT) ++last;
        uint32_t end = (last | (step - 1)) + 1;               /* exclusive, host-page aligned */
        size_t bytes = (size_t)(end - first) << 12, pages = bytes / host_page;
        if (!mincore(GPTR(first << 12), bytes, vec))
            for (size_t i = 0; i < pages; ++i) in_core += vec[i] & 1;
        p = end;
    }
    pthread_mutex_unlock(&vm_lock);
    *committed = c << 12;
    *reserved = r << 12;
    *resident = in_core * host_page;
}

uint32_t void_galloc(uint32_t size) { return void_heap_alloc(void_process_heap(), size, 1); }
void void_gfree(uint32_t va) { if (va) void_heap_free(void_process_heap(), va); }

uint32_t void_gstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    uint32_t va = void_galloc((uint32_t)n);
    if (va) memcpy(GPTR(va), s, n);
    return va;
}
