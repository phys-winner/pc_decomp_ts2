"""The native renderers' shader library (generated/shaders/s2r_shaderlib.bin).

The Sims 2 builds its shaders at run time from its own material text (D3DX
assembles them on the device, d3dx9.c), so no build step can find them in the
exe the way Heroes V's tools/build_shaderlib.py does. The library is made from
the bytecode a device saw instead:

    python -X utf8 -B tools/build_shaderlib.py capture SERIAL PROFILE
        pulls <profile>/shaderdump/<hash>.vs|ps, which a run with
        `adb shell setprop debug.s2.shaderdump 1` writes for every game shader
        the library lacks, into generated/shaders/capture (the hash is checked);
    python -X utf8 -B tools/build_shaderlib.py build [--force]
        translates every captured shader once on the PC with
        tools/shaderlib/s2_shaderc.c (MojoShader plus
        android/render/s2r_translate.c, the code the renderers run on the device
        for a shader the library lacks), compiles the Vulkan form with glslc of
        the pinned NDK and writes one file: GLSL ES text for OpenGL ES, SPIR-V
        words for Vulkan, the facts the renderers need per shader (inputs,
        samplers, constant packing), and the fixed-function and present
        shaders (android/render/shaders). tools/build_game_apk.py runs this
        step and packs the file into runtime.zip (shaders/s2r_shaderlib.bin).

A shader the library lacks is still translated on the device (a mod's, a
material no capture saw, a ps_1_x sampler-type map it was not built for), so
the library only takes the first-use translations off the device.

ps_1_x shaders declare no sampler types: the bound texture decides. They get
one entry per map: all 2D, each used register alone as a cube map, and all of
them cube maps.

The captured bytecode and the library are game-derived like the lifted code:
generated/ is private.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_android as B  # noqa: E402  (the builtins' texts, the MojoShader switches)

SDK = Path(os.environ.get("ANDROID_SDK", str(Path(os.environ["LOCALAPPDATA"]) / "Android/Sdk")))
NDK = SDK / "ndk/27.2.12479018"
GLSLC = NDK / "shader-tools/windows-x86_64/glslc.exe"
CLANG_CL = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin/clang-cl.exe"
ADB = SDK / "platform-tools/adb.exe"
CAPTURE = ROOT / "generated/shaders/capture"
OUT_DIR = ROOT / "generated/shaders"
LIBRARY = OUT_DIR / "s2r_shaderlib.bin"
WORK = ROOT / "build/shaderlib"
RENDER = ROOT / "android/render"
SHADERS = RENDER / "shaders"
MOJO = ROOT / "reference/mojoshader"
MOJO_SOURCES = [
    "mojoshader.c", "mojoshader_common.c", "mojoshader_assembler.c", "mojoshader_preprocessor.c",
    "mojoshader_lexer.c", "profiles/mojoshader_profile_common.c", "profiles/mojoshader_profile_glsl.c",
    "profiles/mojoshader_profile_d3d.c", "profiles/mojoshader_profile_bytecode.c",
]
# The switches of tools/build_android.py MOJO_FLAGS: the device's translation and this library
# must produce the same text.
MOJO_DEFINES = [f[2:] for f in B.MOJO_FLAGS if f.startswith("-D")] + ["_CRT_SECURE_NO_WARNINGS"]
FORMAT = 1   # s2r_shaderlib.c LIB_FORMAT
MAGIC = b"S2SHLIB\0"
# Records of the library file, little-endian and packed; s2r_shaderlib.c parse() reads the same fields.
SHADER_RECORD = "<QIIBBBBBIIIB16s16sHHIBBBHHHHHHHB8s8HI"
BUILTIN_RECORD = "<IIII"
assert struct.calcsize(SHADER_RECORD) == 120 and struct.calcsize(BUILTIN_RECORD) == 16


def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


# ------------------------------------------------------------------ capture
def capture(serial, profile):
    """<profile>/shaderdump/<hash>.vs|ps of a device into generated/shaders/capture."""
    if not re.fullmatch(r"userdata(-qa-[A-Za-z0-9_-]{1,64})?", profile):
        raise SystemExit("PROFILE: userdata or userdata-qa-NAME")
    remote = f"/sdcard/Sims2/{profile}/shaderdump"
    listing = subprocess.run([str(ADB), "-s", serial, "shell", f"ls {remote}"], capture_output=True, text=True)
    names = [n.strip() for n in listing.stdout.split() if re.fullmatch(r"[0-9a-f]{16}\.(vs|ps)", n.strip())]
    if not names:
        raise SystemExit(f"no shaders in {remote} (run with `adb shell setprop debug.s2.shaderdump 1` first)")
    CAPTURE.mkdir(parents=True, exist_ok=True)
    WORK.mkdir(parents=True, exist_ok=True)
    pulled = WORK / "pulled"
    pulled.mkdir(exist_ok=True)
    added = 0
    for name in names:
        target = pulled / name
        subprocess.run([str(ADB), "-s", serial, "pull", f"{remote}/{name}", str(target)], capture_output=True, check=True,
                       env=dict(os.environ, MSYS_NO_PATHCONV="1"))
        data = target.read_bytes()
        if f"{fnv64(data):016x}" != name[:16]:
            print(f"skipped {name}: the hash of its bytes is {fnv64(data):016x}")
            continue
        final = CAPTURE / name
        if not final.is_file():
            final.write_bytes(data)
            added += 1
    total = len(list(CAPTURE.glob("*.vs"))) + len(list(CAPTURE.glob("*.ps")))
    print(f"{added} new shaders captured from {len(names)}; {total} in {CAPTURE.relative_to(ROOT).as_posix()}")


# ------------------------------------------------------------ host tool
def host_environment():
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    installation = Path(subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
    msvc = sorted((installation / "VC/Tools/MSVC").iterdir())[-1]
    sdk = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits/10"
    version = sorted((sdk / "Lib").iterdir())[-1].name
    env = os.environ.copy()
    env["PATH"] = str(msvc / "bin/Hostx64/x64") + os.pathsep + env["PATH"]
    env["INCLUDE"] = ";".join(map(str, [msvc / "include", sdk / f"Include/{version}/ucrt",
                                        sdk / f"Include/{version}/shared", sdk / f"Include/{version}/um"]))
    env["LIB"] = ";".join(map(str, [msvc / "lib/x64", sdk / f"Lib/{version}/ucrt/x64", sdk / f"Lib/{version}/um/x64"]))
    return env, msvc / "bin/Hostx64/x64/link.exe"


def tool_sources():
    return [ROOT / "tools/shaderlib/s2_shaderc.c", RENDER / "s2r_translate.c"] + [MOJO / name for name in MOJO_SOURCES]


def build_tool():
    sources = tool_sources()
    digest = hashlib.sha256()
    for path in sources + [RENDER / "s2r_translate.h"] + sorted(MOJO.glob("*.h")) + sorted((MOJO / "profiles").glob("*.h")) + \
            [ROOT / "android/runtime/mojoshader_version.h"]:
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    digest.update(json.dumps(MOJO_DEFINES).encode())
    out = WORK / "host"
    exe = out / "s2_shaderc.exe"
    stamp = out / "s2_shaderc.sha256"
    if exe.is_file() and stamp.is_file() and stamp.read_text() == digest.hexdigest():
        return exe
    out.mkdir(parents=True, exist_ok=True)
    env, linker = host_environment()
    objects = []
    for source in sources:
        obj = out / (source.stem + ".obj")
        cmd = [str(CLANG_CL), "/nologo", "/O2", "/std:c11", "/clang:--target=x86_64-pc-windows-msvc", "/clang:-Wno-int-conversion",
               "/W0", "/I" + str(MOJO), "/I" + str(ROOT / "android/runtime"), "/I" + str(RENDER)] + \
              ["/D" + d for d in MOJO_DEFINES] + ["/c", str(source), "/Fo" + str(obj)]
        result = subprocess.run(cmd, env=env, capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"host compile failed: {source.name}\n{result.stdout}{result.stderr}")
        objects.append(str(obj))
    result = subprocess.run([str(linker), "/nologo", *objects, "/OUT:" + str(exe)], env=env, capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"host link failed\n{result.stdout}{result.stderr}")
    stamp.write_text(digest.hexdigest())
    return exe


def translate(tool, jobs, name):
    """jobs: (pixel, types, path); the tool's JSON records in order."""
    listing = WORK / f"{name}.txt"
    listing.write_text("".join(f"{'ps' if p else 'vs'} {t:x} {path}\n" for p, t, path in jobs), encoding="utf-8")
    out = WORK / f"{name}.json"
    result = subprocess.run([str(tool), str(listing), str(out)], capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"s2_shaderc failed:\n{result.stdout}{result.stderr}")
    records = json.loads(out.read_text(encoding="utf-8"))
    if len(records) != len(jobs):
        raise SystemExit(f"s2_shaderc answered {len(records)} of {len(jobs)} shaders")
    return records


