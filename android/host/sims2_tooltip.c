/* Explicit catalog descriptions and owner-thread cancellation of their native selection. */
#include "host.h"
#include "input.h"
#include "sims2_settings.h"
#include <pthread.h>

static uint32_t explicit_catalog_popup;
static int preparing_catalog_popup;
static pthread_mutex_t catalog_lock = PTHREAD_MUTEX_INITIALIZER;
static struct catalog_snapshot {
    uint32_t thumbnail, manager, root, product, index, serial;
    unsigned context, epoch;
    int width, height;
} tracked_catalog;

void s2_catalog_track(uint32_t thumbnail)
{
    uint32_t manager = s2_touch_owner_manager();
    unsigned context = s2_touch_context();
    if ((context != S2_TOUCH_BUY && context != S2_TOUCH_BUILD) || !manager ||
        !void_vm_readable(manager, 0x48) || !void_vm_readable(thumbnail, 0x44) ||
        MEM32(thumbnail) != 0xA0E023D0u) return;
    uint32_t window = MEM32(thumbnail + 8), root = MEM32(manager + 0x44);
    if (!root || !void_vm_readable(window, 0x130) || MEM32(window + 4) != manager ||
        MEM32(window + 0x12C) != thumbnail) return;
    int width, height;
    void_input_screen_size(&width, &height);
    if (width <= 0 || height <= 0) return;
    pthread_mutex_lock(&catalog_lock);
    tracked_catalog.thumbnail = thumbnail; tracked_catalog.manager = manager; tracked_catalog.root = root;
    tracked_catalog.product = MEM32(thumbnail + 0x14); tracked_catalog.index = MEM32(thumbnail + 0x3C);
    tracked_catalog.context = context; tracked_catalog.epoch = s2_touch_layout_epoch();
    tracked_catalog.width = width; tracked_catalog.height = height;
    if (!++tracked_catalog.serial) ++tracked_catalog.serial;
    pthread_mutex_unlock(&catalog_lock);
}

uint32_t s2_catalog_clear_request(void)
{
    pthread_mutex_lock(&catalog_lock);
    uint32_t result = tracked_catalog.thumbnail ? tracked_catalog.serial : 0;
    pthread_mutex_unlock(&catalog_lock);
    return result;
}

static int catalog_mode(void)
{
    unsigned context = s2_touch_context() & ~S2_TOUCH_FOCUSED_TEXT;
    return context == S2_TOUCH_BUY || context == S2_TOUCH_BUILD;
}

static int product_popup(uint32_t popup)
{
    return popup >= 8 && void_vm_readable(popup - 8, 0x3A8) &&
        MEM32(popup - 8) == 0xA0DFCF28u && MEM32(popup) == 0xA0DFCFA8u;
}

void sub_A07495DA_lifted(void);
void sub_A07495DA(void)
{
    if (catalog_mode() && product_popup(g_ecx) && explicit_catalog_popup != g_ecx) {
        g_eax = 0;
        g_esp += 4;
        return;
    }
    sub_A07495DA_lifted();
}

static uint32_t selected_catalog(uint32_t thumbnail)
{
    uint32_t manager = s2_touch_owner_manager();
    if (!manager || !void_vm_readable(manager, 0x48) || MEM32(manager + 0x34) ||
        !catalog_mode() || !void_vm_readable(thumbnail, 0x44) ||
        MEM32(thumbnail) != 0xA0E023D0u || !MEM8(thumbnail + 50)) return 0;
    uint32_t window = MEM32(thumbnail + 8);
    if (!void_vm_readable(window, 0x130) || MEM32(window) != 0xA0E54498u ||
        MEM32(window + 4) != manager || MEM32(window + 0xF8) != 0xA0E54754u ||
        MEM32(window + 0x12C) != thumbnail || (MEM32(window + 0xE8) & 3) != 3) return 0;
    uint32_t parent = MEM32(window + 0x58);
    if (!void_vm_readable(parent, 0x130) || MEM32(parent) != 0xA0DE3704u ||
        MEM32(parent + 4) != manager || MEM32(parent + 0xF8) != 0xA0DE39C0u ||
        (MEM32(parent + 0xE8) & 3) != 3) return 0;
    uint32_t catalog = MEM32(parent + 0x12C);
    if (!void_vm_readable(catalog, 0x19C) || MEM32(catalog) != 0xA0DF7438u ||
        MEM32(catalog + 0x54) != parent || MEM32(catalog + 0x160) != 0xA0DF751Cu) return 0;
    uint32_t items = MEM32(catalog + 0x10), count = MEM32(catalog + 0x24), slot = count;
    if (!count || count > 512 || !void_vm_readable(items, count * 8)) return 0;
    for (uint32_t n = 0; n < count; ++n)
        if (MEM32(items + n * 8) == window && MEM32(items + n * 8 + 4) == thumbnail) { slot = n; break; }
    if (slot == count || MEM32(catalog + 0xA4) != 1) return 0;
    uint32_t head = MEM32(catalog + 0xA0);
    if (!void_vm_readable(head, 4)) return 0;
    uint32_t selected = MEM32(head), index = MEM32(thumbnail + 0x3C);
    if (selected == head || !void_vm_readable(selected, 0x14) || MEM32(selected + 0x10) != index) return 0;
    uint32_t products = MEM32(catalog + 0x16C), end = MEM32(catalog + 0x170), product = MEM32(thumbnail + 0x14);
    if (!product || end < products || (end - products) % 4 || end - products > 4000000 ||
        index >= (end - products) / 4 || !void_vm_readable(products + index * 4, 4) ||
        MEM32(products + index * 4) != product) return 0;
    return catalog;
}

