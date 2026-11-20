"""Host compiler for the native audio output and SND driver fixtures."""
import os
from pathlib import Path
import subprocess
from build_android import ROOT, CLANG as NDK_CLANG
CLANG = NDK_CLANG.with_name("clang-cl.exe")

def compile_probe(name, harness, headers, extra_flags=(), compiler_override=None):
    out = ROOT / "build/tests" / name
    out.mkdir(parents=True, exist_ok=True)
    for filename, text in headers.items():
        (out / filename).write_text(text, encoding="utf-8")
    (out / "probe.c").write_text(harness, encoding="utf-8")
    vswhere = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe")
    installation = Path(subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"], text=True).strip())
    msvc = sorted((installation / "VC/Tools/MSVC").iterdir())[-1]
    sdk = Path(r"C:\Program Files (x86)\Windows Kits\10")
    version = sorted((sdk / "Lib").iterdir())[-1].name
    compiler = msvc / "bin/Hostx64/x64/cl.exe"
    env = os.environ.copy()
    env["PATH"] = str(compiler.parent) + os.pathsep + env["PATH"]
    env["INCLUDE"] = ";".join(map(str, [msvc / "include", sdk / f"Include/{version}/ucrt", sdk / f"Include/{version}/shared", sdk / f"Include/{version}/um"]))
    env["LIB"] = ";".join(map(str, [msvc / "lib/x64", sdk / f"Lib/{version}/ucrt/x64", sdk / f"Lib/{version}/um/x64"]))
    outputs = ["/c", "/Foprobe.obj"] if compiler_override else ["/Fe:probe.exe"]
    compiled = subprocess.run([str(compiler_override or compiler), "/nologo", "/W4", "/WX", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS", *extra_flags, "probe.c", *outputs], cwd=out, env=env, text=True, capture_output=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    if compiler_override:
        linked = subprocess.run([str(compiler), "/nologo", "probe.obj", "/Fe:probe.exe"], cwd=out, env=env, text=True, capture_output=True)
        assert linked.returncode == 0, linked.stdout + linked.stderr
    tested = subprocess.run([str(out / "probe.exe")], text=True, capture_output=True, timeout=10)
    assert tested.returncode == 0, tested.stdout + tested.stderr
    print(tested.stdout.strip())
