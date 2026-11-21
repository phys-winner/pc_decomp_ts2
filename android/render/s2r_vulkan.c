/* Vulkan 1.1 backend of the native renderers (Painkiller's pkr_vulkan.c,
 * wiki note 23, as Heroes V's h5r_vulkan.c).
 *
 * Programs come from the build-time library (the fixed-function shaders and
 * the game shaders it knows, as SPIR-V) or, for a game shader translated on
 * the device, from glslang once (s2r_glslang.cpp, the SPIR-V kept in the app
 * cache); variants are specialization constants.
 * The same top-down storage (vpFlip = -1), D3D9 half-pixel offset and
 * component swizzles as the OpenGL ES backend, so both draw the same picture.
 * Vulkan's depth range and framebuffer origin already match Direct3D's:
 * shaders keep z, and the present pass needs no flip.
 *
 * Recording runs on the port's render thread (d3d9_thread.c), which replays
 * what the game's thread recorded. Each frame records two command buffers:
 * `pre` (uploads and layout changes of images the frame has not touched yet;
 * it executes first) and `main` (render passes in draw order). Render passes
 * begin lazily; a full clear as the first operation becomes the pass's CLEAR
 * load, which tilers resolve without reading memory. Streaming data
 * (uniforms, staging, DrawPrimitiveUP vertices) lives in persistently mapped
 * rings whose chunks remember the submission serial that used them last. */
#define VK_USE_PLATFORM_ANDROID_KHR 1
#define VK_NO_PROTOTYPES 1
#include "s2r_internal.h"
#include "s2r_spec.h"
#include <vulkan/vulkan.h>
#include "s2r_vkdriver.h"
#include <android/native_window.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void *void_android_window_current(uint32_t *serial, int wait);
void void_android_window_ack(uint32_t serial);
void *void_android_window_acquire(uint32_t *serial);
void void_android_gpu_strings(const char *renderer, const char *version) __attribute__((weak));

#define LOGVK(...) void_log("[s2r-vk] " __VA_ARGS__)

/* Every Vulkan function the renderer calls, from the driver s2r_vkdriver.c opened (the system loader
 * or an app-local Mesa driver): static pointers named as the functions, so the code reads as usual. */
#define S2R_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define S2R_VK_INSTANCE(X) \
    X(vkCreateAndroidSurfaceKHR) \
    X(vkCreateDevice) \
    X(vkDestroySurfaceKHR) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkDestroyInstance) \
    X(vkDestroyDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceProperties2)
#define S2R_VK_DEVICE(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginQuery) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBlitImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdEndQuery) \
    X(vkCmdEndRenderPass) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPushConstants) \
    X(vkCmdResetQueryPool) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewport) \
    X(vkCmdWriteTimestamp) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateFence) \
    X(vkCreateFramebuffer) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateQueryPool) \
    X(vkCreateRenderPass) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyFramebuffer) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyPipeline) \
    X(vkDestroyRenderPass) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceQueue) \
    X(vkGetFenceStatus) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetPipelineCacheData) \
    X(vkGetQueryPoolResults) \
    X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkResetCommandPool) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)
#define S2R_VK_DECLARE(name) static PFN_##name name;
S2R_VK_GLOBAL(S2R_VK_DECLARE)
S2R_VK_INSTANCE(S2R_VK_DECLARE)
S2R_VK_DEVICE(S2R_VK_DECLARE)
static PFN_vkGetInstanceProcAddr vk_gipa;
#define FRAMES 2
#define MAX_IMAGES 8
#define QUERIES_PER_SLOT 1024u

/* ---------------------------------------------------------------- memory */
typedef struct vk_mem { VkDeviceMemory mem; VkDeviceSize offset, size; int block; uint8_t *map; } vk_mem;

typedef struct vk_block {
    VkDeviceMemory mem;
    VkDeviceSize size;
    uint32_t type;
    int linear, dedicated, live;
    uint8_t *map;
    VkDeviceSize *free_off, *free_size;
    uint32_t free_count, free_cap;
} vk_block;

/* ---------------------------------------------------------------- objects */
typedef struct vk_tex {
    VkImage image;
    vk_mem mem;
    VkImageView view;             /* sampled: all levels/layers, with the format's swizzle */
    VkImageView att[6];           /* attachment views: level 0, one per face */
    VkFormat format;
    int kind, depth, stencil, compressed, render_target, is_bb;
    uint32_t width, height, layers, levels, depth3d, samples;
    VkImageLayout layout;         /* of every subresource, after everything recorded so far */
    uint32_t used;                /* serial of the main command buffer that last used it */
    /* Scene MSAA: a screen-sized target's multisampled twin, transient (lazily allocated memory
       where the driver has it: tile memory only), resolved into `image` as a pass ends. */
    VkImage ms_image;
    vk_mem ms_mem;
    VkImageView ms_view;
    uint32_t ms_samples;
    int screen_sized, ms_failed;
} vk_tex;

typedef struct vk_buf_version { VkBuffer buf; vk_mem mem; uint32_t used; } vk_buf_version;
typedef struct vk_buf {
    vk_buf_version v[16];
    int count, cur;
    uint32_t size;
    VkBufferUsageFlags usage;
    struct vk_buf *next_multi;    /* in V.multi while it has more than one version (buf_trim) */
    int listed;
} vk_buf;

typedef struct vk_module {
    int pixel;
    const s2r_lib_shader *lib;
    uint32_t ff_hash;
    uint64_t variant;
    uint8_t ff[96];
    VkShaderModule module;        /* owned by the library entry or the builtin cache */
    uint32_t uid;
    uint32_t inputs;              /* vertex input locations the stage declares */
    uint32_t samplers;            /* descriptor bindings (4..23) of samplers it declares */
    uint64_t sampler_types;       /* 2 bits per binding: 0 2D, 1 cube, 2 3D */
    uint32_t spec_count;
    VkSpecializationMapEntry spec_map[112];
    int32_t spec_data[112];
    struct vk_module *next;
} vk_module;

typedef struct vk_pipe_key {
    uint32_t vs_uid, ps_uid;
    uint32_t cfmt, dfmt, samples;
    uint32_t xfmt[3];             /* render targets 1-3 (MRT) */
    uint8_t xcount, xwmask[3];
    uint8_t topology, cull, ztest, zwrite, zfunc, stencil, twosided, has_color, has_depth, bias;
    uint8_t sfail, szfail, spass, sfunc, bfail, bzfail, bpass, bfunc;
    uint8_t blend, src, dst, op, asrc, adst, aop, wmask, nattr, pad[3];
    struct { uint8_t loc, binding, format, pad; uint16_t offset, pad2; } attr[16];
    uint16_t stride[17];
    uint32_t bindings;            /* bit per used binding (16: the constant input) */
    uint32_t instanced;           /* bit per binding read per instance (D3DSTREAMSOURCE_INSTANCEDATA) */
} vk_pipe_key;

typedef struct vk_pipe { vk_pipe_key key; uint32_t hash; VkPipeline pipe; struct vk_pipe *next; } vk_pipe;

typedef struct vk_rp_key {
    VkFormat cfmt, dfmt;
    uint32_t samples;             /* > 1: multisampled colour and depth, colour resolved into the target */
    uint8_t cload, dload, xcount, pad;
    VkImageLayout cinit, cfinal, dinit, dfinal;
    VkFormat xfmt[3];             /* render targets 1-3 (MRT): loaded like 0, in and out as colour/sampled */
} vk_rp_key;

typedef struct vk_query { uint32_t type, index, serial; int active, split, in_pass; } vk_query;

typedef struct vk_deferred {
    uint32_t serial;
    VkImage image; VkImageView views[7]; VkBuffer buffer; VkFramebuffer fb;
    vk_mem mem;
} vk_deferred;

typedef struct vk_ring {
    VkBuffer buf;
    vk_mem mem;
    uint32_t size, chunk, chunks, used, cur;
    uint32_t chunk_serial[64];
} vk_ring;

/* A frame's commands: `pre` (uploads) and `main` (the scene) in one batch with `fence`; the
 * present pass in `present`, a batch of its own with `present_fence` that alone waits for the
 * swapchain image. The game's event queries wait for the scene batch only: with one batch the
 * acquire semaphore held the whole scene back until the display released an image (68 fps with
 * 4.9 ms a frame in GetData polls on the Adreno 650 test phone, 2026-11-20). */
typedef struct vk_slot {
    VkCommandPool pool;
    VkCommandBuffer pre, main, present;
    VkFence fence, present_fence;
    VkSemaphore acquire;
    VkDescriptorPool dpools[16];
    int dpool_count, dpool_cur;
    uint32_t serial;
    int submitted, scene_done, present_submitted;
} vk_slot;

typedef struct vk_target {
    vk_tex *color, *depth;
    uint32_t face;
    uint32_t w, h;
    vk_tex *extra[3];             /* render targets 1-3 (MRT), level 0, the size of colour 0 */
    uint32_t xcount;
    float scale;                  /* stored pixels per Direct3D pixel (the render scale) */
    uint32_t samples;             /* scene MSAA of this pass (1: none) */
} vk_target;

typedef struct vk_state {
    int ready;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties memprops;
    VkDevice dev;
    VkQueue queue;
    uint32_t queue_family;
    VkFormat depth_format;
    int has_aniso, has_etc2, has_astc;
    float max_aniso;
    uint32_t ubo_align;
    vk_block *blocks; int block_count, block_cap;
    /* window */
    ANativeWindow *window;
    uint32_t window_serial;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    VkSurfaceTransformFlagBitsKHR sc_transform;
    uint32_t sc_count;
    VkImage sc_images[MAX_IMAGES];
    VkImageView sc_views[MAX_IMAGES];
    VkFramebuffer sc_fbs[MAX_IMAGES];
    VkSemaphore sc_done[MAX_IMAGES];
    int sc_dirty, sc_fifo;
    uint32_t win_w, win_h;
    /* frames */
    vk_slot slots[FRAMES];
    int cur;
    uint32_t serial, completed;   /* completed: whole slots (resources); scene_completed: scene batches (queries) */
    uint32_t scene_completed;
    vk_ring uniforms, staging, stream;
    /* pass state */
    int pass_active;
    vk_target pass;
    /* caches */
    struct { vk_rp_key key; VkRenderPass rp; } rps[64];
    int rp_count;
    struct { VkImageView views[6]; uint32_t count, w, h; VkFramebuffer fb; } fbs[128];
    int fb_count;
    vk_module *modules[256];
    uint32_t next_uid;
    vk_pipe *pipes[1024];
    int pipe_count, pipes_new;
    VkPipelineCache pcache;
    struct { uint64_t key; VkSampler s; } samplers[256];
    int sampler_count;
    VkDescriptorSetLayout dsl, present_dsl;
    VkPipelineLayout layout, present_layout;
    VkPipeline present_pipe;
    VkRenderPass present_rp;
    VkFormat present_format;
    VkSampler present_sampler;
    /* the present pass: the gamma table (identity: the game sets no ramp) */
    vk_tex gamma_lut;
    uint32_t gamma_serial;
    int gamma_valid;
    int has_bc;
    vk_tex dummy[3];              /* black 2D, cube, 3D */
    VkBuffer default_vb; vk_mem default_vb_mem;
    VkQueryPool qpool;
    VkQueryPool tpool;            /* two timestamps a slot: the scene batch's start and end (GPU time) */
    int ts_ok;
    double ts_ns;                 /* nanoseconds per timestamp tick */
    uint32_t qnext[FRAMES];
    /* bound state of the main command buffer */
    VkPipeline cur_pipe;
    VkDescriptorSet cur_set;
    uint64_t cur_set_sig;
    VkBuffer cur_vb[17]; VkDeviceSize cur_vb_off[17];
    VkBuffer cur_ib; VkDeviceSize cur_ib_off; VkIndexType cur_ib_type;
    VkViewport cur_vp; VkRect2D cur_sc; uint32_t cur_ref, cur_cmask, cur_wmask, cur_blend;
    float cur_bias[2];
    int dyn_valid;
    vk_deferred *deferred; int deferred_count, deferred_cap;
    vk_buf *multi;                /* buffers with more than one version (released when idle, buf_trim) */
    VkBuffer rb_buf; vk_mem rb_mem; uint32_t rb_size;
    char cache_dir[1024];
    uint32_t vtype_float;
    VkShaderModule builtin_modules[S2R_BUILTIN_COUNT];
    uint64_t gpu_bytes;
    /* scene MSAA */
    uint32_t msaa_samples;
    int lazy_memory;
    vk_tex reload_img;            /* a copy of a multisampled pass's target that loads (its first draw fills the samples) */
    struct { VkFormat cfmt, dfmt; uint32_t samples; VkPipeline pipe; } reloads[8];
    int reload_count;
    /* the present's filters (s2r_filter.c): output-size images used in turn */
    vk_tex filter_img[2];
    uint32_t filter_w, filter_h;
    VkPipeline filter_pipe[S2R_FILTER_COUNT];
    int filter_failed[S2R_FILTER_COUNT];
    VkShaderModule filter_vs, filter_fs[S2R_FILTER_COUNT];
    VkDescriptorSetLayout filter_dsl;
    VkPipelineLayout filter_layout;
    VkSampler filter_sampler;
    VkPhysicalDeviceFeatures have;   /* what the device offers */
    int driver;                      /* S2R_VK_*: the driver this session runs on */
} vk_state;

static vk_state V;
static vk_query *open_query;

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 60) LOGVK("%s failed: %d", what, (int)r);
    }
}