def glslc(stage, source, path):
    path.write_text(source, encoding="utf-8", newline="\n")
    spv = path.with_suffix(path.suffix + ".spv")
    result = subprocess.run([str(GLSLC), f"-fshader-stage={stage}", "--target-env=vulkan1.1", "-O", "-o", str(spv), str(path)],
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"glslc failed for {path.name}:\n{result.stdout}{result.stderr}")
    data = spv.read_bytes()
    return list(struct.unpack(f"<{len(data) // 4}I", data))


def sampler_maps(record):
    """The ps_1_x sampler-type maps besides all 2D: each used register alone as a cube map, all of them."""
    regs = sorted({r for r, _ in record["samplers"] if 0 <= r < 8})
    maps = [1 << (2 * r) for r in regs]
    if len(regs) > 1:
        maps.append(sum(1 << (2 * r) for r in regs))
    return maps


def builtin_texts():
    """(name, pixel, GLES body or None, Vulkan text) of android/render/shaders, as build_android.py embeds them."""
    lists = B.spec_lists()

    def vulkan(body):
        def declare(match):
            return "".join(f"layout(constant_id = {n}) const int {name} = 0;\n" for n, name in lists[match.group(1)])
        body = re.sub(r"#pragma s2r_spec (\w+)\n", declare, body)
        return body if body.startswith("#version") else "#version 450\n" + body

    out = []
    for name, pixel in B.RENDER_BUILTINS:
        body = (SHADERS / name).read_text(encoding="utf-8").replace("\r\n", "\n")
        gles = None if body.startswith("#version") else re.sub(r"#pragma s2r_spec \w+\n", "", body)
        out.append((name, pixel, gles, vulkan(body)))
    return out


