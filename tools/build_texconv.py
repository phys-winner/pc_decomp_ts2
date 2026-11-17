"""Build libs2texconv.so, the launcher's texture converter (android/texconv), and
s2texconv, the same converter as an adb-shell program, for arm64.

It links astcenc (reference/astcenc, NEON) and etcpak (reference/etcpak), both
pinned in dependencies.json. Every invocation recompiles and relinks the
converter. tools/build_game_apk.py runs this and packages the library.
    python -X utf8 -B tools/build_texconv.py
Tests run the arm64 program on a device (tools/check_texconv_zip.py --serial SERIAL).
"""
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build_android import CLANG, TARGET  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/arm64-texconv"
LIBRARY = OUT / "libs2texconv.so"
PROGRAM = OUT / "s2texconv"
CXX = CLANG.with_name("clang++.exe")
ASTC = ["-DASTCENC_NEON=1", "-DASTCENC_SVE=0", "-DASTCENC_SSE=0", "-DASTCENC_AVX=0", "-DASTCENC_POPCNT=0", "-DASTCENC_F16C=0"]
COMMON = ["--target=" + TARGET, "-O2", "-fPIC", "-fvisibility=hidden", "-ffunction-sections", "-fdata-sections", "-std=c++17",
          "-ffile-prefix-map=" + str(ROOT) + "=."]
STRICT = ["-Wall", "-Wextra", "-Werror"]


def units():
    out = [(p, ASTC) for p in sorted((ROOT / "reference/astcenc/Source").glob("astcenc_*.cpp"))]
    out += [(ROOT / "reference/etcpak" / n, ["-DBCDEC_STATIC", "-DBCDEC_IMPLEMENTATION"])
            for n in ("ProcessRGB.cpp", "Tables.cpp", "Dither.cpp", "Decode.cpp")]
    out += [(ROOT / "android/texconv/s2texconv.cpp", ASTC + STRICT)]
    out += [(ROOT / "android/launcher/native/zipmount.c", [])]
    return out


def compile_unit(unit):
    source, flags = unit
    obj = OUT / "obj" / (source.parent.name + "_" + source.stem + ".o")
    command = [str(CLANG if source.suffix == ".c" else CXX),
               *(f for f in COMMON if source.suffix != ".c" or not f.startswith("-std=")), *flags, "-c", str(source), "-o", str(obj)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"{source.name}: {result.stdout}{result.stderr}")
    return obj


def link(objects, output, extra):
    command = [str(CXX), "--target=" + TARGET, "-fuse-ld=lld", "-static-libstdc++", "-Wl,--gc-sections",
               "-Wl,-z,max-page-size=16384", "-Wl,--build-id=sha1", *map(str, objects), *extra, "-llog", "-o", str(output)]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"link {output.name}: {result.stdout}{result.stderr}")


def main():
    from prepare_dependencies import prepare
    prepare("astcenc", "etcpak")
    (OUT / "obj").mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(max_workers=12) as pool:
        objects = list(pool.map(compile_unit, units()))
        jni, cli = pool.map(compile_unit, [(ROOT / "android/texconv/s2texconv_jni.cpp", STRICT),
                                           (ROOT / "android/texconv/s2texconv_cli.cpp", STRICT)])
    link(objects + [jni], LIBRARY, ["-shared", "-Wl,--no-undefined"])
    link(objects + [cli], PROGRAM, ["-pie"])
    print(f"texconv: {LIBRARY} ({LIBRARY.stat().st_size / 1e6:.1f} MB), {PROGRAM} ({PROGRAM.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
