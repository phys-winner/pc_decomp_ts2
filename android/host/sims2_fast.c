/* Native versions of hot Sims2EP9.exe routines (tools/build_android.py
 * NATIVE_OVERRIDES) whose lifted form spends its time on call and loop
 * overhead rather than on work. Each keeps what the guest observes: the
 * memory it writes, its result and the registers its callers rely on. */
#include "host.h"

/* Material parameter names: a vector of nullable entries, each holding an
 * MSVC small string. Keep the original short-circuit loads and scratch GPRs. */
void sub_A0ADFAE0(void)
{
    uint32_t self = g_ecx, result = 0xA0D9B9D0u;
    uint32_t kind = FK_NARROW(FK_CMP, 24), a = (uint32_t)MEM8(self + 4) << 24, b = 0;
    if (a) {
        uint32_t end = MEM32(self + 0x2C), begin = MEM32(self + 0x28);
        uint32_t count = (uint32_t)((int32_t)(end - begin) >> 2), index = MEM32(g_esp + 4);
        g_ecx = index; g_edx = begin;
        kind = FK_CMP; a = index; b = count;
        if (index < count) {
            uint32_t entry = MEM32(begin + index * 4);
            kind = FK_TEST; a = b = entry;
            if (entry) {
                result = entry + 0x14u;
                kind = FK_CMP; a = MEM32(result + 0x14); b = 15;
                if (a > b) result = MEM32(result);
            }
        }
    }
    g_eax = result; g_esp += 8;
    g_flag_k = kind; g_flag_a = a; g_flag_b = b;
    g_flag_cf = kind == FK_CMP && a < b;
}

/* Object/person word attributes: unsigned 16-bit index, exact bound and flags.
 * Profiled on the paused lot; paired Poco CPU measurements favor this leaf. */
void sub_A052F900(void)
{
    uint32_t self = g_ecx, sp = g_esp;
    uint32_t index = MEM16(sp + 4), carry = index < 0x71u;
    if (__builtin_expect(index <= 0x71u, 1)) {
        g_eax = MEM16(self + index * 2u + 0xA6u);
        g_flag_k = FK_NARROW(FK_CMP, 16);
        g_flag_a = index << 16; g_flag_b = 0x71u << 16;
        g_flag_cf = carry;
    } else {
        g_eax = 0;
        g_flag_k = FK_TEST; g_flag_a = g_flag_b = g_flag_cf = 0;
    }
    g_esp = sp + 8;
}

/* Keep DBPF exact-first/wildcard ordering without the lifted search loop. */
static int resource_entry(uint32_t self,uint32_t key,unsigned flags,uint32_t *entry)
{
    if(!void_vm_readable(self,0x38)||!void_vm_readable(key,16))return 0;
    *entry=0;
    uint32_t count=MEM32(self+0x28);
    if(!count)return 1;
    if(count>0x100000u)return 0;
    uint32_t nodes=MEM32(self+0x1C),slot=(MEM32(key)+MEM32(key+4))%count;
    for(unsigned visited=0;visited<4096;++visited){
        uint64_t address=(uint64_t)nodes+(uint64_t)slot*16;
        if(address>UINT32_MAX-28||!void_vm_readable((uint32_t)address,28))return 0;
        uint32_t node=(uint32_t)address;
        if(MEM32(node)==MEM32(key)&&MEM32(node+4)==MEM32(key+4)){
            uint32_t first=MEM32(node+8),start=first,end=MEM32(node+0x18),rows=MEM32(self+4);
            if(first>=end||end>0x1000000u||(uint64_t)rows+(uint64_t)end*16>UINT32_MAX||
                !void_vm_readable(rows+first*16,(end-first)*16))return 0;
            uint32_t lo=MEM32(key+8),hi=MEM32(key+12),last=end;
            while(first<last){
                uint32_t mid=first+(last-first)/2,row=rows+mid*16;
                if(MEM32(row+4)<hi||(MEM32(row+4)==hi&&MEM32(row)<lo))first=mid+1;else last=mid;
            }
            if(first<end){uint32_t row=rows+first*16;if(MEM32(row)==lo&&MEM32(row+4)==hi){*entry=row;return 1;}}
            int any_high=(flags&1)&&!hi,zero_high=(flags&2)&&hi;
            if(any_high||zero_high)for(uint32_t i=start;i<end;++i){
                uint32_t row=rows+i*16;
                if(MEM32(row)==lo&&(any_high||!MEM32(row+4))){*entry=row;break;}
            }
            return 1;
        }
        slot=MEM32(node+12);
        if((int32_t)slot<0)return 1;
    }
    return 0;
}

void sub_A09AB1A0_lifted(void);
void sub_A09AB1A0(void)
{
    uint32_t self=g_ecx,sp=g_esp,entry;
    if(!void_vm_readable(sp,16)||!void_vm_writable(self,0x38)||
        !resource_entry(self,MEM32(sp+4),(MEM8(sp+8)!=0)|((MEM8(sp+12)!=0)<<1),&entry))goto lifted;
    uint32_t offset=0,length=0,size=0;
    if(entry){
        offset=MEM32(entry+8);length=MEM16(entry+12);size=MEM16(entry+14);
        if(length==0xFFFFu){
            uint64_t ext=(uint64_t)MEM32(self+0x10)+(uint64_t)size*8;
            if(ext>UINT32_MAX-8||!void_vm_readable((uint32_t)ext,8))goto lifted;
            length=MEM32((uint32_t)ext);size=MEM32((uint32_t)ext+4);
        }
        size|=0x80000000u;MEM32(self+0x2C)=offset;MEM32(self+0x30)=length;MEM32(self+0x34)=size;
        g_ecx=MEM16(entry+12)==0xFFFFu?size:0xFFFFu;
    }
    g_eax=entry?self+0x2C:0;g_esp=sp+16;
    g_flag_k=FK_TEST;g_flag_a=size;g_flag_b=size;g_flag_cf=0;
    return;
lifted:
    sub_A09AB1A0_lifted();
}

