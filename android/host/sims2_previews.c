/* The original thumbnail generators draw using the guest scene. Start them
 * from the owning game's message pump after the window has had a frame to
 * show its empty thumbnail slot. */
#include "host.h"

enum { FAMILY_THUMBNAIL, LOT_THUMBNAIL };
typedef struct {
    uint32_t owner, key, bitmap, posted_frame;
    unsigned kind;
} preview_job;

static preview_job jobs[64];
static unsigned job_count;
static uint32_t submitted_frame, processed_frame, active_owner;

int void_is_main_guest_thread(void);

static uint32_t preview_key_offset(unsigned kind)
{
    return kind == FAMILY_THUMBNAIL ? 4 : 36;
}

static void remove_preview(unsigned index)
{
    --job_count;
    memmove(&jobs[index], &jobs[index + 1], (job_count - index) * sizeof jobs[0]);
}

static int queue_preview(uint32_t owner, unsigned kind)
{
    if (!void_is_main_guest_thread() || owner == active_owner) return 0;
    uint32_t expected = kind == FAMILY_THUMBNAIL ? 0xA0DDB040u : 0xA0DDB444u;
    if (!void_vm_readable(owner, 40) || MEM32(owner) != expected) return 0;
    uint32_t bitmap = MEM32(owner + 20);
    if (!void_vm_readable(bitmap, 8)) return 0;
    uint32_t table = MEM32(bitmap);
    if (table != 0xA0E54460u && table != 0xA0DE3220u) return 0;

    uint32_t key = MEM32(owner + preview_key_offset(kind));
    unsigned index = 0;
    while (index < job_count && jobs[index].owner != owner) ++index;
    if (index == job_count && job_count == sizeof jobs / sizeof jobs[0]) return 0;
    if (index < job_count && jobs[index].key == key && jobs[index].bitmap == bitmap) return 1;

    void_regs saved;
    void_regs_save(&saved);
    if (index == job_count) {
        void_call_guest_this(0xA0146A55u, owner, 0, NULL); /* Secondary interface AddRef */
        ++job_count;
    }
    jobs[index] = (preview_job){owner, key, bitmap, submitted_frame, kind};
    uint32_t empty = 0;
    void_call_guest_this(MEM32(table + 16), bitmap, 1, &empty);
    void_regs_load(&saved);
    return 1;
}

void s2_defer_family_preview(void)
{
    if (queue_preview(g_edi, FAMILY_THUMBNAIL)) {
        g_eax = 0;
        g_esp += 24; /* Return address, ID, width, height, image IID and output */
    } else {
        void_resolve(MEM32(g_esi + 24))();
    }
}

void sub_A07444D5(void);
void s2_defer_lot_preview(void)
{
    if (queue_preview(g_ebx, LOT_THUMBNAIL)) {
        g_eax = 0;
        g_esp += 4; /* cdecl: the caller removes the arguments */
    } else {
        sub_A07444D5();
    }
}

static void cancel_preview(uint32_t owner)
{
    for (unsigned index = 0; index < job_count; ++index) {
        if (jobs[index].owner != owner) continue;
        remove_preview(index);
        void_regs saved;
        void_regs_save(&saved);
        void_call_guest_this(0xA05BDFC7u, owner, 0, NULL);
        void_regs_load(&saved);
        return;
    }
}

void sub_A05D8ED0_lifted(void);
void sub_A05D8ED0(void)
{
    cancel_preview(g_ecx + 8);
    sub_A05D8ED0_lifted();
}

void sub_A05DBD1C_lifted(void);
void sub_A05DBD1C(void)
{
    cancel_preview(g_ecx + 8);
    sub_A05DBD1C_lifted();
}

void s2_preview_frame_submitted(void)
{
    if (void_is_main_guest_thread()) ++submitted_frame;
}

void s2_preview_tick(void)
{
    if (!job_count || active_owner || submitted_frame == processed_frame ||
        submitted_frame == jobs[0].posted_frame) return;

    preview_job job = jobs[0];
    remove_preview(0);
    processed_frame = submitted_frame;
    active_owner = job.owner;
    void_regs saved;
    void_regs_save(&saved);
    if (MEM32(job.owner + 12) && MEM32(job.owner + 20) == job.bitmap &&
        MEM32(job.owner + preview_key_offset(job.kind)) == job.key) {
        uint64_t start = void_monotonic_ns();
        if (job.kind == FAMILY_THUMBNAIL) {
            void_call_guest_this(0xA05D8015u, job.owner, 1, &job.key);
        } else {
            void_call_guest_this(0xA05DA671u, job.owner, 0, NULL);
        }
        double ms = (void_monotonic_ns() - start) / 1e6;
        if (ms >= 12) void_log("[preview] %s took %.1f ms", job.kind == FAMILY_THUMBNAIL ? "family" : "lot", ms);
    }
    void_call_guest_this(0xA05BDFC7u, job.owner, 0, NULL);
    void_regs_load(&saved);
    active_owner = 0;
}
