"""Check explicit APK libraries and the actual C profiling startup, without adb."""
import contextlib
import ctypes
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from unittest.mock import patch
import zipfile

import build_game_apk as apk
from build_android import CLANG, ROOT


def check_package(base):
    default = base / 'build/arm64/libsims2.so'
    default.parent.mkdir(parents=True)
    default.write_bytes(b'production')
    chosen = base / 'instrumented.so'
    chosen.write_bytes(b'profile')
    key = base / 'build/android/debug.keystore'
    key.parent.mkdir(parents=True)
    key.touch()
    calls = []
    launcher = apk.launcher_root()
    sys.path.insert(0, str(launcher / 'tools'))
    import launcher as port_launcher
    (base / 'build/dxvk/runtime').mkdir(parents=True)
    (base / 'build/dxvk/licenses').mkdir()
    (base / 'build/dxvk/runtime/libs2dxvk.so').write_bytes(b'bridge')
    (base / 'build/arm64-texconv').mkdir()
    (base / 'build/arm64-texconv/libs2texconv.so').write_bytes(b'texconv')
    (base / 'build/fsr3-android-sdk').mkdir()
    (base / 'build/fsr3-android-sdk/libs2fsr3.so').write_bytes(b'fsr3')
    (base / 'android/host').mkdir(parents=True)
    (base / 'android/host/fsr1.h').write_text('FSR1 fixture license')
    (base / 'reference/fsr3-android/sdk').mkdir(parents=True)
    (base / 'reference/fsr3-android/sdk/LICENSE.txt').write_text('FSR3 fixture license')
    for encoder in ('astcenc', 'etcpak'):
        folder = base / 'reference' / encoder
        folder.mkdir(parents=True)
        (folder / 'LICENSE.txt').write_text('fixture license')
    for name, license_name in (('pcrecomp', 'LICENSE'), ('mojoshader', 'LICENSE.txt')):
        folder = base / 'reference' / name
        folder.mkdir(parents=True)
        (folder / license_name).write_text('fixture license')

    def prepare(spec, manifest, out):
        assets = out / 'assets'
        assets.mkdir(parents=True)
        return dict(assets=str(assets), manifest=str(manifest), sources=[], package='org.sims2.port.launcher')

    def probe(out, package, ndk, api):
        assert Path(ndk).is_dir()
        assert api == 29
        out.mkdir(parents=True)
        library = out / 'libportprobe.so'
        library.write_bytes(b'probe')
        return library

    def run(*args, **kwargs):
        calls.append(args)
        tool = Path(args[0]).name
        if tool == 'aapt2.exe':
            with zipfile.ZipFile(args[args.index('-o') + 1], 'w'):
                pass
        elif tool == 'd8.bat':
            assert str(args[-1]).startswith('@') and Path(str(args[-1])[1:]).is_file()
            (Path(args[args.index('--output') + 1]) / 'classes.dex').write_bytes(b'dex')
        elif tool == 'zipalign.exe':
            assert args[1:5] == ('-f', '-P', '16', '4'), args
            shutil.copyfile(args[-2], args[-1])
        elif tool == 'apksigner.bat':
            shutil.copyfile(args[-1], args[args.index('--out') + 1])

    with patch.object(apk, 'ROOT', base), patch.object(apk, 'OUT', base / 'build/android-game'), \
            patch.object(apk, 'run', run), patch.object(apk, 'launcher_root', lambda: launcher), \
            patch.object(port_launcher, 'prepare', prepare), patch.object(port_launcher, 'build_probe', probe), \
            patch.object(apk.subprocess, 'run') as native:
        for args, expected in [([], b'production'), (['--library', str(chosen)], b'profile'),
                               (['--skip-native', '--library', str(chosen)], b'profile'),
                               (['--skip-native', '--install', '--serial', 'TEST_SERIAL'], b'production')]:
            native.reset_mock()
            with patch.object(sys, 'argv', ['build_game_apk.py', *args]):
                apk.main()
            assert native.call_count == 4 + int(not args) + int('--install' in args), args
            with zipfile.ZipFile(apk.OUT / 'sims2.apk') as archive:
                assert archive.read('lib/arm64-v8a/libsims2.so') == expected
                assert archive.read('lib/arm64-v8a/libs2fsr3.so') == b'fsr3'
                assert (apk.OUT / 'assets/licenses/fidelityfx-fsr3.txt').read_bytes() == b'FSR3 fixture license'
            if '--install' in args:
                stop = next(i for i,c in enumerate(calls) if c[-3:] == ('am', 'force-stop', apk.PACKAGE))
                install = next(i for i,c in enumerate(calls) if 'install' in c)
                assert stop < install
        calls.clear()
        with patch.object(sys, 'argv', ['build_game_apk.py', '--java-only', '--push-lib', '--serial', 'TEST_SERIAL', '--library', str(chosen)]):
            apk.main()
        with zipfile.ZipFile(apk.OUT / 'sims2.apk') as archive:
            assert 'lib/arm64-v8a/libsims2.so' not in archive.namelist()
        push = next(c for c in calls if 'push' in c)
        assert push[-2:] == (chosen, '/data/local/tmp/sims2/libsims2.so'), push
        assert 'files/libsims2.so.new' in calls[-1][-1], calls[-1]
        assert 'chmod 444 files/libsims2.so.new' in calls[-1][-1], calls[-1]
        marker = apk.OUT / 'keep'
        marker.touch()
        for flag in ('--install', '--push-lib'):
            with patch.dict(apk.os.environ, {'S2_SERIAL': ''}), \
                    patch.object(sys, 'argv', ['build_game_apk.py', flag]), contextlib.redirect_stderr(io.StringIO()):
                try:
                    apk.main()
                except SystemExit as error:
                    assert error.code == 2
                else:
                    raise AssertionError('device operation accepted without a serial')
            assert marker.exists(), 'missing serial removed previous APK output'
        with patch.object(sys, 'argv', ['build_game_apk.py', '--library', str(base / 'missing.so')]), \
                contextlib.redirect_stderr(io.StringIO()):
            try:
                apk.main()
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError('missing library accepted')
        assert marker.exists(), 'invalid library removed previous APK output'
        key.unlink()
        calls.clear()
        native.reset_mock()
        with patch.object(sys, 'argv', ['build_game_apk.py']), \
                contextlib.redirect_stderr(io.StringIO()):
            try:
                apk.main()
            except SystemExit as error:
                assert error.code == 2
            else:
                raise AssertionError('missing signing key accepted')
        assert marker.exists() and not key.exists() and not calls, 'missing key changed build output'
        native.assert_not_called()