/* RefPack copies advance forward even when the back-reference overlaps its
 * output: later bytes may use bytes just produced. No restrict/memmove here. */
static void qfs_copy(uint32_t to, uint32_t from, uint32_t count)
{
    uint8_t *dst = GPTR(to);
    const uint8_t *src = GPTR(from);
    for (uint32_t i = 0; i < count; ++i) dst[i] = src[i];
}

/* QFS/RefPack decoder, cdecl(input, consumed?, output, capacity), A09CC369.
 * Keep the guest's header variants, command-before-copy capacity checks,
 * overlapping recurrence, scratch writes, volatile GPRs and final flags.
 * Validation/allocation remain in its existing A09A9100/A09CC174 callers. */
void sub_A09CC369(void)
{
    uint32_t sp = g_esp, input = MEM32(sp + 4), left = MEM32(sp + 16);
    MEM32(sp - 16) = g_ebx;
    MEM32(sp - 20) = g_ebp;
    uint32_t out = MEM32(sp + 12);
    MEM32(sp - 24) = g_esi;
    MEM32(sp - 28) = g_edi;
    uint32_t at = input + ((MEM8(input) & 1) ? 5u : 2u);
    MEM32(sp - 4) = ((uint32_t)MEM8(at) << 16) | ((uint32_t)MEM8(at + 1) << 8) | MEM8(at + 2);
    at += 3;
    uint32_t ecx, result;
    for (;;) {
        uint32_t c = MEM8(at++), plain, count, distance;
        if (c < 0x80u) {
            uint32_t b = MEM8(at++);
            MEM8(sp + 12) = (uint8_t)b;
            plain = c & 3u;
            count = ((c >> 2) & 7u) + 3u;
            distance = ((c & 0x60u) << 3) + 1u;
            ecx = plain;
        } else if (c < 0xC0u) {
            uint32_t b = MEM8(at++), d = MEM8(at++);
            MEM8(sp + 12) = (uint8_t)d;
            plain = b >> 6;
            count = (c & 0x3Fu) + 4u;
            distance = ((b & 0x3Fu) << 8) + 1u;
            ecx = plain;
        } else if (c < 0xE0u) {
            uint32_t b = MEM8(at++), d = MEM8(at++), n = MEM8(at++);
            MEM8(sp + 12) = (uint8_t)b;
            MEM8(sp - 9) = (uint8_t)d;
            plain = c & 3u;
            count = ((c & 0xCu) << 6) + n + 5u;
            distance = ((c & 0x10u) << 12) + 1u;
            ecx = count - 1u;
            MEM32(sp - 8) = ecx;
        } else if (c < 0xFCu) {
            count = 0;
            plain = ((c & 0x1Fu) << 2) + 4u;
            distance = 0;
            ecx = plain;
        } else {
            plain = c & 3u;
            ecx = ((c & 0x1Fu) << 2) + 4u;
            if ((int32_t)(left - plain) < 0) { result = UINT32_MAX; break; }
            qfs_copy(out, at, plain);
            at += plain;
            ecx = MEM32(sp + 8);
            if (ecx) {
                at -= MEM32(sp + 4);
                MEM32(ecx) = at;
            }
            result = MEM32(sp - 4);
            break;
        }
        left -= plain + count;
        if ((int32_t)left < 0) { result = UINT32_MAX; break; }
        qfs_copy(out, at, plain);
        out += plain;
        at += plain;
        /* The x86 body rereads these scratch bytes after copying literals.
         * Keep that ordering even if a caller aliases output with its stack. */
        if (c < 0xC0u) distance += MEM8(sp + 12);
        else if (c < 0xE0u) {
            distance += ((uint32_t)MEM8(sp + 12) << 8) + MEM8(sp - 9);
            count = MEM32(sp - 8) + 1u;
        }
        qfs_copy(out, out - distance, count);
        out += count;
    }
    g_eax = result;
    g_ecx = ecx;
    g_edx = at;
    g_edi = MEM32(sp - 28);
    g_esi = MEM32(sp - 24);
    g_ebp = MEM32(sp - 20);
    g_ebx = MEM32(sp - 16);
    g_flag_k = FK_ADD; g_flag_a = sp - 12u; g_flag_b = 12u;
    g_flag_cf = sp < sp - 12u;
    g_esp = sp + 4u;
}

/* MSVC's uint32 hash-table lookup, __thiscall(out, key, hash), retn 12:
 * buckets hold the first/last nodes of a doubly linked range. Keep the
 * returned iterator pair, volatile registers and final comparison flags. */
void sub_A014189B(void)
{
    uint32_t self = g_ecx, out = MEM32(g_esp + 4);
    uint32_t bucket = MEM32(self + 0xC) + (MEM32(self + 0x18) & MEM32(g_esp + 12)) * 8u;
    uint32_t first = MEM32(self + 4), node = MEM32(bucket + 4), found = 0;
    if (node != first) {
        first = MEM32(bucket);
        uint32_t key = MEM32(MEM32(g_esp + 8));
        for (;;) {
            if (key == MEM32(node + 8)) { found = node; first = MEM32(node); break; }
            if (node == first) break;
            node = MEM32(node + 4);
        }
    }
    MEM32(out + 4) = found;
    MEM32(out) = first;
    g_eax = out;
    g_ecx = first;
    g_edx = node;
    g_flag_k = FK_TEST; g_flag_a = g_flag_b = g_flag_cf = 0;
    g_esp += 16;
}

