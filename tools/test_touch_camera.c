/* Actual native camera queue/math; the original world-boundary callback is a fixture. */
#include "../android/host/host.h"
#include "../android/host/sims2_settings.h"
#include <assert.h>
#include <sys/mman.h>
#include <math.h>
recomp_machine_t g_m;
x87_window_t g_x87;
int g_fp_bias, void_verbose;
uint64_t g_mm[8];
static float fixture_matrix[9];
static unsigned context = S2_TOUCH_TOWN, barrier;
static int screen_w = 1346, screen_h = 600, setters, pages, picker_reenter;
unsigned s2_touch_context(void) { return context; }
int s2_touch_rewards(void) { return 0; }
uint32_t void_thread_id(void) { return 1; }
void s2_touch_cancel_text(void) {}
void s2_cheats_owner(unsigned value,unsigned manager){(void)value;(void)manager;}
void sub_A0B4C1B0_lifted(void){}
int s2_touch_text_editable(uint32_t window) { (void)window;return 0; }
int s2_catalog_info(uint32_t proc) { (void)proc;return 0; }
void s2_catalog_track(uint32_t proc){(void)proc;}
uint32_t s2_catalog_clear_request(void){return 1;}
int s2_catalog_clear_owner(uint32_t serial){return serial==1;}
void void_input_screen_size(int *w, int *h) { *w = screen_w; *h = screen_h; }
int void_input_touch_barrier(unsigned serial) { barrier = serial; return 1; }
int void_vm_readable(uint32_t a, uint32_t n)
{
    return (a >= 0x10000 && (uint64_t)a + n <= 0x30000) ||
        (a >= 0xA0DB8000u && (uint64_t)a + n <= 0xA0DBB000u);
}
int void_vm_writable(uint32_t a, uint32_t n) { return a >= 0x10000 && (uint64_t)a + n <= 0x30000; }
uint32_t void_call_guest(uint32_t va,int n,const uint32_t *args,int pops)
{
    assert(va==0xA01A77F0u&&n==2&&!pops);memcpy(GPTR(args[0]),fixture_matrix,sizeof fixture_matrix);return args[0];
}
uint32_t void_call_guest_this(uint32_t va, uint32_t self, int n, const uint32_t *args)
{
    if(va==0xA0B4CEC0u){
        assert(n==4&&args[3]==1);float angle,distance;memcpy(&angle,GPTR(self+196),4);memcpy(&distance,&args[2],4);
        float c=cosf(angle),s=sinf(angle),q=.707106781f,eye[3];
        memcpy(eye,GPTR(self+364),12);eye[0]+=distance*q*c;eye[1]+=distance*q*s;eye[2]+=distance*q;
        memcpy(GPTR(args[0]),eye,12);
        float matrix[9]={-s,-q*c,-q*c,c,-q*s,-q*s,0,-q,q};memcpy(fixture_matrix,matrix,sizeof matrix);return 1;
    }
    if (va == 0x12345678u) {
        assert(self == 0x12000 && n == 2 && args[0] == 700 && args[1] == 500);
        if (picker_reenter) context |= S2_TOUCH_MODAL;
        return 0x13000;
    }
    if (va == 0xA07479F0u) {
        assert(self == 0x16000 && n == 1 && args[0] < 100);
        ++pages;MEM32(self + 0x20) = args[0];return 1;
    }
    assert((va == 0xA035B8C0u || va == 0xA0359AC0u || va == 0xA035A850u) && (self == 0x15000 || self == 0x15500) && n == 1 && args[0] == g_esp);
    assert(MEM8(self + 388) == 0);
    ++setters;
    memcpy(GPTR(self + 364), GPTR(args[0]), 12);
    memcpy(GPTR(self + 376), GPTR(args[0]), 12);
    MEM8(self + 388) = 1;
    return 1;
}
static uint64_t test_now = 1000000000;
static int test_clock_gettime(clockid_t which, struct timespec *now)
{
    (void)which; now->tv_sec = test_now / 1000000000; now->tv_nsec = test_now % 1000000000; return 0;
}
double s2_ui_factor(void) { return 1; }
uint32_t s2_ui_pick(uint32_t root, int x, int y)
{
    uint32_t point[] = {(uint32_t)x, (uint32_t)y};
    return void_call_guest_this(MEM32(MEM32(root) + 0xBC), root, 2, point);
}
#define clock_gettime test_clock_gettime
#include "../android/host/sims2_touch.c"
static void put(uint32_t address, float value) { memcpy(GPTR(address), &value, 4); }
int main(void)
{
    assert(mmap((void *)(VOID_GUEST_BASE + 0x10000), 0x20000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
    assert(mmap((void *)(VOID_GUEST_BASE + 0xA0DB8000u), 0x4000, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != MAP_FAILED);
    town_camera = 0x15000; g_esp = 0x2F000;
    MEM32(town_camera) = 0xA0DBAABCu;
    MEM32(0xA0DBAAB8u) = 0x18000;
    MEM32(0x1800C) = 0x18100;
    strcpy(GPTR(0x18108), ".?AVcCameraNeighborhood@nTSSG@@");
    put(town_camera + 0x134, 0.575958653f); put(town_camera + 196, 0); put(town_camera + 224, 650);
    put(town_camera + 364, 500); put(town_camera + 368, 500); put(town_camera + 372, 8);
    uint64_t size = (uint64_t)screen_h << 32 | screen_w;
    unsigned epoch = s2_touch_layout_epoch();
    printf("camera fixture context=%u layout=%d readable=%d table=%08X type=%s angle=%g distance=%g esp=%X\n",
        s2_touch_context(), touch_layout_matches(S2_TOUCH_TOWN, size, epoch), void_vm_writable(town_camera,0x458),
        MEM32(town_camera), touch_type(town_camera), (double)touch_float(town_camera+196),
        (double)touch_float(town_camera+224),g_esp);
    assert(touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size));
    assert(setters == 1 && g_esp == 0x2F000 && touch_float(town_camera + 368) < 465);
    assert(fabsf(touch_float(town_camera + 364) - 500) < .001f);
    assert(!memcmp(GPTR(town_camera + 364), GPTR(town_camera + 376), 12));
    uint32_t captured=town_camera;town_camera=last_clamp_camera=last_clamp_vtable=0;
    g_ecx=captured+0x454;sub_A0B4C1B0();assert(town_camera==captured);
    assert(touch_camera_apply(-.1f, 0, 0, 0, 0, epoch, size));
    assert(fabsf(touch_float(town_camera + 364) - 500) < .001f);
    assert(touch_camera_apply(0, 0, 1.570796327f, 0, 0, epoch, size));
    assert(touch_float(town_camera + 196) == touch_float(town_camera + 200));
    assert(touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size));
    assert(fabsf(touch_float(town_camera + 368) - 500) < .001f && touch_float(town_camera + 364) > 535);
    for (unsigned heading = 0; heading < 4; ++heading) {
        static const float right[4][2] = {{0,1},{-1,0},{0,-1},{1,0}};
        static const float up[4][2] = {{-1,0},{0,-1},{1,0},{0,1}};
        put(town_camera + 196, heading * 1.570796327f);
        put(town_camera + 364, 500); put(town_camera + 368, 500);
        assert(touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size));
        float x = touch_float(town_camera + 364) - 500, y = touch_float(town_camera + 368) - 500;
        assert(x*right[heading][0]+y*right[heading][1] < -38);
        assert(fabsf(x*up[heading][0]+y*up[heading][1]) < .001f);
        put(town_camera + 364, 500); put(town_camera + 368, 500);
        assert(touch_camera_apply(0, .1f, 0, 0, 0, epoch, size));
        x = touch_float(town_camera + 364) - 500; y = touch_float(town_camera + 368) - 500;
        assert(x*up[heading][0]+y*up[heading][1] > 20);
        assert(fabsf(x*right[heading][0]+y*right[heading][1]) < .001f);
    }
    context |= S2_TOUCH_MODAL;
    assert(!touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size) && !s2_touch_camera(.1f, 0, 0, 0, 0));
    context = S2_TOUCH_TOWN;
    assert(!touch_camera_apply(.1f, 0, 0, 0, 0, epoch + 1, size));
    assert(!touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size + 1));
    assert(!s2_touch_camera(NAN, 0, 0, 0, 0) && !s2_touch_camera(0, INFINITY, 0, 0, 0));
    assert(!s2_touch_camera(1.1f, 0, 0, 0, 0) && !s2_touch_camera(0, 0, 4, 0, 0));
    assert(!s2_touch_camera(0, 0, 0, 0, 0));
    assert(s2_touch_camera(.02f, .03f, .1f, 0, 0));
    assert(s2_touch_camera(.04f, -.01f, .2f, 0, 0));
    assert(ui_command_kind == S2_UI_CAMERA && barrier == pick_serial);
    assert(fabsf(camera_request.dx - .06f) < .00001f && fabsf(camera_request.dy - .02f) < .00001f);
    assert(fabsf(camera_request.radians - .3f) < .00001f);
    ui_command_started = 1;
    assert(!s2_touch_camera(.1f, 0, 0, 0, 0)); ui_command_started = 0;
    slider_state = SLIDER_STARTED;
    assert(!s2_touch_camera(.1f, 0, 0, 0, 0)); slider_state = SLIDER_IDLE;
    test_now += 400000000; ui_command_expire();
    assert(ui_command_kind == S2_UI_CAMERA && pick_pending);
    test_now += 100000000; ui_command_expire();
    assert(!ui_command_kind && !pick_pending);
    MEM32(town_camera) = 0xA0DBAA00u;
    assert(!touch_camera_apply(.1f, 0, 0, 0, 0, epoch, size));
    lot_camera = 0x15500;
    for (unsigned mode = S2_TOUCH_TOWN; mode <= S2_TOUCH_BUILD; mode *= 2) {
        context = mode;
        uint32_t camera = mode == S2_TOUCH_TOWN ? town_camera : lot_camera;
        MEM32(camera) = mode == S2_TOUCH_TOWN ? 0xA0DBAABCu : mode == S2_TOUCH_BUILD ? 0xA0DBA8F4u : 0xA0DBA6FCu;
        MEM32(MEM32(camera)-4) = 0x18000;
        strcpy(GPTR(0x18108), mode == S2_TOUCH_TOWN ? ".?AVcCameraNeighborhood@nTSSG@@" :
            mode == S2_TOUCH_BUILD ? ".?AVcCameraBuild@nTSSG@@" : ".?AVcCameraLive@nTSSG@@");
        put(camera + 0x134, .785398163f);
        put(camera + 196, 0);put(camera + 364, 500);put(camera + 368, 500);
        MEM32(camera + 0x70)=0x18500;MEM32(camera+0x74)=0x18508;
        put(0x18500,650);put(0x18504,100);put(camera+224,650);
        assert(touch_camera_apply(.1f,0,0,0, 0,epoch,size));
        assert(fabsf(touch_float(camera+368)-(500-650*.828427125f*.1f))<.002f);
        if (mode != S2_TOUCH_TOWN) {
            for (unsigned transform = 0; transform < 3; ++transform) {
                put(camera+196,0);put(camera+200,0);put(camera+224,650);put(camera+228,650);
                put(camera+0x150,.3f);put(camera+0x154,.3f);
                assert(touch_camera_apply(.01f,0,transform==0?.1f:0,
                    transform==1?.1f:0,transform==2?.1f:0,epoch,size));
                assert(touch_camera_apply(.01f,0,0,0,0,epoch,size));
            }
            put(camera+196,0);put(camera+200,0);put(camera+224,650);put(camera+228,650);
            ui_command_kind=pick_pending=0;
            assert(s2_touch_pan(.2f,.3f,.21f,.3f));
            assert(s2_touch_camera(0,0,.1f,0,0));
            assert(camera_request.dx>0 && camera_request.radians==.1f);
            assert(touch_camera_apply(camera_request.dx,camera_request.dy,camera_request.radians,
                camera_request.zoom,camera_request.pitch,epoch,size));
            ui_command_kind=pick_pending=0;
            put(camera+196,0);put(camera+200,0);put(camera+224,650);put(camera+228,650);
        }
        float x=touch_float(camera+364),y=touch_float(camera+368);
        put(camera+372,.75f);put(camera+384,.75f);
        uint8_t position[24];memcpy(position,GPTR(camera+364),sizeof position);
        int pan_setters=setters;
        assert(touch_camera_apply(0,0,.2f,0, 0,epoch,size));
        assert(setters==pan_setters&&!memcmp(position,GPTR(camera+364),sizeof position));
        MEM8(camera+0x200)=0;
        assert(touch_camera_apply(0,0,0,1, 0,epoch,size));
        assert(setters==pan_setters&&!memcmp(position,GPTR(camera+364),sizeof position));
        assert(MEM8(camera+0x200)==1);
        assert(fabsf(touch_float(camera+224)-650*expf(-1))<.001f);
        assert(touch_float(camera+224)==touch_float(camera+228));
        assert(touch_float(camera+364)==x&&touch_float(camera+368)==y);
        assert(touch_camera_apply(0,0,0,1, 0,epoch,size)&&touch_float(camera+224)==100);
        for(unsigned n=0;n<3;++n)assert(touch_camera_apply(0,0,0,-1, 0,epoch,size));
        assert(touch_float(camera+224)==650);
        ui_command_kind=pick_pending=0;
        assert(s2_touch_camera(0,0,0,.2f, 0)&&camera_request.context==mode);
        assert(s2_touch_camera(0,0,0,.1f, 0)&&fabsf(camera_request.zoom-.3f)<.00001f);
        assert(!s2_touch_camera(0,0,0,NAN, 0)&&!s2_touch_camera(0,0,0,1.1f, 0));
        put(camera+0xFC,.2f);put(camera+0x118,2000.f);
        put(camera+0x150,.3f);put(camera+0x154,.3f);
        assert(touch_camera_apply(0,0,0,0,0,epoch,size)&&touch_float(camera+0x154)==.3f);
        assert(touch_camera_apply(0,0,0,0,.2f,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-.5f)<.00001f);
        assert(touch_camera_apply(0,0,0,0,1.5f,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-1.553343034f)<.00001f);
        assert(touch_camera_apply(0,0,0,0,-.1f,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-1.453343034f)<.00001f);
        assert(touch_camera_apply(0,0,0,0,-1.5f,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-.017453293f)<.00001f);
        assert(touch_camera_apply(0,0,0,0,.1f,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-.117453293f)<.00001f);
        for(unsigned i=0;i<120;++i)assert(touch_camera_apply(0,0,0,0,0,epoch,size));
        assert(fabsf(touch_float(camera+0x154)-.117453293f)<.00001f);
        assert(touch_float(camera+0x150)==touch_float(camera+0x154)&&MEM8(camera+0x158)==1);
        assert(touch_float(camera+0xFC)==.2f&&touch_float(camera+0x118)==2000.f);
        assert(MEM8(camera+0x201)==1);
        ui_command_kind=pick_pending=0;
        assert(s2_touch_camera(0,0,0,0,.2f)&&s2_touch_camera(0,0,0,0,-.05f));
        assert(fabsf(camera_request.pitch-.15f)<.00001f);
        assert(!s2_touch_camera(0,0,0,0,NAN)&&!s2_touch_camera(0,0,0,0,2));
        put(camera+0x154,NAN);assert(!touch_camera_apply(0,0,0,0,.1f,epoch,size));
        put(camera+0x134,NAN);assert(!touch_camera_apply(.1f,0,0,0, 0,epoch,size));
    }
    ui_command_kind=pick_pending=0;
    assert(s2_touch_pan(.2f,.3f,.24f,.35f));assert(s2_touch_pan(.24f,.35f,.3f,.4f));
    assert(fabsf(camera_request.dx-.1f)<.00001f && fabsf(camera_request.dy-.1f)<.00001f);
    assert(!s2_touch_pan(NAN,.3f,.4f,.4f) && !s2_touch_pan(.2f,-.1f,.4f,.4f));
    assert(!s2_touch_pan(.2f,.3f,1.1f,.4f) && !s2_touch_pan(.2f,.3f,.2f,.3f));
    ui_command_kind=pick_pending=0;
    win_manager = 0x11000;win_manager_thread = void_thread_id();
    MEM32(win_manager + 0x44) = 0x12000;MEM32(0x12000) = 0x18000;
    MEM32(0x180BC) = 0x12345678u;
    MEM32(0x130A0) = 0x14000;MEM32(0x14000) = 0x14100;
    MEM32(0x14100) = 0x14000;MEM32(0x14108) = 0x16000;
    MEM32(0x16000) = 0x19000;MEM32(0x18FFC) = 0x1A000;MEM32(0x1A00C) = 0x1B000;
    strcpy(GPTR(0x1B008), ".?AVcTSWinProcSimGrid@@");
    MEM32(0x19068) = 0xA07479F0u;MEM32(0x16054) = 0x13000;
    MEM32(0x16020) = 3;MEM32(0x16024) = 12;MEM32(0x16028) = 2;
    context = S2_TOUCH_CAS;
    assert(touch_widget(0x13000) & 32);
    assert(touch_catalog_apply(700,500,1,0,context,size,epoch) && pages==1 && MEM32(0x16020)==2);
    assert(touch_catalog_apply(700,500,-2,0,context,size,epoch) && pages==2 && MEM32(0x16020)==4);
    assert(touch_catalog_apply(700,500,8,0,context,size,epoch) && pages==3 && !MEM32(0x16020));
    assert(touch_catalog_apply(700,500,1,0,context,size,epoch) && pages==3);
    picker_reenter=1;assert(!touch_catalog_apply(700,500,-1,0,S2_TOUCH_CAS,size,epoch) && pages==3);
    picker_reenter=0;context=S2_TOUCH_CAS;
    MEM32(0x19068)=0;assert(!touch_catalog_apply(700,500,-1,0,context,size,epoch) && pages==3);
    MEM32(0x19068)=0xA07479F0u;
    pick_pending=0;ui_command_kind=0;
    assert(s2_touch_scroll(700,500,1,context,size,epoch));
    assert(s2_touch_scroll(700,500,-1,context,size,epoch));
    assert(catalog_request.amount==0 && !catalog_request.info);
    assert(!s2_touch_scroll(700,500,0,context,size,epoch) && !s2_touch_scroll(700,500,9,context,size,epoch));
    assert(!s2_touch_scroll(700,500,1,context,size,epoch+1));
    puts("catalog PASS: exact shared Grid2 setter/subclass, rehit/owner/epoch/type guards, page clamp, no writes on reentrant modal, coalesced opposite swipes never request info");
    puts("tilt PASS: native pitch controller endpoints, zero-delta identity, near/far planes untouched, limits/reversal without overscroll deadzone, invalid requests, coalesced queue");
    puts("camera PASS: direct pan/reverse/rotation, pure zoom/twist retain XYZ without terrain reposition in Town/Live/Buy/Build, equal current+target, owner/layout/epoch/modal/type gates, finite bounds, coalesced queue, busy exclusion");
    return 0;
}
