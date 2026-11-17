"""Compile the actual text-input queue and check atomic commits/pump recovery."""
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/host/input.c').read_text(encoding='utf-8')
    strings = (ROOT / 'android/host/strings.c').read_text(encoding='utf-8')
    state = source[source.index('typedef struct { uint32_t msg, wparam, lparam; } pending_t;'):
                   source.index('void void_input_screen_size(')]
    prelude = r'''
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long size_t;
static uint32_t input_lock, pump_lock, lock_depth, posts_unlocked;
static void s2_touch_pump(void) {}
static void pthread_mutex_lock(uint32_t *lock) { if(lock==&input_lock)++lock_depth; }
static void pthread_mutex_unlock(uint32_t *lock) { if(lock==&input_lock)--lock_depth; }
static void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *out=dst; const uint8_t *in=src; while(n--) *out++=*in++; return dst;
}
'''
    delivery = r'''
static pending_t delivered[1024];
static uint32_t delivered_count, destination;
static uint32_t void_window_main(void) { return 123; }
static void void_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    destination=hwnd; posts_unlocked += lock_depth == 0;
    delivered[delivered_count++]=(pending_t){msg,wp,lp};
}
'''
    wrappers = r'''
__declspec(dllexport) int test_text(const uint16_t *text, uint32_t length) { return void_input_text(text,length); }
__declspec(dllexport) uint32_t test_pending(void) { return (uint32_t)pending_count; }
__declspec(dllexport) uint32_t *test_delivered(void) { return (uint32_t *)delivered; }
__declspec(dllexport) uint32_t test_count(void) { return delivered_count; }
__declspec(dllexport) uint32_t test_pump(void) {
    void_input_pump(); return destination==123 && lock_depth==0 && posts_unlocked==delivered_count;
}
__declspec(dllexport) void test_reset(void) {
    pending_count=0; delivered_count=0; posts_unlocked=0; destination=0;
}
'''
    code = (prelude + state + delivery + function(strings, 'size_t void_utf8_encode(') +
            function(source, 'static void pend(') +
            function(source, 'int void_input_text(') + function(source, 'void void_input_pump(') + wrappers)
    with tempfile.TemporaryDirectory(prefix='sims2-text-input-') as directory:
        base = Path(directory)
        cfile, obj, library = base / 'check.c', base / 'check.obj', base / 'check.dll'
        cfile.write_text(code, encoding='utf-8')
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-mno-stack-arg-probe', '-c', str(cfile), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(library), str(obj)], check=True)
        dll = ctypes.CDLL(str(library))
        dll.test_text.argtypes = [ctypes.POINTER(ctypes.c_uint16), ctypes.c_uint32]
        dll.test_delivered.restype = ctypes.POINTER(ctypes.c_uint32)
        try:
            def commit(value):
                raw = value.encode('utf-16-le', errors='surrogatepass')
                units = (ctypes.c_uint16 * (len(raw) // 2)).from_buffer_copy(raw)
                return dll.test_text(units, len(units))

            def check_delivery(expected):
                assert dll.test_pump() == 1 and dll.test_pending() == 0 and dll.test_count() == len(expected)
                delivered = dll.test_delivered()
                for i, byte in enumerate(expected):
                    assert list(delivered[i * 3:i * 3 + 3]) == [0x0102, byte, 1]

            # Empty commits close the Android editor without changing the game field.
            text = (ctypes.c_uint16 * 129)(*range(129))
            assert dll.test_text(text, 0) == 1 and dll.test_text(None, 0) == 1
            assert dll.test_pending() == 0
            for pointer, length in ((None, 1), (text, 129), (text, 0xFFFFFFFF)):
                assert dll.test_text(pointer, length) == 0 and dll.test_pending() == 0

            # UTF-16 commits become UTF-8 bytes, including composed and supplementary text.
            for value, expected in (
                    ('Маша', 'Маша'), ('姓名', '姓名'), ('e\u0301', 'e\u0301'), ('😀', '😀'),
                    ('A\ud800B\udc00C', 'A\ufffdB\ufffdC'), ('\ud800\udc00', '\U00010000'),
                    ('\0\x7f\x80\u07ff\u0800\ud7ff\ue000\uffff', '\0\x7f\x80\u07ff\u0800\ud7ff\ue000\uffff'),
                    ('\ud800' * 128, '\ufffd' * 128), ('😀' * 64, '😀' * 64)):
                dll.test_reset()
                assert commit(value) == 1
                check_delivery(expected.encode('utf-8'))

            # Worst-case 128-unit text expands to 384 bytes and exactly fills the queue with ASCII.
            dll.test_reset()
            assert commit('名' * 128) == 1 and commit('x' * 128) == 1
            assert dll.test_pending() == 512 and dll.test_count() == 0
            assert commit('x') == 0 and dll.test_pending() == 512
            check_delivery(('名' * 128 + 'x' * 128).encode('utf-8'))
            assert dll.test_pump() == 1 and dll.test_count() == 512

            # A multibyte character must never use only the remaining partial capacity.
            for value in ('é', '名', '😀'):
                dll.test_reset()
                size = len(value.encode('utf-8'))
                prefix = '名' * 128 + 'x' * (129 - size)
                assert commit('名' * 128) == 1 and commit('x' * (129 - size)) == 1
                assert commit(value) == 0 and dll.test_pending() == 513 - size
                check_delivery(prefix.encode('utf-8'))
                assert commit(value) == 1 and dll.test_pending() == size
                check_delivery((prefix + value).encode('utf-8'))
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(dll._handle)
    print('Text input actual C: empty no-op, invalid/null/oversized rejection, atomic capacity, '
          'UTF-16 to UTF-8 Cyrillic/Chinese/composition/surrogate handling, ordered WM_CHAR '
          'transport and recovery after pump PASS')


if __name__ == '__main__':
    main()