/* The uint32 red-black tree's find, __thiscall(key), retn 4: combine its
 * lower_bound and equality check without constructing a guest iterator. */
void sub_A0144C35(void)
{
    uint32_t head = MEM32(g_ecx), node = MEM32(head + 4), candidate = head, key = 0;
    if (!MEM8(node + 0xD)) {
        key = MEM32(MEM32(g_esp + 4));
        do {
            if (MEM32(node + 0x10) >= key) { candidate = node; node = MEM32(node); }
            else node = MEM32(node + 8);
        } while (!MEM8(node + 0xD));
    }
    uint32_t found = !MEM8(candidate + 0xD) && key >= MEM32(candidate + 0x10);
    g_eax = found ? candidate : head;
    g_ecx = candidate;
    g_edx = g_esp - 16;
    g_flag_k = FK_TEST; g_flag_a = g_flag_b = found; g_flag_cf = 0;
    g_esp += 8;
}

/* Tree iterator increment, __thiscall(): the next node is the right
 * subtree's leftmost child, or the first ancestor reached from its left.
 * Keep the intermediate iterator writes and the final comparison flags. */
void sub_A015DC50(void)
{
    uint32_t out = g_ecx, node = MEM32(out), next = MEM32(node + 8), sibling = 0;
    uint32_t nil = MEM8(next + 0xD);
    if (!nil) {
        uint32_t child = MEM32(next);
        while (!(nil = MEM8(child + 0xD))) { next = child; child = MEM32(child); }
        g_ecx = child;
    } else {
        next = MEM32(node + 4);
        while (!(nil = MEM8(next + 0xD))) {
            sibling = MEM32(next + 8);
            if (node != sibling) break;
            MEM32(out) = next;
            node = next;
            next = MEM32(next + 4);
        }
    }
    MEM32(out) = next;
    g_eax = out;
    g_edx = node;
    g_flag_k = nil ? FK_NARROW(FK_CMP, 24) : FK_CMP;
    g_flag_a = nil ? nil << 24 : node;
    g_flag_b = nil ? 0 : sibling;
    g_flag_cf = !nil && node < sibling;
    g_esp += 4;
}

/* The window tree's search by ID (__thiscall, uint32 id, `retn 4`,
 * 0xA0931546): depth first over the child list at [this+0x54] (a ring of
 * {next, prev, child} nodes behind a sentinel, reference-counted at +8 while
 * it is walked), each child's own search (vtable +0x9C), then the window's
 * own ID (vtable +0x138; 83 of the 86 window classes use the plain getter
 * 0xA0149CF0, [this+0x10]). The UI looks windows up by ID many times a frame
 * and every lookup walks the whole tree: lifted, the walk was 13% of a
 * live-mode frame. Here a child that searches the same way is searched in
 * place; anything else is the guest's call. */
#define FIND_CHILD_VA 0xA0931546u
#define GET_ID_VA 0xA0149CF0u
#define LIST_RELEASE_VA 0xA0933FEAu

static uint32_t find_child(uint32_t self, uint32_t id)
{
    uint32_t list = MEM32(self + 0x54);
    MEM32(list + 8) += 1;
    uint32_t head = MEM32(list), found = 0;
    for (uint32_t node = MEM32(head); node != head; node = MEM32(node)) {
        uint32_t child = MEM32(node + 8);
        uint32_t search = MEM32(MEM32(child) + 0x9C);
        if (search == FIND_CHILD_VA && MEM32(child + 0x58) != self) {
            static int noted;
            if (noted++ < 4) void_log("[fast] window %08X on %08X's list names parent %08X", child, self, MEM32(child + 0x58));
        }
        found = search == FIND_CHILD_VA ? find_child(child, id) : void_call_guest_this(search, child, 1, &id);
        if (found) break;
    }
    /* 0xA0933FEA drops the reference and frees a list the window replaced
     * during the walk; only that last case needs the guest. */
    if (MEM32(list + 8) == 1 && MEM32(self + 0x54) != list) {
        uint32_t arg = list;
        void_call_guest_this(LIST_RELEASE_VA, self, 1, &arg);
    } else {
        MEM32(list + 8) -= 1;
    }
    if (!found) {
        uint32_t get_id = MEM32(MEM32(self) + 0x138);
        uint32_t mine = get_id == GET_ID_VA ? MEM32(self + 0x10) : void_call_guest_this(get_id, self, 0, NULL);
        if (mine == id) found = self;
    }
    return found;
}

/* The CRT's case-insensitive comparisons fold ASCII only while no locale was
 * set (__locale_changed, 0xA10DFCAC); otherwise the guest's own code runs. */
#define LOCALE_CHANGED_VA 0xA10DFCACu

