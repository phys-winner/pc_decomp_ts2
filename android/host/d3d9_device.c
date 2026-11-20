/* IDirect3D9 and IDirect3DDevice9. */
#include "d3d9_internal.h"
#include "input.h"
#include "sims2_settings.h"
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "d3d9_thread.inc"

d3d_device *g_d3d_device;


/* What Direct3D 9 answered on the Windows PC (NVIDIA GeForce RTX 4060, see
 * build/d3d9-native/caps-dump.txt); a few limits follow the Mali GPU. */
static const char pc_caps_hex[] =
    "010000000000000000000200000002E0A00300000F00008001000000F0BE1B00F2CE2F0091217307FF000000FF3F0000FF3F0000FF000000"
    "08420800C5EC01000007030300030303000303033F0000003F0000001F000000004000000040000000080000002000000040000010000000"
    "F902155020BCBECC20BCBECC20BCBE4C20BCBE4C00000000FF01000008001800FFFFFE0308000000080000003B0100000800000008000000"
    "040000000000000000000046FFFFFF00FFFFFF0010000000FF0000000003FEFF000100000003FFFF00E07F47510000000000000000000000"
    "0000000000000000020000000F0300000400000000030003010000001800000020000000040000001F000000180000002000000004000000"
    "0002000000070303FFFF0000FFFF00000010000000100000";

typedef struct { void_com_object com; } d3d9_t;

