"""Count actual DDS headers in loose files and ZIP-based game archives, without extracting payloads."""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct
import zipfile
import zlib

DXGI = dict(zip(range(70, 85), ("BC1_TYPELESS", "BC1", "BC1_SRGB", "BC2_TYPELESS", "BC2", "BC2_SRGB",
                                "BC3_TYPELESS", "BC3", "BC3_SRGB", "BC4_TYPELESS", "BC4", "BC4_SNORM",
                                "BC5_TYPELESS", "BC5", "BC5_SNORM")))
DXGI.update(dict(zip(range(94, 100), ("BC6H_TYPELESS", "BC6H_UFLOAT", "BC6H_SFLOAT", "BC7_TYPELESS", "BC7", "BC7_SRGB"))))


def format_name(header):
    if len(header) < 128 or header[:4] != b"DDS " or struct.unpack_from("<I", header, 4)[0] != 124:
        return "invalid DDS"
    if not struct.unpack_from("<I", header, 80)[0] & 4:
        return "uncompressed"
    fourcc = header[84:88]
    if fourcc == b"DX10":
        if len(header) < 148:
            return "truncated DX10 header"
        number = struct.unpack_from("<I", header, 128)[0]
        return DXGI.get(number, f"DXGI_{number}")
    return {b"ATI1": "BC4", b"ATI2": "BC5", b"BC4U": "BC4", b"BC4S": "BC4_SNORM",
            b"BC5U": "BC5", b"BC5S": "BC5_SNORM"}.get(fourcc, fourcc.decode("ascii") if fourcc.isalnum() else fourcc.hex())


def inventory(root):
    counts, skipped = Counter(), Counter()
    for path in root.rglob("*") if root.is_dir() else [root]:
        suffix = path.suffix.lower()
        if suffix not in (".dds", ".pak", ".cpk", ".zip", ".h5m") or not path.is_file():
            continue
        try:
            if suffix == ".dds":
                with path.open("rb") as stream:
                    counts[format_name(stream.read(148))] += 1
                continue
            with zipfile.ZipFile(path) as archive:
                for item in archive.infolist():
                    if item.filename.lower().endswith(".dds"):
                        try:
                            with archive.open(item) as stream:
                                counts[format_name(stream.read(148))] += 1
                        except (OSError, RuntimeError, ValueError, zipfile.BadZipFile, zlib.error) as error:
                            skipped[type(error).__name__ + " entries"] += 1
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            skipped[suffix + " " + type(error).__name__] += 1
    return {"headers": counts, "unreadable": skipped}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="+", type=Path)
    for root in parser.parse_args().roots:
        if not root.exists():
            parser.error(f"Missing input: {root}")
        print(root.name, json.dumps(inventory(root), sort_keys=True), flush=True)