static int same_ignoring_case(const uint8_t *a, const uint8_t *b, uint32_t n)
{
    if (MEM32(LOCALE_CHANGED_VA)) {
        if (!memcmp(a, b, n)) return 1;
        uint32_t args[3] = { GVA(a), GVA(b), n };
        return void_call_guest(0xA0845872u, 3, args, 0) == 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t x = a[i], y = b[i];
        if (x - 'A' < 26u) x += 32;
        if (y - 'A' < 26u) y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* A name's offset in a packed table of NUL-terminated names ([this+0x18] up
 * to [this+0x1C]), compared without case (_memicmp); -1 if absent; the scan
 * ends where it stopped (`end` when absent). The property sets look names up
 * many times a frame while drawing, and the byte-by-byte strlen over the
 * whole table was the hottest loop in the game: three billion iterations in
 * a few minutes of play. */
static uint32_t name_scan(uint32_t self, uint32_t key, uint32_t *stop)
{
    uint32_t length = MEM32(key + 16);
    uint32_t chars = MEM32(key + 20) > 15 ? MEM32(key) : key;      /* the string's own buffer, or the heap */
    uint32_t start = MEM32(self + 0x18), end = MEM32(self + 0x1C);
    const uint8_t *k = (const uint8_t *)GPTR(chars);
    for (uint32_t p = start; p != end;) {
        uint32_t n = (uint32_t)strlen((const char *)GPTR(p));
        if (n == length && same_ignoring_case((const uint8_t *)GPTR(p), k, n)) { *stop = p; return p - start; }
        p += n + 1;
    }
    *stop = end;
    return 0xFFFFFFFFu;
}

void sub_A01BEFFF(void)             /* __thiscall, std::string const *key, `retn 4` */
{
    uint32_t self = g_ecx, stop;
    g_eax = name_scan(self, MEM32(g_esp + 4), &stop);
    g_ecx = self;
    g_esp += 8;                     /* the return address and the argument (retn 4) */
}

/* The same lookup, adding an absent name at the end of the table through
 * vector<char>::insert(where, first, count) (0xA01BE8B6, its NUL included)
 * and answering its offset (0xA01BF191; ecx is left at the vector, as there). */
#define NAMES_INSERT_VA 0xA01BE8B6u

void sub_A01BF191(void)
{
    uint32_t self = g_ecx, key = MEM32(g_esp + 4), stop;
    uint32_t vector = self + 0x18;
    uint32_t found = name_scan(self, key, &stop);
    if (found == 0xFFFFFFFFu) {
        found = MEM32(vector + 4) - MEM32(vector);
        uint32_t args[3] = { stop, MEM32(key + 20) > 15 ? MEM32(key) : key, MEM32(key + 16) + 1 };
        void_call_guest_this(NAMES_INSERT_VA, vector, 3, args);
    }
    g_eax = found;
    g_ecx = vector;
    g_esp += 8;
}

/* _stricmp (0xA0845799, cdecl): with both strings present and no locale set
 * it is __ascii_stricmp (0xA0845760), the difference of the first two
 * characters that differ after folding A-Z. Containers keyed by names
 * compare through it; lifted, each character cost a dozen guest operations. */
void sub_A0845799_lifted(void);
void sub_A0845799(void)
{
    uint32_t a = MEM32(g_esp + 4), b = MEM32(g_esp + 8);
    if (MEM32(LOCALE_CHANGED_VA) || !a || !b) { sub_A0845799_lifted(); return; }
    const uint8_t *x = (const uint8_t *)GPTR(a), *y = (const uint8_t *)GPTR(b);
    uint32_t f, l;
    do {
        f = *x++;
        if (f - 'A' < 26u) f += 32;
        l = *y++;
        if (l - 'A' < 26u) l += 32;
    } while (f == l && f);
    g_eax = f - l;
    g_esp += 4;
}

/* A job queue's per-frame pump (0xA0229E45, __thiscall on the queue's second
 * base, `retn 4`, al = 1): before and after, a pass over two lists of the
 * queue (0xA022A593, 0xA022AB39); between them it runs the queued jobs
 * (0xA022995A, one per call) until its timer (0xA01C9256 at [this+0x38],
 * microseconds) has seen 500 pass, and on an empty queue it went on calling
 * the runner until then: half a millisecond of spinning in every frame. Here
 * the pump stops as soon as the queue (an std::list, its size at
 * [queue+0x38]) is empty: only jobs that ran could have added to it. */
#define PUMP_BEFORE_VA 0xA022A593u
#define PUMP_RUN_ONE_VA 0xA022995Au
#define PUMP_AFTER_VA 0xA022AB39u
#define TIMER_ELAPSED_VA 0xA01C9256u

void sub_A0229E45(void)
{
    uint32_t self = g_ecx, queue = self - 8, timer = self + 0x38;
    void_call_guest_this(PUMP_BEFORE_VA, queue, 0, NULL);
    uint32_t start = void_call_guest_this(TIMER_ELAPSED_VA, timer, 0, NULL);
    do {
        void_call_guest_this(PUMP_RUN_ONE_VA, queue, 0, NULL);
        if (!MEM32(queue + 0x38)) break;
    } while (void_call_guest_this(TIMER_ELAPSED_VA, timer, 0, NULL) - start < 500u);
    void_call_guest_this(PUMP_AFTER_VA, queue, 0, NULL);
    g_eax = (g_eax & 0xFFFFFF00u) | 1u;
    g_esp += 8;                     /* the return address and the argument (retn 4) */
}

void sub_A0931546(void)
{
    uint32_t id = MEM32(g_esp + 4);
    g_eax = find_child(g_ecx, id);
    g_esp += 8;                     /* the return address and the argument (retn 4) */
}

/* The embedded D3DX X8R8G8B8/A8 row writers. Keep transforms and error
 * diffusion in the lifted bodies. The ordinary finite [0,1] path below
 * retains both single-precision stores, x87 chop rounding, row direction,
 * ordered bias, guest scratch and caller state. No replacement compressor. */
void sub_A0039752_lifted(void);
void sub_A003AA9F_lifted(void);

static int quant_overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

/* Positive, normal, bounded inputs: casting can round up in the host FPCR's
 * mode. Correct that one ULP to the x87 routine's explicit RC=chop store.
 * Products and addition of an n/32 bias fit exactly in double here. */
static float quant_float_rz(double v)
{
    union { float f; uint32_t u; } r = { .f = (float)v };
    if ((double)r.f > v) --r.u;
    return r.f;
}

static int quant_row(int alpha)
{
    uint32_t self = g_ecx, sp = g_esp, in = MEM32(sp + 12);
    uint32_t n = MEM32(self + 0x68), x = MEM32(sp + 4), y = MEM32(sp + 8);
    uint32_t size = alpha ? 1u : 4u, frame = alpha ? 84u : 96u;
    if ((self | sp | in) & 3u || !n || n > 4096u || sp < frame || sp > UINT32_MAX - 16u ||
        self > UINT32_MAX - 128u || MEM32(self + 4) != (alpha ? 28u : 22u) ||
        MEM32(self + 0x54) || MEM32(self + 0x10) || MEM32(self + 0x5C) ||
        (g_fpu_cw & 0x3Fu) != 0x3Fu || (g_fpu_cw & 0x300u) == 0x100u ||
        MEM32(0xA0D9C79Cu) != 0x437F0000u ||
        (recomp_yield_hook && (n >= RECOMP_YIELD_EVERY ||
         g_backedges >= RECOMP_YIELD_EVERY - n))) return 0;
    uintptr_t fp = (uintptr_t)g_stp;
    if ((fp & 7u) || fp < (uintptr_t)(g_stbuf + 2) || fp > (uintptr_t)(g_stbuf + X87_WINDOW - 10)) return 0;
    int depth = X87_DEPTH(g_stp);
    if (depth < 0 || depth > 6) return 0;
    uint32_t table = MEM32(self + 0x34);
    if (table != 0xA10518B0u && table != 0xA1051930u) return 0;
    uint64_t start = (uint64_t)MEM32(self + 0x20) +
        (uint64_t)MEM32(self + 0x60) * x + (uint64_t)MEM32(self + 0x64) * y;
    if (start > UINT32_MAX - n * size || in > UINT32_MAX - n * 16u) return 0;
    uint32_t out = (uint32_t)start, bias = table + 4u * ((y & 3u) + 8u * (x & 3u));
    if (!alpha && (out & 3u)) return 0;
    uint32_t at[6] = { self, sp - frame, in, out, table, 0xA0D9C79Cu };
    uint32_t bytes[6] = { 128, frame + 16u, n * 16u, n * size, 128, 4 };
    for (unsigned i = 0; i < 6; ++i)
        for (unsigned j = 0; j < i; ++j)
            if (quant_overlap(at[i], bytes[i], at[j], bytes[j])) return 0;
    /* Check integer encodings before any FP operation or mutation. Only the
     * components/entries the original row actually reads are inspected. */
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        uint32_t b = MEM32(bias + 4u * i);
        if (b < 0x3D000000u || b > 0x3F780000u) return 0;
        unsigned shift = 145u - (b >> 23);
        if ((b & 0x7FFFFFu) & ((1u << shift) - 1u)) return 0;
    }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t c = alpha ? 3u : 0u; c < (alpha ? 4u : 3u); ++c) {
            uint32_t v = MEM32(in + 16u * i + 4u * c);
            if (v && (v < 0x33800000u || v > 0x3F800000u)) return 0;
        }
    uint32_t _flag_k, _flag_a, _flag_b, _cf;
    RECOMP_ENTER(alpha ? 0xA003AA9Fu : 0xA0039752u);
    (void)_flag_k; (void)_flag_a; (void)_flag_b; (void)_cf;
    uint64_t fpsr;
    __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr) :: "memory");
    uint32_t bp = sp - 4u, saved = g_fpu_cw | (alpha ? y & 0xFFFF0000u : 0u);
    int32_t dir = x & 1u ? -1 : 1;
    uint32_t first = x & 1u ? n - 1u : 0u;
    float scaled[3] = {0}, biased[3] = {0};
    uint32_t value[3] = {0};
    double red_sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t j = (x & 1u) ? n - 1u - i : i;
        float d = *(float *)GPTR(bias + 4u * (i & 3u));
        for (uint32_t c = 0; c < (alpha ? 1u : 3u); ++c) {
            float v = *(float *)GPTR(in + 16u * j + 4u * (alpha ? 3u : c));
            scaled[c] = quant_float_rz((double)v * 255.0);
            double sum = (double)scaled[c] + (double)d;
            biased[c] = quant_float_rz(sum);
            value[c] = (uint32_t)biased[c];
            if (!c) red_sum = sum;
        }
        if (alpha) MEM8(out + j) = (uint8_t)value[0];
        else MEM32(out + 4u * j) = (value[0] << 16) | (value[1] << 8) | value[2];
    }
    /* These are the exact bytes left by the original prologue, last row
     * iteration and epilogue. The no-alias guard permits deferring scratch. */
    MEM32(bp) = g_ebp;
    MEM32(bp - (alpha ? 0x48u : 0x54u)) = g_ebx;
    MEM32(bp - (alpha ? 0x4Cu : 0x58u)) = g_esi;
    MEM32(bp - (alpha ? 0x50u : 0x5Cu)) = g_edi;
    MEM32(self + 0x7C) = saved;
    MEM32(bp + 0x10) = saved;
    MEM32(bp - 8u) = bias;
    if (alpha) {
        MEM32(bp + 8u) = (uint32_t)dir;
        MEM32(bp + 12u) = n;
        MEM32(bp - 4u) = 16u * (first + (n - 1u) * (uint32_t)dir);
        MEM32(bp - 0x14u) = 16u * (uint32_t)dir;
        MEM32(bp - 0x28u) = 0;
        *(float *)GPTR(bp - 0x18u) = scaled[0];
        *(float *)GPTR(bp - 0xCu) = biased[0];
        MEM32(bp - 0x10u) = MEM32(bp - 0x38u) = value[0];
        g_stp[-1] = (double)biased[0];
    } else {
        MEM32(bp + 8u) = n;
        *(float *)GPTR(bp + 12u) = biased[2];
        MEM32(bp - 4u) = out + size * (first + n * (uint32_t)dir);
        MEM32(bp - 0x1Cu) = size * (uint32_t)dir;
        MEM32(bp - 0x20u) = 16u * (uint32_t)dir;
        MEM32(bp - 0x50u) = MEM32(bp - 0x4Cu) = MEM32(bp - 0x48u) = 0;
        for (unsigned c = 0; c < 3; ++c) *(float *)GPTR(bp - 0x40u + 4u * c) = scaled[c];
        *(float *)GPTR(bp - 0xCu) = biased[1];
        MEM32(bp - 0x10u) = MEM32(bp - 0x30u) = value[0];
        MEM32(bp - 0x14u) = MEM32(bp - 0x2Cu) = value[1];
        MEM32(bp - 0x18u) = MEM32(bp - 0x28u) = value[2];
        g_stp[-1] = (double)biased[2];
        g_stp[-2] = (g_fpu_cw & 0x300u) ? red_sum : (double)biased[0];
        X87_SHADOW(g_stp)[-2] = 0;
    }
    X87_SHADOW(g_stp)[-1] = 0;
    __asm__ volatile("msr fpsr, %0" :: "r"(fpsr) : "memory");
    g_eax = saved;
    g_ecx = 16u * (first + (n + 1u) * (uint32_t)dir);
    g_edx = n;
    g_esp = sp + 16u;
    g_flag_k = FK_CMP; g_flag_a = g_flag_b = n; g_flag_cf = 0;
    g_backedges += n;
    return 1;
}

