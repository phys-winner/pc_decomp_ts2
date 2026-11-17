"""Find `jmp [reg*4 + table]` sites whose table holds targets the lifted body
has no label for (the decoder sized the table too short): such a target traps
at run time as an unresolved control-flow target. Prints a suggested
config/<Module>.jump-tables.json declaration for each.

    python tools/check_jump_tables.py [--module Sims2EP9]

(The Void port's tool, adapted to this workspace's port.json layout.)
"""
import argparse
import json
from pathlib import Path
import re
import struct

import pefile

ROOT = Path(__file__).resolve().parents[1]
JUMP = re.compile(r"MEM32\((e[a-d]x|e[sd]i|ebp) \* 4 \+ (?:0x([0-9A-F]{8})|\(-0x([0-9A-F]{1,8})\))\)\); goto _ljump;")
FUNC = re.compile(r"^void sub_([0-9A-F]{8})\(void\) \{\r?$", re.M)   # generated files use CRLF
LABEL = re.compile(r"^L_([0-9A-F]{8}):", re.M)


def image_reader(path):
    pe = pefile.PE(str(path), fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    code = [(base + s.VirtualAddress, base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
            for s in pe.sections if s.Characteristics & 0x20000000 and s.Name.rstrip(b"\0") != b".bind"]
    return base, data, code


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--module", default="Sims2EP9")
    args = parser.parse_args()
    config = json.loads((ROOT / "port.json").read_text())
    module = next(m for m in config["modules"] if m["name"] == args.module)
    directory = ROOT / f"generated/{module['prefix']}-x86"
    report = json.loads((directory / "report.json").read_text())
    base, data, code = image_reader(ROOT / module["file"])
    findings = []
    for name in report["sources"]:
        text = (directory / name).read_text()
        starts = [(m.start(), m.group(1)) for m in FUNC.finditer(text)]
        for i, (start, func) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            body = text[start:end]
            jumps = set(JUMP.findall(body))
            if not jumps:
                continue
            labels = {int(label, 16) for label in LABEL.findall(body)}
            low, high = int(func, 16), max(labels) if labels else int(func, 16)
            for _, positive, negative in jumps:
                table = int(positive, 16) if positive else (-int(negative, 16)) & 0xFFFFFFFF
                missing, count, nulls = [], 0, 0
                for index in range(256):
                    off = table - base + 4 * index
                    if off + 4 > len(data):
                        break
                    target = struct.unpack_from("<I", data, off)[0]
                    if target == 0:
                        nulls += 1
                        if nulls > 4:
                            break
                        count = index + 1
                        continue
                    if not any(a <= target < b for a, b in code) or not low <= target <= high + 0x4000:
                        break
                    count = index + 1
                    if target not in labels:
                        missing.append(target)
                if missing:
                    findings.append({"function": "0x" + func, "address": hex(table), "count": count,
                                     "missing": [hex(m) for m in missing[:8]], "missing_count": len(missing)})
    for f in findings:
        print(json.dumps(f))
    print(f"{len(findings)} tables with unlabelled targets")


if __name__ == "__main__":
    main()
