"""Check that the lifted JPEG codec handles every arm of its register jumps."""
import re
import struct

from check_jump_tables import ROOT, image_reader
from liftlib import CHUNK_BITS


def main():
    base, image, _ = image_reader(ROOT / "generated/input/ijl15.dll")
    for function, table, count in (
        (0x60024810, 0x60052880, 257),  # JPEG marker dispatch, including APP0
        (0x6002B120, 0x60052EA0, 102),
        (0x6002E780, 0x60053638, 8),
    ):
        targets = struct.unpack_from(f"<{count}I", image, table - base)
        chunk = (function >> CHUNK_BITS) << CHUNK_BITS
        filename = f"ijl15_{chunk:08X}.c"
        source = (ROOT / "generated/ijl15-x86" / filename).read_text()
        start = source.index(f"void sub_{function:08X}(void) {{")
        body = source[start:source.index("\n}\n", start)]
        local_cases = {int(address, 16) for address in
                       re.findall(r"case 0x([0-9A-F]{8})u: goto L_\1;", body)}
        missing = sorted(set(targets) - local_cases)
        assert not missing, f"{function:#x}: missing local switch targets {[hex(addr) for addr in missing]}"
    print("PASS all three ijl15 register switch tables have complete local dispatch")


if __name__ == "__main__":
    main()