/* Distinct C names keep old guest profile records off the new host CFG;
 * aliases supply the dispatch symbols without changing fallback bodies. */
static void __attribute__((no_profile_instrument_function)) sims2_rgbx_row(void) { if (!quant_row(0)) sub_A0039752_lifted(); }
static void __attribute__((no_profile_instrument_function)) sims2_alpha_row(void) { if (!quant_row(1)) sub_A003AA9F_lifted(); }
void sub_A0039752(void) __attribute__((alias("sims2_rgbx_row")));
void sub_A003AA9F(void) __attribute__((alias("sims2_alpha_row")));

/* An unchanged pipeline still increments its application count. Dirty state,
 * changed programs and guest-stack aliases keep the original update path. */
void sub_A017FC00_lifted(void);
void sub_A017FC00(void)
{
    uint32_t sp = g_esp, self = g_ecx;
    if (sp < 16 || sp > UINT32_MAX - 8u || self > UINT32_MAX - 0xA0u ||
        quant_overlap(self, 0xA0, sp - 16, 24)) goto original;
    uint32_t arg = MEM32(sp + 4);
    if (arg > UINT32_MAX - 16u || quant_overlap(arg, 16, sp - 16, 24)) goto original;
    uint32_t descriptor = MEM32(arg + 12);
    if (descriptor > UINT32_MAX - 3u || quant_overlap(descriptor, 3, sp - 16, 24)) goto original;
    uint32_t flags = MEM8(descriptor + 2);
    if (MEM32(self + 0x74) || MEM32(self + 0x78) || MEM32(self + 0x7C)) goto original;
    if ((flags & 1u) && (MEM32(self + 0x98) != arg || MEM32(self + 0x1C) || MEM32(self + 0x20) ||
                        MEM32(self + 0x24) || MEM32(self + 0x28) || MEM32(self + 0x2C))) goto original;
    if ((flags & 2u) && (MEM32(self + 0x9C) != arg || MEM32(self + 0x70))) goto original;
    uint32_t _flag_k, _flag_a, _flag_b, _cf;
    RECOMP_ENTER(0xA017FC00u);
    (void)_flag_k; (void)_flag_a; (void)_flag_b; (void)_cf;
    MEM32(sp - 4) = self;
    MEM32(sp - 8) = g_ebx;
    MEM32(sp - 12) = g_esi;
    MEM32(sp - 16) = g_edi;
    MEM32(sp + 4) = flags;
    uint32_t count = MEM32(self + 0x18);
    MEM32(self + 0x18) = count + 1u;
    g_eax = 0;
    g_flag_k = FK_INC; g_flag_a = count; g_flag_b = 1; g_flag_cf = 0;
    g_esp = sp + 8;
    return;
original:
    sub_A017FC00_lifted();
}