/* ============================================================ IDirect3D9 */
static int hex_digit(char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static void fill_caps(uint32_t out)
{
    size_t n = strlen(pc_caps_hex) / 2;
    for (size_t i = 0; i < n && i < 304; ++i)
        MEM8(out + (uint32_t)i) = (uint8_t)(hex_digit(pc_caps_hex[2 * i]) * 16 + hex_digit(pc_caps_hex[2 * i + 1]));
    /* Limits the GPU cannot reach are lowered to what GL reports (d3d_gl_init);
     * everything else stays exactly as the PC answered. */
    uint32_t tex = d3d_gl_limits.max_texture ? d3d_gl_limits.max_texture : 8192;
    uint32_t vol = d3d_gl_limits.max_3d_texture ? d3d_gl_limits.max_3d_texture : 2048;
    uint32_t rts = d3d_gl_limits.max_draw_buffers ? d3d_gl_limits.max_draw_buffers : 4;
    if (MEM32(out + 88) > tex) MEM32(out + 88) = tex;      /* MaxTextureWidth */
    if (MEM32(out + 92) > tex) MEM32(out + 92) = tex;      /* MaxTextureHeight */
    if (MEM32(out + 96) > vol) MEM32(out + 96) = vol;      /* MaxVolumeExtent */
    if (MEM32(out + 104) > tex) MEM32(out + 104) = tex;    /* MaxTextureAspectRatio */
    if (MEM32(out + 260) > rts) MEM32(out + 260) = rts;    /* NumSimultaneousRTs */
}

/* Display modes offered: the PC's familiar set plus the phone's aspect. */
static const uint32_t modes[][2] = {
    {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 720}, {1280, 768}, {1280, 800}, {1280, 960},
    {1280, 1024}, {1360, 768}, {1366, 768}, {1440, 900}, {1560, 720}, {1600, 900}, {1600, 1200}, {1680, 1050},
    {1920, 1080}, {2340, 1080},
};
#define MODE_COUNT (sizeof modes / sizeof modes[0])

COM_METHOD(d3d9, QueryInterface, 3)
{
    d3d9_t *d = (d3d9_t *)void_com_host(THIS_);
    if (void_guid_equal(ARG(1), IID_IDirect3D9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&d->com);
        MEM32(ARG(2)) = THIS_;
        return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(d3d9, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(d3d9, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(d3d9, RegisterSoftwareDevice, 2) { return D3DERR_INVALIDCALL; }
COM_METHOD(d3d9, GetAdapterCount, 1) { return 1; }

COM_METHOD(d3d9, GetAdapterIdentifier, 4)
{
    if (ARG(1)) return D3DERR_INVALIDCALL;
    uint32_t out = ARG(3);
    memset(GPTR(out), 0, 1100);
    void_utf8_to_ga("nvldumd.dll", out, 512);
    void_utf8_to_ga("NVIDIA GeForce RTX 4060", out + 512, 512);
    void_utf8_to_ga("\\\\.\\DISPLAY1", out + 1024, 32);
    MEM32(out + 1056) = 0x0010069Cu;    /* DriverVersion low */
    MEM32(out + 1060) = 0x00200000u;    /* DriverVersion high */
    MEM32(out + 1064) = 0x10DE;         /* VendorId */
    MEM32(out + 1068) = 0x2882;         /* DeviceId */
    MEM32(out + 1072) = 0x51611462u;    /* SubSysId */
    MEM32(out + 1076) = 161;            /* Revision */
    return D3D_OK;
}

static int mode_format_ok(uint32_t format) { return format == 22 || format == 23; }

/* The modes offered: the screen's (the game's resolution, which the UI
 * scale sets: sims2_settings.c) and the familiar ones that fit inside it. */
static uint32_t mode_list(uint32_t out[][2])
{
    int w, h;
    void_input_screen_size(&w, &h);
    uint32_t n = 0;
    out[n][0] = (uint32_t)w; out[n][1] = (uint32_t)h; ++n;
    for (uint32_t i = 0; i < MODE_COUNT; ++i) {
        if (modes[i][0] > (uint32_t)w || modes[i][1] > (uint32_t)h) continue;
        if (modes[i][0] == (uint32_t)w && modes[i][1] == (uint32_t)h) continue;
        out[n][0] = modes[i][0]; out[n][1] = modes[i][1]; ++n;
    }
    return n;
}

COM_METHOD(d3d9, GetAdapterModeCount, 3)
{
    uint32_t list[MODE_COUNT + 1][2];
    uint32_t n = mode_format_ok(ARG(2)) ? mode_list(list) : 0;
    VLOG("[d3d9] GetAdapterModeCount(adapter %u, format %u) = %u", ARG(1), ARG(2), n);
    return n;
}

COM_METHOD(d3d9, EnumAdapterModes, 5)
{
    uint32_t format = ARG(2), index = ARG(3), out = ARG(4);
    uint32_t list[MODE_COUNT + 1][2];
    uint32_t n = mode_list(list);
    VLOG("[d3d9] EnumAdapterModes(adapter %u, format %u, index %u)", ARG(1), format, index);
    if (!mode_format_ok(format) || index >= n) return D3DERR_INVALIDCALL;
    MEM32(out) = list[index][0]; MEM32(out + 4) = list[index][1]; MEM32(out + 8) = 60; MEM32(out + 12) = format;
    return D3D_OK;
}

COM_METHOD(d3d9, GetAdapterDisplayMode, 3)
{
    int w, h;
    void_input_screen_size(&w, &h);
    uint32_t out = ARG(2);
    MEM32(out) = (uint32_t)w; MEM32(out + 4) = (uint32_t)h; MEM32(out + 8) = 60; MEM32(out + 12) = 22;
    return D3D_OK;
}

COM_METHOD(d3d9, CheckDeviceType, 6)
{
    uint32_t adapter_format = ARG(3), back = ARG(4);
    VLOG("[d3d9] CheckDeviceType(type %u, adapter format %u, back %u, windowed %u)", ARG(2), adapter_format, back, ARG(5));
    return (adapter_format == 22 || adapter_format == 23) && (back == 21 || back == 22 || back == 23 || back == 0)
        ? D3D_OK : D3DERR_NOTAVAILABLE;
}

/* The PC's CheckDeviceFormat answers, restricted to what GLES can store. */
static int format_supported(uint32_t usage, uint32_t rtype, uint32_t format)
{
    const d3d_format *f = d3d_format_find(format);
    if (!f) return 0;
    uint32_t base = usage & 0xFFFFu;
    if (rtype == D3DRTYPE_VERTEXBUFFER || rtype == D3DRTYPE_INDEXBUFFER) return 0;
    if (base & D3DUSAGE_DEPTHSTENCIL) {
        if (!f->depth) return 0;
        /* Depth formats usable as depth-stencil surfaces; as textures only
         * INTZ/RAWZ/DF16/DF24 (raw depth reads), not NVIDIA's comparing D24S8. */
        if (rtype == D3DRTYPE_SURFACE) return 1;
        if (rtype == D3DRTYPE_TEXTURE)
            return format == FOURCC('I','N','T','Z') || format == FOURCC('D','F','2','4') || format == FOURCC('D','F','1','6');
        return 0;
    }
    if (f->depth) return 0;
    if (base & D3DUSAGE_RENDERTARGET) {
        if (!f->render_target) return 0;
        if (format == FOURCC('N','U','L','L')) return rtype == D3DRTYPE_SURFACE;
        return 1;
    }
    if (rtype == D3DRTYPE_VOLUMETEXTURE && f->block_bytes) return 1;
    if (usage & 0x00100000u) return f->is_float || format == 21;   /* QUERY_VERTEXTEXTURE: like the PC */
    if (usage & 0x00080000u) return !f->depth;                       /* POSTPIXELSHADER_BLENDING */
    if (usage & 0x00020000u) {                                       /* QUERY_FILTER */
        return !(format == 114 || format == 115 || format == 116 || format == 34 || format == 36 || format == 81);
    }
    if (usage & 0x00040000u) return format == 21 || format == 22 || format == FOURCC('D','X','T','1') ||
        format == FOURCC('D','X','T','3') || format == FOURCC('D','X','T','5');  /* QUERY_SRGBREAD */
    if (usage & 0x00010000u) return format == 21 || format == 22;    /* QUERY_SRGBWRITE */
    return format != FOURCC('N','U','L','L');
}

COM_METHOD(d3d9, CheckDeviceFormat, 7)
{
    uint32_t usage = ARG(4), rtype = ARG(5), format = ARG(6);
    int ok = format_supported(usage, rtype, format);
    VLOG("[d3d9] CheckDeviceFormat(usage %08X, rtype %u, format %u/%.4s) = %s", usage, rtype, format,
        format > 0xFFFF ? (const char *)&format : "", ok ? "OK" : "NOTAVAILABLE");
    if (ok && (usage & D3DUSAGE_AUTOGENMIPMAP)) {
        const d3d_format *f = d3d_format_find(format);
        if (f && (f->block_bytes || f->conv != CONV_NONE)) return D3DOK_NOAUTOGEN;
    }
    return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
}

COM_METHOD(d3d9, CheckDeviceMultiSampleType, 7)
{
    uint32_t type = ARG(5), quality = ARG(6);
    if (type == 0) { if (quality) MEM32(quality) = 1; return D3D_OK; }
    if (type == 2 || type == 4) { if (quality) MEM32(quality) = 1; return D3DERR_NOTAVAILABLE; }
    return D3DERR_NOTAVAILABLE;
}

COM_METHOD(d3d9, CheckDepthStencilMatch, 6) { return D3D_OK; }
COM_METHOD(d3d9, CheckDeviceFormatConversion, 5) { return D3D_OK; }

COM_METHOD(d3d9, GetDeviceCaps, 4)
{
    VLOG("[d3d9] GetDeviceCaps(adapter %u, type %u)", ARG(1), ARG(2));
    fill_caps(ARG(3));
    return D3D_OK;
}

COM_METHOD(d3d9, GetAdapterMonitor, 2) { return 0x00070001u; }

uint32_t d3d_device_create(uint32_t d3d9, uint32_t window, uint32_t behavior, uint32_t pp, uint32_t *out);
COM_METHOD(d3d9, CreateDevice, 7)
{
    uint32_t out = 0;
    uint32_t hr = d3d_device_create(THIS_, ARG(3), ARG(4), ARG(5), &out);
    MEM32(ARG(6)) = out;
    return hr;
}

static const void_fn d3d9_methods[] = {
    d3d9_QueryInterface, d3d9_AddRef, d3d9_Release, d3d9_RegisterSoftwareDevice, d3d9_GetAdapterCount,
    d3d9_GetAdapterIdentifier, d3d9_GetAdapterModeCount, d3d9_EnumAdapterModes, d3d9_GetAdapterDisplayMode,
    d3d9_CheckDeviceType, d3d9_CheckDeviceFormat, d3d9_CheckDeviceMultiSampleType, d3d9_CheckDepthStencilMatch,
    d3d9_CheckDeviceFormatConversion, d3d9_GetDeviceCaps, d3d9_GetAdapterMonitor, d3d9_CreateDevice,
};
static void_com_class d3d9_class = { "IDirect3D9", 17, d3d9_methods, 0 };

WINAPI_EXPORT("d3d9.dll", Direct3DCreate9, 1)
{
    d3d9_t *d = (d3d9_t *)calloc(1, sizeof *d);
    uint32_t guest = void_com_wrap(&d->com, &d3d9_class, 0);
    void_log("[d3d9] Direct3DCreate9(%u) = %08X", ARG(0), guest);
    return guest;
}

/* ==================================== IDirect3DShaderValidator9 (by name)
 * The validator d3d9.dll exports for D3DX (Wine: d3d9_main.c). The Sims 2
 * validates each shader it assembles with it, and takes a d3d9.dll without
 * the export for one with it: 0xA002696E calls through a NULL validator. As
 * on Wine, every shader validates (the callback the game passes to Begin is
 * never called, so it sees no errors). */
typedef struct { void_com_object com; } shader_validator_t;
COM_METHOD(validator, QueryInterface, 3) { MEM32(ARG(2)) = 0; return E_NOINTERFACE_; }
COM_METHOD(validator, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(validator, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(validator, Begin, 4) { return D3D_OK; }          /* (callback, context, flags) */
COM_METHOD(validator, Instruction, 5) { return D3D_OK; }    /* (file, line, tokens, count) */
COM_METHOD(validator, End, 1) { return D3D_OK; }
static const void_fn validator_methods[] = {
    validator_QueryInterface, validator_AddRef, validator_Release, validator_Begin, validator_Instruction,
    validator_End,
};
static void_com_class validator_class = { "IDirect3DShaderValidator9", 6, validator_methods, 0 };

WINAPI_EXPORT("d3d9.dll", Direct3DShaderValidatorCreate9, 0)
{
    shader_validator_t *v = (shader_validator_t *)calloc(1, sizeof *v);
    return void_com_wrap(&v->com, &validator_class, 0);
}

WINAPI_EXPORT("d3d9.dll", D3DPERF_SetOptions, 1) { return 0; }
WINAPI_EXPORT("d3d9.dll", D3DPERF_BeginEvent, 2) { return 0; }
WINAPI_EXPORT("d3d9.dll", D3DPERF_EndEvent, 0) { return 0; }
WINAPI_EXPORT("d3d9.dll", D3DPERF_GetStatus, 0) { return 0; }
/* The debug runtime's mute switch (cdecl, returns the old state). D3DX looks
 * it up before every operation until it finds it (0xA0029601): without it,
 * each surface conversion paid GetModuleHandle + LoadLibrary + GetProcAddress. */
CDECL_EXPORT("d3d9.dll", DebugSetMute) { return 0; }

/* ===================================================== device: defaults */
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void default_state(d3d_device *dev, d3d_state *s)
{
    memset(s, 0, sizeof *s);
    uint32_t *rs = s->rs;
    rs[7] = dev->pp[9] ? 1 : 0;   /* ZENABLE follows EnableAutoDepthStencil */
    rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[23] = 4;
    rs[25] = 8; rs[37] = f2u(1.0f); rs[38] = f2u(1.0f);
    rs[53] = rs[54] = rs[55] = 1; rs[56] = 8; rs[58] = rs[59] = 0xFFFFFFFFu; rs[60] = 0xFFFFFFFFu;
    rs[136] = 1; rs[137] = 1; rs[141] = 1; rs[142] = 1; rs[145] = 1; rs[146] = 2;
    rs[154] = f2u(1.0f); rs[155] = f2u(1.0f); rs[158] = f2u(1.0f); rs[161] = 1; rs[162] = 0xFFFFFFFFu;
    rs[165] = 0xBAADCAFEu; rs[166] = f2u(64.0f); rs[168] = 0xF; rs[171] = 1; rs[172] = 3; rs[173] = 1;
    rs[178] = f2u(1.0f); rs[179] = f2u(1.0f); rs[182] = f2u(1.0f);
    rs[186] = rs[187] = rs[188] = 1; rs[189] = 8; rs[190] = rs[191] = rs[192] = 0xF; rs[193] = 0xFFFFFFFFu;
    rs[207] = 2; rs[208] = 1; rs[209] = 1;
    for (int i = 0; i < 20; ++i) {
        s->ss[i][1] = s->ss[i][2] = s->ss[i][3] = 1;   /* ADDRESS = WRAP */
        s->ss[i][5] = s->ss[i][6] = 1;                 /* MAG/MIN = POINT */
        s->ss[i][10] = 1;                              /* MAXANISOTROPY */
    }
    for (int i = 0; i < 8; ++i) {
        s->tss[i][1] = i ? 1 : 4;     /* COLOROP: MODULATE / DISABLE */
        s->tss[i][2] = 2; s->tss[i][3] = 1;
        s->tss[i][4] = i ? 1 : 2;     /* ALPHAOP: SELECTARG1 / DISABLE */
        s->tss[i][5] = 2; s->tss[i][6] = 1;
        s->tss[i][11] = (uint32_t)i;  /* TEXCOORDINDEX */
        s->tss[i][26] = s->tss[i][27] = s->tss[i][28] = 1;
    }
    for (int t = 0; t < 512; ++t) for (int k = 0; k < 4; ++k) s->transforms[t][k * 5] = 1.0f;
    for (int i = 0; i < 16; ++i) s->stream_freq[i] = 1;
}

static void set_default_viewport(d3d_device *dev, d3d_surface *rt)
{
    dev->st.viewport[0] = dev->st.viewport[1] = 0;
    dev->st.viewport[2] = rt ? rt->width : 1;
    dev->st.viewport[3] = rt ? rt->height : 1;
    dev->st.viewport_z[0] = 0.0f;
    dev->st.viewport_z[1] = 1.0f;
    dev->st.scissor[0] = dev->st.scissor[1] = 0;
    dev->st.scissor[2] = rt ? rt->width : 1;
    dev->st.scissor[3] = rt ? rt->height : 1;
    d3d_dirty_field(dev, viewport);
    d3d_dirty_field(dev, viewport_z);
    d3d_dirty_field(dev, scissor);
}

/* Reference counts the device holds on bound objects, like D3D. */
void d3d_ref_set(void **slot, void *object)
{
    if (*slot == object) return;
    if (object) void_com_addref((void_com_object *)object);
    if (*slot) void_com_release((void_com_object *)*slot);
    *slot = object;
}
#define bind_ref d3d_ref_set

static void *host_or_null(uint32_t guest) { return guest ? void_com_host(guest) : NULL; }

/* bind_ref by the guest's pointer: the game sets the same texture, stream or
 * shader again many times a frame. The device remembers which guest pointer
 * each slot's object answers to: while the slot still holds that object (its
 * reference keeps it alive, so its guest pointer cannot change), the same
 * pointer needs neither a lookup nor a load from the object itself, which a
 * draw-heavy frame mostly finds cold. Any other path that rebinds the slot
 * changes *slot and so falls through to the full check. Returns whether the
 * slot changed (an unchanged one needs no state sync). */
static int bind_guest(d3d_device *d, int memo, void **slot, uint32_t guest)
{
    void *now = *slot;
    if (now == d->bound[memo].host && guest == d->bound[memo].guest) return 0;
    void_com_object *held = (void_com_object *)now;
    int same = held ? held->guest == guest : !guest;
    if (!same) bind_ref(slot, host_or_null(guest));
    d->bound[memo].host = *slot;
    d->bound[memo].guest = guest;
    return !same;
}
enum { BOUND_TEXTURE = 0, BOUND_STREAM = 20, BOUND_INDICES = 36, BOUND_DECL, BOUND_VS, BOUND_PS };

/* Render target and depth slots hold a reference on the surface's owner: a
 * texture level keeps its texture alive while bound (as in D3D9). */
/* Returns whether the slot changed. */
static int bind_surface(d3d_surface **slot, d3d_surface *s)
{
    if (*slot == s) return 0;
    if (s) void_com_addref(d3d_surface_owner(s));
    if (*slot) void_com_release(d3d_surface_owner(*slot));
    *slot = s;
    return 1;
}

static void make_backbuffer(d3d_device *dev)
{
    uint32_t w = dev->pp[0], h = dev->pp[1];
    if (!w || !h) {
        int ww, hh;
        void_window_client_size(dev->window, &ww, &hh);
        w = ww > 0 ? (uint32_t)ww : 1280;
        h = hh > 0 ? (uint32_t)hh : 720;
        dev->pp[0] = w; dev->pp[1] = h;
    }
    uint32_t format = dev->pp[2] ? dev->pp[2] : 22;
    if (format == 0) format = 22;
    if (dev->backbuffer) void_com_release(&dev->backbuffer->com);
    dev->backbuffer = d3d_surface_new(dev, w, h, format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, 0);
    if (dev->auto_depth) { void_com_release(&dev->auto_depth->com); dev->auto_depth = NULL; }
    if (dev->pp[9]) dev->auto_depth = d3d_surface_new(dev, w, h, dev->pp[10], D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, 0);
    for (int i = 0; i < 4; ++i) bind_surface(&dev->rt[i], NULL);
    bind_surface(&dev->ds, NULL);
    bind_surface(&dev->rt[0], dev->backbuffer);
    if (dev->auto_depth) bind_surface(&dev->ds, dev->auto_depth);
    dev->fbo_dirty = 1;
}

/* ======================================================= device methods */
#define DEV ((d3d_device *)void_com_host(THIS_))

COM_METHOD(dev, QueryInterface, 3)
{
    d3d_device *d = DEV;
    if (void_guid_equal(ARG(1), IID_IDirect3DDevice9) || void_guid_equal(ARG(1), IID_IUnknown)) {
        void_com_addref(&d->com);
        MEM32(ARG(2)) = THIS_;
        return 0;
    }
    MEM32(ARG(2)) = 0;
    return E_NOINTERFACE_;
}
COM_METHOD(dev, AddRef, 1) { return void_com_addref(&DEV->com); }
COM_METHOD(dev, Release, 1) { return void_com_release(&DEV->com); }
COM_METHOD(dev, TestCooperativeLevel, 1) { return D3D_OK; }
COM_METHOD(dev, GetAvailableTextureMem, 1) { return 1024u << 20; }
COM_METHOD(dev, EvictManagedResources, 1) { return D3D_OK; }
COM_METHOD(dev, GetDirect3D, 2)
{
    d3d_device *d = DEV;
    void_com_addref((void_com_object *)void_com_host(d->d3d9));
    MEM32(ARG(1)) = d->d3d9;
    return D3D_OK;
}
COM_METHOD(dev, GetDeviceCaps, 2) { fill_caps(ARG(1)); return D3D_OK; }
COM_METHOD(dev, GetDisplayMode, 3)
{
    int w, h;
    void_input_screen_size(&w, &h);
    uint32_t out = ARG(2);
    MEM32(out) = (uint32_t)w; MEM32(out + 4) = (uint32_t)h; MEM32(out + 8) = 60; MEM32(out + 12) = 22;
    return D3D_OK;
}
COM_METHOD(dev, GetCreationParameters, 2)
{
    d3d_device *d = DEV;
    uint32_t out = ARG(1);
    MEM32(out) = 0; MEM32(out + 4) = 1; MEM32(out + 8) = d->window; MEM32(out + 12) = d->behavior;
    return D3D_OK;
}
COM_METHOD(dev, SetCursorProperties, 4) { return D3D_OK; }
COM_METHOD(dev, SetCursorPosition, 4) { return 0; }
COM_METHOD(dev, ShowCursor, 2) { return 0; }
COM_METHOD(dev, CreateAdditionalSwapChain, 3) { void_log("[d3d9] CreateAdditionalSwapChain unsupported"); return D3DERR_INVALIDCALL; }
COM_METHOD(dev, GetSwapChain, 3)
{
    d3d_device *d = DEV;
    if (ARG(1)) return D3DERR_INVALIDCALL;
    void_com_addref((void_com_object *)void_com_host(d->swapchain));
    MEM32(ARG(2)) = d->swapchain;
    return D3D_OK;
}
COM_METHOD(dev, GetNumberOfSwapChains, 1) { return 1; }

COM_METHOD(dev, Reset, 2)
{
    d3d_device *d = DEV;
    uint32_t pp = ARG(1);
    int dimensions_only = 1;
    for (int i = 2; i < 14; ++i) {
        uint32_t value = MEM32(pp + 4u * (uint32_t)i);
        if (i == 2 && !value) value = 22;
        if (value != d->pp[i]) dimensions_only = 0;
    }
    for (int i = 0; i < 14; ++i) d->pp[i] = MEM32(pp + 4u * (uint32_t)i);
    if (!d->pp[2]) d->pp[2] = 22;
    make_backbuffer(d);
    for (int i = 0; i < 14; ++i) MEM32(pp + 4u * (uint32_t)i) = d->pp[i];
    default_state(d, &d->st);
    memset(d->bound, 0, sizeof d->bound);   /* slots rebound outside bind_guest */
    set_default_viewport(d, d->backbuffer);
    d3d_rt_device_reset(d);
    s2_ui_device_reset(dimensions_only);
    void_log("[d3d9] Reset %ux%u format %u depth %u/%u", d->pp[0], d->pp[1], d->pp[2], d->pp[9], d->pp[10]);
    return D3D_OK;
}

/* ------------------------------------------------- to the render thread
 * What the GL side does for the device's methods runs there, on its copy of
 * the device (d3d_rt_device), after the state that changed (d3d9_thread.c). */
/* The frame statistics report the game's thread: its CPU time and machine
 * waits travel with the present. */
typedef struct { uint64_t game_cpu_ns, game_wait_ns, syncs, rt_wait_ns; } present_args;
static void run_present(void *args)
{
    present_args *a = (present_args *)args;
    d3d_device *m = d3d_rt_device();
    if (s2r_on()) s2r_present(m, a->game_cpu_ns, a->game_wait_ns, a->syncs, a->rt_wait_ns);
    else d3d_gl_present(m, a->game_cpu_ns, a->game_wait_ns, a->syncs, a->rt_wait_ns);
    ++m->frame;
    d3d_rt_frame_done();
}

/* The frame's budget for the performance hints: the frame rate limit, or
 * the display's 120 Hz without one. */
static int64_t frame_budget_ns(void)
{
    int fps = s2_fps_limit();
    return 1000000000ll / (fps > 0 ? fps : 120);
}

void d3d_present(d3d_device *d)
{
    static uint64_t frame_start;
    uint64_t now = void_monotonic_ns();
    if (frame_start) s2_load_present(now - frame_start);
    if (frame_start) {
        if (d->frame == 2) void_perf_hint_start((int32_t)gettid(), (int32_t)d3d_rt_render_tid(), frame_budget_ns());
        void_perf_hint_report((int64_t)(now - frame_start), frame_budget_ns());
    }
    d3d_rt_sync_state(d);
    struct timespec cpu;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    present_args a = { (uint64_t)cpu.tv_sec * 1000000000ull + (uint64_t)cpu.tv_nsec, void_lock_wait_ns(), 0, 0 };
    d3d_rt_wait_stats(&a.syncs, &a.rt_wait_ns);
    {
        /* Buffer upload volume, every 300 frames. */
        extern uint64_t d3d_buffer_bytes, d3d_buffer_spare_bytes, d3d_buffer_uploads;
        static uint64_t bytes0, spare0, uploads0;
        static uint32_t frame0;
        if (d->frame - frame0 >= 300) {
            uint32_t n = d->frame - frame0;
            d3d_query_log(n);
            if (frame0) void_log("[d3d9] buffers: %.0f uploads, %.1f KB per frame (%.1f KB outside the written ranges in full uploads)",
                (double)(d3d_buffer_uploads - uploads0) / n, (double)(d3d_buffer_bytes - bytes0) / 1024.0 / n,
                (double)(d3d_buffer_spare_bytes - spare0) / 1024.0 / n);
            bytes0 = d3d_buffer_bytes; spare0 = d3d_buffer_spare_bytes; uploads0 = d3d_buffer_uploads; frame0 = d->frame;
        }
    }
    d3d_rt_frame_queued();
    d3d_rt_call(run_present, &a, sizeof a);
    d3d_rt_flush();
    ++d->frame;
    d3d_rt_frame_wait(1);
    frame_start = void_monotonic_ns();   /* the next frame's work starts here */
}

COM_METHOD(dev, Present, 5) { d3d_present(DEV); return D3D_OK; }

COM_METHOD(dev, GetBackBuffer, 5)
{
    d3d_device *d = DEV;
    if (ARG(1) || ARG(2)) return D3DERR_INVALIDCALL;
    void_com_addref(&d->backbuffer->com);
    MEM32(ARG(4)) = d->backbuffer->com.guest;
    return D3D_OK;
}

COM_METHOD(dev, GetRasterStatus, 3) { MEM32(ARG(2)) = 0; MEM32(ARG(2) + 4) = 0; return D3D_OK; }
COM_METHOD(dev, SetDialogBoxMode, 2) { return D3D_OK; }
COM_METHOD(dev, SetGammaRamp, 4) { return 0; }
COM_METHOD(dev, GetGammaRamp, 3)
{
    uint32_t ramp = ARG(2);
    for (uint32_t i = 0; i < 256; ++i) MEM16(ramp + 2 * i) = MEM16(ramp + 512 + 2 * i) = MEM16(ramp + 1024 + 2 * i) = (uint16_t)(i * 257);
    return 0;
}

COM_METHOD(dev, CreateTexture, 9)
{
    d3d_device *d = DEV;
    uint32_t w = ARG(1), h = ARG(2), levels = ARG(3), usage = ARG(4), format = ARG(5), pool = ARG(6);
    if (!d3d_format_find(format)) { void_log("[d3d9] CreateTexture: unsupported format %u", format); return D3DERR_INVALIDCALL; }
    d3d_texture *t = d3d_texture_new(d, D3DRTYPE_TEXTURE, w, h, 1, levels, usage, format, pool);
    if (!t) return D3DERR_INVALIDCALL;
    MEM32(ARG(7)) = d3d_wrap_texture(t);
    if (d3d_trace_active()) void_log("[trace] CreateTexture %ux%u levels %u usage %X format %u pool %u = %08X", w, h, t->levels,
        usage, format, pool, MEM32(ARG(7)));
    VLOG("[d3d9] CreateTexture %ux%u levels %u usage %X format %u pool %u = %08X", w, h, t->levels, usage, format, pool, MEM32(ARG(7)));
    return D3D_OK;
}

COM_METHOD(dev, CreateVolumeTexture, 10)
{
    d3d_device *d = DEV;
    uint32_t w = ARG(1), h = ARG(2), depth = ARG(3), levels = ARG(4), usage = ARG(5), format = ARG(6), pool = ARG(7);
    if (!d3d_format_find(format)) return D3DERR_INVALIDCALL;
    d3d_texture *t = d3d_texture_new(d, D3DRTYPE_VOLUMETEXTURE, w, h, depth, levels, usage, format, pool);
    if (!t) return D3DERR_INVALIDCALL;
    MEM32(ARG(8)) = d3d_wrap_texture(t);
    return D3D_OK;
}

COM_METHOD(dev, CreateCubeTexture, 8)
{
    d3d_device *d = DEV;
    uint32_t edge = ARG(1), levels = ARG(2), usage = ARG(3), format = ARG(4), pool = ARG(5);
    if (!d3d_format_find(format)) { void_log("[d3d9] CreateCubeTexture: unsupported format %u", format); return D3DERR_INVALIDCALL; }
    d3d_texture *t = d3d_texture_new(d, D3DRTYPE_CUBETEXTURE, edge, edge, 1, levels, usage, format, pool);
    if (!t) return D3DERR_INVALIDCALL;
    MEM32(ARG(6)) = d3d_wrap_texture(t);
    return D3D_OK;
}

uint32_t d3d_buffer_create(d3d_device *dev, int index, uint32_t length, uint32_t usage, uint32_t format, uint32_t pool);

COM_METHOD(dev, CreateVertexBuffer, 7)
{
    uint32_t b = d3d_buffer_create(DEV, 0, ARG(1), ARG(2), ARG(3), ARG(4));
    MEM32(ARG(5)) = b;
    return b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

COM_METHOD(dev, CreateIndexBuffer, 7)
{
    uint32_t b = d3d_buffer_create(DEV, 1, ARG(1), ARG(2), ARG(3), ARG(4));
    MEM32(ARG(5)) = b;
    return b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

COM_METHOD(dev, CreateRenderTarget, 9)
{
    d3d_device *d = DEV;
    if (!d3d_format_find(ARG(3))) return D3DERR_INVALIDCALL;
    d3d_surface *s = d3d_surface_new(d, ARG(1), ARG(2), ARG(3), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ARG(4));
    MEM32(ARG(7)) = s->com.guest;
    VLOG("[d3d9] CreateRenderTarget %ux%u format %u = %08X", ARG(1), ARG(2), ARG(3), s->com.guest);
    return D3D_OK;
}

COM_METHOD(dev, CreateDepthStencilSurface, 9)
{
    d3d_device *d = DEV;
    if (!d3d_format_find(ARG(3))) return D3DERR_INVALIDCALL;
    d3d_surface *s = d3d_surface_new(d, ARG(1), ARG(2), ARG(3), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ARG(4));
    MEM32(ARG(7)) = s->com.guest;
    return D3D_OK;
}

uint32_t d3d_update_surface(d3d_surface *src, uint32_t src_rect, d3d_surface *dst, uint32_t dst_point);
COM_METHOD(dev, UpdateSurface, 5)
{
    return d3d_update_surface((d3d_surface *)void_com_host(ARG(1)), ARG(2), (d3d_surface *)void_com_host(ARG(3)), ARG(4));
}

void d3d_update_texture(d3d_texture *src, d3d_texture *dst);
COM_METHOD(dev, UpdateTexture, 3)
{
    d3d_update_texture((d3d_texture *)void_com_host(ARG(1)), (d3d_texture *)void_com_host(ARG(2)));
    return D3D_OK;
}

typedef struct { d3d_surface *src; uint32_t mem, pitch; } read_args;
static void run_read(void *args)
{
    read_args *a = (read_args *)args;
    if (s2r_on()) s2r_read_surface(a->src, a->mem, a->pitch);
    else d3d_gl_read_surface(a->src, a->mem, a->pitch);
}

/* A GPU surface into guest memory: the game waits for the render thread. */
void d3d_read_surface(d3d_device *d, d3d_surface *src, uint32_t mem, uint32_t pitch)
{
    if (d) d3d_rt_sync_state(d);
    read_args a = { src, mem, pitch };
    d3d_rt_sync(run_read, &a);
}

typedef struct { d3d_surface *s; const uint32_t *rect; float *out; int ok; } rgba_args;
static void run_read_rgba(void *args)
{
    rgba_args *a = (rgba_args *)args;
    a->ok = s2r_on() ? s2r_read_rgba(a->s, a->rect, a->out) : d3d_gl_read_rgba(a->s, a->rect, a->out);
}

int d3d_read_rgba(d3d_surface *s, const uint32_t *rect, float *out)
{
    rgba_args a = { s, rect, out, 0 };
    d3d_rt_sync(run_read_rgba, &a);
    return a.ok;
}

COM_METHOD(dev, GetRenderTargetData, 3)
{
    d3d_surface *src = (d3d_surface *)void_com_host(ARG(1)), *dst = (d3d_surface *)void_com_host(ARG(2));
    if (!dst->sysmem || src->width != dst->width || src->height != dst->height) return D3DERR_INVALIDCALL;
    d3d_read_surface(DEV, src, dst->sysmem, dst->pitch);
    return D3D_OK;
}

COM_METHOD(dev, GetFrontBufferData, 3)
{
    d3d_device *d = DEV;
    d3d_surface *dst = (d3d_surface *)void_com_host(ARG(2));
    if (!dst->sysmem) return D3DERR_INVALIDCALL;
    d3d_read_surface(d, d->backbuffer, dst->sysmem, dst->pitch);
    return D3D_OK;
}

static int read_rect(uint32_t va, d3d_surface *s, uint32_t r[4])
{
    if (!va) { r[0] = r[1] = 0; r[2] = s->width; r[3] = s->height; return 1; }
    for (int i = 0; i < 4; ++i) r[i] = MEM32(va + 4u * (uint32_t)i);
    return 1;
}

typedef struct { d3d_surface *src, *dst; uint32_t sr[4], dr[4], filter; } blit_args;
static void run_blit(void *args)
{
    blit_args *a = (blit_args *)args;
    if (s2r_on()) s2r_blit(a->src, a->sr, a->dst, a->dr, a->filter);
    else d3d_gl_blit(a->src, a->sr, a->dst, a->dr, a->filter);
}

/* StretchRect between GPU surfaces (d3dx9.c too). */
void d3d_blit(d3d_device *d, d3d_surface *src, const uint32_t *sr, d3d_surface *dst, const uint32_t *dr, uint32_t filter)
{
    if (d) d3d_rt_sync_state(d);
    blit_args a = { src, dst, { sr[0], sr[1], sr[2], sr[3] }, { dr[0], dr[1], dr[2], dr[3] }, filter };
    d3d_rt_call(run_blit, &a, sizeof a);
}

COM_METHOD(dev, StretchRect, 6)
{
    d3d_surface *src = (d3d_surface *)void_com_host(ARG(1)), *dst = (d3d_surface *)void_com_host(ARG(3));
    uint32_t sr[4], dr[4];
    read_rect(ARG(2), src, sr);
    read_rect(ARG(4), dst, dr);
    d3d_blit(DEV, src, sr, dst, dr, ARG(5));
    return D3D_OK;
}

typedef struct { d3d_surface *s; uint32_t r[4], color; } fill_args;
static void run_fill(void *args)
{
    fill_args *a = (fill_args *)args;
    if (s2r_on()) s2r_fill(a->s, a->r, a->color);
    else d3d_gl_fill(a->s, a->r, a->color);
}

COM_METHOD(dev, ColorFill, 4)
{
    d3d_surface *s = (d3d_surface *)void_com_host(ARG(1));
    fill_args a;
    a.s = s;
    read_rect(ARG(2), s, a.r);
    a.color = ARG(3);
    d3d_rt_sync_state(DEV);
    d3d_rt_call(run_fill, &a, sizeof a);
    return D3D_OK;
}

COM_METHOD(dev, CreateOffscreenPlainSurface, 7)
{
    d3d_device *d = DEV;
    if (!d3d_format_find(ARG(3))) return D3DERR_INVALIDCALL;
    d3d_surface *s = d3d_surface_new(d, ARG(1), ARG(2), ARG(3), 0, ARG(4), 0);
    MEM32(ARG(5)) = s->com.guest;
    return D3D_OK;
}

COM_METHOD(dev, SetRenderTarget, 3)
{
    d3d_device *d = DEV;
    uint32_t index = ARG(1);
    if (index > 3 || (index == 0 && !ARG(2))) return D3DERR_INVALIDCALL;
    d3d_surface *s = (d3d_surface *)host_or_null(ARG(2));
    if (bind_surface(&d->rt[index], s)) d->fbo_dirty = 1;
    if (index == 0) set_default_viewport(d, s);
    return D3D_OK;
}

COM_METHOD(dev, GetRenderTarget, 3)
{
    d3d_device *d = DEV;
    uint32_t index = ARG(1);
    if (index > 3) return D3DERR_INVALIDCALL;
    if (!d->rt[index]) { MEM32(ARG(2)) = 0; return D3DERR_NOTFOUND; }
    void_com_addref(d3d_surface_owner(d->rt[index]));
    MEM32(ARG(2)) = d->rt[index]->com.guest;
    return D3D_OK;
}

COM_METHOD(dev, SetDepthStencilSurface, 2)
{
    d3d_device *d = DEV;
    if (bind_surface(&d->ds, (d3d_surface *)host_or_null(ARG(1)))) d->fbo_dirty = 1;
    return D3D_OK;
}

COM_METHOD(dev, GetDepthStencilSurface, 2)
{
    d3d_device *d = DEV;
    if (!d->ds) { MEM32(ARG(1)) = 0; return D3DERR_NOTFOUND; }
    void_com_addref(d3d_surface_owner(d->ds));
    MEM32(ARG(1)) = d->ds->com.guest;
    return D3D_OK;
}

COM_METHOD(dev, BeginScene, 1) { DEV->in_scene = 1; return D3D_OK; }
COM_METHOD(dev, EndScene, 1) { DEV->in_scene = 0; return D3D_OK; }

typedef struct { uint32_t count, flags, color, stencil; float z; } clear_args;   /* then count rects */
static void run_clear(void *args)
{
    clear_args *a = (clear_args *)args;
    const uint32_t *rects = a->count ? (const uint32_t *)(a + 1) : NULL;
    if (s2r_on()) s2r_clear(d3d_rt_device(), a->count, rects, a->flags, a->color, a->z, a->stencil);
    else d3d_gl_clear(d3d_rt_device(), a->count, rects, a->flags, a->color, a->z, a->stencil);
}

COM_METHOD(dev, Clear, 7)
{
    d3d_device *d = DEV;
    float z;
    uint32_t zbits = ARG(5), count = ARG(2) ? ARG(1) : 0;
    memcpy(&z, &zbits, 4);
    d3d_rt_sync_state(d);
    clear_args *a = (clear_args *)d3d_rt_begin(run_clear, (uint32_t)sizeof *a + 16u * count);
    a->count = count; a->flags = ARG(3); a->color = ARG(4); a->z = z; a->stencil = ARG(6);
    if (count) memcpy(a + 1, GPTR(ARG(2)), 16u * count);
    d3d_rt_commit();
    return D3D_OK;
}

/* ---------------------------------------------------------- recording */
#define REC(field) (d->recording ? (d->recording->field = 1) : 0)
#define DIRTY(field) do { if (!d->recording) d3d_dirty_field(d, field); } while (0)

static d3d_state *target_state(d3d_device *d) { return d->recording ? &d->recording->state : &d->st; }

static uint32_t transform_slot(uint32_t state)
{
    if (state < 256) return state & 511;
    return 256u + ((state - 256u) & 255u);
}

COM_METHOD(dev, SetTransform, 3)
{
    d3d_device *d = DEV;
    uint32_t slot = transform_slot(ARG(1));
    memcpy(target_state(d)->transforms[slot], GPTR(ARG(2)), 64);
    if (d->recording) d->recording->rec_transform[slot] = 1;
    else d3d_dirty_transform(d, slot);
    return D3D_OK;
}

COM_METHOD(dev, GetTransform, 3)
{
    memcpy(GPTR(ARG(2)), DEV->st.transforms[transform_slot(ARG(1))], 64);
    return D3D_OK;
}

COM_METHOD(dev, MultiplyTransform, 3)
{
    d3d_device *d = DEV;
    uint32_t slot = transform_slot(ARG(1));
    float a[16], b[16], r[16];
    memcpy(a, d->st.transforms[slot], 64);
    memcpy(b, GPTR(ARG(2)), 64);
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        r[i * 4 + j] = s;
    }
    memcpy(target_state(d)->transforms[slot], r, 64);
    if (!d->recording) d3d_dirty_transform(d, slot);
    return D3D_OK;
}

COM_METHOD(dev, SetViewport, 2)
{
    d3d_device *d = DEV;
    d3d_state *s = target_state(d);
    uint32_t vp = ARG(1);
    for (int i = 0; i < 4; ++i) s->viewport[i] = MEM32(vp + 4u * (uint32_t)i);
    memcpy(s->viewport_z, GPTR(vp + 16), 8);
    if (d->recording) d->recording->rec_viewport = 1;
    DIRTY(viewport);
    DIRTY(viewport_z);
    return D3D_OK;
}

COM_METHOD(dev, GetViewport, 2)
{
    d3d_device *d = DEV;
    uint32_t vp = ARG(1);
    for (int i = 0; i < 4; ++i) MEM32(vp + 4u * (uint32_t)i) = d->st.viewport[i];
    memcpy(GPTR(vp + 16), d->st.viewport_z, 8);
    return D3D_OK;
}

COM_METHOD(dev, SetMaterial, 2)
{
    d3d_device *d = DEV;
    memcpy(target_state(d)->material, GPTR(ARG(1)), 68);
    if (d->recording) d->recording->rec_material = 1;
    DIRTY(material);
    return D3D_OK;
}
COM_METHOD(dev, GetMaterial, 2) { memcpy(GPTR(ARG(1)), DEV->st.material, 68); return D3D_OK; }

COM_METHOD(dev, SetLight, 3)
{
    d3d_device *d = DEV;
    if (ARG(1) >= 8) return D3D_OK;
    memcpy(target_state(d)->lights[ARG(1)], GPTR(ARG(2)), 104);
    if (d->recording) d->recording->rec_light[ARG(1)] = 1;
    DIRTY(lights[ARG(1)]);
    return D3D_OK;
}
COM_METHOD(dev, GetLight, 3)
{
    if (ARG(1) >= 8) return D3DERR_INVALIDCALL;
    memcpy(GPTR(ARG(2)), DEV->st.lights[ARG(1)], 104);
    return D3D_OK;
}
COM_METHOD(dev, LightEnable, 3)
{
    d3d_device *d = DEV;
    if (ARG(1) >= 8) return D3D_OK;
    if (!d->recording && d->st.light_enable[ARG(1)] == (ARG(2) != 0)) return D3D_OK;
    target_state(d)->light_enable[ARG(1)] = ARG(2) != 0;
    if (d->recording) d->recording->rec_light[ARG(1)] = 1;
    DIRTY(light_enable[ARG(1)]);
    return D3D_OK;
}
COM_METHOD(dev, GetLightEnable, 3)
{
    if (ARG(1) >= 8) return D3DERR_INVALIDCALL;
    MEM32(ARG(2)) = DEV->st.light_enable[ARG(1)] ? 128u : 0u;
    return D3D_OK;
}
COM_METHOD(dev, SetClipPlane, 3)
{
    d3d_device *d = DEV;
    if (ARG(1) >= 6) return D3DERR_INVALIDCALL;
    memcpy(target_state(d)->clip_planes[ARG(1)], GPTR(ARG(2)), 16);
    if (d->recording) d->recording->rec_clip[ARG(1)] = 1;
    DIRTY(clip_planes[ARG(1)]);
    return D3D_OK;
}
COM_METHOD(dev, GetClipPlane, 3)
{
    if (ARG(1) >= 6) return D3DERR_INVALIDCALL;
    memcpy(GPTR(ARG(2)), DEV->st.clip_planes[ARG(1)], 16);
    return D3D_OK;
}

COM_METHOD(dev, SetRenderState, 3)
{
    d3d_device *d = DEV;
    uint32_t state = ARG(1);
    if (state >= 256) return D3D_OK;
    if (!d->recording && d->st.rs[state] == ARG(2)) return D3D_OK;
    target_state(d)->rs[state] = ARG(2);
    if (d->recording) d->recording->rec_rs[state] = 1;
    DIRTY(rs[state]);
    return D3D_OK;
}

COM_METHOD(dev, GetRenderState, 3)
{
    uint32_t state = ARG(1);
    MEM32(ARG(2)) = state < 256 ? DEV->st.rs[state] : 0;
    return D3D_OK;
}

/* ---------------------------------------------------------- state blocks */
uint32_t d3d_stateblock_create(d3d_device *dev, uint32_t type);
void d3d_stateblock_capture(d3d_stateblock *sb);

COM_METHOD(dev, CreateStateBlock, 3)
{
    uint32_t sb = d3d_stateblock_create(DEV, ARG(1));
    MEM32(ARG(2)) = sb;
    return sb ? D3D_OK : D3DERR_INVALIDCALL;
}

COM_METHOD(dev, BeginStateBlock, 1)
{
    d3d_device *d = DEV;
    if (d->recording) return D3DERR_INVALIDCALL;
    uint32_t guest = d3d_stateblock_create(d, 0);   /* empty: records nothing yet */
    d->recording = (d3d_stateblock *)void_com_host(guest);
    /* Values start from the device's (only recorded items are applied);
     * object slots start empty: the block holds references only on the
     * objects recorded into it. */
    d->recording->state = d->st;
    d3d_state *s = &d->recording->state;
    memset(s->textures, 0, sizeof s->textures);
    memset(s->streams, 0, sizeof s->streams);
    s->indices = s->decl = s->vs = s->ps = NULL;
    return D3D_OK;
}

COM_METHOD(dev, EndStateBlock, 2)
{
    d3d_device *d = DEV;
    if (!d->recording) return D3DERR_INVALIDCALL;
    MEM32(ARG(1)) = d->recording->com.guest;
    d->recording = NULL;
    return D3D_OK;
}

COM_METHOD(dev, SetClipStatus, 2) { return D3D_OK; }
COM_METHOD(dev, GetClipStatus, 2) { MEM32(ARG(1)) = 0; MEM32(ARG(1) + 4) = 0; return D3D_OK; }

static int sampler_slot(uint32_t sampler)
{
    if (sampler < 16) return (int)sampler;
    if (sampler >= 256 && sampler <= 260) return 16 + (int)(sampler - 257 < 4 ? sampler - 257 : 0);
    return -1;
}

COM_METHOD(dev, GetTexture, 3)
{
    d3d_device *d = DEV;
    int slot = sampler_slot(ARG(1));
    void_com_object *t = slot >= 0 ? (void_com_object *)d->st.textures[slot] : NULL;
    if (t) void_com_addref(t);
    MEM32(ARG(2)) = t ? t->guest : 0;
    return D3D_OK;
}

COM_METHOD(dev, SetTexture, 3)
{
    d3d_device *d = DEV;
    int slot = sampler_slot(ARG(1));
    if (d3d_trace_active()) {
        d3d_texture *t = (d3d_texture *)host_or_null(ARG(2));
        void_log("[trace] SetTexture(%u -> slot %d, %08X: %ux%u fmt %u)%s", ARG(1), slot, ARG(2), t ? t->width : 0,
            t ? t->height : 0, t ? t->format : 0, d->recording ? " recording" : "");
    }
    if (slot < 0) return D3DERR_INVALIDCALL;
    if (d->recording) {
        d3d_ref_set(&d->recording->state.textures[slot], host_or_null(ARG(2)));
        d->recording->rec_tex[slot] = 1;
        return D3D_OK;
    }
    if (bind_guest(d, BOUND_TEXTURE + slot, &d->st.textures[slot], ARG(2))) d3d_dirty_field(d, textures[slot]);
    return D3D_OK;
}

COM_METHOD(dev, GetTextureStageState, 4)
{
    MEM32(ARG(3)) = (ARG(1) < 8 && ARG(2) < 33) ? DEV->st.tss[ARG(1)][ARG(2)] : 0;
    return D3D_OK;
}

COM_METHOD(dev, SetTextureStageState, 4)
{
    d3d_device *d = DEV;
    if (ARG(1) >= 8 || ARG(2) >= 33) return D3D_OK;
    if (!d->recording && d->st.tss[ARG(1)][ARG(2)] == ARG(3)) return D3D_OK;
    target_state(d)->tss[ARG(1)][ARG(2)] = ARG(3);
    if (d->recording) d->recording->rec_tss[ARG(1)][ARG(2)] = 1;
    DIRTY(tss[ARG(1)][ARG(2)]);
    return D3D_OK;
}

COM_METHOD(dev, GetSamplerState, 4)
{
    int slot = sampler_slot(ARG(1));
    MEM32(ARG(3)) = (slot >= 0 && ARG(2) < 14) ? DEV->st.ss[slot][ARG(2)] : 0;
    return D3D_OK;
}

COM_METHOD(dev, SetSamplerState, 4)
{
    d3d_device *d = DEV;
    int slot = sampler_slot(ARG(1));
    if (slot < 0 || ARG(2) >= 14) return D3D_OK;
    if (!d->recording && d->st.ss[slot][ARG(2)] == ARG(3)) return D3D_OK;
    target_state(d)->ss[slot][ARG(2)] = ARG(3);
    if (d->recording) d->recording->rec_ss[slot][ARG(2)] = 1;
    DIRTY(ss[slot][ARG(2)]);
    return D3D_OK;
}

COM_METHOD(dev, ValidateDevice, 2) { if (ARG(1)) MEM32(ARG(1)) = 1; return D3D_OK; }
COM_METHOD(dev, SetPaletteEntries, 3) { return D3D_OK; }
COM_METHOD(dev, GetPaletteEntries, 3) { return D3DERR_INVALIDCALL; }
COM_METHOD(dev, SetCurrentTexturePalette, 2) { return D3D_OK; }
COM_METHOD(dev, GetCurrentTexturePalette, 2) { MEM32(ARG(1)) = 0; return D3D_OK; }

COM_METHOD(dev, SetScissorRect, 2)
{
    d3d_device *d = DEV;
    d3d_state *s = target_state(d);
    uint32_t r = ARG(1);
    s->scissor[0] = MEM32(r); s->scissor[1] = MEM32(r + 4);
    s->scissor[2] = MEM32(r + 8) - MEM32(r); s->scissor[3] = MEM32(r + 12) - MEM32(r + 4);
    if (d->recording) d->recording->rec_scissor = 1;
    DIRTY(scissor);
    return D3D_OK;
}

COM_METHOD(dev, GetScissorRect, 2)
{
    d3d_device *d = DEV;
    uint32_t r = ARG(1);
    MEM32(r) = d->st.scissor[0]; MEM32(r + 4) = d->st.scissor[1];
    MEM32(r + 8) = d->st.scissor[0] + d->st.scissor[2]; MEM32(r + 12) = d->st.scissor[1] + d->st.scissor[3];
    return D3D_OK;
}

COM_METHOD(dev, SetSoftwareVertexProcessing, 2) { DEV->software_vp = ARG(1) != 0; return D3D_OK; }
COM_METHOD(dev, GetSoftwareVertexProcessing, 1) { return DEV->software_vp; }
COM_METHOD(dev, SetNPatchMode, 2) { return D3D_OK; }
/* float return value: ST0. */
static void dev_GetNPatchMode(void) { void_x87_push(0.0); g_esp += 8; }

/* ----------------------------------------------------------------- draws */
static uint32_t vertex_count_for(uint32_t primitive, uint32_t count)
{
    switch (primitive) {
    case 1: return count;              /* POINTLIST */
    case 2: return count * 2;          /* LINELIST */
    case 3: return count + 1;          /* LINESTRIP */
    case 4: return count * 3;          /* TRIANGLELIST */
    case 5: return count + 2;          /* TRIANGLESTRIP */
    case 6: return count + 2;          /* TRIANGLEFAN */
    default: return 0;
    }
}

/* A draw for the render thread: what the bound buffers got since their last
 * upload goes first, then the state, then the call with a copy of the
 * vertices and indices a UP draw passes. */
typedef struct {
    uint32_t primitive, start_vertex, vertex_count, prim_count, indexed, base_vertex, start_index, min_index;
    uint32_t up_stride, up_index32, up_vertex_bytes, up_index_bytes;
    uint32_t ui;
    float projection[3];
} draw_args;
#define ALIGN16(n) (((n) + 15u) & ~15u)

static void run_draw(void *args)
{
    draw_args *a = (draw_args *)args;
    d3d_rt_device()->ui_draw=a->ui;
    memcpy(d3d_rt_device()->projection,a->projection,sizeof a->projection);
    const uint8_t *data = (const uint8_t *)args + ALIGN16((uint32_t)sizeof *a);
    if (s2r_on())
        s2r_draw_call(d3d_rt_device(), a->primitive, a->start_vertex, a->vertex_count, a->prim_count, (int)a->indexed, a->base_vertex,
            a->start_index, a->min_index, a->up_vertex_bytes ? data : NULL, a->up_stride,
            a->up_index_bytes ? data + ALIGN16(a->up_vertex_bytes) : NULL, (int)a->up_index32);
    else
        d3d_gl_draw(d3d_rt_device(), a->primitive, a->start_vertex, a->vertex_count, a->prim_count, a->indexed, a->base_vertex,
            a->start_index, a->min_index, a->up_vertex_bytes ? data : NULL, a->up_stride,
            a->up_index_bytes ? data + ALIGN16(a->up_vertex_bytes) : NULL, (int)a->up_index32);
}

void d3d_buffer_flush(d3d_buffer *b);

static void record_draw(d3d_device *d, uint32_t primitive, uint32_t start_vertex, uint32_t vertex_count, uint32_t prim_count,
    int indexed, uint32_t base_vertex, uint32_t start_index, uint32_t min_index, const void *up_vertices, uint32_t up_stride,
    const void *up_indices, int up_index32)
{
    for (int i = 0; i < 16; ++i) if (d->st.streams[i]) d3d_buffer_flush((d3d_buffer *)d->st.streams[i]);
    if (d->st.indices) d3d_buffer_flush((d3d_buffer *)d->st.indices);
    d3d_rt_sync_state(d);
    uint32_t vertex_bytes = 0, index_bytes = 0;
    if (up_vertices) {
        uint32_t vertices = vertex_count;
        if (indexed && up_indices) {
            uint32_t max = 0;
            for (uint32_t i = 0; i < vertex_count; ++i) {
                uint32_t v = up_index32 ? ((const uint32_t *)up_indices)[i] : ((const uint16_t *)up_indices)[i];
                if (v > max) max = v;
            }
            vertices = max + 1;
        }
        vertex_bytes = vertices * up_stride;
        if (indexed && up_indices) index_bytes = vertex_count * (up_index32 ? 4u : 2u);
    }
    uint32_t head = ALIGN16((uint32_t)sizeof(draw_args));
    uint8_t *p = (uint8_t *)d3d_rt_begin(run_draw, head + ALIGN16(vertex_bytes) + index_bytes);
    draw_args a = { primitive, start_vertex, vertex_count, prim_count, (uint32_t)indexed, base_vertex, start_index, min_index,
                    up_stride, (uint32_t)up_index32, vertex_bytes, index_bytes, (uint32_t)s2_ui_drawing(), {0,0,0} };
    s2_touch_camera_projection(a.projection);
    memcpy(p, &a, sizeof a);
    if (vertex_bytes) memcpy(p + head, up_vertices, vertex_bytes);
    if (index_bytes) memcpy(p + head + ALIGN16(vertex_bytes), up_indices, index_bytes);
    d3d_rt_commit();
}

COM_METHOD(dev, DrawPrimitive, 4)
{
    d3d_device *d = DEV;
    uint32_t primitive = ARG(1), start = ARG(2), count = ARG(3);
    if (!count) return D3D_OK;
    record_draw(d, primitive, start, vertex_count_for(primitive, count), count, 0, 0, 0, 0, NULL, 0, NULL, 0);
    return D3D_OK;
}

COM_METHOD(dev, DrawIndexedPrimitive, 7)
{
    d3d_device *d = DEV;
    uint32_t primitive = ARG(1), base = ARG(2), min_index = ARG(3), vertices = ARG(4), start_index = ARG(5), count = ARG(6);
    if (!count) return D3D_OK;
    (void)vertices;
    record_draw(d, primitive, 0, vertex_count_for(primitive, count), count, 1, base, start_index, min_index, NULL, 0, NULL, 0);
    return D3D_OK;
}

COM_METHOD(dev, DrawPrimitiveUP, 5)
{
    d3d_device *d = DEV;
    uint32_t primitive = ARG(1), count = ARG(2), data = ARG(3), stride = ARG(4);
    if (!count) return D3D_OK;
    record_draw(d, primitive, 0, vertex_count_for(primitive, count), count, 0, 0, 0, 0, GPTR(data), stride, NULL, 0);
    /* D3D unbinds stream 0 and the index buffer after a UP draw. */
    bind_ref(&d->st.streams[0], NULL);
    d->st.stream_offset[0] = d->st.stream_stride[0] = 0;
    d3d_dirty_field(d, streams[0]);
    d3d_dirty_field(d, stream_offset[0]);
    d3d_dirty_field(d, stream_stride[0]);
    return D3D_OK;
}

COM_METHOD(dev, DrawIndexedPrimitiveUP, 9)
{
    d3d_device *d = DEV;
    uint32_t primitive = ARG(1), min_index = ARG(2), vertices = ARG(3), count = ARG(4), indices = ARG(5);
    uint32_t index_format = ARG(6), data = ARG(7), stride = ARG(8);
    if (!count) return D3D_OK;
    (void)vertices;
    record_draw(d, primitive, 0, vertex_count_for(primitive, count), count, 1, 0, 0, min_index, GPTR(data), stride,
        GPTR(indices), index_format == 102);
    bind_ref(&d->st.streams[0], NULL);
    bind_ref(&d->st.indices, NULL);
    d->st.stream_offset[0] = d->st.stream_stride[0] = 0;
    d3d_dirty_field(d, streams[0]);
    d3d_dirty_field(d, indices);
    d3d_dirty_field(d, stream_offset[0]);
    d3d_dirty_field(d, stream_stride[0]);
    return D3D_OK;
}

COM_METHOD(dev, ProcessVertices, 7)
{
    /* the native renderers run it too: an OpenGL ES context for the transform feedback (s2r_process.c) */
    return d3d_process_vertices(DEV, ARG(1), ARG(2), ARG(3),
        (d3d_buffer *)host_or_null(ARG(4)), (d3d_decl *)host_or_null(ARG(5)), ARG(6));
}

uint32_t d3d_decl_create(d3d_device *dev, uint32_t elements, uint32_t fvf);
COM_METHOD(dev, CreateVertexDeclaration, 3)
{
    uint32_t decl = d3d_decl_create(DEV, ARG(1), 0);
    MEM32(ARG(2)) = decl;
    return decl ? D3D_OK : D3DERR_INVALIDCALL;
}

COM_METHOD(dev, SetVertexDeclaration, 2)
{
    d3d_device *d = DEV;
    if (d->recording) { d3d_ref_set(&d->recording->state.decl, host_or_null(ARG(1))); d->recording->rec_decl = 1; return D3D_OK; }
    if (bind_guest(d, BOUND_DECL, &d->st.decl, ARG(1))) d3d_dirty_field(d, decl);
    if (d->st.fvf) { d->st.fvf = 0; d3d_dirty_field(d, fvf); }
    return D3D_OK;
}

COM_METHOD(dev, GetVertexDeclaration, 2)
{
    d3d_device *d = DEV;
    void_com_object *o = (void_com_object *)d->st.decl;
    if (o) void_com_addref(o);
    MEM32(ARG(1)) = o ? o->guest : 0;
    return D3D_OK;
}

/* SetFVF selects an implicit declaration built from the FVF code. */
static struct { uint32_t fvf; uint32_t guest; } fvf_decls[64];
static int fvf_decl_count;

/* The implicit declaration of an FVF code (cached; the cache owns the
 * creation reference). */
static void *fvf_declaration(d3d_device *d, uint32_t fvf)
{
    uint32_t guest = 0;
    for (int i = 0; i < fvf_decl_count; ++i) if (fvf_decls[i].fvf == fvf) guest = fvf_decls[i].guest;
    if (!guest) {
        guest = d3d_decl_create(d, 0, fvf);
        if (fvf_decl_count < 64) { fvf_decls[fvf_decl_count].fvf = fvf; fvf_decls[fvf_decl_count].guest = guest; ++fvf_decl_count; }
    }
    return void_com_host(guest);
}

COM_METHOD(dev, SetFVF, 2)
{
    d3d_device *d = DEV;
    uint32_t fvf = ARG(1);
    if (d->recording) {
        /* SetFVF replaces the vertex declaration: a block records both. */
        d->recording->state.fvf = fvf;
        d->recording->rec_fvf = 1;
        if (fvf) {
            d3d_ref_set(&d->recording->state.decl, fvf_declaration(d, fvf));
            d->recording->rec_decl = 1;
        }
        return D3D_OK;
    }
    d->st.fvf = fvf;
    d3d_dirty_field(d, fvf);
    if (!fvf) return D3D_OK;
    bind_ref(&d->st.decl, fvf_declaration(d, fvf));
    d->bound[BOUND_DECL].host = NULL; d->bound[BOUND_DECL].guest = 0;   /* rebound outside bind_guest */
    d3d_dirty_field(d, decl);
    return D3D_OK;
}

COM_METHOD(dev, GetFVF, 2) { MEM32(ARG(1)) = DEV->st.fvf; return D3D_OK; }

COM_METHOD(dev, CreateVertexShader, 3)
{
    d3d_shader *s = d3d_shader_create(DEV, 0, ARG(1));
    if (!s) { MEM32(ARG(2)) = 0; return D3DERR_INVALIDCALL; }
    MEM32(ARG(2)) = d3d_wrap_shader(s);
    return D3D_OK;
}

COM_METHOD(dev, SetVertexShader, 2)
{
    d3d_device *d = DEV;
    if (d->recording) { d3d_ref_set(&d->recording->state.vs, host_or_null(ARG(1))); d->recording->rec_vs = 1; return D3D_OK; }
    if (bind_guest(d, BOUND_VS, &d->st.vs, ARG(1))) d3d_dirty_field(d, vs);
    return D3D_OK;
}

COM_METHOD(dev, GetVertexShader, 2)
{
    void_com_object *o = (void_com_object *)DEV->st.vs;
    if (o) void_com_addref(o);
    MEM32(ARG(1)) = o ? o->guest : 0;
    return D3D_OK;
}

#define CONST_SET(field, rec, limit, width) do { \
    d3d_device *d = DEV; d3d_state *s = target_state(d); \
    uint32_t start = ARG(1), data = ARG(2), count = ARG(3); \
    if (start > (limit) || count > (limit) - start) { \
        static unsigned failures; \
        if (failures++ < 8) void_log("[pv] rejected constant " #field " start %u count %u limit %u", start, count, (unsigned)(limit)); \
        return D3DERR_INVALIDCALL; \
    } \
    size_t bytes = (size_t)count * (width); \
    if (!d->recording && !memcmp(&s->field[start], GPTR(data), bytes)) return D3D_OK; \
    memcpy(&s->field[start], GPTR(data), bytes); \
    if (d->recording) for (uint32_t i = 0; i < count; ++i) d->recording->rec[start + i] = 1; \
    else d3d_dirty_range(d, offsetof(d3d_state, field) + start * (width), count * (width)); \
    d->constants_dirty = 1; return D3D_OK; } while (0)
#define CONST_GET(field, limit, width) do { \
    d3d_device *d = DEV; uint32_t start = ARG(1), data = ARG(2), count = ARG(3); \
    if (start > (limit) || count > (limit) - start) return D3DERR_INVALIDCALL; \
    memcpy(GPTR(data), &d->st.field[start], (size_t)count * (width)); return D3D_OK; } while (0)

COM_METHOD(dev, SetVertexShaderConstantF, 4) { CONST_SET(vs_f, rec_vs_f, D3D_SOFTWARE_VS_CONSTANTS, 16); }
COM_METHOD(dev, GetVertexShaderConstantF, 4) { CONST_GET(vs_f, D3D_SOFTWARE_VS_CONSTANTS, 16); }
COM_METHOD(dev, SetVertexShaderConstantI, 4) { CONST_SET(vs_i, rec_vs_i, D3D_SOFTWARE_VS_INT_CONSTANTS, 16); }
COM_METHOD(dev, GetVertexShaderConstantI, 4) { CONST_GET(vs_i, D3D_SOFTWARE_VS_INT_CONSTANTS, 16); }
COM_METHOD(dev, SetVertexShaderConstantB, 4) { CONST_SET(vs_b, rec_vs_b, D3D_SOFTWARE_VS_BOOL_CONSTANTS, 4); }
COM_METHOD(dev, GetVertexShaderConstantB, 4) { CONST_GET(vs_b, D3D_SOFTWARE_VS_BOOL_CONSTANTS, 4); }

COM_METHOD(dev, SetStreamSource, 5)
{
    d3d_device *d = DEV;
    uint32_t n = ARG(1);
    if (n >= 16) return D3DERR_INVALIDCALL;
    if (d->recording) {
        d3d_ref_set(&d->recording->state.streams[n], host_or_null(ARG(2)));
        d->recording->state.stream_offset[n] = ARG(3);
        d->recording->state.stream_stride[n] = ARG(4);
        d->recording->rec_stream[n] = 1;
        return D3D_OK;
    }
    if (bind_guest(d, BOUND_STREAM + (int)n, &d->st.streams[n], ARG(2))) d3d_dirty_field(d, streams[n]);
    if (d->st.stream_offset[n] != ARG(3)) { d->st.stream_offset[n] = ARG(3); d3d_dirty_field(d, stream_offset[n]); }
    if (d->st.stream_stride[n] != ARG(4)) { d->st.stream_stride[n] = ARG(4); d3d_dirty_field(d, stream_stride[n]); }
    return D3D_OK;
}

COM_METHOD(dev, GetStreamSource, 5)
{
    d3d_device *d = DEV;
    uint32_t n = ARG(1);
    if (n >= 16) return D3DERR_INVALIDCALL;
    void_com_object *o = (void_com_object *)d->st.streams[n];
    if (o) void_com_addref(o);
    MEM32(ARG(2)) = o ? o->guest : 0;
    MEM32(ARG(3)) = d->st.stream_offset[n];
    MEM32(ARG(4)) = d->st.stream_stride[n];
    return D3D_OK;
}

COM_METHOD(dev, SetStreamSourceFreq, 3)
{
    d3d_device *d = DEV;
    if (ARG(1) >= 16) return D3DERR_INVALIDCALL;
    target_state(d)->stream_freq[ARG(1)] = ARG(2);
    DIRTY(stream_freq[ARG(1)]);
    return D3D_OK;
}
COM_METHOD(dev, GetStreamSourceFreq, 3) { MEM32(ARG(2)) = ARG(1) < 16 ? DEV->st.stream_freq[ARG(1)] : 1; return D3D_OK; }

COM_METHOD(dev, SetIndices, 2)
{
    d3d_device *d = DEV;
    if (d->recording) { d3d_ref_set(&d->recording->state.indices, host_or_null(ARG(1))); d->recording->rec_indices = 1; return D3D_OK; }
    if (bind_guest(d, BOUND_INDICES, &d->st.indices, ARG(1))) d3d_dirty_field(d, indices);
    return D3D_OK;
}

COM_METHOD(dev, GetIndices, 2)
{
    void_com_object *o = (void_com_object *)DEV->st.indices;
    if (o) void_com_addref(o);
    MEM32(ARG(1)) = o ? o->guest : 0;
    return D3D_OK;
}

COM_METHOD(dev, CreatePixelShader, 3)
{
    if (getenv("VOID_TRACE_PSDB")) {
        /* RendererD3D's shader database loader (16255330): database, key at [ebp + 8], [ebp + 0xC]. */
        uint32_t k = (g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1);
        if (g_icall_from[k] == 0x16255330u)
            void_log("[psdb] db %08X key %08X code %08X first %08X", MEM32(g_ebp + 8), MEM32(g_ebp + 0xC), ARG(1), MEM32(ARG(1)));
    }
    d3d_shader *s = d3d_shader_create(DEV, 1, ARG(1));
    if (!s) { MEM32(ARG(2)) = 0; return D3DERR_INVALIDCALL; }
    MEM32(ARG(2)) = d3d_wrap_shader(s);
    return D3D_OK;
}

COM_METHOD(dev, SetPixelShader, 2)
{
    d3d_device *d = DEV;
    if (d->recording) { d3d_ref_set(&d->recording->state.ps, host_or_null(ARG(1))); d->recording->rec_ps = 1; return D3D_OK; }
    if (bind_guest(d, BOUND_PS, &d->st.ps, ARG(1))) d3d_dirty_field(d, ps);
    return D3D_OK;
}

COM_METHOD(dev, GetPixelShader, 2)
{
    void_com_object *o = (void_com_object *)DEV->st.ps;
    if (o) void_com_addref(o);
    MEM32(ARG(1)) = o ? o->guest : 0;
    return D3D_OK;
}

COM_METHOD(dev, SetPixelShaderConstantF, 4)
{
    if (d3d_trace_active()) {
        uint32_t k = (g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1);
        const float *f = (const float *)GPTR(ARG(2));
        void_log("[trace] SetPixelShaderConstantF(c%u x%u) = (%g %g %g %g) from %08X", ARG(1), ARG(3),
            f[0], f[1], f[2], f[3], g_icall_from[k]);
    }
    CONST_SET(ps_f, rec_ps_f, 224, 16);
}
COM_METHOD(dev, GetPixelShaderConstantF, 4) { CONST_GET(ps_f, 224, 16); }
COM_METHOD(dev, SetPixelShaderConstantI, 4) { CONST_SET(ps_i, rec_ps_i, 16, 16); }
COM_METHOD(dev, GetPixelShaderConstantI, 4) { CONST_GET(ps_i, 16, 16); }
COM_METHOD(dev, SetPixelShaderConstantB, 4) { CONST_SET(ps_b, rec_ps_b, 16, 4); }
COM_METHOD(dev, GetPixelShaderConstantB, 4) { CONST_GET(ps_b, 16, 4); }

COM_METHOD(dev, DrawRectPatch, 4) { return D3DERR_INVALIDCALL; }
COM_METHOD(dev, DrawTriPatch, 4) { return D3DERR_INVALIDCALL; }
COM_METHOD(dev, DeletePatch, 2) { return D3D_OK; }

uint32_t d3d_query_create(d3d_device *dev, uint32_t type);
COM_METHOD(dev, CreateQuery, 3)
{
    uint32_t type = ARG(1);
    if (type != 8 && type != 9 && type != 4) {           /* EVENT, OCCLUSION, VCACHE */
        VLOG("[d3d9] CreateQuery(%u): unsupported", type);
        return D3DERR_NOTAVAILABLE;
    }
    uint32_t out = ARG(2);
    if (!out) return D3D_OK;                              /* support check */
    uint32_t query = d3d_query_create(DEV, type);
    MEM32(out) = query;
    return query ? D3D_OK : E_OUTOFMEMORY_;
}

static const void_fn device_methods[119] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_TestCooperativeLevel, dev_GetAvailableTextureMem,
    dev_EvictManagedResources, dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
    dev_SetCursorProperties, dev_SetCursorPosition, dev_ShowCursor, dev_CreateAdditionalSwapChain, dev_GetSwapChain,
    dev_GetNumberOfSwapChains, dev_Reset, dev_Present, dev_GetBackBuffer, dev_GetRasterStatus, dev_SetDialogBoxMode,
    dev_SetGammaRamp, dev_GetGammaRamp, dev_CreateTexture, dev_CreateVolumeTexture, dev_CreateCubeTexture,
    dev_CreateVertexBuffer, dev_CreateIndexBuffer, dev_CreateRenderTarget, dev_CreateDepthStencilSurface,
    dev_UpdateSurface, dev_UpdateTexture, dev_GetRenderTargetData, dev_GetFrontBufferData, dev_StretchRect,
    dev_ColorFill, dev_CreateOffscreenPlainSurface, dev_SetRenderTarget, dev_GetRenderTarget,
    dev_SetDepthStencilSurface, dev_GetDepthStencilSurface, dev_BeginScene, dev_EndScene, dev_Clear,
    dev_SetTransform, dev_GetTransform, dev_MultiplyTransform, dev_SetViewport, dev_GetViewport, dev_SetMaterial,
    dev_GetMaterial, dev_SetLight, dev_GetLight, dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane,
    dev_GetClipPlane, dev_SetRenderState, dev_GetRenderState, dev_CreateStateBlock, dev_BeginStateBlock,
    dev_EndStateBlock, dev_SetClipStatus, dev_GetClipStatus, dev_GetTexture, dev_SetTexture,
    dev_GetTextureStageState, dev_SetTextureStageState, dev_GetSamplerState, dev_SetSamplerState,
    dev_ValidateDevice, dev_SetPaletteEntries, dev_GetPaletteEntries, dev_SetCurrentTexturePalette,
    dev_GetCurrentTexturePalette, dev_SetScissorRect, dev_GetScissorRect, dev_SetSoftwareVertexProcessing,
    dev_GetSoftwareVertexProcessing, dev_SetNPatchMode, dev_GetNPatchMode, dev_DrawPrimitive,
    dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP, dev_ProcessVertices,
    dev_CreateVertexDeclaration, dev_SetVertexDeclaration, dev_GetVertexDeclaration, dev_SetFVF, dev_GetFVF,
    dev_CreateVertexShader, dev_SetVertexShader, dev_GetVertexShader, dev_SetVertexShaderConstantF,
    dev_GetVertexShaderConstantF, dev_SetVertexShaderConstantI, dev_GetVertexShaderConstantI,
    dev_SetVertexShaderConstantB, dev_GetVertexShaderConstantB, dev_SetStreamSource, dev_GetStreamSource,
    dev_SetStreamSourceFreq, dev_GetStreamSourceFreq, dev_SetIndices, dev_GetIndices, dev_CreatePixelShader,
    dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstantF, dev_GetPixelShaderConstantF,
    dev_SetPixelShaderConstantI, dev_GetPixelShaderConstantI, dev_SetPixelShaderConstantB,
    dev_GetPixelShaderConstantB, dev_DrawRectPatch, dev_DrawTriPatch, dev_DeletePatch, dev_CreateQuery,
};
static void_com_class device_class = { "IDirect3DDevice9", 119, device_methods, 0 };

uint32_t d3d_swapchain_create(d3d_device *dev);

/* The last Release of a device. The Sims 2 creates a device to probe the
 * adapter, releases it and creates the real one later, so the slot must
 * free up. The GL objects (FBOs, VAO, upload buffers) are the context's and
 * stay; surfaces the device itself holds go with it. */
static void device_destroy(void_com_object *o)
{
    d3d_device *dev = (d3d_device *)o;
    void_log("[d3d9] device released after %u frames", dev->frame);
    for (int i = 0; i < 4; ++i) bind_surface(&dev->rt[i], NULL);
    bind_surface(&dev->ds, NULL);
    if (dev->backbuffer) { void_com_release(&dev->backbuffer->com); dev->backbuffer = NULL; }
    if (dev->auto_depth) { void_com_release(&dev->auto_depth->com); dev->auto_depth = NULL; }
    if (dev->swapchain) {
        void_com_object *sc = (void_com_object *)void_com_host(dev->swapchain);
        dev->swapchain = 0;
        void_com_release(sc);
    }
    if (g_d3d_device == dev) g_d3d_device = NULL;
    free(dev);
}

/* The native backend the launcher chose (android/render), else the Direct3D 9 -> GLES layer. */
static void run_gl_init(void *arg)
{
    *(int *)arg = s2r_init(d3d_rt_device()) || d3d_gl_init();
    if (*(int *)arg) d3d_rt_apply_cores();
}

uint32_t d3d_device_create(uint32_t d3d9, uint32_t window, uint32_t behavior, uint32_t pp, uint32_t *out)
{
    if (g_d3d_device) { void_log("[d3d9] a second device is not supported"); return D3DERR_NOTAVAILABLE; }
    d3d_rt_start();
    int gl_ok = 0;
    d3d_rt_sync(run_gl_init, &gl_ok);
    if (!gl_ok) return D3DERR_NOTAVAILABLE;
    d3d_device *dev = (d3d_device *)calloc(1, sizeof *dev);
    dev->d3d9 = d3d9;
    dev->window = window;
    dev->behavior = behavior;
    dev->software_vp = (behavior & 0x20u) != 0;
    for (int i = 0; i < 14; ++i) dev->pp[i] = MEM32(pp + 4u * (uint32_t)i);
    if (!dev->pp[2]) dev->pp[2] = 22;
    if (!dev->pp[7]) dev->pp[7] = window;
    void_log("[d3d9] CreateDevice window=%08X behavior=%08X backbuffer=%ux%u format=%u count=%u ms=%u swap=%u "
        "windowed=%u autodepth=%u/%u flags=%08X refresh=%u interval=%08X",
        window, behavior, dev->pp[0], dev->pp[1], dev->pp[2], dev->pp[3], dev->pp[4], dev->pp[6], dev->pp[8],
        dev->pp[9], dev->pp[10], dev->pp[11], dev->pp[12], dev->pp[13]);
    g_d3d_device = dev;
    *out = void_com_wrap(&dev->com, &device_class, 0);
    dev->com.destroy = device_destroy;
    void_com_addref((void_com_object *)void_com_host(d3d9));
    make_backbuffer(dev);
    for (int i = 0; i < 14; ++i) MEM32(pp + 4u * (uint32_t)i) = dev->pp[i];
    default_state(dev, &dev->st);
    set_default_viewport(dev, dev->backbuffer);
    d3d_rt_device_reset(dev);
    dev->swapchain = d3d_swapchain_create(dev);
    return D3D_OK;
}

/* Index of the next set flag at or after i (n if none), skipping eight clear
 * flags a step: a block records a handful of the ~1,900 states it could. */
static inline int next_flag(const uint8_t *f, int i, int n)
{
    for (; i < n && (i & 7); ++i) if (f[i]) return i;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, f + i, 8);
        if (w) return i + (__builtin_ctzll(w) >> 3);
    }
    for (; i < n; ++i) if (f[i]) return i;
    return n;
}
#define FOR_FLAGS(i, flags, n) for (int i = next_flag((flags), 0, (n)); i < (n); i = next_flag((flags), i + 1, (n)))

/* State block application (d3d9_resource.c builds and captures them). */
void d3d_stateblock_apply(d3d_stateblock *sb)
{
    d3d_device *d = sb->dev;
    d3d_state *s = &sb->state;
    memset(d->bound, 0, sizeof d->bound);   /* slots rebound outside bind_guest */
#define COPY_FIELD(field) do { memcpy(&d->st.field, &s->field, sizeof d->st.field); d3d_dirty_field(d, field); } while (0)
    FOR_FLAGS(i, sb->rec_rs, 256) COPY_FIELD(rs[i]);
    FOR_FLAGS(i, &sb->rec_ss[0][0], 20 * 14) COPY_FIELD(ss[i / 14][i % 14]);
    FOR_FLAGS(i, &sb->rec_tss[0][0], 8 * 33) COPY_FIELD(tss[i / 33][i % 33]);
    FOR_FLAGS(i, sb->rec_tex, 20) { bind_ref(&d->st.textures[i], s->textures[i]); d3d_dirty_field(d, textures[i]); }
    FOR_FLAGS(i, sb->rec_stream, 16) {
        bind_ref(&d->st.streams[i], s->streams[i]);
        d3d_dirty_field(d, streams[i]);
        COPY_FIELD(stream_offset[i]);
        COPY_FIELD(stream_stride[i]);
        COPY_FIELD(stream_freq[i]);
    }
    int constants = 0;   /* shader constants recorded: programs must upload again */
    FOR_FLAGS(i, sb->rec_vs_f, D3D_SOFTWARE_VS_CONSTANTS) { COPY_FIELD(vs_f[i]); constants = 1; }
    FOR_FLAGS(i, sb->rec_ps_f, 224) { COPY_FIELD(ps_f[i]); constants = 1; }
    FOR_FLAGS(i, sb->rec_vs_i, D3D_SOFTWARE_VS_INT_CONSTANTS) { COPY_FIELD(vs_i[i]); constants = 1; }
    FOR_FLAGS(i, sb->rec_vs_b, D3D_SOFTWARE_VS_BOOL_CONSTANTS) { COPY_FIELD(vs_b[i]); constants = 1; }
    for (int i = 0; i < 16; ++i) {
        if (sb->rec_ps_i[i]) { COPY_FIELD(ps_i[i]); constants = 1; }
        if (sb->rec_ps_b[i]) { COPY_FIELD(ps_b[i]); constants = 1; }
    }
    FOR_FLAGS(i, sb->rec_transform, 512) COPY_FIELD(transforms[i]);
    if (sb->rec_viewport) { COPY_FIELD(viewport); COPY_FIELD(viewport_z); }
    if (sb->rec_scissor) COPY_FIELD(scissor);
    if (sb->rec_indices) { bind_ref(&d->st.indices, s->indices); d3d_dirty_field(d, indices); }
    if (sb->rec_decl) { bind_ref(&d->st.decl, s->decl); d3d_dirty_field(d, decl); }
    if (sb->rec_fvf) COPY_FIELD(fvf);
    if (sb->rec_vs) { bind_ref(&d->st.vs, s->vs); d3d_dirty_field(d, vs); }
    if (sb->rec_ps) { bind_ref(&d->st.ps, s->ps); d3d_dirty_field(d, ps); }
    if (sb->rec_material) COPY_FIELD(material);
    for (int i = 0; i < 8; ++i) if (sb->rec_light[i]) { COPY_FIELD(lights[i]); COPY_FIELD(light_enable[i]); }
    for (int i = 0; i < 6; ++i) if (sb->rec_clip[i]) COPY_FIELD(clip_planes[i]);
#undef COPY_FIELD
    if (constants) d->constants_dirty = 1;   /* the render thread's copy bumps d3d_constant_generation */
}
