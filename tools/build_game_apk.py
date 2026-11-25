"""Package the game library into an APK (no Gradle) and optionally install it.

    python tools/build_game_apk.py [--abi both|arm64|arm32] [--install] [--serial SERIAL]
                                   [--skip-native] [--java-only] [--push-lib] [--library PATH]

The default APK supports arm64-v8a and armeabi-v7a. Select one ABI for
development builds; --library and --push-lib default to arm64.

The library comes from tools/build_android.py --shared (or --library PATH,
which skips the native build). Port resources and the two pinned PE images
from the PC installation ship in the APK. The game data and writable profile
live under /sdcard/Sims2 on the device.
--java-only packs no library: the activity then loads the one pushed into its
own files by --push-lib (run-as, debuggable APK), which keeps iterations on
the touch layer to a small install. (The Void port's tool, adapted.)

The library travels as xz in assets/native (LZMA2 behind the ARM64 branch
filter, one stream per 128 MiB chunk): 36 MB instead of 69 MB deflated in lib/.
The first start of a build unpacks it into the app's files with libs2xz.so
(android/unpack/unpack.c, XZ Embedded; NativeLibrary.java).
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_SDK", str(Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk")))
TOOLS = SDK / "build-tools/36.1.0"
ANDROID_JAR = SDK / "platforms/android-37.0/android.jar"
JAVA_HOME = Path(os.environ.get("JAVA_HOME", r"C:\Portable\jdk-24.0.1"))
OUT = ROOT / "build/android-game"
ADB = SDK / "platform-tools/adb.exe"
PACKAGE = "org.sims2.port"


def launcher_root():
    for folder in (Path(os.environ.get("PORT_LAUNCHER", ROOT.parent / "launcher")), ROOT / "android/launcher"):
        if (folder / "tools/launcher.py").is_file():
            return folder
    raise SystemExit("Launcher sources not found: set PORT_LAUNCHER or supply android/launcher")


def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kwargs)
    if result.returncode:
        print(result.stdout[-3000:], result.stderr[-3000:])
        raise SystemExit(f"{Path(str(args[0])).name} failed ({result.returncode})")
    return result


XZ_EMBEDDED = ROOT / "reference/xz-embedded"   # dependencies.json (0BSD)
NATIVE_XZ = ROOT / "build/native-xz"
CHUNK = 128 << 20   # NativeLibrary.java / android/unpack/unpack.c take it from assets/native/index.txt
NDK = Path(os.environ.get("ANDROID_NDK", str(SDK / "ndk/27.2.12479018")))


def xz_tool():
    """XZ Utils 5.4+ (the ARM64 branch filter; Python's lzma module lacks it):
    $XZ, the PATH, else Git for Windows' copy."""
    for candidate in (os.environ.get("XZ"), shutil.which("xz"), r"C:\Program Files\Git\mingw64\bin\xz.exe"):
        if candidate and Path(candidate).is_file():
            version = run(candidate, "--robot", "--version").stdout
            number = int(next(line for line in version.splitlines() if line.startswith("XZ_VERSION=")).split("=")[1])
            if number >= 50040002:
                return candidate
    raise SystemExit("xz 5.4 or newer is needed (XZ Utils; Git for Windows has one in mingw64/bin)")


def compress_library(library, bcj="arm64"):
    """XZ with an ARM branch filter and word-aligned LZMA literals (lc=0, lp=2), a CHUNK-sized
    dictionary and one independent stream per CHUNK, which the device unpacks in
    parallel straight into the mapped library file. Cached by content.
    bcj is the xz filter name: arm64, or armthumb for armeabi-v7a.
    Returns (path, sha256, unpacked size, comma-separated stream sizes)."""
    data = library.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    cache = NATIVE_XZ if bcj == "arm64" else NATIVE_XZ / bcj
    out = cache / f"{library.name}.{digest[:16]}.{CHUNK >> 20}m-lp2.xz"
    table = out.with_suffix(".sizes")
    if not (out.is_file() and table.is_file()):
        cache.mkdir(parents=True, exist_ok=True)
        for old in cache.glob(f"{library.name}.*"):
            old.unlink()
        tool = xz_tool()

        def pack(at):
            return subprocess.run([tool, "--stdout", "--" + bcj, f"--lzma2=preset=9e,dict={CHUNK},lc=0,lp=2,pb=2",
                                   "--check=crc32", "--threads=1"],
                                  input=data[at:at + CHUNK], capture_output=True, check=True).stdout

        with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as pool:
            streams = list(pool.map(pack, range(0, len(data), CHUNK)))
        part = out.with_suffix(".part")
        part.write_bytes(b"".join(streams))
        table.write_text(",".join(str(len(s)) for s in streams))
        part.replace(out)
    return out, digest, len(data), table.read_text()


def build_unpacker(abi="arm64"):
    """libs2xz.so: XZ Embedded and its JNI entry (android/unpack/unpack.c), which
    unpacks the engine from assets/native on the first start of a build.
    On arm32 its constructor also holds the PE image addresses."""
    if not (XZ_EMBEDDED / "linux/include/linux/xz.h").is_file():
        raise SystemExit("reference/xz-embedded is missing: git submodule update --init reference/xz-embedded")
    out = OUT / abi / "libs2xz.so"
    out.parent.mkdir(exist_ok=True)
    xz = XZ_EMBEDDED / "linux/lib/xz"
    if abi == "arm32":
        target, define, page = "armv7a-linux-androideabi26", "XZ_DEC_ARMTHUMB", "4096"
    else:
        target, define, page = "aarch64-linux-android26", "XZ_DEC_ARM64", "16384"
    run(NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe", "--target=" + target, "-O2",
        "-std=gnu11", "-fPIC", "-shared", "-Wall", "-Wextra", "-D" + define, "-DXZ_USE_CRC64", "-DXZ_DEC_SINGLE",
        "-I" + str(XZ_EMBEDDED / "linux/include/linux"), "-I" + str(XZ_EMBEDDED / "userspace"),
        "-ffile-prefix-map=" + str(ROOT) + "=.",
        ROOT / "android/unpack/unpack.c", *(xz / name for name in ("xz_crc32.c", "xz_crc64.c", "xz_dec_stream.c",
                                                                  "xz_dec_lzma2.c", "xz_dec_bcj.c")),
        f"-Wl,-z,max-page-size={page}", "-Wl,--build-id=sha1", "-Wl,-s", "-pthread", "-landroid", "-llog", "-o", out)
    return out


def glslang_notice():
    """The distinct leading license comments of the glslang sources tools/build_android.py links."""
    sys.path.insert(0, str(ROOT / "tools"))
    import build_android
    seen, parts = set(), ["glslang (Khronos Group), compiled from the Android NDK's shaderc sources.\n"
                          "The license notices of the source files in libsims2.so follow.\n"]
    for name in build_android.GLSLANG_SOURCES:
        text = (build_android.GLSLANG / name).read_text(encoding="utf-8", errors="replace")
        lines = []
        for line in text.splitlines():
            if line.startswith("//") or line.startswith("/*") or line.startswith(" *") or line.startswith("*") or not line.strip():
                lines.append(line)
                if line.strip().endswith("*/") and not line.startswith("//"):
                    break
                continue
            break
        block = "\n".join(lines).strip()
        if block and "Copyright" in block and block not in seen:
            seen.add(block)
            parts.append(f"--- {name} (and the files with the same notice)\n{block}\n")
    return "\n".join(parts)


# The app-local Vulkan drivers of the native Vulkan renderer (port.json modes "vulkan-turnip" and
# "vulkan-panvk", android/render/s2r_vkdriver.c): adrenotools with its hooks and libc++_shared, Mesa
# Turnip, Mesa PanVK. Each library must match its pin in dependencies.json. PanVK is the shared
# reference of all ports, ../launcher/drivers/panvk (S2_PANVK_DIR overrides), whose MANIFEST.json must
# name the same hash; the others come from build/vulkan-runtime (or S2_VULKAN_RUNTIME), staged from the
# ut3 port's Turnip runtime.
VULKAN_RUNTIME = Path(os.environ.get("S2_VULKAN_RUNTIME", ROOT / "build/vulkan-runtime"))
VULKAN_PINS = ("adrenotools", "turnip", "panvk", "libcxx_shared")
VULKAN_LICENSES = {"adrenotools.txt": ("adrenotools", "adrenotools.BSD-2-Clause.txt"),
                   "linkernsbypass.txt": ("adrenotools", "linkernsbypass.BSD-2-Clause.txt"),
                   "turnip-mesa.txt": ("turnip", "Turnip-LICENSE-NOTE.txt"),
                   "PanVK-NOTICE.txt": ("panvk", "NOTICE.txt"),
                   "libcxx-ndk.txt": ("libcxx_shared", "NOTICE")}


def panvk_reference():
    """The shared PanVK folder (launcher/drivers/panvk), or None when this checkout has none."""
    folder = Path(os.environ.get("S2_PANVK_DIR", ROOT.parent / "launcher/drivers/panvk"))
    return folder if (folder / "MANIFEST.json").is_file() else None


def vulkan_drivers():
    """[(file, packaged name)] of the driver libraries and [(file, license name)], checked against the pins."""
    import json
    pins = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))
    reference = panvk_reference()
    if reference:
        manifest = json.loads((reference / "MANIFEST.json").read_text(encoding="utf-8"))
        for name, digest in pins["panvk"]["runtime"].items():
            if manifest["library"]["file"] != name or manifest["library"]["sha256"] != digest:
                raise SystemExit(f"{reference / 'MANIFEST.json'}: names {manifest['library']['sha256'][:12]}..., "
                                 f"dependencies.json pins {digest[:12]}... for {name}")
    libraries, licenses = [], []
    for component in VULKAN_PINS:
        for name, digest in pins[component]["runtime"].items():
            path = (reference if component == "panvk" and reference else VULKAN_RUNTIME) / name
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise SystemExit(f"{path}: missing or not the pinned {component} library ({digest[:12]}...)")
            libraries.append((path, name))
    for name, (component, pinned) in VULKAN_LICENSES.items():
        path = reference / pinned if component == "panvk" and reference else VULKAN_RUNTIME / "licenses" / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != pins[component]["licenses"][pinned]:
            raise SystemExit(f"{path}: missing or not the pinned {component} notice")
        licenses.append((path, name))
    return libraries, licenses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi", choices=("both", "arm64", "arm32"))
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--serial", default=os.environ.get("S2_SERIAL"))
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--java-only", action="store_true", help="no native library in the APK (see --push-lib)")
    parser.add_argument("--push-lib", action="store_true",
                        help="copy the built library into the installed app's files (development)")
    parser.add_argument("--library", type=Path,
                        help="package/push this existing library instead of building the default one")
    args = parser.parse_args()
    abi = args.abi or ("arm64" if args.library or args.push_lib else "both")
    if abi == "both" and (args.library or args.push_lib):
        parser.error("--library and --push-lib require a single ABI")
    abis = ("arm64", "arm32") if abi == "both" else (abi,)
    if (args.install or args.push_lib) and not args.serial:
        parser.error("--install/--push-lib requires --serial or S2_SERIAL")
    keystore = ROOT / "build/android/debug.keystore"
    if not keystore.is_file():
        parser.error("Signing key missing: restore build/android/debug.keystore; a replacement cannot update installed APKs")
    if args.java_only or args.library:
        args.skip_native = True
    libraries = {target: args.library.resolve() if args.library else ROOT / "build" / target / "libsims2.so"
                 for target in abis}
    if not args.skip_native:
        for target in abis:
            subprocess.run([sys.executable, str(ROOT / "tools/build_android.py"), "--shared", "--jobs", "12",
                            "--abi", target], check=True)
    if not args.java_only or args.push_lib:
        for library in libraries.values():
            if not library.is_file():
                parser.error(f"library does not exist: {library}")
    if OUT.exists():
        shutil.rmtree(OUT)
    (OUT / "classes").mkdir(parents=True)
    (OUT / "dex").mkdir()
    (OUT / "generated").mkdir()
    sys.path.insert(0, str(launcher_root() / "tools"))
    import launcher as port_launcher
    prepared = port_launcher.prepare(ROOT / "android/port.json", ROOT / "android/app/AndroidManifest.xml", OUT / "launcher")
    assets = OUT / "assets"
    shutil.copytree(prepared["assets"], assets)
    subprocess.run([sys.executable, str(ROOT / "tools/build_overlay.py")], check=True)
    # The native renderers' shader library (tools/build_shaderlib.py; none without captured shaders:
    # the renderers then translate every shader on the device, as they do for mods).
    subprocess.run([sys.executable, str(ROOT / "tools/build_shaderlib.py"), "build"], check=True)
    shader_library = ROOT / "generated/shaders/s2r_shaderlib.bin"
    runtime = assets / "runtime.zip"
    with zipfile.ZipFile(runtime, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted((ROOT / "build/overlay").rglob("*")):
            if path.is_file():
                archive.write(path, "overlay/" + path.relative_to(ROOT / "build/overlay").as_posix())
        if shader_library.is_file():
            archive.write(shader_library, "shaders/s2r_shaderlib.bin")
    (assets / "runtime.version").write_text(hashlib.sha256(runtime.read_bytes()).hexdigest(), encoding="ascii")
    (assets / "licenses").mkdir(exist_ok=True)
    shutil.copyfile(ROOT / "reference/pcrecomp/LICENSE", assets / "licenses/pcrecomp.txt")
    shutil.copyfile(ROOT / "reference/mojoshader/LICENSE.txt", assets / "licenses/mojoshader.txt")
    # glslang (the native Vulkan renderer's device translations), from the pinned NDK's shaderc sources,
    # which carry no LICENSE file: the license headers of the files the build compiles, each once.
    (assets / "licenses/glslang.txt").write_text(glslang_notice(), encoding="utf-8", newline="\n")
    shutil.copyfile(ROOT / "android/host/fsr1.h", assets / "licenses/fidelityfx-fsr1.txt")
    # The launcher's texture converter (port.json "textures": TextureCache). arm32 is ETC2 only.
    probes = {}
    ndk = os.environ.get("ANDROID_NDK", str(NDK))
    for target in abis:
        subprocess.run([sys.executable, str(ROOT / "tools/build_texconv.py"), "--abi", target], check=True)
        probes[target] = port_launcher.build_probe(OUT / "probe" / target, prepared["package"], ndk=ndk, api=26, abi=target)
    shutil.copyfile(ROOT / "reference/astcenc/LICENSE.txt", assets / "licenses/astcenc.txt")
    shutil.copyfile(ROOT / "reference/etcpak/LICENSE.txt", assets / "licenses/etcpak.txt")
    shutil.copyfile(XZ_EMBEDDED / "COPYING", assets / "licenses/xz-embedded.txt")
    if "arm64" in abis:
        drivers, driver_licenses = vulkan_drivers()
        for path, name in driver_licenses:
            shutil.copyfile(path, assets / "licenses" / name)
    else:
        drivers = []
    run(TOOLS / "aapt2.exe", "compile", "--dir", ROOT / "android/app/res", "-o", OUT / "resources.zip")
    run(TOOLS / "aapt2.exe", "link", "-I", ANDROID_JAR, "--manifest", prepared["manifest"],
        "--java", OUT / "generated", "-A", assets, "-o", OUT / "base.apk", OUT / "resources.zip")
    sources = sorted((ROOT / "android/app/src").rglob("*.java")) + [Path(p) for p in prepared["sources"]]
    sources += sorted((OUT / "generated").rglob("*.java"))
    run(JAVA_HOME / "bin/javac.exe", "--release", "8", "-cp", ANDROID_JAR, "-d", OUT / "classes", *sources)
    classes = sorted((OUT / "classes").rglob("*.class"))
    # cmd.exe limits d8.bat to 8191 characters; the launcher's class list exceeds it.
    inputs = OUT / "d8-inputs.txt"
    inputs.write_text("".join(f"{path}\n" for path in classes), encoding="utf-8")
    run(TOOLS / "d8.bat", "--lib", ANDROID_JAR, "--min-api", "26", "--output", OUT / "dex", "@" + str(inputs), shell=False)
    unsigned = OUT / "unsigned.apk"
    shutil.copy(OUT / "base.apk", unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(OUT / "dex/classes.dex", "classes.dex")
        for path, name in drivers:
            apk.write(path, "lib/arm64-v8a/" + name)
        for target, library in libraries.items():
            arm32 = target == "arm32"
            libdir = "lib/armeabi-v7a" if arm32 else "lib/arm64-v8a"
            native = "assets/native/armeabi-v7a" if arm32 else "assets/native"
            probe = probes[target]
            texconv = ROOT / "build" / f"{target}-texconv" / "libs2texconv.so"
            apk.write(probe, libdir + "/" + probe.name)
            apk.write(texconv, libdir + "/" + texconv.name)
            apk.write(build_unpacker(target), libdir + "/libs2xz.so")
            if not args.java_only:
                # libs2xz.so maps this stored asset directly and unpacks it in parallel.
                packed, digest, size, sizes = compress_library(library, "armthumb" if arm32 else "arm64")
                apk.write(packed, native + "/libsims2.so.xz", compress_type=zipfile.ZIP_STORED)
                apk.writestr(native + "/index.txt", f"libsims2.so {size} {digest} {CHUNK} {sizes}\n")
    aligned = OUT / "aligned.apk"
    run(TOOLS / "zipalign.exe", "-f", "-P", "16", "4", unsigned, aligned)
    apk = OUT / "sims2.apk"
    run(TOOLS / "apksigner.bat", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
        "--out", apk, aligned)
    print(f"APK: {apk} ({apk.stat().st_size / 1e6:.1f} MB)")
    if args.install:
        # Package replacement can relaunch the default profile without the QA intent.
        run(ADB, "-s", args.serial, "shell", "am", "force-stop", PACKAGE)
        run(ADB, "-s", args.serial, "install", "-r", "-g", apk)
        print("installed")
        if not args.java_only and not args.push_lib:
            subprocess.run([str(ADB), "-s", args.serial, "shell", f"run-as {PACKAGE} sh -c 'rm -f files/lib*.so'"],
                           capture_output=True)
    if args.push_lib:
        library = libraries[abi]
        run(ADB, "-s", args.serial, "push", library, "/data/local/tmp/sims2/libsims2.so")
        run(ADB, "-s", args.serial, "shell", f"run-as {PACKAGE} sh -c 'mkdir -p files && "
            "rm -f files/libsims2.so.new && cp /data/local/tmp/sims2/libsims2.so files/libsims2.so.new && chmod 444 files/libsims2.so.new && mv files/libsims2.so.new files/libsims2.so'")
        print(f"pushed {library} into the app's files/libsims2.so")


if __name__ == "__main__":
    main()