static int persistence_ascii_string(uint32_t text, uint32_t sp, uint32_t ptd, uint32_t locale, uint32_t table)
{
    if (text > UINT32_MAX - 24u || !void_vm_readable(text, 24) ||
        quant_overlap(text, 24, sp - 4096u, 4112) || (ptd && (
        quant_overlap(text, 24, ptd, 0x354) || quant_overlap(text, 24, locale, 0xB4) ||
        quant_overlap(text, 24, table, 128)))) return 0;
    uint32_t at = MEM32(text + 20) <= 15 ? text : MEM32(text);
    if (at > UINT32_MAX - 1024u || !void_vm_readable(at, 1024) ||
        quant_overlap(at, 1024, sp - 4096u, 4112) || (ptd && (
        quant_overlap(at, 1024, ptd, 0x354) || quant_overlap(at, 1024, locale, 0xB4) ||
        quant_overlap(at, 1024, table, 128)))) return 0;
    for (unsigned i = 0; i < 1024; ++i) {
        uint32_t c = MEM8(at + i);
        if (!c) return 1;
        if (c >= 128) return 0;
    }
    return 0;
}

static int persistence_ascii_bucket(uint32_t self, uint32_t sp, uint32_t ptd, uint32_t locale, uint32_t table)
{
    if (sp < 4096u || sp > UINT32_MAX - 16u || !void_vm_writable(sp - 4096u, 4112) ||
        !void_vm_readable(self, 28) || quant_overlap(self, 28, sp - 4096u, 4112) || (ptd && (
        quant_overlap(self, 28, ptd, 0x354) || quant_overlap(self, 28, locale, 0xB4) ||
        quant_overlap(self, 28, table, 128) || quant_overlap(sp, 16, ptd, 0x354) ||
        quant_overlap(sp, 16, locale, 0xB4) || quant_overlap(sp, 16, table, 128)))) {
        return 0;
    }
    uint32_t bucket = MEM32(self + 12) + (MEM32(self + 24) & MEM32(sp + 12)) * 8u;
    uint32_t out = MEM32(sp + 4);
    if (!void_vm_readable(bucket, 8) || quant_overlap(bucket, 8, sp - 4096u, 4112) ||
        !void_vm_writable(out, 8) || quant_overlap(out, 8, sp - 4096u, 4112) || (ptd && (
        quant_overlap(bucket, 8, ptd, 0x354) || quant_overlap(bucket, 8, locale, 0xB4) ||
        quant_overlap(bucket, 8, table, 128) || quant_overlap(out, 8, ptd, 0x354) ||
        quant_overlap(out, 8, locale, 0xB4) || quant_overlap(out, 8, table, 128)))) {
        return 0;
    }
    uint32_t node = MEM32(bucket + 4), first = MEM32(bucket), head = MEM32(self + 4);
    if (node == head) return 1;
    if (!persistence_ascii_string(MEM32(sp + 8) + 4u, sp, ptd, locale, table)) return 0;
    /* shortcut: buckets over 8192 nodes use the original; profile before widening again. */
    for (unsigned i = 0; i < 8192; ++i) {
        if (!void_vm_readable(node, 36) || quant_overlap(node, 36, sp - 4096u, 4112) ||
            (ptd && (quant_overlap(node, 36, ptd, 0x354) || quant_overlap(node, 36, locale, 0xB4) ||
             quant_overlap(node, 36, table, 128))) ||
            !persistence_ascii_string(node + 12u, sp, ptd, locale, table)) {
            return 0;
        }
        if (node == first) return 1;
        node = MEM32(node + 4);
    }
    return 0;
}