int s2_catalog_clear_owner(uint32_t serial)
{
    pthread_mutex_lock(&catalog_lock);
    struct catalog_snapshot request = tracked_catalog;
    if (serial == tracked_catalog.serial) tracked_catalog.thumbnail = 0;
    pthread_mutex_unlock(&catalog_lock);
    if (!serial || serial != request.serial || !request.thumbnail || s2_touch_owner_manager() != request.manager ||
        !void_vm_readable(request.manager, 0x48) || MEM32(request.manager + 0x44) != request.root ||
        s2_touch_context() != request.context || s2_touch_layout_epoch() != request.epoch) return 0;
    int width, height;
    void_input_screen_size(&width, &height);
    if (width != request.width || height != request.height) return 0;
    uint32_t catalog = selected_catalog(request.thumbnail);
    if (!catalog || MEM32(request.thumbnail + 0x14) != request.product ||
        MEM32(request.thumbnail + 0x3C) != request.index) return 0;
    /* Original ClearSelection empties its selected tree and refreshes every thumbnail. */
    void_call_guest_this(0xA07486C3u, catalog, 0, NULL);
    return 1;
}

static uint32_t catalog_popup(uint32_t catalog)
{
    /* A07522C6 stores Popup+90 at catalog+18C; thumbnail+10 is its button interface. */
    uint32_t adapter = MEM32(catalog + 0x18C), popup = adapter >= 0x88 ? adapter - 0x88 : 0;
    if (!product_popup(popup) || MEM32(adapter) != 0xA0DFD000u ||
        !MEM32(popup - 8 + 0x68) || MEM32(popup - 8 + 0x60) != MEM32(catalog + 0x54)) return 0;
    return popup;
}

int s2_catalog_info(uint32_t thumbnail)
{
    if (preparing_catalog_popup) return 0;
    uint32_t catalog = selected_catalog(thumbnail);
    if (!catalog) return 0;
    uint32_t product = MEM32(thumbnail + 0x14), index = MEM32(thumbnail + 0x3C);
    uint32_t popup = catalog_popup(catalog);
    if (MEM32(catalog + 0x18C) && !popup) return 0;
    if (!popup || MEM32(popup - 8 + 0x3A4) != product) {
        uint32_t manager = s2_touch_owner_manager(), root = MEM32(manager + 0x44);
        unsigned context = s2_touch_context(), epoch = s2_touch_layout_epoch();
        int width, height, after_width, after_height;
        void_input_screen_size(&width, &height);
        if (!root || width <= 0 || height <= 0) return 0;
        /* Auto-selected Build entries have no popup until this original info action. */
        preparing_catalog_popup = 1;
        void_call_guest_this(0xA07522C6u, catalog, 1, &index);
        preparing_catalog_popup = 0;
        void_input_screen_size(&after_width, &after_height);
        if (s2_touch_owner_manager() != manager || !void_vm_readable(manager, 0x48) ||
            MEM32(manager + 0x44) != root || s2_touch_context() != context ||
            s2_touch_layout_epoch() != epoch || after_width != width || after_height != height ||
            selected_catalog(thumbnail) != catalog || MEM32(thumbnail + 0x14) != product ||
            MEM32(thumbnail + 0x3C) != index) return 0;
        popup = catalog_popup(catalog);
        if (!popup || MEM32(popup - 8 + 0x3A4) != product) return 0;
    }
    uint32_t previous = explicit_catalog_popup;
    explicit_catalog_popup = popup;
    void_call_guest_this(0xA07495DAu, popup, 0, NULL);
    explicit_catalog_popup = previous;
    return 1;
}
