"""Package the ARM64 game library into an APK (no Gradle) and optionally install it.

    python tools/build_game_apk.py [--install] [--serial SERIAL] [--skip-native]
                                   [--java-only] [--push-lib] [--library PATH]

The library comes from tools/build_android.py --shared (or --library PATH,
which skips the native build). Port resources and the two pinned PE images
from the PC installation ship in the APK. The game data and writable profile
live under /sdcard/Sims2 on the device.
--java-only packs no library: the activity then loads the one pushed into its
own files by --push-lib (run-as, debuggable APK), which keeps iterations on
the touch layer to a small install. (The Void port's tool, adapted.)
"""
import argparse
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
    for folder in (Path(os.environ.get("PORT_LAUNCHER", ROOT / "android/launcher")), ROOT.parent / "launcher"):
        if (folder / "tools/launcher.py").is_file():
            return folder
    raise SystemExit("Launcher sources not found: set PORT_LAUNCHER or supply android/launcher")


def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, **kwargs)
    if result.returncode:
        print(result.stdout[-3000:], result.stderr[-3000:])
        raise SystemExit(f"{Path(str(args[0])).name} failed ({result.returncode})")
    return result


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


# The app-local Vulkan drivers of the native Vulkan renderer (port.json modes "turnip" and "panvk",
# android/render/s2r_vkdriver.c): adrenotools with its hooks and libc++_shared, Mesa Turnip, Mesa PanVK.
# Each library must match its pin in dependencies.json; they come from build/vulkan-runtime (or
# S2_VULKAN_RUNTIME), staged from the ut3 port's Turnip runtime and Frostpunk's PanVK build.
VULKAN_RUNTIME = Path(os.environ.get("S2_VULKAN_RUNTIME", ROOT / "build/vulkan-runtime"))
VULKAN_PINS = ("adrenotools", "turnip", "panvk", "libcxx_shared")
VULKAN_LICENSES = {"adrenotools.txt": ("adrenotools", "adrenotools.BSD-2-Clause.txt"),
                   "linkernsbypass.txt": ("adrenotools", "linkernsbypass.BSD-2-Clause.txt"),
                   "turnip-mesa.txt": ("turnip", "Turnip-LICENSE-NOTE.txt"),
                   "PanVK-JimVulkan-NOTICE.txt": ("panvk", "PanVK-JimVulkan-NOTICE.txt"),
                   "libcxx-ndk.txt": ("libcxx_shared", "NOTICE")}


def vulkan_drivers():
    """[(file, packaged name)] of the driver libraries and [(file, license name)], checked against the pins."""
    import json
    pins = json.loads((ROOT / "dependencies.json").read_text(encoding="utf-8"))
    libraries, licenses = [], []
    for component in VULKAN_PINS:
        for name, digest in pins[component]["runtime"].items():
            path = VULKAN_RUNTIME / name
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise SystemExit(f"{path}: missing or not the pinned {component} library ({digest[:12]}...)")
            libraries.append((path, name))
    for name, (component, pinned) in VULKAN_LICENSES.items():
        path = VULKAN_RUNTIME / "licenses" / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != pins[component]["licenses"][pinned]:
            raise SystemExit(f"{path}: missing or not the pinned {component} notice")
        licenses.append((path, name))
    return libraries, licenses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--serial", default=os.environ.get("S2_SERIAL"))
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--java-only", action="store_true", help="no native library in the APK (see --push-lib)")
    parser.add_argument("--push-lib", action="store_true",
                        help="copy the built library into the installed app's files (development)")
    parser.add_argument("--library", type=Path,
                        help="package/push this existing library instead of building the default one")
    args = parser.parse_args()
    if (args.install or args.push_lib) and not args.serial:
        parser.error("--install/--push-lib requires --serial or S2_SERIAL")
    keystore = ROOT / "build/android/debug.keystore"
    if not keystore.is_file():
        parser.error("Signing key missing: restore build/android/debug.keystore; a replacement cannot update installed APKs")
    if args.java_only or args.library:
        args.skip_native = True
    if not args.skip_native:
        subprocess.run([sys.executable, str(ROOT / "tools/build_android.py"), "--shared", "--jobs", "12"], check=True)
    library = args.library.resolve() if args.library else ROOT / "build/arm64/libsims2.so"
    if (not args.java_only or args.push_lib) and not library.is_file():
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
    # The launcher's texture converter (port.json "textures": TextureCache) and its encoders' licenses.
    subprocess.run([sys.executable, str(ROOT / "tools/build_texconv.py")], check=True)
    texconv = ROOT / "build/arm64-texconv/libs2texconv.so"
    shutil.copyfile(ROOT / "reference/astcenc/LICENSE.txt", assets / "licenses/astcenc.txt")
    shutil.copyfile(ROOT / "reference/etcpak/LICENSE.txt", assets / "licenses/etcpak.txt")
    drivers, driver_licenses = vulkan_drivers()
    for path, name in driver_licenses:
        shutil.copyfile(path, assets / "licenses" / name)
    probe = port_launcher.build_probe(OUT / "probe", prepared["package"], ndk=os.environ.get("ANDROID_NDK", str(SDK / "ndk/27.2.12479018")), api=29)
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
    run(TOOLS / "d8.bat", "--lib", ANDROID_JAR, "--min-api", "29", "--output", OUT / "dex", "@" + str(inputs), shell=False)
    unsigned = OUT / "unsigned.apk"
    shutil.copy(OUT / "base.apk", unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(OUT / "dex/classes.dex", "classes.dex")
        apk.write(probe, "lib/arm64-v8a/" + probe.name)
        apk.write(texconv, "lib/arm64-v8a/" + texconv.name)
        for path, name in drivers:
            apk.write(path, "lib/arm64-v8a/" + name)
        if not args.java_only:
            apk.write(library, "lib/arm64-v8a/libsims2.so")
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
        run(ADB, "-s", args.serial, "push", library, "/data/local/tmp/sims2/libsims2.so")
        run(ADB, "-s", args.serial, "shell", f"run-as {PACKAGE} sh -c 'mkdir -p files && "
            "rm -f files/libsims2.so.new && cp /data/local/tmp/sims2/libsims2.so files/libsims2.so.new && chmod 444 files/libsims2.so.new && mv files/libsims2.so.new files/libsims2.so'")
        print(f"pushed {library} into the app's files/libsims2.so")


if __name__ == "__main__":
    main()