static int persistence_locale_release(uint32_t guard)
{
    uint32_t ptd = MEM32(guard);
    if (ptd > UINT32_MAX - 0x354u || !void_vm_readable(ptd, 0x354) ||
        !void_vm_writable(ptd + 0x350u, 4)) return 0;
    if (MEM8(guard + 12)) MEM32(ptd + 0x350u) &= ~2u;
    return 1;
}

static int persistence_needs_lower(uint32_t self, uint32_t sp)
{
    uint32_t bucket = MEM32(self + 12) + (MEM32(self + 24) & MEM32(sp + 12)) * 8u;
    uint32_t node = MEM32(bucket + 4);
    if (node == MEM32(self + 4)) return 0;
    uint32_t key = MEM32(sp + 8) + 4u;
    uint32_t a = MEM32(key + 20) <= 15 ? key : MEM32(key);
    uint32_t b = MEM32(node + 32) <= 15 ? node + 12u : MEM32(node + 12);
    for (;;) {
        uint32_t x = MEM8(a++), y = MEM8(b++);
        if ((!x || x == '.') && (!y || y == '.')) return 0;
        if (x != y) return 1;
    }
}

/* Snapshot the original current-locale table once per ASCII bucket lookup;
 * signed high-byte, malformed and stack-alias inputs retain the original path. */