/* =============================================================== memory */
static int find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < V.memprops.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (V.memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static void block_free_insert(vk_block *b, VkDeviceSize off, VkDeviceSize size)
{
    if (!size) return;
    uint32_t i = 0;
    while (i < b->free_count && b->free_off[i] < off) ++i;
    int merged_prev = i > 0 && b->free_off[i - 1] + b->free_size[i - 1] == off;
    int merged_next = i < b->free_count && off + size == b->free_off[i];
    if (merged_prev && merged_next) {
        b->free_size[i - 1] += size + b->free_size[i];
        memmove(b->free_off + i, b->free_off + i + 1, (b->free_count - i - 1) * sizeof *b->free_off);
        memmove(b->free_size + i, b->free_size + i + 1, (b->free_count - i - 1) * sizeof *b->free_size);
        --b->free_count;
    } else if (merged_prev) b->free_size[i - 1] += size;
    else if (merged_next) { b->free_off[i] = off; b->free_size[i] += size; }
    else {
        if (b->free_count == b->free_cap) {
            b->free_cap = b->free_cap ? b->free_cap * 2 : 16;
            b->free_off = (VkDeviceSize *)realloc(b->free_off, b->free_cap * sizeof *b->free_off);
            b->free_size = (VkDeviceSize *)realloc(b->free_size, b->free_cap * sizeof *b->free_size);
        }
        memmove(b->free_off + i + 1, b->free_off + i, (b->free_count - i) * sizeof *b->free_off);
        memmove(b->free_size + i + 1, b->free_size + i, (b->free_count - i) * sizeof *b->free_size);
        b->free_off[i] = off; b->free_size[i] = size;
        ++b->free_count;
    }
}

static int block_take(vk_block *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *out)
{
    for (uint32_t i = 0; i < b->free_count; ++i) {
        VkDeviceSize off = b->free_off[i], fsize = b->free_size[i];
        VkDeviceSize at = (off + align - 1) / align * align;
        if (at + size > off + fsize) continue;
        VkDeviceSize tail = off + fsize - (at + size);
        if (at > off) {
            b->free_size[i] = at - off;
            if (tail) block_free_insert(b, at + size, tail);
        } else if (tail) { b->free_off[i] = at + size; b->free_size[i] = tail; }
        else {
            memmove(b->free_off + i, b->free_off + i + 1, (b->free_count - i - 1) * sizeof *b->free_off);
            memmove(b->free_size + i, b->free_size + i + 1, (b->free_count - i - 1) * sizeof *b->free_size);
            --b->free_count;
        }
        *out = at;
        return 1;
    }
    return 0;
}

static int mem_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, int linear, vk_mem *out)
{
    memset(out, 0, sizeof *out);
    int type = find_type(req->memoryTypeBits, want);
    if (type < 0 && (want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        type = find_type(req->memoryTypeBits, want & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) { LOGVK("no memory type for bits %X flags %X", req->memoryTypeBits, want); return 0; }
    int host = (V.memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    VkDeviceSize block_size = linear ? (16u << 20) : (64u << 20);
    int dedicated = req->size > block_size / 2;
    if (!dedicated) {
        for (int i = 0; i < V.block_count; ++i) {
            vk_block *b = &V.blocks[i];
            if (!b->live || b->dedicated || b->type != (uint32_t)type || b->linear != linear) continue;
            VkDeviceSize at;
            if (block_take(b, req->size, req->alignment ? req->alignment : 1, &at)) {
                out->mem = b->mem; out->offset = at; out->size = req->size; out->block = i;
                out->map = b->map ? b->map + at : NULL;
                return 1;
            }
        }
    }
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = dedicated ? req->size : block_size;
    ai.memoryTypeIndex = (uint32_t)type;
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(V.dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) { LOGVK("vkAllocateMemory %llu bytes type %d failed: %d", (unsigned long long)ai.allocationSize, type, (int)r); return 0; }
    V.gpu_bytes += ai.allocationSize;
    int index = -1;
    for (int i = 0; i < V.block_count; ++i) if (!V.blocks[i].live) { index = i; break; }
    if (index < 0) {
        if (V.block_count == V.block_cap) {
            V.block_cap = V.block_cap ? V.block_cap * 2 : 64;
            V.blocks = (vk_block *)realloc(V.blocks, (size_t)V.block_cap * sizeof *V.blocks);
        }
        index = V.block_count++;
    }
    vk_block *b = &V.blocks[index];
    memset(b, 0, sizeof *b);
    b->mem = mem; b->size = ai.allocationSize; b->type = (uint32_t)type; b->linear = linear;
    b->dedicated = dedicated; b->live = 1;
    if (host) {
        void *p = NULL;
        check(vkMapMemory(V.dev, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
        b->map = (uint8_t *)p;
    }
    if (dedicated) {
        out->mem = mem; out->offset = 0; out->size = req->size; out->block = index; out->map = b->map;
        return 1;
    }
    block_free_insert(b, 0, b->size);
    VkDeviceSize at;
    block_take(b, req->size, req->alignment ? req->alignment : 1, &at);
    out->mem = mem; out->offset = at; out->size = req->size; out->block = index;
    out->map = b->map ? b->map + at : NULL;
    return 1;
}

/* A block nothing lives in any more goes back to the driver, unless it is the last of its kind
 * (the next allocation would only make it again). Blocks used to stay until the device went away:
 * textures and buffers of a lot left behind 64 MiB blocks after the lot was gone. */
static void block_release_if_empty(int index)
{
    vk_block *b = &V.blocks[index];
    if (!b->live || b->dedicated || b->free_count != 1 || b->free_off[0] != 0 || b->free_size[0] != b->size) return;
    int others = 0;
    for (int i = 0; i < V.block_count; ++i)
        if (i != index && V.blocks[i].live && !V.blocks[i].dedicated && V.blocks[i].type == b->type &&
            V.blocks[i].linear == b->linear) ++others;
    if (!others) return;
    if (b->map) vkUnmapMemory(V.dev, b->mem);
    vkFreeMemory(V.dev, b->mem, NULL);
    V.gpu_bytes -= b->size;
    free(b->free_off); free(b->free_size);
    memset(b, 0, sizeof *b);
}

static void mem_free_now(vk_mem *m)
{
    if (!m->mem) return;
    vk_block *b = &V.blocks[m->block];
    if (b->dedicated) {
        vkFreeMemory(V.dev, b->mem, NULL);
        V.gpu_bytes -= b->size;
        free(b->free_off); free(b->free_size);
        memset(b, 0, sizeof *b);
    } else {
        block_free_insert(b, m->offset, m->size);
        block_release_if_empty(m->block);
    }
    memset(m, 0, sizeof *m);
}

/* ============================================================ deferred */
static vk_deferred *defer_new(void)
{
    if (V.deferred_count == V.deferred_cap) {
        V.deferred_cap = V.deferred_cap ? V.deferred_cap * 2 : 256;
        V.deferred = (vk_deferred *)realloc(V.deferred, (size_t)V.deferred_cap * sizeof *V.deferred);
    }
    vk_deferred *e = &V.deferred[V.deferred_count++];
    memset(e, 0, sizeof *e);
    e->serial = V.serial;
    return e;
}

static void deferred_run(int all)
{
    int w = 0;
    for (int i = 0; i < V.deferred_count; ++i) {
        vk_deferred *e = &V.deferred[i];
        if (!all && e->serial > V.completed) { V.deferred[w++] = *e; continue; }
        for (int k = 0; k < 7; ++k) if (e->views[k]) vkDestroyImageView(V.dev, e->views[k], NULL);
        if (e->fb) vkDestroyFramebuffer(V.dev, e->fb, NULL);
        if (e->image) vkDestroyImage(V.dev, e->image, NULL);
        if (e->buffer) vkDestroyBuffer(V.dev, e->buffer, NULL);
        mem_free_now(&e->mem);
    }
    V.deferred_count = w;
}

/* ============================================================== frames */
static void reset_bound_state(void)
{
    V.cur_pipe = VK_NULL_HANDLE;
    V.cur_set = VK_NULL_HANDLE;
    V.cur_set_sig = 0;
    memset(V.cur_vb, 0, sizeof V.cur_vb);
    V.cur_ib = VK_NULL_HANDLE;
    V.dyn_valid = 0;
}

static VkCommandBuffer pre_cb(void) { return V.slots[V.cur].pre; }
static VkCommandBuffer main_cb(void) { return V.slots[V.cur].main; }

static void slot_begin(void)
{
    vk_slot *s = &V.slots[V.cur];
    check(vkResetCommandPool(V.dev, s->pool, 0), "vkResetCommandPool");
    for (int i = 0; i < s->dpool_count; ++i) vkResetDescriptorPool(V.dev, s->dpools[i], 0);
    s->dpool_cur = 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s->pre, &bi);
    vkBeginCommandBuffer(s->main, &bi);
    vkCmdResetQueryPool(s->pre, V.qpool, (uint32_t)V.cur * QUERIES_PER_SLOT, QUERIES_PER_SLOT);
    if (V.ts_ok) {
        vkCmdResetQueryPool(s->pre, V.tpool, (uint32_t)V.cur * 2u, 2);
        vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, V.tpool, (uint32_t)V.cur * 2u);
    }
    V.qnext[V.cur] = 0;
    reset_bound_state();
}

/* 1 when the fence has signalled (reset then); with `block`, waits for it. */
static int fence_done(VkFence *f, int block)
{
    VkResult r = vkGetFenceStatus(V.dev, *f);
    if (r == VK_NOT_READY) {
        if (!block) return 0;
        d3d_rt_wait_begin();
        r = vkWaitForFences(V.dev, 1, f, VK_TRUE, UINT64_MAX);
        d3d_rt_wait_end();
    }
    check(r, "vkWaitForFences");
    vkResetFences(V.dev, 1, f);
    return 1;
}

/* A submitted slot's batches: the scene batch done answers queries, both done free its resources. */
static void slot_check(vk_slot *s, int block)
{
    if (!s->submitted) return;
    if (!s->scene_done) {
        if (!fence_done(&s->fence, block)) return;
        s->scene_done = 1;
        if (s->serial > V.scene_completed) V.scene_completed = s->serial;
        if (V.ts_ok && s2r_dev) {
            uint64_t ts[2];
            uint32_t slot = (uint32_t)(s - V.slots);
            if (vkGetQueryPoolResults(V.dev, V.tpool, slot * 2u, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[1] > ts[0]) {
                s2r_dev->gpu_ns += (uint64_t)((double)(ts[1] - ts[0]) * V.ts_ns);
                ++s2r_dev->gpu_frames;
            }
        }
    }
    if (s->present_submitted) {
        if (!fence_done(&s->present_fence, block)) return;
        s->present_submitted = 0;
    }
    s->submitted = 0;
    if (s->serial > V.completed) V.completed = s->serial;
    deferred_run(0);
}

static void slot_wait(int slot) { slot_check(&V.slots[slot], 1); }

/* The submitted slots the GPU has finished, without waiting. */
static void poll_completed(void)
{
    for (int i = 0; i < FRAMES; ++i) slot_check(&V.slots[i], 0);
}

static void pass_end(void);

static void slot_submit(VkSemaphore wait, VkSemaphore signal)
{
    pass_end();
    vk_slot *s = &V.slots[V.cur];
    if (V.ts_ok) vkCmdWriteTimestamp(s->main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.tpool, (uint32_t)V.cur * 2u + 1u);
    vkEndCommandBuffer(s->pre);
    vkEndCommandBuffer(s->main);
    VkCommandBuffer cbs[2] = { s->pre, s->main };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = wait ? 1 : 0; si.pWaitSemaphores = &wait; si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 2; si.pCommandBuffers = cbs;
    si.signalSemaphoreCount = signal ? 1 : 0; si.pSignalSemaphores = &signal;
    check(vkQueueSubmit(V.queue, 1, &si, s->fence), "vkQueueSubmit");
    s->serial = V.serial;
    s->submitted = 1;
    s->scene_done = 0;
    s->present_submitted = 0;
    ++V.serial;
}

/* Mid-frame: everything recorded so far reaches the GPU and completes. */
static void flush_wait(void)
{
    slot_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
    slot_wait(V.cur);
    slot_begin();
}

static void wait_serial(uint32_t serial)
{
    if (serial <= V.completed) return;
    if (serial >= V.serial) { flush_wait(); return; }
    for (int i = 0; i < FRAMES; ++i) if (V.slots[i].submitted && V.slots[i].serial <= serial) slot_wait(i);
    for (int i = 0; i < FRAMES && serial > V.completed; ++i) if (V.slots[i].submitted) slot_wait(i);
}

/* =============================================================== rings */
static int ring_create(vk_ring *r, uint32_t size, uint32_t chunk, VkBufferUsageFlags usage, uint32_t slack)
{
    memset(r, 0, sizeof *r);
    r->size = size; r->chunk = chunk; r->chunks = size / chunk;
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size + slack; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(V.dev, &bi, NULL, &r->buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev, r->buf, &req);
    if (!mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, &r->mem)) return 0;
    vkBindBufferMemory(V.dev, r->buf, r->mem.mem, r->mem.offset);
    return r->mem.map != NULL;
}

static uint64_t ring_bytes[3], ring_bytes_start[3], ring_frame_max[3], ring_frame_start[3];

/* Every 600 frames: what the uniform, staging and stream rings took a frame (their size is a
   choice: a ring a frame outgrows waits for the GPU). */
static void ring_report(uint32_t frame)
{
    for (int i = 0; i < 3; ++i) {
        uint64_t used = ring_bytes[i] - ring_frame_start[i];
        if (used > ring_frame_max[i]) ring_frame_max[i] = used;
        ring_frame_start[i] = ring_bytes[i];
    }
    if (frame % 600) return;
    LOGVK("rings a frame: uniforms %.1f KiB average, %.1f KiB peak of %u KiB; staging %.1f / %.1f of %u KiB; stream %.1f / %.1f of %u KiB",
        (double)(ring_bytes[0] - ring_bytes_start[0]) / 600.0 / 1024.0, (double)ring_frame_max[0] / 1024.0, V.uniforms.size >> 10,
        (double)(ring_bytes[1] - ring_bytes_start[1]) / 600.0 / 1024.0, (double)ring_frame_max[1] / 1024.0, V.staging.size >> 10,
        (double)(ring_bytes[2] - ring_bytes_start[2]) / 600.0 / 1024.0, (double)ring_frame_max[2] / 1024.0, V.stream.size >> 10);
    for (int i = 0; i < 3; ++i) { ring_bytes_start[i] = ring_bytes[i]; ring_frame_max[i] = 0; }
}

/* Space for `size` bytes aligned to `align`; returns the offset. */
static uint32_t ring_alloc(vk_ring *r, uint32_t size, uint32_t align)
{
    ring_bytes[r == &V.uniforms ? 0 : r == &V.staging ? 1 : 2] += size;
    uint32_t at = (r->used + align - 1) / align * align;
    if (at + size > (r->cur + 1) * r->chunk) {
        r->chunk_serial[r->cur] = V.serial;
        uint32_t next = (r->cur + 1) % r->chunks;
        wait_serial(r->chunk_serial[next]);
        r->cur = next;
        at = next * r->chunk;
    }
    r->used = at + size;
    r->chunk_serial[r->cur] = V.serial;
    return at;
}

/* ============================================================= formats */
static VkFormat vk_format(uint8_t gpu)
{
    switch (gpu) {
    case S2R_GPU_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case S2R_GPU_RG8: return VK_FORMAT_R8G8_UNORM;
    case S2R_GPU_R8: return VK_FORMAT_R8_UNORM;
    case S2R_GPU_RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case S2R_GPU_RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case S2R_GPU_RGB565: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case S2R_GPU_RGBA4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case S2R_GPU_RGB5A1: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case S2R_GPU_RGB10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case S2R_GPU_R16F: return VK_FORMAT_R16_SFLOAT;
    case S2R_GPU_RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case S2R_GPU_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case S2R_GPU_R32F: return VK_FORMAT_R32_SFLOAT;
    case S2R_GPU_RG32F: return VK_FORMAT_R32G32_SFLOAT;
    case S2R_GPU_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case S2R_GPU_D16: return VK_FORMAT_D16_UNORM;
    case S2R_GPU_D24S8: case S2R_GPU_D24X8: return V.depth_format;
    case S2R_GPU_D32F: return VK_FORMAT_D32_SFLOAT;
    case S2R_GPU_ETC2_RGB: return VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK;
    case S2R_GPU_ETC2_RGBA: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    case S2R_GPU_ASTC_4x4: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case S2R_GPU_ASTC_5x5: return VK_FORMAT_ASTC_5x5_UNORM_BLOCK;
    case S2R_GPU_ASTC_6x6: return VK_FORMAT_ASTC_6x6_UNORM_BLOCK;
    /* DXT as it is where the device samples BC (V.has_bc). Without these the image became RGBA8 and
       the blocks were sampled as pixels: coloured noise on Adreno 7xx (system driver and Turnip). */
    case S2R_GPU_BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case S2R_GPU_BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case S2R_GPU_BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

static VkComponentSwizzle vk_swizzle(uint8_t s)
{
    static const VkComponentSwizzle map[6] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE };
    return s < 6 ? map[s] : VK_COMPONENT_SWIZZLE_ZERO;
}

static int format_has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT; }
static int format_is_depth(VkFormat f)
{
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_X8_D24_UNORM_PACK32;
}

static VkImageAspectFlags aspect_of(const vk_tex *t)
{
    if (!t->depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (format_has_stencil(t->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

/* ============================================================ barriers */
static void layout_info(VkImageLayout l, VkPipelineStageFlags *stage, VkAccessFlags *access)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_UNDEFINED: *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; *access = 0; break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = VK_ACCESS_TRANSFER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT; *access = VK_ACCESS_SHADER_READ_BIT; break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; break;
    default: *stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT; *access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT; break;
    }
}

static void barrier(VkCommandBuffer cb, vk_tex *t, VkImageLayout to)
{
    VkImageLayout from = t->layout;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkPipelineStageFlags ss, ds;
    layout_info(from, &ss, &b.srcAccessMask);
    layout_info(to, &ds, &b.dstAccessMask);
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange.aspectMask = aspect_of(t);
    b.subresourceRange.levelCount = t->levels;
    b.subresourceRange.layerCount = t->layers;
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
    t->layout = to;
}

static void transition(VkCommandBuffer cb, vk_tex *t, VkImageLayout to)
{
    if (t->layout == to) return;
    barrier(cb, t, to);
}

/* The command buffer for work on `t`: `pre` while the frame has not used it,
 * else `main` outside a render pass. */
static VkCommandBuffer cb_for(vk_tex *t)
{
    if (t->used != V.serial) return pre_cb();
    pass_end();
    return main_cb();
}

/* ============================================================ textures */
static int tex_init(vk_tex *t, int kind, VkFormat format, uint32_t w, uint32_t h, uint32_t d3, uint32_t levels,
    const uint8_t sw[4], int render_target, int depth, int compressed)
{
    memset(t, 0, sizeof *t);
    t->kind = kind; t->format = format; t->width = w; t->height = h; t->levels = levels ? levels : 1;
    t->layers = kind == S2R_TEX_CUBE ? 6 : 1; t->depth3d = kind == S2R_TEX_3D ? (d3 ? d3 : 1) : 1;
    t->render_target = render_target; t->depth = depth; t->samples = 1;
    t->stencil = depth && format_has_stencil(format);
    t->compressed = compressed;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = kind == S2R_TEX_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w; ci.extent.height = h; ci.extent.depth = t->depth3d;
    ci.mipLevels = t->levels; ci.arrayLayers = t->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    if (depth) ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    else {
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (render_target) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    ci.flags = kind == S2R_TEX_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(V.dev, &ci, NULL, &t->image);
    if (r != VK_SUCCESS) { LOGVK("vkCreateImage %ux%u format %d usage %X failed: %d", w, h, (int)format, ci.usage, (int)r); return 0; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.dev, t->image, &req);
    if (!mem_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)) {
        vkDestroyImage(V.dev, t->image, NULL); t->image = VK_NULL_HANDLE; return 0;
    }
    vkBindImageMemory(V.dev, t->image, t->mem.mem, t->mem.offset);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->image;
    vi.format = format;
    vi.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = t->levels;
    vi.subresourceRange.layerCount = t->layers;
    if (!depth) {
        vi.viewType = kind == S2R_TEX_CUBE ? VK_IMAGE_VIEW_TYPE_CUBE : kind == S2R_TEX_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        vi.components.r = vk_swizzle(sw[0]); vi.components.g = vk_swizzle(sw[1]);
        vi.components.b = vk_swizzle(sw[2]); vi.components.a = vk_swizzle(sw[3]);
        check(vkCreateImageView(V.dev, &vi, NULL, &t->view), "vkCreateImageView");
    }
    if (render_target || depth) {
        VkImageViewCreateInfo ai = vi;
        ai.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ai.components = (VkComponentMapping){ 0 };
        ai.subresourceRange.aspectMask = aspect_of(t);
        ai.subresourceRange.levelCount = 1;
        ai.subresourceRange.layerCount = 1;
        for (uint32_t f = 0; f < t->layers && f < 6; ++f) {
            ai.subresourceRange.baseArrayLayer = f;
            check(vkCreateImageView(V.dev, &ai, NULL, &t->att[f]), "vkCreateImageView (attachment)");
        }
    }
    return 1;
}

static void forget_framebuffers(const vk_tex *t);

static void ms_release(vk_tex *t)
{
    if (!t->ms_image) return;
    vk_deferred *e = defer_new();
    e->image = t->ms_image;
    e->views[0] = t->ms_view;
    e->mem = t->ms_mem;
    t->ms_image = VK_NULL_HANDLE;
    t->ms_view = VK_NULL_HANDLE;
    memset(&t->ms_mem, 0, sizeof t->ms_mem);
    t->ms_samples = 0;
}

static void tex_release(vk_tex *t)
{
    if (!t->image) return;
    int in_pass = V.pass.color == t || V.pass.depth == t;
    for (uint32_t i = 0; i < V.pass.xcount; ++i) if (V.pass.extra[i] == t) in_pass = 1;
    if (V.pass_active && in_pass) pass_end();
    forget_framebuffers(t);
    ms_release(t);
    vk_deferred *e = defer_new();
    e->image = t->image;
    e->views[0] = t->view;
    for (int f = 0; f < 6; ++f) e->views[1 + f] = t->att[f];
    e->mem = t->mem;
    memset(t, 0, sizeof *t);
}

/* The multisampled twin of a screen-sized colour or depth target: transient, so a tiler keeps it in
 * tile memory (no RAM where lazily allocated memory exists); passes clear or reload it. */
static int ms_attachment(vk_tex *t, uint32_t samples)
{
    if (t->ms_image && t->ms_samples == samples) return 1;
    if (t->ms_failed) return 0;
    ms_release(t);
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = t->format;
    ci.extent.width = t->width; ci.extent.height = t->height; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
        (t->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(V.dev, &ci, NULL, &t->ms_image) != VK_SUCCESS) {
        LOGVK("multisampled %s %ux%u (%u samples) failed: single sample", t->depth ? "depth" : "colour", t->width, t->height, samples);
        t->ms_image = VK_NULL_HANDLE;
        t->ms_failed = 1;
        return 0;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.dev, t->ms_image, &req);
    int lazy = V.lazy_memory && find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) >= 0;
    if (!mem_alloc(&req, lazy ? VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->ms_mem)) {
        vkDestroyImage(V.dev, t->ms_image, NULL);
        t->ms_image = VK_NULL_HANDLE;
        t->ms_failed = 1;
        return 0;
    }
    vkBindImageMemory(V.dev, t->ms_image, t->ms_mem.mem, t->ms_mem.offset);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t->ms_image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = t->format;
    vi.subresourceRange.aspectMask = aspect_of(t);
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    check(vkCreateImageView(V.dev, &vi, NULL, &t->ms_view), "vkCreateImageView (multisampled)");
    t->ms_samples = samples;
    static int logged;
    if (logged++ < 4) LOGVK("multisampled %s %ux%u, %u samples, %s memory", t->depth ? "depth" : "colour", t->width, t->height,
        samples, lazy ? "lazily allocated" : "device");
    return 1;
}

static int vk_image_create(s2r_device *d, s2r_image *img)
{
    (void)d;
    if (!V.ready) return 0;
    vk_tex *t = (vk_tex *)calloc(1, sizeof *t);
    uint32_t bw, bh, bb;
    int compressed = s2r_gpu_block(img->gpu, &bw, &bh, &bb);
    if (compressed && vk_format(img->gpu) == VK_FORMAT_R8G8B8A8_UNORM) {
        /* blocks in an RGBA8 image would be sampled as pixels */
        static int logged;
        if (!logged++) LOGVK("no Vulkan format for %s: image not created", s2r_gpu_name(img->gpu));
        free(t);
        return 0;
    }
    if (!tex_init(t, img->kind, vk_format(img->gpu), img->width, img->height, img->depth, img->levels, img->swizzle,
            img->render_target, img->depth_stencil, compressed)) {
        free(t);
        return 0;
    }
    t->is_bb = img->is_backbuffer;
    t->screen_sized = img->screen_sized;
    img->backend = t;
    return 1;
}

static void vk_image_destroy(s2r_device *d, s2r_image *img)
{
    (void)d;
    vk_tex *t = (vk_tex *)img->backend;
    if (!t) return;
    tex_release(t);
    free(t);
    img->backend = NULL;
}

static void vk_image_upload(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, uint32_t x, uint32_t y,
    uint32_t w, uint32_t h, uint32_t depth, const void *data, uint32_t pitch, uint32_t slice)
{
    (void)d;
    vk_tex *t = (vk_tex *)img->backend;
    if (!t || !t->image || t->depth || level >= t->levels) return;
    uint32_t row = s2r_gpu_pitch(img, w), rows = s2r_gpu_rows(img, h);
    uint32_t slices = t->kind == S2R_TEX_3D ? (depth ? depth : 1) : 1;
    uint32_t total = row * rows * slices;
    if (!pitch) pitch = row;
    if (!slice) slice = pitch * rows;
    VkBuffer src_buf;
    uint32_t src_off;
    uint8_t *dst;
    if (total <= V.staging.chunk) {
        src_off = ring_alloc(&V.staging, total, 16);
        src_buf = V.staging.buf;
        dst = V.staging.mem.map + src_off;
    } else {
        /* larger than a staging chunk: a buffer of its own, destroyed when the frame completes */
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = total; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vk_deferred *temp = defer_new();
        if (vkCreateBuffer(V.dev, &bi, NULL, &temp->buffer) != VK_SUCCESS) return;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(V.dev, temp->buffer, &req);
        if (!mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, &temp->mem)) return;
        vkBindBufferMemory(V.dev, temp->buffer, temp->mem.mem, temp->mem.offset);
        src_buf = temp->buffer; src_off = 0; dst = temp->mem.map;
    }
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t s = 0; s < slices; ++s)
        for (uint32_t r = 0; r < rows; ++r)
            memcpy(dst + ((size_t)s * rows + r) * row, src + (size_t)s * slice + (size_t)r * pitch, row);
    VkCommandBuffer cb = cb_for(t);
    transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = { 0 };
    c.bufferOffset = src_off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.mipLevel = level;
    c.imageSubresource.baseArrayLayer = t->kind == S2R_TEX_CUBE ? face : 0;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)x; c.imageOffset.y = (int32_t)y;
    uint32_t lw = t->width >> level, lh = t->height >> level;
    if (!lw) lw = 1;
    if (!lh) lh = 1;
    c.imageExtent.width = x + w > lw ? lw - x : w;
    c.imageExtent.height = y + h > lh ? lh - y : h;
    c.imageExtent.depth = slices;
    vkCmdCopyBufferToImage(cb, src_buf, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    transition(cb, t, t->render_target ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static void vk_image_mips(s2r_device *d, s2r_image *img)
{
    (void)d;
    vk_tex *t = (vk_tex *)img->backend;
    if (!t || !t->image || t->compressed || t->depth || t->levels < 2 || t->kind == S2R_TEX_3D) return;
    VkCommandBuffer cb = cb_for(t);
    transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    for (uint32_t l = 1; l < t->levels; ++l) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = t->image;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.baseMipLevel = l - 1; b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = t->layers;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkImageBlit blit = { 0 };
        blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = l - 1; blit.dstSubresource.mipLevel = l;
        blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = t->layers;
        uint32_t sw = t->width >> (l - 1), sh = t->height >> (l - 1), dw = t->width >> l, dh = t->height >> l;
        blit.srcOffsets[1].x = (int32_t)(sw ? sw : 1); blit.srcOffsets[1].y = (int32_t)(sh ? sh : 1); blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)(dw ? dw : 1); blit.dstOffsets[1].y = (int32_t)(dh ? dh : 1); blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);
    }
    VkImageMemoryBarrier b[2] = { { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }, { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER } };
    for (int i = 0; i < 2; ++i) {
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = t->image;
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.layerCount = t->layers;
        b[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].subresourceRange.baseMipLevel = 0; b[0].subresourceRange.levelCount = t->levels - 1;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].subresourceRange.baseMipLevel = t->levels - 1; b[1].subresourceRange.levelCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
        0, 0, NULL, 0, NULL, 2, b);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/* A rect of a level into a host-visible buffer, waited for. */
static int vk_image_read(s2r_device *d, s2r_image *img, uint32_t face, uint32_t level, const uint32_t *rect, void *out,
    uint32_t pitch)
{
    (void)d;
    vk_tex *t = (vk_tex *)img->backend;
    if (!t || !t->image || t->compressed || t->depth || level >= t->levels) return 0;
    uint32_t w = rect[2] - rect[0], h = rect[3] - rect[1];
    uint32_t row = s2r_gpu_pitch(img, w);
    uint32_t size = row * h;
    if (V.rb_size < size) {
        if (V.rb_buf) { vkDestroyBuffer(V.dev, V.rb_buf, NULL); mem_free_now(&V.rb_mem); V.rb_buf = VK_NULL_HANDLE; }
        VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = size; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(V.dev, &bi, NULL, &V.rb_buf) != VK_SUCCESS) return 0;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(V.dev, V.rb_buf, &req);
        if (!mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, &V.rb_mem)) return 0;
        vkBindBufferMemory(V.dev, V.rb_buf, V.rb_mem.mem, V.rb_mem.offset);
        V.rb_size = size;
    }
    pass_end();
    VkCommandBuffer cb = main_cb();
    VkImageLayout back = t->layout;
    transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = { 0 };
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.mipLevel = level;
    c.imageSubresource.baseArrayLayer = t->kind == S2R_TEX_CUBE ? face : 0;
    c.imageSubresource.layerCount = 1;
    c.imageOffset.x = (int32_t)rect[0]; c.imageOffset.y = (int32_t)rect[1];
    c.imageExtent.width = w; c.imageExtent.height = h; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.rb_buf, 1, &c);
    transition(cb, t, back == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : back);
    t->used = V.serial;
    flush_wait();
    for (uint32_t r = 0; r < h; ++r) memcpy((uint8_t *)out + (size_t)r * pitch, V.rb_mem.map + (size_t)r * row, row);
    return 1;
}

