/* Sims 2 caches borrowed texture pointers while the device/state block owns their bindings.
 * DXVK's private resource references keep native textures alive, but not their guest proxies. */
#define DXVK_TEXTURE_SLOTS 21u
#define DXVK_TEXTURE_MASK ((1u << DXVK_TEXTURE_SLOTS) - 1u)
struct dxvk_texture_refs { dxvk_object *slots[DXVK_TEXTURE_SLOTS]; uint32_t mask; };

static void dxvk_object_hold(dxvk_object *object)
{
    if (object) { bridge->addref(object->native); void_com_addref(&object->com); }
}

static void dxvk_object_drop(dxvk_object *object)
{
    if (object) { bridge->release(object->native); void_com_release(&object->com); }
}

static dxvk_texture_refs *dxvk_texture_refs_get(dxvk_texture_refs **at)
{
    if (!*at) {
        *at = (dxvk_texture_refs *)calloc(1, sizeof **at);
        if (!*at) void_fatal("DXVK texture reference allocation failed");
    }
    return *at;
}

static void dxvk_texture_refs_set(dxvk_texture_refs *refs, unsigned slot, dxvk_object *object)
{
    refs->mask |= 1u << slot;
    if (refs->slots[slot] == object) return;
    dxvk_object_hold(object);
    dxvk_object *old = refs->slots[slot];
    refs->slots[slot] = object;
    dxvk_object_drop(old);
}

static void dxvk_texture_refs_free(dxvk_texture_refs **at)
{
    dxvk_texture_refs *refs = *at;
    if (!refs) return;
    *at = NULL;
    for (unsigned i = 0; i < DXVK_TEXTURE_SLOTS; ++i) dxvk_object_drop(refs->slots[i]);
    free(refs);
}

static void dxvk_texture_refs_copy(dxvk_texture_refs *to, const dxvk_texture_refs *from, uint32_t mask)
{
    for (unsigned i = 0; i < DXVK_TEXTURE_SLOTS; ++i)
        if (mask & (1u << i)) dxvk_texture_refs_set(to, i, from ? from->slots[i] : NULL);
}

static int dxvk_texture_slot(uint32_t stage)
{
    return stage < 16u ? (int)stage : stage >= 256u && stage <= 260u ? (int)(stage - 240u) : -1;
}

/* Allocate before native mutation so allocation failure cannot leave an untracked binding. */
static void dxvk_texture_refs_prepare(dxvk_object *object, unsigned slot)
{
    if (object->kind != KIND_DEVICE) return;
    if (slot == 65) dxvk_texture_refs_get(object->texture_recording ? &object->texture_recording : &object->textures);
}

static void dxvk_texture_refs_called(dxvk_object *object, unsigned slot, const uint32_t *args, uint32_t result)
{
    /* Pinned DXVK clears bindings even when Reset fails; an active recorder receives those writes. */
    if (object->kind == KIND_DEVICE && slot == SLOT_RESET) {
        if (object->texture_recording) dxvk_texture_refs_copy(object->texture_recording, NULL, DXVK_TEXTURE_MASK);
        else dxvk_texture_refs_free(&object->textures);
        return;
    }
    if (result) return;
    if (object->kind == KIND_DEVICE) {
        if (slot == 65) { /* SetTexture: recording updates the block, not the current device. */
            int index = dxvk_texture_slot(args[0]);
            if (index >= 0) dxvk_texture_refs_set(object->texture_recording ? object->texture_recording : object->textures,
                (unsigned)index, args[1] ? dxvk_object_of(args[1]) : NULL);
        } else if (slot == SLOT_BEGIN_STATE_BLOCK) {
            dxvk_texture_refs_get(&object->texture_recording);
        } else if (slot == 59 || slot == SLOT_END_STATE_BLOCK) { /* CreateStateBlock / EndStateBlock */
            uint32_t out = args[slot == 59 ? 1 : 0];
            dxvk_object *block = out && MEM32(out) ? dxvk_object_of(MEM32(out)) : NULL;
            if (!block) return;
            block->device = object;
            dxvk_object_hold(object);
            if (slot == SLOT_END_STATE_BLOCK) {
                block->textures = object->texture_recording;
                object->texture_recording = NULL;
            } else {
                /* Pinned DXVK CaptureType captures textures only for D3DSBT_ALL. */
                dxvk_texture_refs_copy(dxvk_texture_refs_get(&block->textures), object->textures,
                    args[0] == 1u ? DXVK_TEXTURE_MASK : 0u);
            }
        }
    } else if (object->kind == KIND_STATEBLOCK && object->device && object->textures) {
        if (slot == 4) /* Capture keeps the original block's recorded mask. */
            dxvk_texture_refs_copy(object->textures, object->device->textures, object->textures->mask);
        if (slot == 5)
            dxvk_texture_refs_copy(dxvk_texture_refs_get(object->device->texture_recording ?
                &object->device->texture_recording : &object->device->textures), object->textures, object->textures->mask);
    }
}