def check_startup(base):
    source = (ROOT / 'android/host/android_main.c').read_text()
    assert 'extern void __llvm_profile_set_filename(const char *) __attribute__((weak));' in source
    start = source.index('static void *game_main(')
    function = source[start:source.index('\n}\n', start) + 3]
    harness = r'''
#define NULL ((void *)0)
typedef unsigned long long size_t;
typedef unsigned int uint32_t;
static char game_dir[512], data_dir[512], images_dir[512];
static const char *environment, *recorded;
static int called, started, exited, failure;
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
static char *getenv(const char *key) {
    if (!equal(key, "LLVM_PROFILE_FILE")) failure |= 1;
    return (char *)environment;
}
static int snprintf(char *out, size_t n, const char *fmt, const char *data) {
    if (!equal(fmt, "%s/sims2.profraw")) failure |= 2;
    size_t count = 0;
    while (*data && count + 1 < n) out[count++] = *data++;
    const char *suffix = "/sims2.profraw";
    while (*suffix && count + 1 < n) out[count++] = *suffix++;
    out[count] = 0;
    return (int)count;
}
static void record(const char *path) { recorded = path; ++called; }
static void (*__llvm_profile_set_filename)(const char *);
static int void_game_main(const char *game, const char *data, const char *images, int test) {
    if (game != game_dir || data != data_dir || images != images_dir || test) failure |= 4;
    started = called;
    return 7;
}
static void void_log(const char *fmt, ...) { (void)fmt; }
static void void_process_exit(unsigned int code, int detach) { exited = code == 7 && !detach; }
''' + function + r'''
__declspec(dllexport) unsigned int check(unsigned int mode) {
    called = started = exited = failure = 0;
    recorded = NULL;
    environment = mode == 2 ? "chosen.profraw" : mode == 3 ? "" : NULL;
    __llvm_profile_set_filename = mode == 0 ? NULL : record;
    const char *normal = "/sdcard/Sims2/userdata";
    unsigned int i = 0;
    if (mode == 4) { for (; i < sizeof data_dir - 1; ++i) data_dir[i] = 'x'; }
    else { for (; normal[i]; ++i) data_dir[i] = normal[i]; }
    data_dir[i] = 0;
    if (game_main(NULL) != NULL || !exited) failure |= 8;
    int expected = mode != 0 && mode != 2;
    if (called != expected || started != expected) failure |= 16;
    if (expected) {
        for (i = 0; data_dir[i]; ++i) if (recorded[i] != data_dir[i]) failure |= 32;
        if (!equal(recorded + i, "/sims2.profraw")) failure |= 64;
    }
    return (unsigned int)failure;
}
'''
    code, obj, library = base / 'startup.c', base / 'startup.obj', base / 'startup.dll'
    code.write_text(harness)
    subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                    '-fno-stack-protector', '-c', str(code), '-o', str(obj)], check=True)
    subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                    '/out:' + str(library), str(obj)], check=True)
    dll = ctypes.CDLL(str(library))
    dll.check.argtypes = [ctypes.c_uint]
    dll.check.restype = ctypes.c_uint
    try:
        for mode in range(5):
            assert dll.check(mode) == 0, mode
    finally:
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(dll._handle)


def main():
    with tempfile.TemporaryDirectory(prefix='sims2-apk-profile-') as directory:
        base = Path(directory)
        check_package(base)
        check_startup(base)
    print('APK profile: explicit/default packaging, canonical push name, missing-file guard and actual C startup PASS')


if __name__ == '__main__':
    main()