/* ============================================================= buffers */
static int buf_version_new(vk_buf *b)
{
    if (b->count == 16) return -1;
    vk_buf_version *v = &b->v[b->count];
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = b->size ? b->size : 4; bi.usage = b->usage;
    if (vkCreateBuffer(V.dev, &bi, NULL, &v->buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev, v->buf, &req);
    if (!mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1, &v->mem)) {
        vkDestroyBuffer(V.dev, v->buf, NULL);
        return -1;
    }
    vkBindBufferMemory(V.dev, v->buf, v->mem.mem, v->mem.offset);
    v->used = 0;
    if (b->count == 1 && !b->listed) { b->next_multi = V.multi; V.multi = b; b->listed = 1; }
    return b->count++;
}

/* Every 120 frames: versions no draw read for 600 submits go (the Frostpunk stream ring's lesson,
 * wiki note 22: copies kept for a burst stayed forever). The current version always stays. */
static void buf_trim(void)
{
    vk_buf **link = &V.multi;
    while (*link) {
        vk_buf *b = *link;
        for (int i = 0; i < b->count && b->count > 1; ++i) {
            vk_buf_version *v = &b->v[i];
            if (i == b->cur || v->used > V.completed || v->used + 600u > V.serial) continue;
            vk_deferred *e = defer_new();
            e->buffer = v->buf;
            e->mem = v->mem;
            for (int k = 0; k < 17; ++k) if (V.cur_vb[k] == v->buf) V.cur_vb[k] = VK_NULL_HANDLE;
            if (V.cur_ib == v->buf) V.cur_ib = VK_NULL_HANDLE;
            int last = b->count - 1;
            if (i != last) {
                b->v[i] = b->v[last];
                if (b->cur == last) b->cur = i;
                --i;
            }
            memset(&b->v[last], 0, sizeof b->v[last]);
            --b->count;
        }
        if (b->count <= 1) { *link = b->next_multi; b->listed = 0; b->next_multi = NULL; }
        else link = &b->next_multi;
    }
}

static void *vk_buffer_create(s2r_device *d, s2r_buffer *hb)
{
    (void)d;
    vk_buf *b = (vk_buf *)calloc(1, sizeof *b);
    b->size = hb->size;
    b->usage = hb->index ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (buf_version_new(b) < 0) { free(b); return NULL; }
    return b;
}

/* A version the GPU no longer reads (new, or the oldest waited for). */
static int buf_free_version(vk_buf *b)
{
    for (int i = 0; i < b->count; ++i) if (i != b->cur && b->v[i].used <= V.completed) return i;
    poll_completed();
    for (int i = 0; i < b->count; ++i) if (i != b->cur && b->v[i].used <= V.completed) return i;
    int pick = buf_version_new(b);
    if (pick >= 0) return pick;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < b->count; ++i) if (i != b->cur && b->v[i].used < oldest) { oldest = b->v[i].used; pick = i; }
    wait_serial(oldest);
    return pick;
}

static void vk_buffer_write(s2r_device *d, s2r_buffer *hb, uint32_t offset, uint32_t size, const void *data, int mode)
{
    (void)d;
    vk_buf *b = (vk_buf *)hb->backend;
    if (!b) return;
    vk_buf_version *c = &b->v[b->cur];
    if (mode == S2R_WRITE_DISCARD) {
        /* Direct3D's renaming: draws already recorded keep the old version. */
        if (c->used > V.completed) { b->cur = buf_free_version(b); c = &b->v[b->cur]; }
    } else if (mode == S2R_WRITE_PLAIN && c->used > V.completed) {
        /* A plain lock of a buffer in flight: a free version with the whole buffer, from the render
           thread's mirror (the guest copy belongs to the game's thread). The mirror is made the first
           time: the current version's contents with this write (s2r_buffer_upload keeps it after). */
        if (!hb->mirror) {
            hb->mirror = (uint8_t *)malloc(b->size ? b->size : 4);
            if (hb->mirror) {
                memcpy(hb->mirror, c->mem.map, b->size);
                if (data && offset < b->size) memcpy(hb->mirror + offset, data, offset + size > b->size ? b->size - offset : size);
                static uint32_t mirrors;
                if (++mirrors <= 4 || !(mirrors & (mirrors - 1)))
                    LOGVK("buffer of %u bytes written in place while drawn: kept on the CPU too (%u such buffers)", b->size, mirrors);
            }
        }
        b->cur = buf_free_version(b);
        c = &b->v[b->cur];
        if (hb->mirror) memcpy(c->mem.map, hb->mirror, b->size);
        else if (data && offset < b->size) memcpy(c->mem.map + offset, data, offset + size > b->size ? b->size - offset : size);
        return;
    }
    if (!data || offset >= b->size) return;
    if (offset + size > b->size) size = b->size - offset;
    memcpy(c->mem.map + offset, data, size);
}

static void vk_buffer_destroy(s2r_device *d, s2r_buffer *hb)
{
    (void)d;
    vk_buf *b = (vk_buf *)hb->backend;
    if (!b) return;
    if (b->listed)
        for (vk_buf **link = &V.multi; *link; link = &(*link)->next_multi)
            if (*link == b) { *link = b->next_multi; break; }
    for (int i = 0; i < b->count; ++i) {
        vk_deferred *e = defer_new();
        e->buffer = b->v[i].buf;
        e->mem = b->v[i].mem;
        for (int k = 0; k < 17; ++k) if (V.cur_vb[k] == b->v[i].buf) V.cur_vb[k] = VK_NULL_HANDLE;
        if (V.cur_ib == b->v[i].buf) V.cur_ib = VK_NULL_HANDLE;
    }
    free(b);
    hb->backend = NULL;
    free(hb->mirror);
    hb->mirror = NULL;
}

/* ======================================================== render passes */
/* Attachments in order: colours 0-3 (MRT), then depth; with scene MSAA (samples > 1, colour 0
 * alone): the multisampled colour, the multisampled depth, then colour 0 itself as the resolve. */
