"""Execute production JNI pointer boundaries and coordinate mapping on the host.

Camera queue/owner timing is covered by check_camera_queue.py. This check
protects the separate JNI boundary: rejected input and releases retain camera
intent, while a genuine pointer action clears it before input is submitted.
"""
import ctypes as C
import contextlib
import hashlib
import json
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function


PRELUDE = r'''
typedef unsigned int uint32_t;
typedef void JNIEnv;
typedef void *jobject;
typedef int jint;
typedef float jfloat;
typedef unsigned char jboolean;
#define JNIEXPORT
#define JNICALL
#define API __declspec(dllexport)
#define isfinite(x) __builtin_isfinite(x)
int _fltused;
#define atomic_store(p,v) __c11_atomic_store((p),(v),5)
#define atomic_load(p) __c11_atomic_load((p),5)
static _Atomic unsigned camera_navigation_intent;
static int window_lock, fixture_w, fixture_h, fixture_sw, fixture_sh;
static void *window;
static uint32_t cursor_x,cursor_y;
static int calls,kind,first,second,intent_at_input;
static void pthread_mutex_lock(int *p){(void)p;}
static void pthread_mutex_unlock(int *p){(void)p;}
static int ANativeWindow_getWidth(void *w){(void)w;return fixture_sw;}
static int ANativeWindow_getHeight(void *w){(void)w;return fixture_sh;}
static void void_input_screen_size(int *w,int *h){*w=fixture_w;*h=fixture_h;}
static float fmaxf(float a,float b){return a>b?a:b;}
static float fminf(float a,float b){return a<b?a:b;}
static long lroundf(float x){return(long)(x>=0?x+.5f:x-.5f);}
static void record(int k,int a,int b){++calls;kind=k;first=a;second=b;intent_at_input=atomic_load(&camera_navigation_intent);}
static void void_input_cursor(uint32_t *x,uint32_t *y){*x=cursor_x;*y=cursor_y;}
static void void_input_mouse_move(int x,int y){record(1,x,y);}
static void void_input_mouse_warp(int x,int y){record(2,x,y);}
static void void_input_mouse_button(int b,int d){record(3,b,d);}
static void void_input_mouse_wheel(int d){record(4,d,0);}
'''

WRAPPERS = r'''
API void prepare(int w,int h,int sw,int sh,unsigned x,unsigned y,int has_window){
 fixture_w=w;fixture_h=h;fixture_sw=sw;fixture_sh=sh;cursor_x=x;cursor_y=y;
 window=has_window?(void *)1:0;calls=kind=first=second=0;intent_at_input=-1;
 atomic_store(&camera_navigation_intent,1);
}
API void move(float x,float y,int relative){Java_org_sims2_port_GameActivity_nativeMouseMove(0,0,x,y,(jboolean)relative);}
API void warp(float x,float y){Java_org_sims2_port_GameActivity_nativeMouseWarp(0,0,x,y);}
API void button(int b,int down){Java_org_sims2_port_GameActivity_nativeMouseButton(0,0,b,(jboolean)down);}
API void wheel(int delta){Java_org_sims2_port_GameActivity_nativeMouseWheel(0,0,delta);}
API int state(int n){switch(n){case 0:return atomic_load(&camera_navigation_intent);case 1:return calls;case 2:return kind;case 3:return first;case 4:return second;default:return intent_at_input;}}
'''


