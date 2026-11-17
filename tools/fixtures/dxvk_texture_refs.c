/* Exercise the production texture-proxy ownership code without a GPU. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "dxvk_types.h"

static dxvk_object objects[16];
static uint32_t words[16];
#define MEM32(a) words[(a) / 4]
static void void_fatal(const char *message) { fputs(message, stderr); abort(); }
static void native_addref(void *native) { ++((dxvk_object *)native)->com.native_refs; }
static void native_release(void *native) {
    dxvk_object *object = native;
    assert(object->com.native_refs > 0);
    --object->com.native_refs;
}
static const struct { void (*addref)(void *); void (*release)(void *); }
    bridge_storage = {native_addref, native_release}, *bridge = &bridge_storage;
static void void_com_addref(void_com_object *com) { assert(com->refs > 0); ++com->refs; }
static void void_com_release(void_com_object *com);
static dxvk_object *dxvk_object_of(uint32_t guest) {
    assert(guest >= 64 && (guest - 64) / 4 < 16);
    dxvk_object *object = &objects[(guest - 64) / 4];
    assert(object->com.refs > 0);
    return object;
}
#include "d3d9_dxvk_refs.inc"

static void void_com_release(void_com_object *com) {
    assert(com->refs > 0);
    if (--com->refs) return;
    dxvk_object *object = (dxvk_object *)com;
    dxvk_texture_refs_free(&object->texture_recording);
    dxvk_texture_refs_free(&object->textures);
    if (object->device) dxvk_object_drop(object->device);
    assert(com->native_refs == 0);
}
static dxvk_object *make(unsigned i, unsigned kind) {
    dxvk_object *object = &objects[i];
    object->kind = kind;
    object->native = object;
    object->com.guest = 64 + i * 4;
    object->com.refs = object->com.native_refs = 1;
    return object;
}
static void call(dxvk_object *object, unsigned slot, uint32_t a, uint32_t b, uint32_t result) {
    uint32_t args[] = {a, b};
    dxvk_texture_refs_prepare(object, slot);
    dxvk_texture_refs_called(object, slot, args, result);
}

int main(void) {
    dxvk_object *device = make(0, KIND_DEVICE), *a = make(1, KIND_TEXTURE), *b = make(2, KIND_CUBE);
    assert(dxvk_texture_slot(15) == 15 && dxvk_texture_slot(256) == 16 && dxvk_texture_slot(260) == 20);
    assert(dxvk_texture_slot(16) == -1 && dxvk_texture_slot(255) == -1 && dxvk_texture_slot(261) == -1);
    call(device, 65, 0, a->com.guest, 0);
    dxvk_object_drop(a); /* The game's borrowed cache pointer must still have a live COM proxy. */
    assert(a->com.refs == 1 && a->com.native_refs == 1);
    call(device, 65, 0, a->com.guest, 0);
    assert(a->com.refs == 1);
    call(device, 65, 260, a->com.guest, 0);
    call(device, 65, 0, b->com.guest, 0);
    assert(a->com.refs == 1 && b->com.refs == 2);
    call(device, 65, 260, b->com.guest, 1); /* Failed SetTexture changes no ownership. */
    assert(device->textures->slots[20] == a && b->com.refs == 2);
    call(device, SLOT_BEGIN_STATE_BLOCK, 0, 0, 1);
    assert(!device->texture_recording);
    call(device, SLOT_BEGIN_STATE_BLOCK, 0, 0, 0);
    call(device, 65, 1, a->com.guest, 0);
    call(device, 65, 2, 0, 0); /* Recording an unbind still captures that slot. */
    assert(!device->textures->slots[1] && a->com.refs == 2);
    dxvk_texture_refs *recording = device->texture_recording;
    call(device, SLOT_BEGIN_STATE_BLOCK, 0, 0, 1);
    call(device, SLOT_END_STATE_BLOCK, 0, 0, 1);
    assert(device->texture_recording == recording);
    dxvk_object *block = make(3, KIND_STATEBLOCK);
    MEM32(4) = block->com.guest;
    call(device, SLOT_END_STATE_BLOCK, 4, 0, 0);
    assert(!device->texture_recording && block->textures == recording && device->com.refs == 2);
    call(block, 5, 0, 0, 0); /* Apply only the recorded slots. */
    assert(device->textures->slots[0] == b && device->textures->slots[1] == a);
    call(device, 65, 1, b->com.guest, 0);
    call(block, 4, 0, 0, 1);
    assert(block->textures->slots[1] == a);
    call(block, 4, 0, 0, 0); /* Capture preserves the original mask. */
    assert(block->textures->slots[1] == b && block->textures->mask == 6);
    call(device, SLOT_BEGIN_STATE_BLOCK, 0, 0, 0);
    call(block, 5, 0, 0, 0); /* Applying while recording changes the new block, not current state. */
    assert(device->texture_recording->slots[1] == b && device->texture_recording->mask == 6);
    call(device, SLOT_RESET, 0, 0, 1);
    assert(device->texture_recording->mask == DXVK_TEXTURE_MASK && !device->texture_recording->slots[1]);
    assert(device->textures->slots[1] == b); /* DXVK Reset routes texture writes to its recorder. */
    dxvk_object *empty = make(4, KIND_STATEBLOCK);
    MEM32(4) = empty->com.guest;
    call(device, SLOT_END_STATE_BLOCK, 4, 0, 0);
    dxvk_object_drop(empty);
    dxvk_object *all = make(5, KIND_STATEBLOCK);
    MEM32(4) = all->com.guest;
    call(device, 59, 1, 4, 0);
    assert(all->textures->mask == DXVK_TEXTURE_MASK && all->textures->slots[20] == a);
    dxvk_object *pixel = make(6, KIND_STATEBLOCK);
    MEM32(4) = pixel->com.guest;
    call(device, 59, 2, 4, 0);
    assert(pixel->textures->mask == 0);
    dxvk_object_drop(pixel);
    call(device, SLOT_RESET, 0, 0, 1); /* Native DXVK clears bindings even on a failed Reset. */
    assert(!device->textures && a->com.refs == 1);
    call(all, 5, 0, 0, 0);
    assert(device->textures->slots[20] == a && a->com.refs == 2);
    dxvk_object_drop(all);
    dxvk_object_drop(block);
    dxvk_object_drop(b);
    dxvk_object_drop(device); /* Device destruction releases all retained texture references. */
    for (unsigned i = 0; i <= 6; ++i) assert(objects[i].com.refs == 0 && objects[i].com.native_refs == 0);
    puts("PASS DXVK texture proxies: borrowed pointers, replace/unbind, 21 samplers, failed calls, Reset, recorded/all/pixel blocks, Capture/Apply, destruction");
    return 0;
}