void sub_A01D5B50_lifted(void);
void sub_A01D5B50(void)
{
    uint32_t sp = g_esp, self = g_ecx;
    const uint8_t *lower = NULL;
    uint8_t ascii_lower[128];
    if (MEM32(0xA10DFCACu)) {
        if (!persistence_ascii_bucket(self, sp, 0, 0, 0)) { sub_A01D5B50_lifted(); return; }
        if (persistence_needs_lower(self, sp)) {
            uint32_t guard = sp - 64u;
            uint32_t current = 0;
            recomp_machine_t saved = g_m;
            g_esp = sp - 128u;
            void_call_guest_this(0xA0831B2Au, guard, 1, &current);
            g_m = saved;
            uint32_t ptd = MEM32(guard);
            uint32_t locale = MEM32(guard + 4);
            uint32_t table = void_vm_readable(locale, 0x98) ? MEM32(locale + 0x94) : 0;
            int valid = table && void_vm_readable(table, 128) && !quant_overlap(table, 128, sp - 4096u, 4112);
            if (valid) memcpy(ascii_lower, GPTR(table), sizeof ascii_lower);
            if (!persistence_locale_release(guard)) valid = 0;
            /* A CRT mapping preserves NUL and cannot turn another byte into NUL. */
            if (valid) {
                valid = !ascii_lower[0];
                for (unsigned i = 1; i < 128; ++i) if (!ascii_lower[i]) valid = 0;
            }
            if (!valid) { sub_A01D5B50_lifted(); return; }
            if (!persistence_ascii_bucket(self, sp, ptd, locale, table)) { sub_A01D5B50_lifted(); return; }
            lower = ascii_lower;
        }
    }
    uint32_t masked = MEM32(self + 0x18) & MEM32(sp + 12);
    uint32_t last_edx = masked;
    uint32_t bucket = MEM32(self + 12) + masked * 8u, head = MEM32(self + 4);
    MEM32(sp - 4) = g_ebp;
    uint32_t node = MEM32(bucket + 4), out;
    if (node == head) {
        out = MEM32(sp + 4);
        g_ebp = MEM32(sp - 4);
        MEM32(out) = head;
        MEM32(out + 4) = 0;
        g_ecx = head;
        g_flag_k = FK_CMP; g_flag_a = node; g_flag_b = head; g_flag_cf = 0;
    } else {
        uint32_t first = MEM32(bucket), key = MEM32(sp + 8) + 4u;
        MEM32(sp - 8) = g_ebx;
        MEM32(sp + 12) = first;
        MEM32(sp - 12) = g_esi;
        MEM32(sp + 8) = key;
        MEM32(sp - 16) = g_edi;
        uint32_t folded = 0, found = 0, end = 0;
        for (;;) {
            key = MEM32(sp + 8);
            uint32_t at = MEM32(key + 20) <= 15 ? key : MEM32(key);
            uint32_t name = node + 12u;
            if (MEM32(node + 32) > 15) name = MEM32(name);
            uint32_t delta = name - at;
            for (;;) {
                uint32_t a = MEM8(at), b = MEM8(delta + at);
                if ((!a || a == '.') && (!b || b == '.')) { found = 1; end = b; break; }
                if (a != b) {
                    uint32_t rhs = (uint32_t)(int32_t)(int8_t)b;
                    MEM32(sp - 20) = rhs;
                    MEM32(sp - 24) = 0xA01D5BCDu;
                    MEM32(sp - 28) = node;
                    if (lower) rhs = lower[rhs];
                    else if (rhs - 'A' < 26u) rhs += 32;
                    uint32_t lhs = (uint32_t)(int32_t)(int8_t)MEM8(at);
                    MEM32(sp - 24) = lhs;
                    MEM32(sp - 28) = 0xA01D5BD8u;
                    MEM32(sp - 32) = node;
                    if (lower) lhs = lower[lhs];
                    else if (lhs - 'A' < 26u) lhs += 32;
                    if (lower) last_edx = lhs;
                    folded = lhs;
                    if (lhs != rhs) break;
                }
                ++at;
            }
            if (found || node == MEM32(sp + 12)) break;
            node = MEM32(node + 4);
        }
        out = MEM32(sp + 4);
        uint32_t next = found ? MEM32(node) : node;
        g_edi = MEM32(sp - 16);
        g_esi = MEM32(sp - 12);
        g_ebx = MEM32(sp - 8);
        if (found) {
            MEM32(out + 4) = node;
            MEM32(out) = next;
            g_ecx = next;
            g_flag_k = FK_NARROW(end ? FK_CMP : FK_TEST, 24);
            g_flag_a = g_flag_b = end << 24;
        } else {
            MEM32(out) = node;
            MEM32(out + 4) = 0;
            g_ecx = folded;
            g_flag_k = FK_CMP; g_flag_a = node; g_flag_b = node;
        }
        g_ebp = MEM32(sp - 4);
        g_flag_cf = 0;
    }
    g_eax = out;
    g_edx = last_edx;
    g_esp = sp + 16;
}

/* Font parser substring search: return after the match; only candidate starts
 * are bounded by end. Even an empty needle searches for a NUL in the haystack. */
void sub_A09187BE_lifted(void);
void sub_A09187BE(void)
{
    uint32_t sp = g_esp;
    if (sp < 16u || sp > UINT32_MAX - 20u || !void_vm_writable(sp - 16u, 36)) goto original;
    uint32_t self = MEM32(sp + 4), needle = MEM32(sp + 8);
    uint32_t begin = MEM32(sp + 12), end = MEM32(sp + 16);
    if (!void_vm_readable(self, 12) || !void_vm_readable(needle, 4096) ||
        ((uint64_t)self < (uint64_t)sp + 20u && (uint64_t)sp - 16u < (uint64_t)self + 12u) ||
        ((uint64_t)needle < (uint64_t)sp + 20u && (uint64_t)sp - 16u < (uint64_t)needle + 4096u)) goto original;
    const uint8_t *nul = memchr(GPTR(needle), 0, 4096);
    if (!nul) goto original;
    uint32_t n = (uint32_t)(nul - (const uint8_t *)GPTR(needle));
    uint32_t base = MEM32(self + 8), width = n ? n : 1u;
    if ((int32_t)begin < (int32_t)end) {
        uint32_t count = end - begin;
        /* shortcut: bound one uninterrupted native search, profile before widening. */
        if ((int32_t)begin < 0 || (int32_t)end < 0 || (uint64_t)count * width > 0x1000000u ||
            (uint64_t)base + end + width > UINT32_MAX ||
            !void_vm_readable(base + begin, count + width - 1u) ||
            ((uint64_t)base + begin < (uint64_t)sp + 20u &&
             (uint64_t)sp - 16u < (uint64_t)base + end + width - 1u)) goto original;
    }
    MEM32(sp - 4) = g_ebx;
    MEM32(sp - 8) = g_ebp;
    MEM32(sp - 12) = g_esi;
    MEM32(sp - 16) = g_edi;
    MEM32(sp + 4) = base;
    uint32_t at = begin, compared = needle + 1u, found = 0;
    while ((int32_t)at < (int32_t)end) {
        const uint8_t *p = memchr(GPTR(base + at), MEM8(needle), end - at);
        if (!p) { at = end; break; }
        at = GVA(p) - base;
        compared = 1;
        while (compared < n && MEM8(base + at + compared) == MEM8(needle + compared)) ++compared;
        if (compared >= n) { found = 1; break; }
        ++at;
    }
    g_eax = found ? base + at + compared : 0;
    g_ecx = n;
    g_edx = compared;
    g_flag_k = found ? FK_ADD : FK_CMP;
    g_flag_a = found ? at + compared : at;
    g_flag_b = found ? base : end;
    g_flag_cf = found ? (g_eax < base) : (at < end);
    g_esp = sp + 4;
    return;
original:
    sub_A09187BE_lifted();
}
