/* The game's sound engine without DirectSound.
 *
 * The Sims 2 plays every sound through EA's SND library (9.04, linked into
 * Sims2EP9.exe: "SND supports %d voices on device main"). SND mixes all voices
 * itself, in 10 ms ticks of 16-frame blocks, and its PC driver only copied that
 * 16-bit mix into a looping half-second DirectSound buffer from SND's service
 * thread, 45 ms ahead of the write cursor. That driver is native here
 * (tools/build_android.py NATIVE_OVERRIDES), so SND renders straight into the
 * FIFO of the native output (audio.c), which an AAudio low-latency callback
 * drains, at the device's own rate (SND mixes at 11025..48000 Hz):
 *
 *   0xA00E9ACB  probe: stereo output                     (was DirectSoundCreate, GetCaps, SetFormat tries)
 *   0xA00E9C68  open: rate, mixer and output             (DirectSoundCreate, the buffers, Play)
 *   0xA00E98CB  close                                    (Release)
 *   0xA00E9931  latency: mixed PCM not yet heard, in ms  (GetCurrentPosition)
 *   0xA00E9E0D  fill: render up to the FIFO's high mark  (GetCurrentPosition, Lock, Unlock)
 *   0xA00EA0DD  service thread: runs when the output asks (a 10 ms Sleep loop)
 *
 * The rest of SND (voices, streams, the mixer, its lock, the start and stop of
 * its thread) runs unchanged. DirectSound itself is not provided any more. */
#include "host.h"

/* SND state. The configuration (156 bytes at 0xA10FE9BC) is restored from a
 * snapshot taken when it was last applied (0xA10FEA6C) whenever it is applied
 * again while SND is open: the output's rate and channels go into both. */
#define SND_CONFIG        0xA10FE9BCu
#define SND_SNAPSHOT      0xA10FEA6Cu
#define SNAPSHOT(field)   (SND_SNAPSHOT + ((field) - SND_CONFIG))
#define SND_RATE_MIN      0xA10FE9A0u   /* u16, 11025 */
#define SND_RATE_MAX      0xA10FE9A2u   /* u16, 48000 */
#define SND_RATE          0xA10FE9C6u   /* u16, 44100 by default */
#define SND_VOICES        0xA10FE9CAu   /* u8 */
#define SND_CHANNELS      0xA10FE9DFu   /* u8: main channels */
#define SND_LFE           0xA10FE9E0u   /* u8: a low-frequency channel */
#define SND_THREAD_RUN    0xA10FE995u   /* u8: the service thread keeps running */
#define SND_THREAD_ALIVE  0xA10FE994u   /* u8: cleared as it exits (its stop spins on it) */
#define SND_SERVICE_PRE   0xA10FEBC0u   /* optional hooks around a service; nothing sets them */
#define SND_SERVICE_POST  0xA10FEBC4u
#define SND_DS_PRIMARY    0xA10FEC3Cu   /* the driver's DirectSound buffers: stay NULL */
#define SND_DS_BUFFER     0xA10FEC40u
#define SND_LOCK          0xA10FEBD4u   /* the HANDLE of SND's lock, a Win32 mutex */

#define SND_MIXER_INIT    0xA00EC268u   /* cdecl (params *): rate, voice callback, voices, channels */
#define SND_MIXER_FREE    0xA00EBEB1u
#define SND_VOICE_DONE    0xA00EB551u   /* the voice callback the PC driver hands the mixer */
#define SND_RENDER        0xA00E99A4u   /* cdecl (int16 **out, frames): whole 16-frame blocks */
#define SND_SERVICE       0xA00EA0CCu   /* SND's lock, the fill (0xA00E9E0D), the unlock */

/* probe(u8 *max_channels, u8 formats[4]) -> 0: mono and stereo. */
void sub_A00E9ACB_lifted(void);
void sub_A00E9ACB(void)
{
    uint32_t max_channels = ARG(0), formats = ARG(1);
    if (max_channels && void_vm_writable(max_channels, 1)) MEM8(max_channels) = 2;
    if (formats && void_vm_writable(formats, 4)) {
        MEM8(formats) = 1;          /* mono */
        MEM8(formats + 1) = 1;      /* stereo */
        MEM8(formats + 2) = 0;      /* quadraphonic */
        MEM8(formats + 3) = 0;      /* 5.1 */
    }
    g_eax = 0;
    g_esp += 4;
}

/* open(int device, bool start_mixer) -> 0. The original reported DirectSound
 * failures here ("No sound hardware is present"); the native output always
 * opens: without a device its null sink keeps SND's clock. */