# ---------------------------------------------------------------- build
def inputs_digest():
    captured = sorted(CAPTURE.glob("*.vs")) + sorted(CAPTURE.glob("*.ps")) if CAPTURE.is_dir() else []
    digest = hashlib.sha256(f"format {FORMAT}\0".encode())
    for path in captured:
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    for path in sorted(SHADERS.glob("*")) + [RENDER / "s2r_spec.h", Path(__file__), ROOT / "tools/build_android.py"] + \
            tool_sources() + [RENDER / "s2r_translate.h"]:
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    return captured, digest.hexdigest()


def build(force=False):
    captured, digest = inputs_digest()
    stamp = OUT_DIR / "s2r_shaderlib.sha256"
    if not captured:
        print("shader library: no captured shaders (generated/shaders/capture): the renderers translate on the device")
        return None
    if not force and LIBRARY.is_file() and stamp.is_file() and stamp.read_text() == digest:
        print("shader library is up to date")
        return LIBRARY
    tool = build_tool()
    WORK.mkdir(parents=True, exist_ok=True)
    shaders = []
    for path in captured:
        data = path.read_bytes()
        if f"{fnv64(data):016x}" != path.stem:
            raise SystemExit(f"{path.name}: the hash of its bytes is {fnv64(data):016x}")
        shaders.append({"hash": fnv64(data), "size": len(data), "pixel": path.suffix == ".ps", "path": path})
    first = translate(tool, [(s["pixel"], 0, s["path"]) for s in shaders], "pass1")
    entries, rejected, retry = [], [], []
    for s, r in zip(shaders, first, strict=True):
        if "error" in r:
            if s["pixel"] and "sampler" in r["error"]:
                retry.append(s)
            else:
                rejected.append((s, r["error"]))
            continue
        entries.append((s, r))
    jobs, owners = [], []
    for s in retry:
        for m in [1 << (2 * reg) for reg in range(6)] + [0x555]:
            jobs.append((1, m, s["path"]))
            owners.append(s)
    for s, r in list(entries):
        if r["pixel"] and r["major"] == 1:
            for m in sampler_maps(r):
                jobs.append((1, m, s["path"]))
                owners.append(s)
    variants = translate(tool, jobs, "pass2") if jobs else []
    answered = set()
    for (pixel, types, _), s, r in zip(jobs, owners, variants, strict=True):
        if "error" in r:
            continue
        answered.add(s["hash"])
        entries.append((s, r))
    for s in retry:
        if s["hash"] not in answered:
            rejected.append((s, "no sampler-type map translates"))

    # ---- SPIR-V of every game shader and of the builtin shaders
    spv_dir = WORK / "spv"
    spv_dir.mkdir(parents=True, exist_ok=True)
    for old in spv_dir.glob("*"):
        old.unlink()
    work = []
    for i, (s, r) in enumerate(entries):
        work.append(("frag" if r["pixel"] else "vert", "#version 450\n" + r["glsl"], spv_dir / f"{i:04d}.glsl", r))
    builtins = []
    for name, pixel, gles, vk in builtin_texts():
        entry = {"name": name, "gles": gles}
        builtins.append(entry)
        work.append(("frag" if pixel else "vert", vk, spv_dir / name, entry))
    with ThreadPoolExecutor(max_workers=max(1, (os.cpu_count() or 8) - 2)) as pool:
        futures = [(job, pool.submit(glslc, job[0], job[1], job[2])) for job in work]
        for job, future in futures:
            job[3]["spv"] = future.result()

    # ---- the file: sections of packed little-endian records (s2r_shaderlib.c reads them)
    text = bytearray()
    text_at = {}
    words = []

    def add_text(value):
        if value is None:
            return 0xFFFFFFFF
        if value not in text_at:
            text_at[value] = len(text)
            text.extend(value.encode("utf-8") + b"\0")
        return text_at[value]

    def add_words(value):
        at = len(words)
        words.extend(value)
        return at, len(value)

    maps, shader_recs = [], []
    entries.sort(key=lambda e: (e[0]["hash"], e[1]["pixel"], e[1].get("types", 0)))
    for s, r in entries:
        glsl_at = add_text(r["glsl"])
        spv_at, spv_n = add_words(r["spv"])
        map_first = len(maps)
        for key in ("fmap", "imap", "bmap"):
            maps.extend(r[key])
        samplers = types = 0
        for reg, kind in r["samplers"]:
            if 0 <= reg < 16:
                samplers |= 1 << reg
                types |= kind << (2 * reg)
        attrs = r["attributes"][:16]
        texbem = r["texbem"][:8]
        shader_recs.append(struct.pack(
            SHADER_RECORD, s["hash"], s["size"], r.get("types", 0), r["pixel"], r["major"], r["minor"],
            int(r["writes_fog"]), int(r["shadow_pcf"]), glsl_at, spv_at, spv_n, len(attrs),
            bytes(a[0] for a in attrs).ljust(16, b"\0"), bytes(a[1] for a in attrs).ljust(16, b"\0"),
            r["input_mask"], samplers, types, r["ps_colors"], r["ps_tex"], r["ps_fog"], r["f_total"], r["i_total"],
            r["b_total"], map_first, len(r["fmap"]), len(r["imap"]), len(r["bmap"]), len(texbem),
            bytes(t[0] for t in texbem).ljust(8, b"\0"), *([t[1] for t in texbem] + [0] * (8 - len(texbem))),
            add_text(f"library {s['hash']:016x}.{'ps' if s['pixel'] else 'vs'}")))
    builtin_recs = []
    for b in builtins:
        at, n = add_words(b["spv"])
        builtin_recs.append(struct.pack(BUILTIN_RECORD, add_text(b["name"]), add_text(b["gles"]), at, n))
    sections = [  # (records, record size) in the order of s2r_shaderlib.c
        (shader_recs, struct.calcsize(SHADER_RECORD)),
        ([struct.pack("<HHH", *m) for m in maps], 6),
        ([bytes(text)], 1),
        ([struct.pack(f"<{len(words)}I", *words)], 4),
        (builtin_recs, struct.calcsize(BUILTIN_RECORD)),
    ]
    header_size = 8 + 4 + 4 + 32 + 12 * len(sections)
    body, table = bytearray(), []
    for records, size in sections:
        while (header_size + len(body)) % 8:
            body.append(0)
        blob = b"".join(records)
        table.append((header_size + len(body), len(blob) // size, len(blob)))
        body.extend(blob)
    header = MAGIC + struct.pack("<II", FORMAT, len(sections)) + bytes.fromhex(digest)
    header += b"".join(struct.pack("<III", *entry) for entry in table)
    assert len(header) == header_size
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tmp = LIBRARY.with_suffix(".tmp")
    tmp.write_bytes(header + bytes(body))
    tmp.replace(LIBRARY)
    stamp.write_text(digest)
    report = {"library": LIBRARY.relative_to(ROOT).as_posix(), "bytes": LIBRARY.stat().st_size, "captured": len(shaders),
              "entries": len(entries), "rejected": [{"shader": f"{s['hash']:016x}", "error": e} for s, e in rejected]}
    (WORK / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"shader library: {len(entries)} entries from {len(shaders)} captured shaders "
          f"({len(rejected)} not translated), {LIBRARY.stat().st_size / 1e6:.2f} MB")
    return LIBRARY


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    c = sub.add_parser("capture")
    c.add_argument("serial")
    c.add_argument("profile")
    b = sub.add_parser("build")
    b.add_argument("--force", action="store_true")
    args = parser.parse_args()
    if args.command == "capture":
        capture(args.serial, args.profile)
    else:
        build(args.force)


if __name__ == "__main__":
    main()