def main():
    jni_path = ROOT / 'android/host/android_main.c'
    touch_path = ROOT / 'android/host/sims2_touch.c'
    jni = jni_path.read_text(encoding='utf-8')
    touch = touch_path.read_text(encoding='utf-8')
    # The atomic setter is intentionally a one-line production definition.
    signature = 'void s2_touch_camera_pointer_intent(void)'
    at = touch.index(signature)
    setter = touch[at:touch.index('\n', at)] + '\n'
    names = ('nativeMouseMove', 'nativeMouseWarp', 'nativeMouseButton', 'nativeMouseWheel')
    code = PRELUDE + setter + function(jni, 'static int view_to_game(')
    for name in names:
        code += function(jni, 'JNIEXPORT void JNICALL Java_org_sims2_port_GameActivity_' + name + '(')
    code += WRAPPERS
    cases = []
    with tempfile.TemporaryDirectory(prefix='sims2-camera-pointer-') as directory, contextlib.ExitStack() as cleanup:
        base = Path(directory)
        source, obj, library = base/'check.c', base/'check.obj', base/'check.dll'
        source.write_text(code, encoding='utf-8')
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-std=c11', '-O2',
                        '-fno-builtin', '-fno-stack-protector', '-c', str(source), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:'+str(library), str(obj)], check=True)
        dll = C.CDLL(str(library))
        C.windll.kernel32.FreeLibrary.argtypes = [C.c_void_p]
        cleanup.callback(C.windll.kernel32.FreeLibrary, dll._handle)
        dll.prepare.argtypes = [C.c_int]*4 + [C.c_uint]*2 + [C.c_int]
        dll.move.argtypes = [C.c_float, C.c_float, C.c_int]
        dll.warp.argtypes = [C.c_float, C.c_float]
        dll.button.argtypes = [C.c_int, C.c_int]
        dll.wheel.argtypes = [C.c_int]
        dll.state.argtypes = [C.c_int]
        dll.state.restype = C.c_int

        def setup(w=1200, h=600, sw=2400, sh=1080, x=600, y=300, window=1):
            dll.prepare(w,h,sw,sh,x,y,window)

        def check(label, clear, submitted=True, expected=None):
            actual = [dll.state(i) for i in range(6)]
            assert actual[0] == (0 if clear else 1), (label, actual)
            assert actual[1] == int(submitted), (label, actual)
            if submitted:
                assert actual[5] == actual[0], ('intent must change before input', label, actual)
            if expected is not None:
                assert actual[2:5] == list(expected), (label, expected, actual)
            cases.append(label)

        # Absolute input expresses pointer intent even at its unchanged position.
        for api in ('move', 'warp'):
            def point(x,y):
                dll.move(x,y,0) if api == 'move' else dll.warp(x,y)
            setup();point(.5,.5)
            check(api+' unchanged absolute position', True, expected=(1,0,0) if api=='move' else (2,600,300))
            setup(x=0,y=0);point(0,0)
            check(api+' absolute origin', True, expected=(1,0,0) if api=='move' else (2,0,0))
            setup();point(-2,3)
            check(api+' finite outside view clamps to edge', True, expected=(1,-600,299) if api=='move' else (2,0,599))
            setup(window=0);point(.5,.5)
            check(api+' without Android window uses game dimensions', True)
            for x,y in ((float('nan'),.5),(.5,float('nan')),(float('inf'),.5),(.5,-float('inf'))):
                setup();point(x,y);check(api+' rejects nonfinite '+repr((x,y)), False, False)
            for dims in ((0,600,2400,1080),(1200,0,2400,1080),(1200,600,0,1080),(1200,600,2400,0)):
                setup(*dims);point(.5,.5);check(api+' rejects dimensions '+repr(dims), False, False)

        for x,y in ((0,0),(.9,-.9),(-.001,.001)):
            setup();dll.move(x,y,1);check('relative truncated zero '+repr((x,y)), False, expected=(1,0,0))
        for x,y,dx,dy in ((1.9,-2.9,1,-2),(0,1,0,1),(-1,0,-1,0),
                          (2147483520,0,2147483520,0),(-2147483648,0,-2147483648,0)):
            setup();dll.move(x,y,1);check('relative accepted '+repr((x,y)), True, expected=(1,dx,dy))
        for x,y in ((2147483648,0),(-2147483904,0),(0,2147483648),(0,-2147483904),
                    (float('nan'),1),(1,float('nan')),(float('inf'),0),(0,-float('inf'))):
            setup();dll.move(x,y,1);check('relative rejected '+repr((x,y)), False, False)
        for b in range(-1,9):
            setup();dll.button(b,1);check('button DOWN '+str(b), 0<=b<=7, expected=(3,b,1))
            setup();dll.button(b,0);check('button UP '+str(b), False, expected=(3,b,0))
        for delta in (0,1,-1,120,-120,-2147483648,2147483647):
            setup();dll.wheel(delta);check('wheel '+str(delta), delta!=0, expected=(4,delta,0))
    result = {'pass':True, 'cases':len(cases),
              'production_sha256':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in (jni_path,touch_path)},
              'scope':'Actual production JNI functions, coordinate mapping and atomic intent setter compiled for host x64. Input queue is a recording boundary; actual ARM64 queue and owner timing are checked in check_camera_queue.py.',
              'checks':cases}
    out = ROOT/'build/test/camera-pointer-intent'
    out.mkdir(parents=True, exist_ok=True)
    (out/'results.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k!='checks'},indent=2))


if __name__ == '__main__':
    main()