void sub_A00E9C68_lifted(void);
void sub_A00E9C68(void)
{
    uint32_t sp = g_esp, start_mixer = ARG(1) & 0xFFu, asked = MEM16(SND_RATE);
    if (MEM8(SND_CHANNELS) != 2 || MEM8(SND_LFE)) {
        void_log("[snd] %u+%u channels configured; the output is stereo", MEM8(SND_CHANNELS), MEM8(SND_LFE));
        MEM8(SND_CHANNELS) = MEM8(SNAPSHOT(SND_CHANNELS)) = 2;
        MEM8(SND_LFE) = MEM8(SNAPSHOT(SND_LFE)) = 0;
    }
    uint32_t low = MEM16(SND_RATE_MIN), high = MEM16(SND_RATE_MAX);
    if (!low || low > high) { low = 11025; high = 48000; }
    uint32_t rate = void_audio_open(low, high);
    MEM16(SND_RATE) = MEM16(SNAPSHOT(SND_RATE)) = (uint16_t)rate;
    if (start_mixer) {
        g_esp = sp - 16;                /* the parameter block, below the caller's frame */
        uint32_t params = g_esp;
        MEM32(params) = rate;
        MEM32(params + 4) = SND_VOICE_DONE;
        MEM8(params + 8) = MEM8(SND_VOICES);
        MEM8(params + 9) = 2;           /* channels */
        MEM8(params + 10) = 1;          /* more than one channel */
        MEM8(params + 11) = 0;
        void_call_guest(SND_MIXER_INIT, 1, &params, 0);
        g_esp = sp;
    }
    MEM32(SND_DS_PRIMARY) = 0;
    MEM32(SND_DS_BUFFER) = 0;
    void_log("[snd] native output: SND mixes %u voices at %u Hz (its default %u Hz), no DirectSound; opened by thread %u",
             MEM8(SND_VOICES), rate, asked, void_thread_id());
    g_eax = 0;
    g_esp = sp + 4;
}

/* close(int device, bool free_mixer) -> 0. */
void sub_A00E98CB_lifted(void);
void sub_A00E98CB(void)
{
    uint32_t free_mixer = ARG(1) & 0xFFu;
    void_audio_close();
    MEM32(SND_DS_PRIMARY) = 0;
    MEM32(SND_DS_BUFFER) = 0;
    if (free_mixer) void_call_guest(SND_MIXER_FREE, 0, NULL, 0);
    g_eax = 0;
    g_esp += 4;
}

/* The milliseconds of mixed PCM not yet heard (streams subtract it from their
 * elapsed time, so pictures and words follow what is audible). */
void sub_A00E9931_lifted(void);
void sub_A00E9931(void)
{
    g_eax = void_audio_pending_ms();
    g_esp += 4;
}

/* Who held SND's lock as the service thread asked for it, and since when
 * (the output's summary names the slowest). */
static uint32_t service_owner;
static uint64_t service_ns;

/* The fill, under SND's lock: render to the FIFO's high-water mark, in at most
 * two pieces where the FIFO wraps. */
void sub_A00E9E0D_lifted(void);
void sub_A00E9E0D(void)
{
    uint32_t sp = g_esp;
    if (service_ns) {
        void_audio_blocked_by(service_owner, void_monotonic_ns() - service_ns);
        service_ns = 0;
    }
    g_esp = sp - 16;
    uint32_t slot = g_esp;              /* the int16 * SND_RENDER reads */
    for (uint32_t want = void_audio_want(); want;) {
        uint32_t at, frames = void_audio_region(&at, want);
        if (!frames) break;
        MEM32(slot) = at;
        uint32_t args[2] = { slot, frames };
        void_call_guest(SND_RENDER, 2, args, 0);
        void_audio_commit(frames);
        want -= frames;
    }
    g_esp = sp + 4;
}

/* SND's service thread (stdcall, one parameter): instead of a 10 ms Sleep loop
 * it waits for the output, so SND mixes as the device consumes. */
void sub_A00EA0DD_lifted(void);
void sub_A00EA0DD(void)
{
    uint32_t sp = g_esp;
    void_log("[snd] service thread %u mixes on the output's demand", void_thread_id());
    while (MEM8(SND_THREAD_RUN)) {
        if (!void_audio_wait(20)) continue;
        uint32_t hook = MEM32(SND_SERVICE_PRE);
        if (hook) void_call_guest(hook, 0, NULL, 0);
        void_object *lock = void_handle_get(MEM32(SND_LOCK), VOID_OBJ_MUTEX);
        service_owner = lock ? __atomic_load_n(&lock->owner, __ATOMIC_RELAXED) : 0;
        service_ns = void_monotonic_ns();
        void_call_guest(SND_SERVICE, 0, NULL, 0);
        hook = MEM32(SND_SERVICE_POST);
        if (hook) void_call_guest(hook, 0, NULL, 0);
    }
    void_audio_producer_stopped();
    MEM8(SND_THREAD_ALIVE) = 0;
    g_eax = 0;
    g_esp = sp + 8;
}

/* dsound.dll #1. Its only caller was SND's PC driver, native above. */
WINAPI_ORDINAL("dsound.dll", DirectSoundCreate, 1, 3)
{
    void_log("[snd] DirectSoundCreate from %08X: DirectSound is not provided", g_cur_func);
    if (ARG(1) && void_vm_writable(ARG(1), 4)) MEM32(ARG(1)) = 0;
    return 0x88780078u;                 /* DSERR_NODRIVER */
}
