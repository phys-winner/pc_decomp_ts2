"""Verify packaged shader assets against build outputs and detect Direct3D shader containers."""
import argparse
import hashlib
import lzma
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zipfile


def has_shader(stream, size):
    """DXBC can also be a plain string in a log or a compiler. Validate the chunk table and shader."""
    carry, position = b"", 0
    while block := stream.read(1 << 20):
        data = carry + block
        at = data.find(b"DXBC")
        while at >= 0:
            base = position - len(carry) + at
            resume = stream.tell()
            stream.seek(base)
            head = stream.read(32)
            if len(head) == 32:
                version, length, count = struct.unpack_from("<III", head, 20)
                if version == 1 and 0 < count <= 256 and 32 + 4 * count <= length <= size - base:
                    offsets = struct.unpack(f"<{count}I", stream.read(4 * count))
                    for offset in offsets:
                        if 32 + 4 * count <= offset <= length - 8:
                            stream.seek(base + offset)
                            kind, n = struct.unpack("<4sI", stream.read(8))
                            if kind in (b"SHDR", b"SHEX", b"DXIL") and 0 < n <= length - offset - 8:
                                return True
            stream.seek(resume)
            at = data.find(b"DXBC", at + 4)
        carry = data[-3:]
        position += len(block)
    return False


def inspect(stream, size, name):
    magic = stream.read(6)
    stream.seek(0)
    if magic.startswith(b"PK\x03\x04"):
        with zipfile.ZipFile(stream) as z:
            for item in z.infolist():
                if not item.is_dir():
                    with z.open(item) as entry:
                        yield from inspect(entry, item.file_size, name + "!/" + item.filename)
    elif magic == b"\xfd7zXZ\x00":
        with tempfile.TemporaryFile() as tmp:
            try:
                with lzma.LZMAFile(stream) as src:
                    shutil.copyfileobj(src, tmp)
            except lzma.LZMAError:
                # The ARM64 BCJ filter in port libraries needs a newer XZ decoder than some Pythons ship.
                decoder = shutil.which("7z") or Path(os.environ.get("ProgramFiles", "")) / "7-Zip/7z.exe"
                if not Path(decoder).is_file():
                    raise ValueError(f"{name}: install an XZ decoder with ARM64 BCJ support (7-Zip)")
                stream.seek(0)
                tmp.seek(0)
                tmp.truncate()
                with tempfile.TemporaryFile() as encoded:
                    shutil.copyfileobj(stream, encoded)
                    encoded.seek(0)
                    subprocess.run([str(decoder), "x", "-txz", "-si", "-so"], stdin=encoded, stdout=tmp, check=True)
            length = tmp.tell()
            tmp.seek(0)
            yield from inspect(tmp, length, name + "!xz")
    elif has_shader(stream, size):
        yield name


def check(apk, assets, allow_dxbc=False):
    issues = []
    with zipfile.ZipFile(apk) as z:
        for name, source in assets:
            with source.open("rb") as f:
                expected = hashlib.file_digest(f, "sha256").digest()
            if name not in z.namelist() or source.stat().st_size == 0:
                issues.append("Missing or empty shader asset " + name)
                continue
            with z.open(name) as f:
                if hashlib.file_digest(f, "sha256").digest() != expected:
                    issues.append("Shader asset differs from build output: " + name)
        if not allow_dxbc:
            for item in z.infolist():
                if not item.is_dir() and item.filename.startswith(("assets/", "lib/", "res/raw/")):
                    with z.open(item) as f:
                        issues.extend("Direct3D shader: " + path for path in inspect(f, item.file_size, item.filename))
    return issues


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    parser.add_argument("--asset", nargs=2, action="append", metavar=("APK_PATH", "BUILD_FILE"), default=[])
    parser.add_argument("--allow-dxbc", action="store_true", help="Explicit shader-mod build")
    args = parser.parse_args()
    errors = check(args.apk, [(name, Path(src)) for name, src in args.asset], args.allow_dxbc)
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"PASS · {len(args.asset)} build assets verified · DXBC {'allowed' if args.allow_dxbc else 'checked'}")
