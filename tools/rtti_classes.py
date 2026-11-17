"""Class names for Sims2EP9.exe methods from MSVC RTTI: every vtable (its
CompleteObjectLocator just before it) lists the functions it points at.
    python vtables.py VA [VA...]        -> the classes whose vtables hold each VA
    python vtables.py --dump OUT.json   -> {va: [class@slot, ...]} for all"""
import json
import struct
import sys

import pefile

ROOT = "E:/static_recompilation/the_sims_2/"
pe = pefile.PE(ROOT + "generated/input/Sims2EP9.exe", fast_load=True)
img = pe.get_memory_mapped_image()
BASE = 0xA0000000
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
TEXT_LO, TEXT_HI = BASE + text.VirtualAddress, BASE + text.VirtualAddress + text.Misc_VirtualSize
IMG_HI = BASE + len(img)


def d(va):
    return struct.unpack_from("<I", img, va - BASE)[0]


def cstr(va):
    s = img[va - BASE: va - BASE + 200]
    return s[:s.index(b"\0")].decode("latin1")


def is_col(v):
    try:
        return BASE <= v < IMG_HI - 20 and d(v) == 0 and BASE <= d(v + 12) < IMG_HI and \
            cstr(d(v + 12) + 8).startswith(".?A")
    except Exception:
        return False


methods = {}
for sec in pe.sections:
    if sec.Name.startswith(b".text"):
        continue
    lo, hi = BASE + sec.VirtualAddress, min(BASE + sec.VirtualAddress + sec.Misc_VirtualSize, IMG_HI - 8)
    for va in range(lo, hi - 8, 4):
        col = d(va)
        if not is_col(col) or not (TEXT_LO <= d(va + 4) < TEXT_HI):
            continue
        name = cstr(d(col + 12) + 8)
        slot = 0
        p = va + 4
        while p < hi and TEXT_LO <= d(p) < TEXT_HI:
            methods.setdefault(d(p), []).append("%s@%d" % (name, slot))
            slot += 1
            p += 4
if sys.argv[1] == "--dump":
    json.dump({"%08X" % k: v for k, v in methods.items()}, open(sys.argv[2], "w"))
    print(len(methods), "methods")
else:
    for a in sys.argv[1:]:
        v = methods.get(int(a, 16), [])
        print(a, v[:6], "(+%d)" % (len(v) - 6) if len(v) > 6 else "")
