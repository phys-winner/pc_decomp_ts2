"""Apply the LC 1.0.0 extension's instruction changes to the lifted game."""
import re


def enabled(option):
    return f"(s2_extender_flags & S2_EXTENDER_{option})".encode()


def change_branch(line, option, op):
    if not line.startswith(b"if ("):
        raise ValueError(f"expected a conditional branch, got {line!r}")
    test = enabled(option)
    if op == b"&&":
        test = b"!" + test
    return b"if (" + test + b" " + op + b" " + line[4:]


def force_argument(line, option, value):
    prefix = b"PUSH32(esp, "
    if not line.startswith(prefix) or not line.endswith(b");"):
        raise ValueError(f"expected a stack argument, got {line!r}")
    return prefix + enabled(option) + b" ? " + value + b" : " + line[len(prefix):]


# Addresses match the original Sims2EP9.exe, including immediate operands
# whose enclosing lifted instructions start one byte earlier.
PATCHES = {
    "A005F7A0": ("A0058000", lambda line: change_branch(line, "PINK", b"&&")),
    "A02766DE": ("A0270000", lambda line: change_branch(line, "SIMANTICS", b"||")),
    "A02F7894": ("A02F0000", lambda line: b"if " + enabled("OCEAN") + b" goto L_A02F78AF;\n    " + line),
    "A030CB82": ("A0308000", lambda line: change_branch(line, "POOL", b"&&")),
    "A0576507": ("A0570000", lambda line: b"if (!" + enabled("SIMANTICS") + b") " + line),
    "A0576525": ("A0570000", lambda line: b"if (!" + enabled("SIMANTICS") + b") " + line),
    "A05EEA4E": ("A05E8000", lambda line: change_branch(line, "ZODIAC", b"||")),
    "A06CF20A": ("A06C8000", lambda line: b"if (!" + enabled("OFB") + b") " + line),
    "A06D292C": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D293A": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D2948": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D55F6": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D5604": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D5612": ("A06D0000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A06D6933": ("A06D0000", lambda line: b"if " + enabled("MAKEUP") + b" s2_extender_note_cas_tab();\n    " + line),
    "A0729829": ("A0728000", lambda line: force_argument(line, "THUMBNAILS", b"0")),
    "A0745ADA": ("A0740000", lambda line: b"if " + enabled("MAKEUP") + b" s2_extender_note_cas_tab();\n    " + line),
    "A0745E61": ("A0740000", lambda line: force_argument(line, "SEPARATES", b"1")),
    "A0932CAB": (("A0930000", "A0960000"), lambda line: b"if (!s2_extender_hidden_clothes(esi)) " + line),
    "A0932E73": (("A0930000", "A0960000"), lambda line: b"if (!s2_extender_hidden_clothes(esi)) " + line),
    "A0A6589D": ("A0A60000", lambda line: change_branch(line, "OUTDOOR", b"&&")),
}


def apply(text, name):
    bucket = name.removeprefix("sims2_").removesuffix(".c")
    targets = [(address, patch) for address, (chunks, patch) in PATCHES.items()
               if bucket in ((chunks,) if isinstance(chunks, str) else chunks)]
    if not targets:
        return text
    newline = b"\r\n" if b"\r\n" in text else b"\n"
    for address, patch in targets:
        pattern = rb"^    ([^\r\n]+?;) (\/\* 0x" + address.encode() + rb": [^\r\n]+)"
        text, matches = re.subn(pattern, lambda m: b"    " + patch(m[1]).replace(b"\n", newline) + b" " + m[2],
                                text, flags=re.MULTILINE)
        if not matches:
            raise ValueError(f"TS2 Extender patch missing from {name} at {address}")
    return b'#include "sims2_extender.h"' + newline + text