static VkRenderPass render_pass(const vk_rp_key *k)
{
    for (int i = 0; i < V.rp_count; ++i) if (!memcmp(&V.rps[i].key, k, sizeof *k)) return V.rps[i].rp;
    VkAttachmentDescription at[6];
    VkAttachmentReference cref[4], dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference rref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    uint32_t n = 0, colors = 0;
    int ms = k->samples > 1 && k->cfmt;
    memset(at, 0, sizeof at);
    if (k->cfmt) {
        at[n].format = k->cfmt; at[n].samples = ms ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = (VkAttachmentLoadOp)k->cload;
        at[n].storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = ms ? VK_IMAGE_LAYOUT_UNDEFINED : k->cinit;
        at[n].finalLayout = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : k->cfinal;
        cref[colors].attachment = n++; cref[colors].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ++colors;
        for (uint32_t i = 0; i < k->xcount && !ms; ++i) {
            at[n].format = k->xfmt[i]; at[n].samples = VK_SAMPLE_COUNT_1_BIT;
            at[n].loadOp = (VkAttachmentLoadOp)k->cload; at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            at[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            at[n].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            cref[colors].attachment = n++; cref[colors].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ++colors;
        }
    }
    if (k->dfmt) {
        int stencil = format_has_stencil(k->dfmt);
        at[n].format = k->dfmt; at[n].samples = ms ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = (VkAttachmentLoadOp)k->dload;
        at[n].storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = stencil ? (VkAttachmentLoadOp)k->dload : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at[n].stencilStoreOp = stencil && !ms ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = ms ? VK_IMAGE_LAYOUT_UNDEFINED : k->dinit;
        at[n].finalLayout = k->dfinal;
        dref.attachment = n++;
    }
    if (ms) {
        /* colour 0 itself: every pixel of the render area is the resolve's, so nothing loads */
        at[n].format = k->cfmt; at[n].samples = VK_SAMPLE_COUNT_1_BIT;
        at[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[n].finalLayout = k->cfinal;
        rref.attachment = n++;
    }
    VkSubpassDescription sp = { 0 };
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = colors;
    sp.pColorAttachments = colors ? cref : NULL;
    sp.pResolveAttachments = ms ? &rref : NULL;
    sp.pDepthStencilAttachment = k->dfmt ? &dref : NULL;
    VkSubpassDependency dep[2];
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    ci.attachmentCount = n; ci.pAttachments = at;
    ci.subpassCount = 1; ci.pSubpasses = &sp;
    ci.dependencyCount = 2; ci.pDependencies = dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    check(vkCreateRenderPass(V.dev, &ci, NULL, &rp), "vkCreateRenderPass");
    if (V.rp_count < 64) { V.rps[V.rp_count].key = *k; V.rps[V.rp_count].rp = rp; ++V.rp_count; }
    return rp;
}

/* The render pass every pipeline of these attachments is compatible with. */
static VkRenderPass compat_pass(VkFormat cfmt, VkFormat dfmt, uint32_t samples, uint32_t xcount, const uint32_t *xfmt)
{
    vk_rp_key k;
    memset(&k, 0, sizeof k);
    k.cfmt = cfmt; k.dfmt = dfmt; k.samples = samples > 1 ? samples : 1;
    k.cload = samples > 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    k.dload = samples > 1 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    k.cinit = k.cfinal = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    k.dinit = k.dfinal = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    k.xcount = (uint8_t)(samples > 1 ? 0 : xcount);
    for (uint32_t i = 0; i < k.xcount; ++i) k.xfmt[i] = (VkFormat)xfmt[i];
    return render_pass(&k);
}

static VkFramebuffer framebuffer(const VkImageView *views, uint32_t count, uint32_t w, uint32_t h, VkRenderPass rp)
{
    for (int i = 0; i < V.fb_count; ++i)
        if (V.fbs[i].count == count && V.fbs[i].w == w && V.fbs[i].h == h &&
            !memcmp(V.fbs[i].views, views, count * sizeof *views)) return V.fbs[i].fb;
    if (V.fb_count == 128) {
        vk_deferred *e = defer_new();
        e->fb = V.fbs[0].fb;
        V.fbs[0] = V.fbs[--V.fb_count];
    }
    VkFramebufferCreateInfo ci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    ci.renderPass = rp; ci.attachmentCount = count; ci.pAttachments = views;
    ci.width = w; ci.height = h; ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    check(vkCreateFramebuffer(V.dev, &ci, NULL, &fb), "vkCreateFramebuffer");
    memset(V.fbs[V.fb_count].views, 0, sizeof V.fbs[V.fb_count].views);
    memcpy(V.fbs[V.fb_count].views, views, count * sizeof *views);
    V.fbs[V.fb_count].count = count;
    V.fbs[V.fb_count].w = w; V.fbs[V.fb_count].h = h; V.fbs[V.fb_count].fb = fb;
    ++V.fb_count;
    return fb;
}

static void forget_framebuffers(const vk_tex *t)
{
    for (int i = 0; i < V.fb_count; ++i) {
        int hit = 0;
        for (uint32_t v = 0; v < V.fbs[i].count; ++v) {
            for (int f = 0; f < 6; ++f) if (t->att[f] && V.fbs[i].views[v] == t->att[f]) hit = 1;
            if (t->ms_view && V.fbs[i].views[v] == t->ms_view) hit = 1;
        }
        if (!hit) continue;
        vk_deferred *e = defer_new();
        e->fb = V.fbs[i].fb;
        V.fbs[i] = V.fbs[--V.fb_count];
        --i;
    }
}

/* The attachments of explicit targets (colours 0-3, NULL entries skipped), or of the device's bound
 * ones; the scene MSAA when colour 0 alone is screen-sized and the depth too (d3d9_gl.c), unless
 * `single` (StretchRect, ColorFill: the target itself). */
static void target_of(const s2r_target *color, uint32_t colors, const s2r_target *depth, int single, vk_target *t)
{
    memset(t, 0, sizeof *t);
    t->scale = 1.0f;
    t->samples = 1;
    for (uint32_t i = 0; i < colors && i < 4; ++i) {
        if (!color[i].img) continue;
        vk_tex *c = (vk_tex *)color[i].img->backend;
        if (!c || !c->att[0] || color[i].level) continue;
        if (!t->color) {
            if (i) break;                         /* Direct3D needs render target 0 */
            t->color = c;
            t->face = color[i].face < c->layers ? color[i].face : 0;
            t->scale = s2r_scale_of(color[i].img);
        } else if (c->width == t->color->width && c->height == t->color->height && c->kind == S2R_TEX_2D && t->xcount < 3) {
            t->extra[t->xcount++] = c;
        }
    }
    if (depth && depth->img) {
        vk_tex *z = (vk_tex *)depth->img->backend;
        if (z && z->att[0]) t->depth = z;
        if (!t->color && t->depth) t->scale = s2r_scale_of(depth->img);
    }
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    if (t->color) { w = t->color->width; h = t->color->height; }
    if (t->depth) {
        if (t->color) {
            /* A depth surface of another size than the colour target: Direct3D draws where both exist. */
            if (t->depth->width < w) w = t->depth->width;
            if (t->depth->height < h) h = t->depth->height;
            if (t->depth->width < t->color->width || t->depth->height < t->color->height) {
                static int logged;
                if (!logged++) LOGVK("depth %ux%u smaller than colour %ux%u: depth dropped for those passes",
                    t->depth->width, t->depth->height, t->color->width, t->color->height);
                t->depth = NULL;
                w = t->color->width; h = t->color->height;
            }
        } else { w = t->depth->width; h = t->depth->height; }
    }
    t->w = w; t->h = h;
    if (!single && V.msaa_samples > 1 && t->color && !t->xcount && t->color->screen_sized && t->color->kind == S2R_TEX_2D &&
        !t->face && (!t->depth || t->depth->screen_sized) && t->color->width == w && t->color->height == h &&
        ms_attachment(t->color, V.msaa_samples) && (!t->depth || ms_attachment(t->depth, V.msaa_samples)))
        t->samples = V.msaa_samples;
}

static void wanted_target(s2r_device *d, vk_target *t)
{
    s2r_target c[4], z;
    int n = s2r_targets(d, c, &z);
    target_of(c, (uint32_t)n, &z, 0, t);
}

static int same_target(const vk_target *a, const vk_target *b)
{
    if (a->color != b->color || a->depth != b->depth || a->face != b->face || a->w != b->w || a->h != b->h ||
        a->xcount != b->xcount || a->samples != b->samples) return 0;
    for (uint32_t i = 0; i < a->xcount; ++i) if (a->extra[i] != b->extra[i]) return 0;
    return 1;
}

static void pass_end(void)
{
    if (!V.pass_active) return;
    if (open_query) {
        vkCmdEndQuery(main_cb(), V.qpool, open_query->index);
        open_query->active = 0;
        open_query->split = 1;
        open_query = NULL;
    }
    vkCmdEndRenderPass(main_cb());
    if (V.pass.color) V.pass.color->layout = V.pass.color->is_bb ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (uint32_t i = 0; i < V.pass.xcount; ++i) V.pass.extra[i]->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    /* a multisampled pass used the depth's twin: the depth image itself was not touched */
    if (V.pass.depth && V.pass.samples <= 1) V.pass.depth->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    V.pass_active = 0;
    V.dyn_valid = 0;
}

static VkPipeline reload_pipeline(VkFormat cfmt, VkFormat dfmt, uint32_t samples);
static VkDescriptorSet filter_set(const vk_tex *source);

/* A multisampled pass that loads: its target is copied aside first (the target is the pass's resolve
 * attachment and cannot be read in it), and the pass's first draw fills every sample from the copy.
 * Depth starts cleared to far, like the multisampled-to-texture depth of OpenGL ES. */
static int reload_copy(vk_tex *c)
{
    vk_tex *r = &V.reload_img;
    if (r->image && (r->width != c->width || r->height != c->height || r->format != c->format)) tex_release(r);
    if (!r->image) {
        static const uint8_t identity[4] = { S2R_SW_R, S2R_SW_G, S2R_SW_B, S2R_SW_A };
        if (!tex_init(r, S2R_TEX_2D, c->format, c->width, c->height, 1, 1, identity, 0, 0, 0)) return 0;
        LOGVK("multisampled passes that load: a %ux%u copy of the target", c->width, c->height);
    }
    VkCommandBuffer cb = main_cb();
    transition(cb, c, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region = { 0 };
    region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
    region.extent.width = c->width; region.extent.height = c->height; region.extent.depth = 1;
    vkCmdCopyImage(cb, c->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(cb, r, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    r->used = V.serial;
    return 1;
}

/* Begins the pass of `t`; `clear` (1 colour, 2 depth, 4 stencil) loads those
 * attachments cleared to the values given. */
static void pass_begin(const vk_target *t, uint32_t clear, const VkClearValue *cc, const VkClearValue *dc)
{
    vk_rp_key k;
    memset(&k, 0, sizeof k);
    k.samples = t->samples > 1 ? t->samples : 1;
    int ms = k.samples > 1;
    int reload = 0;
    VkImageView views[6];
    uint32_t nv = 0;
    if (t->color) {
        k.cfmt = t->color->format;
        k.cload = (clear & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        k.cfinal = t->color->is_bb ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (ms) {
            reload = !(clear & 1) && t->color->layout != VK_IMAGE_LAYOUT_UNDEFINED && reload_copy(t->color);
            k.cload = (clear & 1) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.cinit = VK_IMAGE_LAYOUT_UNDEFINED;
            views[nv++] = t->color->ms_view;
        } else {
            k.cinit = t->color->layout;
            if (k.cinit != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && k.cinit != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                k.cinit != VK_IMAGE_LAYOUT_UNDEFINED) {
                transition(main_cb(), t->color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                k.cinit = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            if (k.cinit == VK_IMAGE_LAYOUT_UNDEFINED && !(clear & 1)) k.cload = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            views[nv++] = t->color->att[t->face];
            for (uint32_t i = 0; i < t->xcount; ++i) {
                vk_tex *x = t->extra[i];
                k.xfmt[i] = x->format;
                transition(main_cb(), x, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                views[nv++] = x->att[0];
                x->used = V.serial;
            }
            k.xcount = (uint8_t)t->xcount;
        }
    }
    if (t->depth) {
        k.dfmt = t->depth->format;
        k.dload = (clear & 6) == 6 || ((clear & 2) && !format_has_stencil(k.dfmt)) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        k.dfinal = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        if (ms) {
            if (k.dload == VK_ATTACHMENT_LOAD_OP_LOAD) k.dload = VK_ATTACHMENT_LOAD_OP_CLEAR;
            k.dinit = VK_IMAGE_LAYOUT_UNDEFINED;
            views[nv++] = t->depth->ms_view;
        } else {
            k.dinit = t->depth->layout;
            if (k.dinit != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && k.dinit != VK_IMAGE_LAYOUT_UNDEFINED) {
                transition(main_cb(), t->depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
                k.dinit = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            }
            if (k.dinit == VK_IMAGE_LAYOUT_UNDEFINED && k.dload == VK_ATTACHMENT_LOAD_OP_LOAD) k.dload = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            views[nv++] = t->depth->att[0];
        }
    }
    if (ms) views[nv++] = t->color->att[0];
    VkRenderPass rp = render_pass(&k);
    uint32_t xfmt[3] = { k.xfmt[0], k.xfmt[1], k.xfmt[2] };
    VkRenderPass compat = compat_pass(k.cfmt, k.dfmt, k.samples, k.xcount, xfmt);
    VkFramebuffer fb = framebuffer(views, nv, t->w, t->h, compat);
    VkClearValue values[6];
    uint32_t n = 0;
    if (t->color) {
        values[n++] = cc ? *cc : (VkClearValue){ 0 };
        for (uint32_t i = 0; !ms && i < t->xcount; ++i) values[n++] = cc ? *cc : (VkClearValue){ 0 };
    }
    if (t->depth) {
        VkClearValue far = { 0 };
        far.depthStencil.depth = 1.0f;
        values[n++] = dc ? *dc : far;
    }
    if (ms) values[n++] = (VkClearValue){ 0 };
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = rp; bi.framebuffer = fb;
    bi.renderArea.extent.width = t->w; bi.renderArea.extent.height = t->h;
    bi.clearValueCount = n; bi.pClearValues = values;
    vkCmdBeginRenderPass(main_cb(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    if (s2r_dev) {
        ++s2r_dev->frame_passes;
        s2r_dev->frame_loads += (t->color && (k.cload == VK_ATTACHMENT_LOAD_OP_LOAD || reload)) +
            (t->depth && k.dload == VK_ATTACHMENT_LOAD_OP_LOAD);
    }
    V.pass = *t;
    V.pass_active = 1;
    if (t->color) t->color->used = V.serial;
    if (t->depth) t->depth->used = V.serial;
    V.dyn_valid = 0;
    if (reload) {
        VkPipeline pipe = reload_pipeline(k.cfmt, k.dfmt, k.samples);
        VkDescriptorSet set = pipe ? filter_set(&V.reload_img) : VK_NULL_HANDLE;
        if (set) {
            VkCommandBuffer cb = main_cb();
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.filter_layout, 0, 1, &set, 0, NULL);
            VkViewport vp = { 0, 0, (float)t->w, (float)t->h, 0, 1 };
            VkRect2D sc = { { 0, 0 }, { t->w, t->h } };
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &sc);
            float push[4] = { (float)t->w, (float)t->h, 0, 0 };
            vkCmdPushConstants(cb, V.filter_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
            vkCmdDraw(cb, 4, 1, 0, 0);
            V.cur_pipe = VK_NULL_HANDLE;
            V.cur_set = VK_NULL_HANDLE;
        }
    }
    /* a clear of only depth or only stencil of a combined format runs inside the pass */
    if (t->depth && (clear & 6) && k.dload != VK_ATTACHMENT_LOAD_OP_CLEAR) {
        VkClearAttachment ca = { 0 };
        ca.aspectMask = ((clear & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((clear & 4) && format_has_stencil(k.dfmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        ca.clearValue = *dc;
        VkClearRect r = { { { 0, 0 }, { t->w, t->h } }, 0, 1 };
        if (ca.aspectMask) vkCmdClearAttachments(main_cb(), 1, &ca, 1, &r);
    }
}

static void target_select(s2r_device *d, vk_target *t)
{
    wanted_target(d, t);
    if (V.pass_active && !same_target(t, &V.pass)) pass_end();
}

static void clear_values(uint32_t color, float z, uint32_t stencil, VkClearValue *cc, VkClearValue *dc)
{
    cc->color.float32[0] = (float)((color >> 16) & 255) / 255.0f;
    cc->color.float32[1] = (float)((color >> 8) & 255) / 255.0f;
    cc->color.float32[2] = (float)(color & 255) / 255.0f;
    cc->color.float32[3] = (float)(color >> 24) / 255.0f;
    dc->depthStencil.depth = z;
    dc->depthStencil.stencil = stencil;
}

/* Clears rects (x0,y0,x1,y1) of target `t`; a full one as the first operation becomes the pass's load. */
static void clear_target(const vk_target *t, uint32_t flags, const int32_t *rects, uint32_t count, uint32_t color,
    float z, uint32_t stencil)
{
    if (!t->color) flags &= ~1u;
    if (!t->depth) flags &= ~6u;
    if (!flags) return;
    VkClearValue cc, dc;
    clear_values(color, z, stencil, &cc, &dc);
    VkClearRect cr[64];
    uint32_t n = 0;
    int full = 0;
    for (uint32_t i = 0; i < count && n < 64; ++i) {
        int32_t x0 = rects[4 * i], y0 = rects[4 * i + 1], x1 = rects[4 * i + 2], y1 = rects[4 * i + 3];
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)t->w) x1 = (int32_t)t->w;
        if (y1 > (int32_t)t->h) y1 = (int32_t)t->h;
        if (x1 <= x0 || y1 <= y0) continue;
        if (x0 == 0 && y0 == 0 && x1 == (int32_t)t->w && y1 == (int32_t)t->h) full = 1;
        cr[n].rect.offset.x = x0; cr[n].rect.offset.y = y0;
        cr[n].rect.extent.width = (uint32_t)(x1 - x0); cr[n].rect.extent.height = (uint32_t)(y1 - y0);
        cr[n].baseArrayLayer = 0; cr[n].layerCount = 1;
        ++n;
    }
    if (!n) return;
    if (V.pass_active && !same_target(t, &V.pass)) pass_end();
    if (!V.pass_active && full) { pass_begin(t, flags, &cc, &dc); return; }
    if (!V.pass_active) pass_begin(t, 0, NULL, NULL);
    VkClearAttachment ca[5];
    uint32_t m = 0;
    if (flags & 1)
        for (uint32_t i = 0; i <= (t->samples > 1 ? 0 : t->xcount); ++i) {
            ca[m].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca[m].colorAttachment = i; ca[m].clearValue = cc; ++m;
        }
    if (flags & 6) {
        VkImageAspectFlags a = ((flags & 2) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
            ((flags & 4) && format_has_stencil(t->depth->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (a) { ca[m].aspectMask = a; ca[m].colorAttachment = 0; ca[m].clearValue = dc; ++m; }
    }
    if (m) vkCmdClearAttachments(main_cb(), m, ca, n, cr);
}

static void vk_clear(s2r_device *d, uint32_t count, const int32_t *rects, uint32_t flags, uint32_t color, float z,
    uint32_t stencil)
{
    if (!V.ready) return;
    vk_target t;
    target_select(d, &t);
    /* The viewport, and the scissor when it is on; explicit rects are clipped to the viewport. */
    const d3d_state *ds = &d->dev->st;
    int32_t x0 = (int32_t)ds->viewport[0], y0 = (int32_t)ds->viewport[1];
    int32_t x1 = x0 + (int32_t)ds->viewport[2], y1 = y0 + (int32_t)ds->viewport[3];
    if (ds->rs[174]) {
        int32_t sl = (int32_t)ds->scissor[0], st = (int32_t)ds->scissor[1];
        int32_t sr = sl + (int32_t)ds->scissor[2], sb = st + (int32_t)ds->scissor[3];
        if (sl > x0) x0 = sl;
        if (st > y0) y0 = st;
        if (sr < x1) x1 = sr;
        if (sb < y1) y1 = sb;
    }
    int32_t clipped[64 * 4];
    uint32_t n = 0;
    if (!count) {
        clipped[0] = x0; clipped[1] = y0; clipped[2] = x1; clipped[3] = y1;
        n = 1;
    } else {
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            int32_t a = rects[4 * i] > x0 ? rects[4 * i] : x0, b = rects[4 * i + 1] > y0 ? rects[4 * i + 1] : y0;
            int32_t c = rects[4 * i + 2] < x1 ? rects[4 * i + 2] : x1, e = rects[4 * i + 3] < y1 ? rects[4 * i + 3] : y1;
            if (c <= a || e <= b) continue;
            clipped[4 * n] = a; clipped[4 * n + 1] = b; clipped[4 * n + 2] = c; clipped[4 * n + 3] = e;
            ++n;
        }
    }
    /* Direct3D pixels in the stored pixels of the targets (the render scale) */
    for (uint32_t i = 0; i < 4 * n; ++i) clipped[i] = s2r_scaled(clipped[i], t.scale);
    clear_target(&t, flags, clipped, n, color, z, stencil);
}

static void vk_fill(s2r_device *d, const s2r_target *dst, const uint32_t *rect, uint32_t color)
{
    (void)d;
    if (!V.ready) return;
    vk_target t;
    target_of(dst, 1, NULL, 1, &t);
    if (!t.color) return;
    int32_t r[4] = { s2r_scaled((int32_t)rect[0], t.scale), s2r_scaled((int32_t)rect[1], t.scale),
                     s2r_scaled((int32_t)rect[2], t.scale), s2r_scaled((int32_t)rect[3], t.scale) };
    clear_target(&t, 1, r, 1, color, 1.0f, 0);
}

/* ============================================================== modules */
static uint64_t fnv64(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static VkShaderModule words_module(const uint32_t *words, uint32_t count)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = (size_t)count * 4; ci.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(V.dev, &ci, NULL, &module), "vkCreateShaderModule");
    return module;
}

/* SPIR-V of a shader translated on the device: glslang, cached in the app
 * cache (vulkan/<hash>.spv) by the hash of the text, so it compiles once
 * (Painkiller's runtime_module). */
extern int s2r_glslang_compile(int pixel, const char *source, uint32_t **words, uint32_t *count, char *log, size_t log_cap);
static uint32_t stat_spirv_compiled, stat_spirv_cached;
static uint64_t stat_spirv_ns;

/* SPIR-V of a complete GLSL text (#version included), cached like the device's translations. */
static VkShaderModule text_module(int pixel, char *src, uint64_t name_hash);

static VkShaderModule runtime_module(const s2r_lib_shader *lib)
{
    size_t n = strlen(lib->text) + 32;
    char *src = (char *)malloc(n);
    snprintf(src, n, "#version 450\n%s", lib->text);
    if (lib->pixel && s2r_ui_fsr()) {
        /* UI FSR 1: the interface's 2D reads through EASU (port_vpos.w, set per draw) */
        char *filtered = s2r_ui_filter_source(src);
        if (filtered) { free(src); src = filtered; }
    }
    return text_module(lib->pixel, src, lib->hash);
}

static VkShaderModule text_module(int pixel, char *src, uint64_t name_hash)
{
    uint64_t hash = fnv64(src, strlen(src)) ^ (pixel ? 0x9E3779B97F4A7C15ull : 0);
    char path[1200];
    uint32_t *words = NULL, count = 0;
    if (V.cache_dir[0]) {
        snprintf(path, sizeof path, "%s/%016llx.spv", V.cache_dir, (unsigned long long)hash);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 20 && size % 4 == 0) {
                words = (uint32_t *)malloc((size_t)size);
                if (words && fread(words, 1, (size_t)size, f) == (size_t)size && words[0] == 0x07230203u) {
                    count = (uint32_t)(size / 4);
                    ++stat_spirv_cached;
                } else { free(words); words = NULL; }
            }
            fclose(f);
        }
    }
    if (!words) {
        char log[4096];
        uint64_t start = s2r_now_ns();
        int ok = s2r_glslang_compile(pixel, src, &words, &count, log, sizeof log);
        uint64_t took = s2r_now_ns() - start;
        if (!ok) {
            LOGVK("%s shader %016llx: GLSL -> SPIR-V failed: %s", pixel ? "pixel" : "vertex", (unsigned long long)name_hash, log);
            free(src);
            return VK_NULL_HANDLE;
        }
        ++stat_spirv_compiled;
        stat_spirv_ns += took;
        if (stat_spirv_compiled <= 8 || took > 20000000u)
            LOGVK("%s shader %016llx: SPIR-V compiled on the device in %.1f ms (%u so far)", pixel ? "pixel" : "vertex",
                (unsigned long long)name_hash, (double)took / 1e6, stat_spirv_compiled);
        if (V.cache_dir[0]) {
            char tmp[1210];
            snprintf(tmp, sizeof tmp, "%s.tmp", path);
            FILE *f = fopen(tmp, "wb");
            if (f) {
                int ok_write = fwrite(words, 4, count, f) == count;
                if (fclose(f) == 0 && ok_write) rename(tmp, path); else unlink(tmp);
            }
        }
    }
    free(src);
    VkShaderModule module = words_module(words, count);
    free(words);
    return module;
}

/* One module per program, shared by all its variants: the library's SPIR-V,
 * or (a shader translated on the device) compiled here once. */
static VkShaderModule library_module(const s2r_lib_shader *lib)
{
    s2r_lib_shader *entry = (s2r_lib_shader *)lib;
    if (!entry->backend_module) {
        /* UI FSR 1: the library's second SPIR-V of a pixel shader (its UI reads through EASU);
           only a shader translated on the device compiles here */
        int ui = lib->pixel && s2r_ui_fsr();
        VkShaderModule m = ui && lib->spirv_ui ? words_module(lib->spirv_ui, lib->spv_ui_words)
                         : !ui && lib->spirv ? words_module(lib->spirv, lib->spv_words)
                         : runtime_module(lib);
        entry->backend_module = (uint64_t)(uintptr_t)m;
    }
    return (VkShaderModule)(uintptr_t)entry->backend_module;
}

static VkShaderModule builtin_module(int which)
{
    if (!V.builtin_modules[which]) {
        uint32_t words = 0;
        const uint32_t *code = s2r_builtin_spirv(which, &words);
        if (code) V.builtin_modules[which] = words_module(code, words);
    }
    return V.builtin_modules[which];
}

void s2r_vulkan_stats(uint32_t *compiled, uint32_t *cached, uint64_t *ns)
{
    *compiled = stat_spirv_compiled; *cached = stat_spirv_cached; *ns = stat_spirv_ns;
}

static void spec_add(vk_module *m, uint32_t id, int32_t value)
{
    if (m->spec_count >= sizeof m->spec_data / sizeof m->spec_data[0]) return;
    uint32_t n = m->spec_count++;
    m->spec_map[n].constantID = id;
    m->spec_map[n].offset = 4 * n;
    m->spec_map[n].size = 4;
    m->spec_data[n] = value;
}

static void sampler_binding(vk_module *m, uint32_t binding, uint32_t type)
{
    if (binding < 4 || binding > 23) return;
    m->samplers |= 1u << binding;
    m->sampler_types |= (uint64_t)type << (2 * binding);
}

/* The variant of a stage for this draw: the program and its constants. */
static vk_module *module_get(int pixel, const s2r_draw *draw, uint64_t variant, const uint8_t modes[16])
{
    const s2r_lib_shader *lib = pixel ? draw->ps : draw->vs;
    const uint8_t *ff = pixel ? draw->ff.ps : draw->ff.vs;
    size_t ff_size = pixel ? sizeof draw->ff.ps : sizeof draw->ff.vs;
    uint32_t ff_hash = lib ? 0 : (pixel ? draw->ff.ps_hash : draw->ff.vs_hash);
    unsigned bucket = ((uint32_t)(uintptr_t)lib * 2654435761u ^ (uint32_t)variant * 40503u ^ (uint32_t)(variant >> 32) ^ ff_hash ^
                       (uint32_t)pixel) & 255u;
    for (vk_module *m = V.modules[bucket]; m; m = m->next) {
        if (m->pixel != pixel || m->lib != lib || m->variant != variant || m->ff_hash != ff_hash) continue;
        if (!lib && memcmp(m->ff, ff, ff_size)) continue;
        return m->module ? m : NULL;
    }
    vk_module *m = (vk_module *)calloc(1, sizeof *m);
    m->pixel = pixel; m->lib = lib; m->variant = variant; m->ff_hash = ff_hash;
    if (!lib) memcpy(m->ff, ff, ff_size);
    m->uid = ++V.next_uid;
    if (lib) {
        m->module = library_module(lib);
        for (uint32_t reg = 0; reg < 16; ++reg)
            if (lib->samplers & (1u << reg)) sampler_binding(m, (pixel ? 4u : 20u) + reg, (lib->sampler_types >> (2 * reg)) & 3u);
        if (pixel) {
            spec_add(m, 20, draw->alpha_func);
            spec_add(m, 21, draw->fog);
        } else {
            m->inputs = lib->input_mask;
            for (uint32_t loc = 0; loc < 16; ++loc)
                if (lib->input_mask & (1u << loc)) spec_add(m, loc, modes[loc]);
            spec_add(m, 16, draw->fog_depth);
        }
    } else if (s2r_ff_valid(&draw->ff, pixel)) {
        m->module = builtin_module(pixel ? S2R_BUILTIN_FF_PS : S2R_BUILTIN_FF_VS);
        uint32_t ids[128];
        int32_t values[128];
        int n = s2r_ff_spec(&draw->ff, pixel, ids, values, 128);
        for (int i = 0; i < n; ++i) spec_add(m, ids[i], values[i]);
        if (pixel) {
            int32_t kind[8] = { 0 }, has[8] = { 0 };
            for (int i = 0; i < n; ++i) {
                if (ids[i] < 40 || ids[i] >= 120) continue;
                uint32_t s = (ids[i] - 40) / 10, k = (ids[i] - 40) % 10;
                if (k == 6) kind[s] = values[i];
                if (k == 8) has[s] = values[i];
            }
            for (uint32_t s = 0; s < 8; ++s) if (has[s]) sampler_binding(m, 4 + s, (uint32_t)kind[s]);
        } else {
            m->inputs = S2R_FF_INPUTS;
            for (uint32_t loc = 0; loc < 16; ++loc)
                if (S2R_FF_INPUTS & (1u << loc)) spec_add(m, loc, modes[loc]);
        }
    }
    m->next = V.modules[bucket];
    V.modules[bucket] = m;
    return m->module ? m : NULL;
}

/* ============================================================ pipelines */
static VkFormat vertex_format(uint8_t type)
{
    switch (type) {
    case 0: return VK_FORMAT_R32_SFLOAT;
    case 1: return VK_FORMAT_R32G32_SFLOAT;
    case 2: return VK_FORMAT_R32G32B32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 4: return VK_FORMAT_R8G8B8A8_UNORM;          /* D3DCOLOR: shaders swizzle from BGRA */
    case 5: return VK_FORMAT_R8G8B8A8_UNORM;          /* integer types through normalized formats, scaled back */
    case 6: return VK_FORMAT_R16G16_SNORM;
    case 7: return VK_FORMAT_R16G16B16A16_SNORM;
    case 8: return VK_FORMAT_R8G8B8A8_UNORM;
    case 9: return VK_FORMAT_R16G16_SNORM;
    case 10: return VK_FORMAT_R16G16B16A16_SNORM;
    case 11: return VK_FORMAT_R16G16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UNORM;
    case 13: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 14: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 15: return VK_FORMAT_R16G16_SFLOAT;
    case 16: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkBlendFactor blend_factor(uint32_t b)
{
    switch (b) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR; case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR; case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t op)
{
    static const VkBlendOp ops[6] = { VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
        VK_BLEND_OP_MIN, VK_BLEND_OP_MAX };
    return ops[op < 6 ? op : 1];
}

static uint8_t stencil_op(uint32_t op) { return (uint8_t)(op >= 1 && op <= 8 ? op - 1 : 0); }
static uint8_t compare_op(uint32_t f, uint8_t fallback) { return (uint8_t)(f >= 1 && f <= 8 ? f - 1 : fallback); }

static VkPrimitiveTopology topology(uint32_t type)
{
    switch (type) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 2: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static VkPipeline pipeline_get(const vk_pipe_key *k, const vk_module *vs, const vk_module *ps)
{
    uint32_t h = (uint32_t)fnv64(k, sizeof *k);
    unsigned slot = h & 1023u;
    for (vk_pipe *p = V.pipes[slot]; p; p = p->next)
        if (p->hash == h && !memcmp(&p->key, k, sizeof *k)) return p->pipe;
    VkPipelineShaderStageCreateInfo stages[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                                  { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vs->module; stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = ps->module; stages[1].pName = "main";
    VkSpecializationInfo spec[2] = {
        { vs->spec_count, vs->spec_map, 4u * vs->spec_count, vs->spec_data },
        { ps->spec_count, ps->spec_map, 4u * ps->spec_count, ps->spec_data } };
    if (vs->spec_count) stages[0].pSpecializationInfo = &spec[0];
    if (ps->spec_count) stages[1].pSpecializationInfo = &spec[1];
    VkVertexInputBindingDescription binds[17];
    VkVertexInputAttributeDescription attrs[16];
    uint32_t nb = 0;
    for (uint32_t b = 0; b < 17; ++b) {
        if (!(k->bindings & (1u << b))) continue;
        binds[nb].binding = b; binds[nb].stride = k->stride[b];
        binds[nb].inputRate = (k->instanced & (1u << b)) ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        ++nb;
    }
    for (uint32_t i = 0; i < k->nattr; ++i) {
        attrs[i].location = k->attr[i].loc; attrs[i].binding = k->attr[i].binding;
        uint8_t f = k->attr[i].format;
        attrs[i].format = f == 255 ? VK_FORMAT_R32G32B32A32_SFLOAT : vertex_format(f);
        attrs[i].offset = k->attr[i].offset;
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = nb; vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = k->nattr; vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = (VkPrimitiveTopology)k->topology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    /* Direct3D's clockwise front stays clockwise in Vulkan's y-down framebuffer. */
    rs.cullMode = k->cull == 2 ? VK_CULL_MODE_FRONT_BIT : k->cull == 3 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k->bias;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = k->samples > 1 ? (VkSampleCountFlagBits)k->samples : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->ztest; ds.depthWriteEnable = k->zwrite;
    ds.depthCompareOp = (VkCompareOp)k->zfunc;
    ds.stencilTestEnable = k->stencil;
    ds.front.failOp = (VkStencilOp)k->sfail; ds.front.depthFailOp = (VkStencilOp)k->szfail; ds.front.passOp = (VkStencilOp)k->spass;
    ds.front.compareOp = (VkCompareOp)k->sfunc;
    ds.back = ds.front;
    if (k->twosided) {
        ds.back.failOp = (VkStencilOp)k->bfail; ds.back.depthFailOp = (VkStencilOp)k->bzfail;
        ds.back.passOp = (VkStencilOp)k->bpass; ds.back.compareOp = (VkCompareOp)k->bfunc;
    }
    /* one blend state for every render target (Direct3D 9), each with its own write mask (MRT) */
    VkPipelineColorBlendAttachmentState cb[4];
    memset(cb, 0, sizeof cb);
    for (uint32_t i = 0; i < 4; ++i) {
        cb[i].blendEnable = k->blend;
        cb[i].srcColorBlendFactor = (VkBlendFactor)k->src; cb[i].dstColorBlendFactor = (VkBlendFactor)k->dst;
        cb[i].colorBlendOp = (VkBlendOp)k->op;
        cb[i].srcAlphaBlendFactor = (VkBlendFactor)k->asrc; cb[i].dstAlphaBlendFactor = (VkBlendFactor)k->adst;
        cb[i].alphaBlendOp = (VkBlendOp)k->aop;
        cb[i].colorWriteMask = i ? k->xwmask[i - 1] : k->wmask;
    }
    VkPipelineColorBlendStateCreateInfo bs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    bs.attachmentCount = k->has_color ? 1u + k->xcount : 0; bs.pAttachments = cb;
    VkDynamicState dyn[7] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 7; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = stages;
    ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia; ci.pViewportState = &vp;
    ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &bs; ci.pDynamicState = &dy;
    ci.layout = V.layout;
    ci.renderPass = compat_pass((VkFormat)k->cfmt, (VkFormat)k->dfmt, k->samples, k->xcount, k->xfmt);
    VkPipeline pipe = VK_NULL_HANDLE;
    d3d_rt_wait_begin();
    VkResult r = vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &pipe);
    d3d_rt_wait_end();
    if (r != VK_SUCCESS) LOGVK("vkCreateGraphicsPipelines failed: %d (vs %s ps %s)", (int)r,
        vs->lib ? vs->lib->name : "ff", ps->lib ? ps->lib->name : "ff");
    vk_pipe *p = (vk_pipe *)calloc(1, sizeof *p);
    p->key = *k; p->hash = h; p->pipe = pipe;
    p->next = V.pipes[slot];
    V.pipes[slot] = p;
    ++V.pipe_count; ++V.pipes_new;
    return pipe;
}

static void pipeline_cache_save(void)
{
    if (!V.pcache || !V.cache_dir[0] || !V.pipes_new) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(V.dev, V.pcache, &size, NULL) != VK_SUCCESS || !size) return;
    void *data = malloc(size);
    if (vkGetPipelineCacheData(V.dev, V.pcache, &size, data) == VK_SUCCESS) {
        char path[1200], tmp[1210];
        snprintf(path, sizeof path, "%s/pipelines%s.bin", V.cache_dir, V.driver == S2R_VK_TURNIP ? "-turnip" : V.driver == S2R_VK_PANVK ? "-panvk" : "");
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            int ok = fwrite(data, 1, size, f) == size;
            if (fclose(f) == 0 && ok) rename(tmp, path); else unlink(tmp);
        }
        V.pipes_new = 0;
    }
    free(data);
}

/* ============================================================= samplers */
static VkSampler sampler_for(const uint32_t *ss, uint32_t levels, int compare_ok, uint32_t first_level)
{
    (void)compare_ok;
    uint32_t mag = ss[5], min = ss[6], mip = levels > 1 ? ss[7] : 0;
    uint32_t au = ss[1], av = ss[2], aw = ss[3];
    uint32_t aniso = (min == 3 || mag == 3) ? (ss[10] ? ss[10] : 1) : 1;
    uint32_t max_level = first_level;                     /* s2r_stage_first_level */
    float bias;
    memcpy(&bias, &ss[8], 4);
    if (!(bias > -16.0f && bias < 16.0f)) bias = 0.0f;
    int32_t bias_q = (int32_t)(bias * 64.0f);
    uint64_t key = (uint64_t)mag | (uint64_t)min << 4 | (uint64_t)mip << 8 | (uint64_t)au << 12 | (uint64_t)av << 16 |
                   (uint64_t)aw << 20 | (uint64_t)(aniso & 31) << 24 | (uint64_t)(max_level & 15) << 29 |
                   (uint64_t)(uint16_t)bias_q << 33 | (uint64_t)(levels > 1) << 49;
    for (int i = 0; i < V.sampler_count; ++i) if (V.samplers[i].key == key) return V.samplers[i].s;
    /* border and mirror-once fall back to clamp and mirror, as the GLES layer does */
    static const VkSamplerAddressMode wraps[6] = { VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT };
    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.minFilter = min == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wraps[au < 6 ? au : 1];
    ci.addressModeV = wraps[av < 6 ? av : 1];
    ci.addressModeW = wraps[aw < 6 ? aw : 1];
    ci.mipLodBias = bias;
    ci.minLod = (float)max_level;
    ci.maxLod = mip ? 1000.0f : (float)max_level;
    if (V.has_aniso && aniso > 1) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)aniso < V.max_aniso ? (float)aniso : V.max_aniso;
    }
    VkSampler s = VK_NULL_HANDLE;
    check(vkCreateSampler(V.dev, &ci, NULL, &s), "vkCreateSampler");
    if (V.sampler_count < 256) { V.samplers[V.sampler_count].key = key; V.samplers[V.sampler_count].s = s; ++V.sampler_count; }
    return s;
}

/* ========================================================== descriptors */
static VkDescriptorSet descriptor_alloc(VkDescriptorSetLayout layout)
{
    vk_slot *s = &V.slots[V.cur];
    for (;;) {
        if (s->dpool_cur == s->dpool_count) {
            if (s->dpool_count == 16) return VK_NULL_HANDLE;
            VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 * 1024 },
                                              { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * 1024 } };
            VkDescriptorPoolCreateInfo ci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            ci.maxSets = 1024; ci.poolSizeCount = 2; ci.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(V.dev, &ci, NULL, &s->dpools[s->dpool_count]) != VK_SUCCESS) return VK_NULL_HANDLE;
            ++s->dpool_count;
        }
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = s->dpools[s->dpool_cur]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(V.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        ++s->dpool_cur;
    }
}

enum { UBO_CONST = 256 * 16 + 64 * 16, UBO_FF = sizeof(s2r_ff_uniforms), UBO_PORT = sizeof(s2r_port_block) };

static uint32_t ubo_put(const void *data, uint32_t size)
{
    uint32_t at = ring_alloc(&V.uniforms, size ? size : 16, V.ubo_align);
    if (size) memcpy(V.uniforms.mem.map + at, data, size);
    return at;
}

/* ================================================================ draw */
static void vk_draw(s2r_device *d, const s2r_draw *draw)
{
    if (!V.ready) return;
    vk_target t;
    target_select(d, &t);
    if (!t.color && !t.depth) return;
    uint8_t modes[16];
    uint64_t decode = s2r_decode_modes(d, draw->vs, 1, modes);
    vk_module *vs = module_get(0, draw, decode | (uint64_t)draw->fog_depth << 48, modes);
    vk_module *ps = module_get(1, draw, draw->ps ? (uint64_t)draw->alpha_func | ((uint64_t)draw->fog << 4) : 0, NULL);
    if (!vs || !ps) return;
    const uint32_t *rs = d->dev->st.rs;
    const d3d_state *st = &d->dev->st;
    /* Uniforms and UP data first: a full ring may submit the frame so far (a
     * new serial), and every resource below is marked with the serial it is drawn in. */
    static uint8_t constants[UBO_CONST];
    uint32_t offs[4] = { 0, 0, 0, 0 };
    if (draw->vs) offs[0] = ubo_put(constants, s2r_pack_constants(draw->vs, st, constants, sizeof constants));
    if (draw->ps) offs[1] = ubo_put(constants, s2r_pack_constants(draw->ps, st, constants, sizeof constants));
    if (!draw->vs || !draw->ps) {
        static s2r_ff_uniforms ffu;
        s2r_ff_uniforms_fill(d, draw, &ffu);
        offs[2] = ubo_put(&ffu, sizeof ffu);
    }
    s2r_port_block port;
    s2r_port_block_fill(d, draw, t.scale, t.h, &port);
    offs[3] = ubo_put(&port, sizeof port);
    uint32_t up_vb = 0, up_ib = 0;
    if (draw->up_vertices) {
        if (draw->up_vertex_bytes + draw->up_index_bytes + 64 > V.stream.chunk) return;
        up_vb = ring_alloc(&V.stream, draw->up_vertex_bytes, 16);
        memcpy(V.stream.mem.map + up_vb, draw->up_vertices, draw->up_vertex_bytes);
        if (draw->up_indices) {
            up_ib = ring_alloc(&V.stream, draw->up_index_bytes, 4);
            memcpy(V.stream.mem.map + up_ib, draw->up_indices, draw->up_index_bytes);
        }
    }
    /* ---- pipeline key */
    vk_pipe_key k;
    memset(&k, 0, sizeof k);
    k.vs_uid = vs->uid; k.ps_uid = ps->uid;
    k.has_color = t.color != NULL; k.has_depth = t.depth != NULL;
    k.cfmt = t.color ? (uint32_t)t.color->format : 0;
    k.dfmt = t.depth ? (uint32_t)t.depth->format : 0;
    k.samples = t.samples > 1 ? t.samples : 1;
    k.xcount = (uint8_t)t.xcount;
    for (uint32_t i = 0; i < t.xcount; ++i) {
        k.xfmt[i] = (uint32_t)t.extra[i]->format;
        k.xwmask[i] = (uint8_t)(d->dev->st.rs[190 + i] & 15u);   /* D3DRS_COLORWRITEENABLE1-3 */
    }
    k.topology = (uint8_t)topology(draw->primitive);
    k.cull = rs[22] == 2 ? 2 : rs[22] == 3 ? 3 : 1;
    int depth_ok = t.depth != NULL;
    k.ztest = depth_ok && rs[7] != 0;
    k.zwrite = k.ztest && rs[14] != 0;
    k.zfunc = compare_op(rs[23], 7);
    if (depth_ok && rs[52] && t.depth->stencil) {
        k.stencil = 1;
        k.sfail = stencil_op(rs[53]); k.szfail = stencil_op(rs[54]); k.spass = stencil_op(rs[55]); k.sfunc = compare_op(rs[56], 7);
        if (rs[185]) {
            /* two-sided: D3D's CCW ops for the faces Direct3D calls counter-clockwise (Vulkan's back) */
            k.twosided = 1;
            k.bfail = stencil_op(rs[186]); k.bzfail = stencil_op(rs[187]); k.bpass = stencil_op(rs[188]); k.bfunc = compare_op(rs[189], 7);
        }
    }
    if (rs[27]) {
        k.blend = 1;
        k.src = (uint8_t)blend_factor(rs[19]);
        k.dst = (uint8_t)blend_factor(rs[20]);
        k.op = (uint8_t)blend_op(rs[171]);
        if (rs[206]) {
            k.asrc = (uint8_t)blend_factor(rs[207]); k.adst = (uint8_t)blend_factor(rs[208]); k.aop = (uint8_t)blend_op(rs[209]);
        } else { k.asrc = k.src; k.adst = k.dst; k.aop = k.op; }
    }
    k.wmask = (uint8_t)(rs[168] & 15u);
    float bias, slope;
    memcpy(&bias, &rs[195], 4);
    memcpy(&slope, &rs[175], 4);
    k.bias = depth_ok && (bias != 0.0f || slope != 0.0f);
    /* ---- vertex input: each declared input from its element, or a constant (0,0,0,1) */
    VkBuffer vbs[17] = { 0 };
    VkDeviceSize vb_offsets[17] = { 0 };
    for (uint32_t loc = 0; loc < 16; ++loc) {
        if (!(vs->inputs & (1u << loc))) continue;
        const d3d_vertex_element *e = s2r_input_element(d, draw->vs, loc);
        uint32_t i = k.nattr++;
        k.attr[i].loc = (uint8_t)loc;
        uint32_t tbit = e && e->type < 32 ? 1u << e->type : 0;
        s2r_buffer *b = e && e->stream < 16 && !draw->up_vertices ? s2r_buffer_of((d3d_buffer *)st->streams[e->stream]) : NULL;
        vk_buf *vb = b ? (vk_buf *)b->backend : NULL;
        int from_up = e && draw->up_vertices && e->stream == 0;
        if ((!vb && !from_up) || !(tbit & V.vtype_float)) {
            k.attr[i].binding = 16; k.attr[i].format = 255; k.attr[i].offset = 0;
            k.bindings |= 1u << 16; k.stride[16] = 0;
            vbs[16] = V.default_vb;
            continue;
        }
        k.attr[i].binding = (uint8_t)e->stream; k.attr[i].offset = e->offset;
        k.attr[i].format = e->type;
        k.bindings |= 1u << e->stream;
        if (draw->instances && (st->stream_freq[e->stream] & 0x80000000u)) k.instanced |= 1u << e->stream;
        if (from_up) {
            k.stride[0] = (uint16_t)draw->up_stride;
            vbs[0] = V.stream.buf;
            vb_offsets[0] = up_vb;
        } else {
            k.stride[e->stream] = (uint16_t)st->stream_stride[e->stream];
            vb->v[vb->cur].used = V.serial;
            vbs[e->stream] = vb->v[vb->cur].buf;
            vb_offsets[e->stream] = st->stream_offset[e->stream];
        }
    }
    VkPipeline pipe = pipeline_get(&k, vs, ps);
    if (!pipe) return;
    /* ---- index buffer */
    VkBuffer ib = VK_NULL_HANDLE;
    VkDeviceSize ib_off = 0;
    VkIndexType ib_type = VK_INDEX_TYPE_UINT16;
    if (draw->indexed) {
        if (draw->up_indices) {
            ib = V.stream.buf; ib_off = up_ib;
            ib_type = draw->up_index32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        } else {
            s2r_buffer *hb = s2r_buffer_of((d3d_buffer *)st->indices);
            vk_buf *b = hb ? (vk_buf *)hb->backend : NULL;
            if (!b) return;
            b->v[b->cur].used = V.serial;
            ib = b->v[b->cur].buf;
            ib_type = hb->format == 102 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        }
    }
    /* ---- textures: shader-readable before the pass begins */
    vk_tex *views[24] = { 0 };
    VkSampler samplers[24] = { 0 };
    uint32_t used = vs->samplers | ps->samplers;
    uint64_t types = vs->sampler_types | ps->sampler_types;
    uint64_t sig = 1469598103934665603ull;
    for (uint32_t b = 4; b < 24; ++b) {
        if (!(used & (1u << b))) continue;
        uint32_t want = (uint32_t)((types >> (2 * b)) & 3u);
        uint32_t stage = b < 20 ? b - 4 : 16 + (b - 20);
        s2r_image *img = s2r_stage_image(d, stage);
        vk_tex *tx = img ? (vk_tex *)img->backend : NULL;
        int bound = tx == t.color || tx == t.depth;
        for (uint32_t x = 0; x < t.xcount; ++x) if (tx == t.extra[x]) bound = 1;
        if (!tx || !tx->view || (uint32_t)tx->kind != want || bound) {
            views[b] = &V.dummy[want < 3 ? want : 0];
            samplers[b] = sampler_for(st->ss[stage], 1, 0, 0);
        } else {
            if (tx->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                VkCommandBuffer cb = cb_for(tx);
                transition(cb, tx, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            tx->used = V.serial;
            views[b] = tx;
            samplers[b] = sampler_for(st->ss[stage], tx->levels, 0, s2r_stage_first_level(d, stage, tx->levels));
        }
        uint64_t pair[2] = { (uint64_t)(uintptr_t)views[b]->view, (uint64_t)(uintptr_t)samplers[b] };
        sig = (sig ^ fnv64(pair, sizeof pair)) * 1099511628211ull ^ b;
    }
    /* the pass (an image read above may have ended it) */
    if (V.pass_active && !same_target(&t, &V.pass)) pass_end();
    if (!V.pass_active) pass_begin(&t, 0, NULL, NULL);
    VkCommandBuffer cb = main_cb();
    if (V.cur_pipe != pipe) { vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); V.cur_pipe = pipe; }
    if (!V.cur_set || V.cur_set_sig != sig) {
        VkDescriptorSet set = descriptor_alloc(V.dsl);
        if (!set) return;
        VkDescriptorBufferInfo bufs[4] = {
            { V.uniforms.buf, 0, UBO_CONST }, { V.uniforms.buf, 0, UBO_CONST }, { V.uniforms.buf, 0, UBO_FF }, { V.uniforms.buf, 0, UBO_PORT } };
        VkDescriptorImageInfo imgs[24];
        VkWriteDescriptorSet w[25];
        uint32_t n = 0;
        w[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w[n].dstSet = set; w[n].dstBinding = 0; w[n].descriptorCount = 4;
        w[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[n].pBufferInfo = bufs;
        ++n;
        for (uint32_t b = 4; b < 24; ++b) {
            if (!views[b]) continue;
            imgs[b].sampler = samplers[b]; imgs[b].imageView = views[b]->view;
            imgs[b].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w[n] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[n].dstSet = set; w[n].dstBinding = b; w[n].descriptorCount = 1;
            w[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n].pImageInfo = &imgs[b];
            ++n;
        }
        vkUpdateDescriptorSets(V.dev, n, w, 0, NULL);
        V.cur_set = set;
        V.cur_set_sig = sig;
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout, 0, 1, &V.cur_set, 4, offs);
    for (uint32_t b = 0; b < 17; ++b) {
        if (!(k.bindings & (1u << b))) continue;
        if (V.cur_vb[b] != vbs[b] || V.cur_vb_off[b] != vb_offsets[b]) {
            vkCmdBindVertexBuffers(cb, b, 1, &vbs[b], &vb_offsets[b]);
            V.cur_vb[b] = vbs[b]; V.cur_vb_off[b] = vb_offsets[b];
        }
    }
    if (draw->indexed && (V.cur_ib != ib || V.cur_ib_off != ib_off || V.cur_ib_type != ib_type)) {
        vkCmdBindIndexBuffer(cb, ib, ib_off, ib_type);
        V.cur_ib = ib; V.cur_ib_off = ib_off; V.cur_ib_type = ib_type;
    }
    /* ---- dynamic state */
    uint32_t viewport[4], scissor[4];
    memcpy(viewport, st->viewport, sizeof viewport);
    memcpy(scissor, st->scissor, sizeof scissor);
    /* Direct3D pixels in the target's stored pixels (the render scale), edges scaled alike */
    float ks = t.scale;
    int32_t vx = s2r_scaled((int32_t)viewport[0], ks), vy = s2r_scaled((int32_t)viewport[1], ks);
    VkViewport vp = { (float)vx, (float)vy, (float)(s2r_scaled((int32_t)(viewport[0] + viewport[2]), ks) - vx),
        (float)(s2r_scaled((int32_t)(viewport[1] + viewport[3]), ks) - vy), st->viewport_z[0], st->viewport_z[1] };
    if (vp.width <= 0) vp.width = 1;
    if (vp.height <= 0) vp.height = 1;
    if (vp.minDepth < 0) vp.minDepth = 0;
    if (vp.maxDepth > 1) vp.maxDepth = 1;
    VkRect2D sc = { { 0, 0 }, { t.w, t.h } };
    if (rs[174]) {
        int32_t l = s2r_scaled((int32_t)scissor[0], ks), tp = s2r_scaled((int32_t)scissor[1], ks);
        int32_t r = s2r_scaled((int32_t)(scissor[0] + scissor[2]), ks), bt = s2r_scaled((int32_t)(scissor[1] + scissor[3]), ks);
        if (l < 0) l = 0;
        if (tp < 0) tp = 0;
        if (r > (int32_t)t.w) r = (int32_t)t.w;
        if (bt > (int32_t)t.h) bt = (int32_t)t.h;
        if (r < l) r = l;
        if (bt < tp) bt = tp;
        sc.offset.x = l; sc.offset.y = tp; sc.extent.width = (uint32_t)(r - l); sc.extent.height = (uint32_t)(bt - tp);
    }
    if (!V.dyn_valid || memcmp(&vp, &V.cur_vp, sizeof vp)) { vkCmdSetViewport(cb, 0, 1, &vp); V.cur_vp = vp; }
    if (!V.dyn_valid || memcmp(&sc, &V.cur_sc, sizeof sc)) { vkCmdSetScissor(cb, 0, 1, &sc); V.cur_sc = sc; }
    uint32_t bf = rs[193];
    if (!V.dyn_valid || bf != V.cur_blend) {
        float c[4] = { (float)((bf >> 16) & 255) / 255.0f, (float)((bf >> 8) & 255) / 255.0f, (float)(bf & 255) / 255.0f, (float)(bf >> 24) / 255.0f };
        vkCmdSetBlendConstants(cb, c);
        V.cur_blend = bf;
    }
    uint32_t ref = rs[57] & 0xFFu, cmask = rs[58] & 0xFFu, wmask = rs[59] & 0xFFu;
    if (!V.dyn_valid || ref != V.cur_ref) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, ref); V.cur_ref = ref; }
    if (!V.dyn_valid || cmask != V.cur_cmask) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, cmask); V.cur_cmask = cmask; }
    if (!V.dyn_valid || wmask != V.cur_wmask) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, wmask); V.cur_wmask = wmask; }
    /* D3D's depth bias is in depth units; D24's unit is 2^-24 */
    float constant = bias * 16777215.0f;
    if (!V.dyn_valid || constant != V.cur_bias[0] || slope != V.cur_bias[1]) {
        vkCmdSetDepthBias(cb, constant, 0.0f, slope);
        V.cur_bias[0] = constant; V.cur_bias[1] = slope;
    }
    V.dyn_valid = 1;
    uint32_t instances = draw->instances ? draw->instances : 1;
    if (draw->indexed) {
        int32_t base = draw->up_indices ? 0 : draw->base_vertex;
        vkCmdDrawIndexed(cb, draw->count, instances, draw->up_indices ? 0 : draw->start, base, 0);
    } else vkCmdDraw(cb, draw->count, instances, draw->up_vertices ? 0 : draw->start, 0);
}

/* ============================================================== copies */
static int vk_blit(s2r_device *d, const s2r_target *src, const uint32_t *sr, const s2r_target *dst, const uint32_t *dr, int linear)
{
    (void)d;
    if (!V.ready) return 0;
    vk_tex *s = (vk_tex *)src->img->backend, *t = (vk_tex *)dst->img->backend;
    if (!s || !t || !s->image || !t->image || s->compressed || t->compressed) return 0;
    if (s->depth != t->depth) return 0;
    pass_end();
    VkCommandBuffer cb = main_cb();
    VkImageLayout sback = s->layout, tback = t->layout;
    if (s == t) {
        LOGVK("StretchRect within one image is not supported");
        return 0;
    }
    transition(cb, s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = { 0 };
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = aspect_of(s);
    blit.srcSubresource.mipLevel = src->level; blit.dstSubresource.mipLevel = dst->level;
    blit.srcSubresource.baseArrayLayer = s->kind == S2R_TEX_CUBE ? src->face : 0;
    blit.dstSubresource.baseArrayLayer = t->kind == S2R_TEX_CUBE ? dst->face : 0;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    /* rects in each image's stored pixels (the render scale) */
    float ks = s2r_scale_of(src->img), kd = s2r_scale_of(dst->img);
    blit.srcOffsets[0].x = s2r_scaled((int32_t)sr[0], ks); blit.srcOffsets[0].y = s2r_scaled((int32_t)sr[1], ks);
    blit.srcOffsets[1].x = s2r_scaled((int32_t)sr[2], ks); blit.srcOffsets[1].y = s2r_scaled((int32_t)sr[3], ks);
    blit.srcOffsets[1].z = 1;
    blit.dstOffsets[0].x = s2r_scaled((int32_t)dr[0], kd); blit.dstOffsets[0].y = s2r_scaled((int32_t)dr[1], kd);
    blit.dstOffsets[1].x = s2r_scaled((int32_t)dr[2], kd); blit.dstOffsets[1].y = s2r_scaled((int32_t)dr[3], kd);
    blit.dstOffsets[1].z = 1;
    int same = blit.srcOffsets[1].x - blit.srcOffsets[0].x == blit.dstOffsets[1].x - blit.dstOffsets[0].x &&
               blit.srcOffsets[1].y - blit.srcOffsets[0].y == blit.dstOffsets[1].y - blit.dstOffsets[0].y;
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
        s->depth || same || !linear ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    transition(cb, s, sback == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : sback);
    VkImageLayout rest = t->depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
        t->is_bb ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    transition(cb, t, tback == VK_IMAGE_LAYOUT_UNDEFINED ? rest : tback == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? rest : tback);
    s->used = t->used = V.serial;
    return 1;
}

/* ============================================================ swapchain */
static void swapchain_destroy(void)
{
    for (uint32_t i = 0; i < V.sc_count; ++i) {
        if (V.sc_fbs[i]) vkDestroyFramebuffer(V.dev, V.sc_fbs[i], NULL);
        if (V.sc_views[i]) vkDestroyImageView(V.dev, V.sc_views[i], NULL);
        if (V.sc_done[i]) vkDestroySemaphore(V.dev, V.sc_done[i], NULL);
        V.sc_fbs[i] = VK_NULL_HANDLE; V.sc_views[i] = VK_NULL_HANDLE; V.sc_done[i] = VK_NULL_HANDLE;
    }
    V.sc_count = 0;
}

static int present_init(VkFormat format);

static int swapchain_create(void)
{
    if (!V.surface) return 0;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surface, &caps) != VK_SUCCESS) return 0;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &n, NULL);
    VkSurfaceFormatKHR formats[32];
    if (n > 32) n = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &n, formats);
    VkSurfaceFormatKHR fmt = formats[0];
    for (uint32_t i = 0; i < n; ++i)
        if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) { fmt = formats[i]; break; }
    /* Pre-rotation: the swapchain keeps the display's own orientation and the
     * present pass rotates the picture, so the compositor need not. */
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { extent.width = V.win_w; extent.height = V.win_h; }
    if (transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) {
        uint32_t w = extent.width; extent.width = extent.height; extent.height = w;
    }
    if (!(transform & (VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                       VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)))
        transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    uint32_t pm_count = 0;
    VkPresentModeKHR modes[8];
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &pm_count, NULL);
    if (pm_count > 8) pm_count = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &pm_count, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!V.sc_fifo)
        for (uint32_t i = 0; i < pm_count; ++i) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    /* the driver's minimum, at least three (OpenGL ES's window has three): one image more cost
       10.5 MB at 2424x1080 and two more were in use on the Pixel 10a */
    uint32_t count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_IMAGES) count = MAX_IMAGES;
    VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = V.surface;
    ci.minImageCount = count;
    ci.imageFormat = fmt.format; ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = transform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = V.swapchain;
    VkSwapchainKHR sc;
    VkResult r = vkCreateSwapchainKHR(V.dev, &ci, NULL, &sc);
    if (V.swapchain) { swapchain_destroy(); vkDestroySwapchainKHR(V.dev, V.swapchain, NULL); V.swapchain = VK_NULL_HANDLE; }
    if (r != VK_SUCCESS) { LOGVK("vkCreateSwapchainKHR failed: %d", (int)r); return 0; }
    V.swapchain = sc;
    V.sc_format = fmt.format; V.sc_extent = extent; V.sc_transform = transform;
    if (!V.present_pipe || V.present_format != fmt.format) {
        if (!present_init(fmt.format)) { LOGVK("present pass failed"); return 0; }
    }
    V.sc_count = MAX_IMAGES;
    vkGetSwapchainImagesKHR(V.dev, sc, &V.sc_count, V.sc_images);
    for (uint32_t i = 0; i < V.sc_count; ++i) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = V.sc_images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        check(vkCreateImageView(V.dev, &vi, NULL, &V.sc_views[i]), "vkCreateImageView (swapchain)");
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fi.renderPass = V.present_rp; fi.attachmentCount = 1; fi.pAttachments = &V.sc_views[i];
        fi.width = extent.width; fi.height = extent.height; fi.layers = 1;
        check(vkCreateFramebuffer(V.dev, &fi, NULL, &V.sc_fbs[i]), "vkCreateFramebuffer (swapchain)");
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(V.dev, &si, NULL, &V.sc_done[i]);
    }
    V.sc_dirty = 0;
    LOGVK("swapchain %ux%u format %d, %u images, transform %X, %s", extent.width, extent.height, (int)fmt.format, V.sc_count,
        (unsigned)transform, mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" : "MAILBOX");
    return 1;
}

/* The window the activity shows now; a new one replaces surface and swapchain.
 * The serial is acknowledged once the old surface is gone (android_main.c). */
static void follow_window(void)
{
    uint32_t serial = 0;
    void *current = void_android_window_current(&serial, 0);   /* an acquired reference */
    int same = serial == V.window_serial && V.window_serial && (current != NULL) == (V.window != NULL);
    if (current) ANativeWindow_release((ANativeWindow *)current);
    if (same) return;
    vkDeviceWaitIdle(V.dev);
    if (V.swapchain) { swapchain_destroy(); vkDestroySwapchainKHR(V.dev, V.swapchain, NULL); V.swapchain = VK_NULL_HANDLE; }
    if (V.surface) { vkDestroySurfaceKHR(V.instance, V.surface, NULL); V.surface = VK_NULL_HANDLE; }
    if (V.window) { ANativeWindow_release(V.window); V.window = NULL; }
    ANativeWindow *window = (ANativeWindow *)void_android_window_acquire(&serial);
    V.window_serial = serial;
    if (window) {
        V.window = window;
        V.win_w = (uint32_t)ANativeWindow_getWidth(window);
        V.win_h = (uint32_t)ANativeWindow_getHeight(window);
        VkAndroidSurfaceCreateInfoKHR si = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        si.window = window;
        if (vkCreateAndroidSurfaceKHR(V.instance, &si, NULL, &V.surface) == VK_SUCCESS) swapchain_create();
        else LOGVK("vkCreateAndroidSurfaceKHR failed");
        LOGVK("presenting into window %ux%u (serial %u)", V.win_w, V.win_h, serial);
    }
    void_android_window_ack(serial);
}

/* =============================================================== present */
/* Pixels of the fitted rect (x, y, w, h of the window, rows down) -> the swapchain's clip space,
 * with the pre-rotation present_corners applies: rows of a 2x3 matrix. */
static void present_xform(int32_t x, int32_t y, float xform[2][4])
{
    float l0[3] = { 2.0f / (float)V.win_w, 0.0f, 2.0f * (float)x / (float)V.win_w - 1.0f };
    float l1[3] = { 0.0f, 2.0f / (float)V.win_h, 2.0f * (float)y / (float)V.win_h - 1.0f };
    for (int i = 0; i < 3; ++i) {
        float a = l0[i], b = l1[i], ox = a, oy = b;
        switch (V.sc_transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: ox = -b; oy = a; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: ox = -a; oy = -b; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: ox = b; oy = -a; break;
        default: break;
        }
        xform[0][i] = ox; xform[1][i] = oy;
    }
    xform[0][3] = xform[1][3] = 0.0f;
}

/* The game's gamma table into its image, in the frame's pre-transfer commands, when it changed. */
static void gamma_upload(s2r_device *d)
{
    if (V.gamma_valid && V.gamma_serial == d->gamma_serial) return;
    uint32_t at = ring_alloc(&V.staging, sizeof d->gamma_lut, 16);
    memcpy(V.staging.mem.map + at, d->gamma_lut, sizeof d->gamma_lut);
    transition(pre_cb(), &V.gamma_lut, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = { 0 };
    c.bufferOffset = at;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = 256; c.imageExtent.height = 1; c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(pre_cb(), V.staging.buf, V.gamma_lut.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    transition(pre_cb(), &V.gamma_lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    V.gamma_serial = d->gamma_serial;
    V.gamma_valid = 1;
    LOGVK("gamma table uploaded (%s; entry 128 = %u,%u,%u)", d->gamma_identity ? "identity" : "applied",
        d->gamma_lut[512], d->gamma_lut[513], d->gamma_lut[514]);
}

static void present_corners(uint32_t sw, uint32_t sh, float pos[8])
{
    int32_t x, y, w, h;
    s2r_fit(sw, sh, V.win_w, V.win_h, &x, &y, &w, &h);
    float l = 2.0f * (float)x / (float)V.win_w - 1.0f, r = 2.0f * (float)(x + w) / (float)V.win_w - 1.0f;
    float t = 2.0f * (float)y / (float)V.win_h - 1.0f, b = 2.0f * (float)(y + h) / (float)V.win_h - 1.0f;
    float c[8] = { l, t, r, t, l, b, r, b };
    for (int i = 0; i < 4; ++i) {
        float px = c[2 * i], py = c[2 * i + 1], ox = px, oy = py;
        switch (V.sc_transform) {
        case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: ox = -py; oy = px; break;
        case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: ox = -px; oy = -py; break;
        case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: ox = py; oy = -px; break;
        default: break;
        }
        pos[2 * i] = ox; pos[2 * i + 1] = oy;
    }
}

/* ============================================================== filters */
/* The present's filters (s2r_filter.c: FSR 1 EASU and RCAS, bilinear, FXAA) and the multisampled
 * passes' reload: a full-screen strip, binding 0 the source, push constants (output size, RCAS
 * sharpness). SPIR-V from glslang on the device, cached with the device's translations. */
static VkShaderModule filter_module(int pixel, int which)
{
    VkShaderModule *slot = pixel ? &V.filter_fs[which] : &V.filter_vs;
    if (*slot) return *slot;
    char *src;
    if (pixel) src = s2r_filter_fragment(which, 1);
    else {
        const char *text = s2r_filter_vertex(1);
        src = (char *)malloc(strlen(text) + 1);
        if (src) strcpy(src, text);
    }
    if (!src) return VK_NULL_HANDLE;
    *slot = text_module(pixel, src, 0xF117E40000000000ull | (uint64_t)(pixel ? which + 1 : 0));
    return *slot;
}

static int filter_layout_init(void)
{
    if (V.filter_layout) return 1;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 1; dci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &V.filter_dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1; lci.pSetLayouts = &V.filter_dsl; lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(V.dev, &lci, NULL, &V.filter_layout) != VK_SUCCESS) return 0;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    return vkCreateSampler(V.dev, &sci, NULL, &V.filter_sampler) == VK_SUCCESS;
}

static VkPipeline filter_pipeline_make(int which, VkRenderPass rp, uint32_t samples)
{
    VkShaderModule vs = filter_module(0, 0), fs = filter_module(1, which);
    if (!vs || !fs || !filter_layout_init()) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = samples > 1 ? (VkSampleCountFlagBits)samples : VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb; ci.pDynamicState = &dy; ci.layout = V.filter_layout; ci.renderPass = rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    return pipe;
}

static VkPipeline reload_pipeline(VkFormat cfmt, VkFormat dfmt, uint32_t samples)
{
    for (int i = 0; i < V.reload_count; ++i)
        if (V.reloads[i].cfmt == cfmt && V.reloads[i].dfmt == dfmt && V.reloads[i].samples == samples) return V.reloads[i].pipe;
    VkPipeline pipe = filter_pipeline_make(S2R_FILTER_COPY, compat_pass(cfmt, dfmt, samples, 0, NULL), samples);
    if (!pipe) LOGVK("multisampled reload pipeline failed: passes that load start black");
    if (V.reload_count < 8) {
        V.reloads[V.reload_count].cfmt = cfmt; V.reloads[V.reload_count].dfmt = dfmt;
        V.reloads[V.reload_count].samples = samples; V.reloads[V.reload_count].pipe = pipe;
        ++V.reload_count;
    }
    return pipe;
}

static VkDescriptorSet filter_set(const vk_tex *source)
{
    if (!filter_layout_init()) return VK_NULL_HANDLE;
    VkDescriptorSet set = descriptor_alloc(V.filter_dsl);
    if (!set) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii = { V.filter_sampler, source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(V.dev, 1, &w, 0, NULL);
    return set;
}

/* The launcher's filters into output-size images, recorded at the end of the frame's scene: the
 * image the present pass shows, or NULL (the back buffer is shown as it is). */
static vk_tex *run_filters(vk_tex *input, uint32_t w, uint32_t h)
{
    int passes[3];
    int count = s2r_filter_chain(input->width, input->height, w, h, passes);
    if (!count || !w || !h) return NULL;
    if (V.filter_w != w || V.filter_h != h || !V.filter_img[0].image) {
        static const uint8_t identity[4] = { S2R_SW_R, S2R_SW_G, S2R_SW_B, S2R_SW_A };
        for (int i = 0; i < 2; ++i) {
            if (V.filter_img[i].image) tex_release(&V.filter_img[i]);
            if (!tex_init(&V.filter_img[i], S2R_TEX_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, identity, 1, 0, 0)) {
                V.filter_w = V.filter_h = 0;
                return NULL;
            }
        }
        V.filter_w = w; V.filter_h = h;
        LOGVK("present filters at %ux%u", w, h);
    }
    VkRenderPass compat = compat_pass(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 1, 0, NULL);
    vk_rp_key k;
    memset(&k, 0, sizeof k);
    k.cfmt = VK_FORMAT_R8G8B8A8_UNORM; k.samples = 1;
    k.cload = VK_ATTACHMENT_LOAD_OP_DONT_CARE; k.cinit = VK_IMAGE_LAYOUT_UNDEFINED;
    k.cfinal = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkRenderPass rp = render_pass(&k);
    pass_end();
    VkCommandBuffer cb = main_cb();
    vk_tex *source = input, *result = NULL;
    transition(cb, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (int i = 0; i < count; ++i) {
        int which = passes[i];
        if (!V.filter_pipe[which] && !V.filter_failed[which]) {
            V.filter_pipe[which] = filter_pipeline_make(which, compat, 1);
            if (!V.filter_pipe[which]) { V.filter_failed[which] = 1; LOGVK("filter %s failed: not applied", s2r_filter_name(which)); }
            else LOGVK("filter %s ready", s2r_filter_name(which));
        }
        VkDescriptorSet set = V.filter_pipe[which] ? filter_set(source) : VK_NULL_HANDLE;
        if (!set) { result = NULL; break; }
        vk_tex *out = &V.filter_img[i & 1];
        VkImageView view = out->att[0];
        VkFramebuffer fb = framebuffer(&view, 1, w, h, compat);
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = rp; bi.framebuffer = fb;
        bi.renderArea.extent.width = w; bi.renderArea.extent.height = h;
        vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.filter_pipe[which]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.filter_layout, 0, 1, &set, 0, NULL);
        VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { w, h } };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        float push[4] = { (float)w, (float)h, 0.757858283f, 0.0f };   /* RCAS: FsrRcasCon(0.4), d3d9_filter.c's */
        vkCmdPushConstants(cb, V.filter_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        vkCmdDraw(cb, 4, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        out->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        out->used = V.serial;
        source->used = V.serial;
        source = result = out;
    }
    V.cur_pipe = VK_NULL_HANDLE;
    V.cur_set = VK_NULL_HANDLE;
    V.dyn_valid = 0;
    return result;
}

static void slot_next(void)
{
    V.cur = (V.cur + 1) % FRAMES;
    slot_wait(V.cur);
    slot_begin();
}

static void vk_present(s2r_device *d)
{
    if (!V.ready) return;
    follow_window();
    s2r_image *shown = s2r_backbuffer_image(d);
    vk_tex *show = shown ? (vk_tex *)shown->backend : NULL;
    pass_end();
    if (!V.sc_fifo) { V.sc_fifo = 1; V.sc_dirty = 1; }
    s2r_pace();
    uint64_t present_start = s2r_now_ns();
    if (V.swapchain && V.sc_dirty) { vkDeviceWaitIdle(V.dev); swapchain_create(); }
    vk_slot *s = &V.slots[V.cur];
    uint32_t image = UINT32_MAX;
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    if (V.swapchain && show) {
        d3d_rt_wait_begin();
        r = vkAcquireNextImageKHR(V.dev, V.swapchain, UINT64_MAX, s->acquire, VK_NULL_HANDLE, &image);
        d3d_rt_wait_end();
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { V.sc_dirty = 1; image = UINT32_MAX; }
    }
    if (image == UINT32_MAX || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)) {
        /* no window: the frame still completes, paced like a display */
        slot_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        slot_next();
        pipeline_cache_save();
        d->present_ms = 0;
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR) V.sc_dirty = 1;
    gamma_upload(d);
    /* the back buffer's aspect as the game knows it (the render scale stores it larger or smaller) */
    uint32_t game_w = shown->d3d_width ? shown->d3d_width : show->width, game_h = shown->d3d_height ? shown->d3d_height : show->height;
    int32_t fx, fy, fw, fh;
    s2r_fit(game_w, game_h, V.win_w, V.win_h, &fx, &fy, &fw, &fh);
    vk_tex *filtered = fw > 0 && fh > 0 ? run_filters(show, (uint32_t)fw, (uint32_t)fh) : NULL;
    vk_tex *source = filtered ? filtered : show;
    transition(main_cb(), show, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    show->used = V.serial;
    source->used = V.serial;
    V.gamma_lut.used = V.serial;
    /* The scene's batch goes now, without the swapchain's semaphore; the present pass follows in
       a batch of its own that waits for the image (the slot stays current until slot_next). */
    pass_end();
    {
        vk_slot *cur = &V.slots[V.cur];
        if (V.ts_ok) vkCmdWriteTimestamp(cur->main, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, V.tpool, (uint32_t)V.cur * 2u + 1u);
        vkEndCommandBuffer(cur->pre);
        vkEndCommandBuffer(cur->main);
        VkCommandBuffer scene[2] = { cur->pre, cur->main };
        VkSubmitInfo ssi = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        ssi.commandBufferCount = 2; ssi.pCommandBuffers = scene;
        check(vkQueueSubmit(V.queue, 1, &ssi, cur->fence), "vkQueueSubmit");
        cur->serial = V.serial;
        cur->submitted = 1;
        cur->scene_done = 0;
        cur->present_submitted = 0;
    }
    VkCommandBuffer cb = s->present;
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    VkClearValue black = { 0 };
    black.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    bi.renderPass = V.present_rp; bi.framebuffer = V.sc_fbs[image];
    bi.renderArea.extent = V.sc_extent;
    bi.clearValueCount = 1; bi.pClearValues = &black;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkDescriptorSet set = descriptor_alloc(V.present_dsl);
    if (set) {
        VkDescriptorImageInfo ii[2] = { { V.present_sampler, source->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                        { V.present_sampler, V.gamma_lut.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        /* one write a binding: a single write spanning both left the table unwritten on the
           Adreno 650 driver (1.1.128), and the gamma pass presented black */
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        for (uint32_t k = 0; k < 2; ++k) {
            w[k].dstSet = set; w[k].dstBinding = k; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(V.dev, 2, w, 0, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.present_pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, V.present_layout, 0, 1, &set, 0, NULL);
        VkViewport vp = { 0, 0, (float)V.sc_extent.width, (float)V.sc_extent.height, 0, 1 };
        VkRect2D sc = { { 0, 0 }, V.sc_extent };
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &sc);
        float push[12] = { 0 };
        present_corners(game_w, game_h, push);
        push[8] = d->gamma_identity ? 0.0f : 1.0f;
        vkCmdPushConstants(cb, V.present_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        vkCmdDraw(cb, 4, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    if (show->is_bb) transition(cb, show, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkEndCommandBuffer(cb);
    V.cur_pipe = VK_NULL_HANDLE;
    V.cur_set = VK_NULL_HANDLE;
    {
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo psi = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        psi.waitSemaphoreCount = 1; psi.pWaitSemaphores = &s->acquire; psi.pWaitDstStageMask = &stage;
        psi.commandBufferCount = 1; psi.pCommandBuffers = &cb;
        psi.signalSemaphoreCount = 1; psi.pSignalSemaphores = &V.sc_done[image];
        check(vkQueueSubmit(V.queue, 1, &psi, s->present_fence), "vkQueueSubmit");
        s->present_submitted = 1;
        ++V.serial;
    }
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &V.sc_done[image];
    pi.swapchainCount = 1; pi.pSwapchains = &V.swapchain; pi.pImageIndices = &image;
    d3d_rt_wait_begin();
    r = vkQueuePresentKHR(V.queue, &pi);
    d3d_rt_wait_end();
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) V.sc_dirty = 1;
    else check(r, "vkQueuePresentKHR");
    slot_next();
    d->present_ms = (double)(s2r_now_ns() - present_start) / 1e6;
    if (V.pipes_new && (d->frame % 600) == 0) pipeline_cache_save();
    if ((d->frame % 120) == 0) buf_trim();
    ring_report(d->frame);
}

/* ============================================================== queries */
static void *vk_query_create(s2r_device *d, uint32_t type)
{
    (void)d;
    vk_query *q = (vk_query *)calloc(1, sizeof(vk_query));
    q->type = type;
    return q;
}

static void vk_query_issue(s2r_device *d, s2r_query *hq, uint32_t flags)
{
    vk_query *q = (vk_query *)hq->backend;
    if (!V.ready || !q) return;
    if (q->type == 8) {                                /* EVENT: done when the work recorded so far is */
        if (flags & 1u) { q->serial = V.serial; q->active = 0; }
        return;
    }
    if ((flags & 2u)) {                                /* occlusion BEGIN */
        if (V.qnext[V.cur] >= QUERIES_PER_SLOT) { q->serial = 0; return; }
        vk_target t;
        target_select(d, &t);
        if (!V.pass_active) pass_begin(&t, 0, NULL, NULL);
        if (open_query) { vkCmdEndQuery(main_cb(), V.qpool, open_query->index); open_query->active = 0; open_query->split = 1; }
        q->index = (uint32_t)V.cur * QUERIES_PER_SLOT + V.qnext[V.cur]++;
        q->serial = V.serial;
        q->active = 1; q->split = 0;
        vkCmdBeginQuery(main_cb(), V.qpool, q->index, 0);
        open_query = q;
    }
    if ((flags & 1u) && q->active) {
        if (open_query == q && V.pass_active) vkCmdEndQuery(main_cb(), V.qpool, q->index);
        if (open_query == q) open_query = NULL;
        q->active = 0;
    }
}

static int vk_query_result(s2r_device *d, s2r_query *hq, uint32_t *value, int flush)
{
    vk_query *q = (vk_query *)hq->backend;
    if (!q || !q->serial) { *value = q && q->type == 8 ? 1u : d->dev->st.viewport[2] * d->dev->st.viewport[3]; return 1; }
    if (q->serial >= V.serial) {
        /* Recorded in the frame not submitted yet: Direct3D's GetData(FLUSH) hands the GPU the
           work so far, which this answer can then wait for (GL's glFlush after the fence). The
           render thread's idle watch asks without FLUSH: the frame waits for its present. */
        if (!flush) return 0;
        static int logged;
        if (logged++ < 4) LOGVK("query of the open frame polled: work submitted mid-frame (frame %u)", d->frame);
        pass_end();
        slot_submit(VK_NULL_HANDLE, VK_NULL_HANDLE);
        slot_next();
        return 0;
    }
    if (q->type == 8) {
        /* EVENT of a submitted frame: reached once the frame before it is complete. The game waits at
           the start of a frame for the event it issued at the end of the previous one; Vulkan gets a
           frame's commands only at its present, so waiting for that frame itself ran the CPU and the
           GPU strictly in turn (9.2 ms + 4.4 ms of GPU a frame on the test phone, 73 fps). The GL
           driver's own flushes give the same frame in flight; the backend's buffer versions, staging
           copies and barriers keep every resource safe without the game's wait. */
        if (q->serial > V.scene_completed + 1u) poll_completed();
        if (q->serial > V.scene_completed + 1u) return 0;
        *value = 1;
        return 1;
    }
    if (q->serial > V.scene_completed) { poll_completed(); if (q->serial > V.scene_completed) return 0; }
    uint32_t count = 0;
    VkResult r = vkGetQueryPoolResults(V.dev, V.qpool, q->index, 1, sizeof count, &count, sizeof count, 0);
    if (r == VK_NOT_READY) return 0;
    *value = r != VK_SUCCESS || q->split ? d->dev->st.viewport[2] * d->dev->st.viewport[3] : count;
    return 1;
}

static void vk_query_destroy(s2r_device *d, s2r_query *hq)
{
    (void)d;
    if (open_query == hq->backend) {
        if (V.pass_active) vkCmdEndQuery(main_cb(), V.qpool, open_query->index);
        open_query = NULL;
    }
    free(hq->backend);
    hq->backend = NULL;
}

/* ======================================================== back buffer */
static void vk_backbuffer(s2r_device *d)
{
    (void)d;
    pass_end();
}

/* ================================================================ init */
static VkShaderModule raw_module(int which)
{
    uint32_t words = 0;
    const uint32_t *code = s2r_builtin_spirv(which, &words);
    return code ? words_module(code, words) : VK_NULL_HANDLE;
}

static VkPipeline present_pipeline(int which_vs, int which_ps, VkPipelineLayout layout, VkPrimitiveTopology topology,
    int blend)
{
    VkShaderModule vs = raw_module(which_vs), fs = raw_module(which_ps);
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(V.dev, vs, NULL);
        if (fs) vkDestroyShaderModule(V.dev, fs, NULL);
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = { { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
                                              { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = topology;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { 0 };
    cba.colorWriteMask = 15;
    if (blend) {
        /* the Direct3D 9 layer's route blend: SRC_ALPHA, ONE_MINUS_SRC_ALPHA; alpha ONE, ONE_MINUS_SRC_ALPHA */
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    ci.stageCount = 2; ci.pStages = st; ci.pVertexInputState = &vi; ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp; ci.pRasterizationState = &rs; ci.pMultisampleState = &ms; ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy; ci.layout = layout; ci.renderPass = V.present_rp;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
    vkDestroyShaderModule(V.dev, vs, NULL);
    vkDestroyShaderModule(V.dev, fs, NULL);
    return pipe;
}

static int present_init(VkFormat format)
{
    if (V.present_pipe) { vkDestroyPipeline(V.dev, V.present_pipe, NULL); V.present_pipe = VK_NULL_HANDLE; }
    if (V.present_rp) { vkDestroyRenderPass(V.dev, V.present_rp, NULL); V.present_rp = VK_NULL_HANDLE; }
    V.present_format = format;
    VkAttachmentDescription at = { 0 };
    at.format = format;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep = { 0 };
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.srcAccessMask = 0;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rci.attachmentCount = 1; rci.pAttachments = &at; rci.subpassCount = 1; rci.pSubpasses = &sp;
    rci.dependencyCount = 1; rci.pDependencies = &dep;
    if (vkCreateRenderPass(V.dev, &rci, NULL, &V.present_rp) != VK_SUCCESS) return 0;
    /* binding 0 the back buffer, 1 the gamma table (shaders/present.frag) */
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 2; dci.pBindings = b;
    if (!V.present_dsl) vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &V.present_dsl);
    VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 48 };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1; lci.pSetLayouts = &V.present_dsl; lci.pushConstantRangeCount = 1; lci.pPushConstantRanges = &pr;
    if (!V.present_layout) vkCreatePipelineLayout(V.dev, &lci, NULL, &V.present_layout);
    if (!V.gamma_lut.image) {
        static const uint8_t identity[4] = { S2R_SW_R, S2R_SW_G, S2R_SW_B, S2R_SW_A };
        if (!tex_init(&V.gamma_lut, S2R_TEX_2D, VK_FORMAT_R8G8B8A8_UNORM, 256, 1, 1, 1, identity, 0, 0, 0)) return 0;
        V.gamma_valid = 0;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!V.present_sampler) vkCreateSampler(V.dev, &sci, NULL, &V.present_sampler);
    V.present_pipe = present_pipeline(S2R_BUILTIN_PRESENT_VS, S2R_BUILTIN_PRESENT_PS, V.present_layout,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0);
    return V.present_pipe != VK_NULL_HANDLE;
}

static int dummies_init(void)
{
    static const uint8_t identity[4] = { S2R_SW_R, S2R_SW_G, S2R_SW_B, S2R_SW_A };
    static const uint8_t black[4 * 6] = { 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255 };
    for (int k = 0; k < 3; ++k) {
        if (!tex_init(&V.dummy[k], k, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, identity, 0, 0, 0)) return 0;
        uint32_t at = ring_alloc(&V.staging, sizeof black, 16);
        memcpy(V.staging.mem.map + at, black, sizeof black);
        transition(pre_cb(), &V.dummy[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = { 0 };
        c.bufferOffset = at;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = V.dummy[k].layers;
        c.imageExtent.width = 1; c.imageExtent.height = 1; c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(pre_cb(), V.staging.buf, V.dummy[k].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        transition(pre_cb(), &V.dummy[k], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = 16; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (vkCreateBuffer(V.dev, &bi, NULL, &V.default_vb) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(V.dev, V.default_vb, &req);
    if (!mem_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 1, &V.default_vb_mem)) return 0;
    vkBindBufferMemory(V.dev, V.default_vb, V.default_vb_mem.mem, V.default_vb_mem.offset);
    static const float one[4] = { 0, 0, 0, 1 };
    memcpy(V.default_vb_mem.map, one, sizeof one);
    return 1;
}

static int format_samples(VkFormat f)
{
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(V.phys, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

/* The driver's functions: global ones before the instance, the rest from it (device-level ones again
   from vkGetDeviceProcAddr once the device exists, which skips the loader's trampolines). */
static int vk_load_global(PFN_vkGetInstanceProcAddr gipa)
{
    vk_gipa = gipa;
#define S2R_VK_LOAD_GLOBAL(name) name = (PFN_##name)gipa(NULL, #name);
    S2R_VK_GLOBAL(S2R_VK_LOAD_GLOBAL)
#undef S2R_VK_LOAD_GLOBAL
    return vkCreateInstance != NULL;
}

static int vk_load_instance(void)
{
    int missing = 0;
#define S2R_VK_LOAD_INSTANCE(name) name = (PFN_##name)vk_gipa(V.instance, #name);
    S2R_VK_INSTANCE(S2R_VK_LOAD_INSTANCE)
    S2R_VK_DEVICE(S2R_VK_LOAD_INSTANCE)
#undef S2R_VK_LOAD_INSTANCE
#define S2R_VK_REQUIRED(name) if (!name && strcmp(#name, "vkGetPhysicalDeviceProperties2") && strcmp(#name, "vkDestroyDevice")) { \
        if (!missing++) LOGVK("the driver has no %s", #name); }
    S2R_VK_INSTANCE(S2R_VK_REQUIRED)
    S2R_VK_DEVICE(S2R_VK_REQUIRED)
#undef S2R_VK_REQUIRED
    return !missing;
}

static void vk_load_device(void)
{
    if (!vkGetDeviceProcAddr) return;
#define S2R_VK_LOAD_DEVICE(name) { PFN_##name f = (PFN_##name)vkGetDeviceProcAddr(V.dev, #name); if (f) name = f; }
    S2R_VK_DEVICE(S2R_VK_LOAD_DEVICE)
#undef S2R_VK_LOAD_DEVICE
}

/* What an app-local driver must be: Mesa Turnip on a Qualcomm GPU, Mesa PanVK on an ARM GPU (the
   bundled or imported library itself, mapped in this process). The player's own Turnip package may be
   another driver for Adreno (Qualcomm's own, say): any Qualcomm device is accepted then. */
static const char *vk_driver_mismatch(int kind)
{
    VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
    if (vkGetPhysicalDeviceProperties2 && V.props.apiVersion >= VK_API_VERSION_1_2) vkGetPhysicalDeviceProperties2(V.phys, &p2);
    if (driver.driverName[0]) LOGVK("driver %s %s (id %d)", driver.driverName, driver.driverInfo, (int)driver.driverID);
    if (kind == S2R_VK_TURNIP) {
        if (V.props.vendorID != 0x5143) return "not a Qualcomm GPU";
        if (!s2r_vk_custom() && driver.driverID != VK_DRIVER_ID_MESA_TURNIP && !strstr(V.props.deviceName, "Turnip"))
            return "the device is not Mesa Turnip";
    } else if (kind == S2R_VK_PANVK) {
        if (V.props.vendorID != 0x13b5) return "not an ARM GPU";
        if (!s2r_vk_custom() && driver.driverID != VK_DRIVER_ID_MESA_PANVK) return "the device is not Mesa PanVK";
        if (!s2r_vk_panvk_mapped()) return "the PanVK library is not the one in use";
    }
    return NULL;
}

static void vk_close_device(void)
{
    if (V.dev && vkDestroyDevice) vkDestroyDevice(V.dev, NULL);
    if (V.instance && vkDestroyInstance) vkDestroyInstance(V.instance, NULL);
    V.dev = VK_NULL_HANDLE; V.instance = VK_NULL_HANDLE; V.phys = VK_NULL_HANDLE;
}

/* The instance, GPU and device of one driver; 0 (everything released) when it cannot run the renderer. */
static int vk_open_device(int kind)
{
    char error[700] = "";
    PFN_vkGetInstanceProcAddr gipa = s2r_vk_open(kind, error, sizeof error);
    if (!gipa || !vk_load_global(gipa)) {
        LOGVK("%s Vulkan driver unavailable: %s", s2r_vk_driver_name(kind), error[0] ? error : "no vkCreateInstance");
        return 0;
    }
    uint32_t loader = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader);
    if (loader < VK_API_VERSION_1_1) {
        LOGVK("%s Vulkan driver: instance version %u.%u, the renderer needs 1.1", s2r_vk_driver_name(kind),
            VK_API_VERSION_MAJOR(loader), VK_API_VERSION_MINOR(loader));
        return 0;
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "The Sims 2"; app.pEngineName = "The Sims 2 native renderer";
    app.apiVersion = VK_API_VERSION_1_1;
    const char *iext[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ii.pApplicationInfo = &app; ii.enabledExtensionCount = 2; ii.ppEnabledExtensionNames = iext;
    VkResult r = vkCreateInstance(&ii, NULL, &V.instance);
    if (r != VK_SUCCESS) { LOGVK("%s driver: vkCreateInstance failed: %d", s2r_vk_driver_name(kind), (int)r); return 0; }
    if (!vk_load_instance()) return 0;
    uint32_t n = 1;
    r = vkEnumeratePhysicalDevices(V.instance, &n, &V.phys);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) { LOGVK("no Vulkan device"); return 0; }
    vkGetPhysicalDeviceProperties(V.phys, &V.props);
    vkGetPhysicalDeviceMemoryProperties(V.phys, &V.memprops);
    if (VK_API_VERSION_MAJOR(V.props.apiVersion) == 1 && VK_API_VERSION_MINOR(V.props.apiVersion) < 1) {
        LOGVK("%s supports Vulkan %u.%u only", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion),
            VK_API_VERSION_MINOR(V.props.apiVersion));
        return 0;
    }
    const char *mismatch = kind != S2R_VK_SYSTEM ? vk_driver_mismatch(kind) : NULL;
    if (mismatch) { LOGVK("%s driver (%s): %s", s2r_vk_driver_name(kind), V.props.deviceName, mismatch); return 0; }
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(V.phys, &have);
    V.have = have;
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &qn, NULL);
    VkQueueFamilyProperties qf[16];
    if (qn > 16) qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &qn, qf);
    V.queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; ++i) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { V.queue_family = i; break; }
    if (V.queue_family == UINT32_MAX) { LOGVK("no graphics queue"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = V.queue_family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures want = { 0 };
    want.samplerAnisotropy = have.samplerAnisotropy;
    want.textureCompressionETC2 = have.textureCompressionETC2;
    want.textureCompressionASTC_LDR = have.textureCompressionASTC_LDR;
    want.textureCompressionBC = have.textureCompressionBC;
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dext;
    di.pEnabledFeatures = &want;
    r = vkCreateDevice(V.phys, &di, NULL, &V.dev);
    if (r != VK_SUCCESS) { LOGVK("%s driver: vkCreateDevice failed: %d", s2r_vk_driver_name(kind), (int)r); return 0; }
    vkGetDeviceQueue(V.dev, V.queue_family, 0, &V.queue);
    vk_load_device();
    V.driver = kind;
    return 1;
}

static int vk_init(s2r_device *d)
{
    (void)d;
    memset(&V, 0, sizeof V);
    V.serial = 1;
    int kind = s2r_vk_driver();
    if (!vk_open_device(kind)) {
        vk_close_device();
        if (kind == S2R_VK_SYSTEM || !vk_open_device(S2R_VK_SYSTEM)) { vk_close_device(); return 0; }
        LOGVK("the %s driver could not start: the system Vulkan driver renders this session", s2r_vk_driver_name(kind));
    }
    V.has_aniso = V.have.samplerAnisotropy;
    V.max_aniso = V.has_aniso ? V.props.limits.maxSamplerAnisotropy : 1.0f;
    V.has_etc2 = V.have.textureCompressionETC2 && format_samples(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
    V.has_astc = V.have.textureCompressionASTC_LDR && format_samples(VK_FORMAT_ASTC_4x4_UNORM_BLOCK);
    V.has_bc = V.have.textureCompressionBC && format_samples(VK_FORMAT_BC3_UNORM_BLOCK) && format_samples(VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    s2r_set_native_support(V.has_bc, V.has_etc2, V.has_astc);
    d3d_gl_limits.max_texture = V.props.limits.maxImageDimension2D;
    d3d_gl_limits.max_3d_texture = V.props.limits.maxImageDimension3D;
    /* Direct3D's four simultaneous render targets (MRT) */
    d3d_gl_limits.max_draw_buffers = V.props.limits.maxColorAttachments >= 4 ? 4 : V.props.limits.maxColorAttachments;
    /* The launcher's scene MSAA: transient multisampled attachments, resolved as passes end */
    uint32_t samples = (uint32_t)s2r_msaa_samples();
    VkSampleCountFlags counts = V.props.limits.framebufferColorSampleCounts & V.props.limits.framebufferDepthSampleCounts;
    if (samples && !(counts & samples)) samples = (counts & VK_SAMPLE_COUNT_2_BIT) ? 2 : 0;
    V.msaa_samples = samples;
    for (uint32_t i = 0; i < V.memprops.memoryTypeCount; ++i)
        if (V.memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) V.lazy_memory = 1;
    if (s2r_msaa_samples()) LOGVK("scene MSAA requested %dx, using %ux (%s)", s2r_msaa_samples(), V.msaa_samples,
        V.lazy_memory ? "lazily allocated attachments" : "device memory attachments");
    V.ubo_align = (uint32_t)V.props.limits.minUniformBufferOffsetAlignment;
    if (V.ubo_align < 16) V.ubo_align = 16;
    static const VkFormat depths[3] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D16_UNORM };
    for (int i = 0; i < 3; ++i) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(V.phys, depths[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { V.depth_format = depths[i]; break; }
    }
    for (uint8_t type = 0; type <= 16; ++type) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(V.phys, vertex_format(type), &fp);
        if (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) { V.vtype_float |= 1u << type; continue; }
        LOGVK("D3DDECLTYPE %u has no vertex format on this device", type);
    }
    for (int i = 0; i < FRAMES; ++i) {
        vk_slot *s = &V.slots[i];
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pci.queueFamilyIndex = V.queue_family;
        vkCreateCommandPool(V.dev, &pci, NULL, &s->pool);
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = s->pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 3;
        VkCommandBuffer cbs[3];
        vkAllocateCommandBuffers(V.dev, &ai, cbs);
        s->pre = cbs[0]; s->main = cbs[1]; s->present = cbs[2];
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkCreateFence(V.dev, &fi, NULL, &s->fence);
        vkCreateFence(V.dev, &fi, NULL, &s->present_fence);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(V.dev, &si, NULL, &s->acquire);
    }
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qpi.queryType = VK_QUERY_TYPE_OCCLUSION; qpi.queryCount = QUERIES_PER_SLOT * FRAMES;
    vkCreateQueryPool(V.dev, &qpi, NULL, &V.qpool);
    V.ts_ns = V.props.limits.timestampPeriod;
    if (V.props.limits.timestampComputeAndGraphics && V.ts_ns > 0) {
        VkQueryPoolCreateInfo tpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        tpi.queryType = VK_QUERY_TYPE_TIMESTAMP; tpi.queryCount = 2u * FRAMES;
        V.ts_ok = vkCreateQueryPool(V.dev, &tpi, NULL, &V.tpool) == VK_SUCCESS;
    }
    /* Uniforms 8 MiB (2 MiB a frame on a lot, Monty Ranch on the Pixel 10a: four frames, two in
       flight), staging 8 MiB (texture levels above a 2 MiB chunk get a buffer of their own, freed with
       the frame), DrawPrimitiveUP data 8 MiB (the interface's quads). They were 16, 64 and 32 MiB,
       resident for good (ring_report logs what a frame takes). */
    if (!ring_create(&V.uniforms, 8u << 20, 1u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 64u << 10) ||
        !ring_create(&V.staging, 8u << 20, 2u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0) ||
        !ring_create(&V.stream, 8u << 20, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, 0)) {
        LOGVK("ring buffers failed");
        return 0;
    }
    VkDescriptorSetLayoutBinding binds[24];
    for (uint32_t b = 0; b < 24; ++b) {
        binds[b].binding = b;
        binds[b].descriptorCount = 1;
        binds[b].pImmutableSamplers = NULL;
        if (b < 4) {
            binds[b].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            binds[b].stageFlags = b == 0 ? VK_SHADER_STAGE_VERTEX_BIT : b == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT :
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        } else {
            binds[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binds[b].stageFlags = b < 20 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        }
    }
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dci.bindingCount = 24; dci.pBindings = binds;
    vkCreateDescriptorSetLayout(V.dev, &dci, NULL, &V.dsl);
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    lci.setLayoutCount = 1; lci.pSetLayouts = &V.dsl;
    vkCreatePipelineLayout(V.dev, &lci, NULL, &V.layout);
    const char *cache = s2r_cache_dir();
    if (cache && cache[0]) {
        snprintf(V.cache_dir, sizeof V.cache_dir, "%s/vulkan", cache);
        if (mkdir(V.cache_dir, 0775) != 0 && errno != EEXIST) V.cache_dir[0] = 0;
    }
    void *pdata = NULL;
    size_t psize = 0;
    if (V.cache_dir[0]) {
        char path[1200];
        snprintf(path, sizeof path, "%s/pipelines%s.bin", V.cache_dir, V.driver == S2R_VK_TURNIP ? "-turnip" : V.driver == S2R_VK_PANVK ? "-panvk" : "");
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (size > 0) { pdata = malloc((size_t)size); psize = fread(pdata, 1, (size_t)size, f); }
            fclose(f);
        }
    }
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    pci.initialDataSize = psize; pci.pInitialData = pdata;
    if (vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache) != VK_SUCCESS) {
        pci.initialDataSize = 0; pci.pInitialData = NULL;
        vkCreatePipelineCache(V.dev, &pci, NULL, &V.pcache);
    }
    free(pdata);
    V.cur = 0;
    slot_begin();
    if (!dummies_init()) { LOGVK("dummy resources failed"); return 0; }
    uint32_t serial = 0;
    void *first = void_android_window_current(&serial, 1);   /* waits for the first window */
    if (first) ANativeWindow_release((ANativeWindow *)first);
    V.window_serial = 0;
    follow_window();
    V.ready = 1;
    LOGVK("%s (%s%s driver), Vulkan %u.%u.%u, driver %08X; BC %s, ETC2 %s, ASTC %s, anisotropy %.0f, depth format %d, UBO align %u, "
        "pipeline cache %zu bytes, GPU timestamps %s",
        V.props.deviceName, s2r_vk_driver_name(V.driver), V.driver != S2R_VK_SYSTEM && s2r_vk_custom() ? " (the player's)" : "",
        VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion),
        VK_API_VERSION_PATCH(V.props.apiVersion), V.props.driverVersion, V.has_bc ? "yes" : "no", V.has_etc2 ? "yes" : "no",
        V.has_astc ? "yes" : "no", V.max_aniso, (int)V.depth_format, V.ubo_align, psize,
        V.ts_ok ? "yes" : V.props.limits.timestampComputeAndGraphics ? "no (query pool failed)" : "no (timestampComputeAndGraphics false)");
    {
        char version[96];
        snprintf(version, sizeof version, "Vulkan %u.%u.%u %s driver %08X", VK_API_VERSION_MAJOR(V.props.apiVersion),
            VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion), s2r_vk_driver_name(V.driver),
            V.props.driverVersion);
        if (void_android_gpu_strings) void_android_gpu_strings(V.props.deviceName, version);
    }
    return 1;
}

static void vk_shutdown(s2r_device *d)
{
    (void)d;
    if (!V.dev) return;
    vkDeviceWaitIdle(V.dev);
    s2r_lib_forget_modules();
    pipeline_cache_save();
    V.ready = 0;
}

/* Everything recorded so far, submitted and complete (a readback of the CPU's view). */
static void vk_finish(s2r_device *d)
{
    (void)d;
    if (!V.ready) return;
    flush_wait();
}

/* The driver the session runs on (S2R_VK_*; the system one before the device exists). */
int s2r_vk_active_driver(void) { return V.ready ? V.driver : S2R_VK_SYSTEM; }

const s2r_backend s2r_vulkan_backend = {
    "vulkan", vk_init, vk_shutdown, vk_image_create, vk_image_destroy, vk_image_upload, vk_image_mips, vk_image_read,
    vk_buffer_create, vk_buffer_write, vk_buffer_destroy, vk_clear, vk_draw, vk_blit, vk_fill, vk_present,
    vk_query_create, vk_query_issue, vk_query_result, vk_query_destroy, vk_backbuffer, vk_finish,
};
