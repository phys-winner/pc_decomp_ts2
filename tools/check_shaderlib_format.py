"""The shader library's writer (tools/build_shaderlib.py) and reader (android/render/s2r_shaderlib.c) agree.

Checks the format number and the record sizes both sides declare, then, when
generated/shaders/s2r_shaderlib.bin exists, reads it the way s2r_shaderlib.c
does: every section inside the file, every text offset in the pool, every
SPIR-V range in the word pool and starting with the SPIR-V magic, and every
pixel shader with a UI FSR 1 SPIR-V (format 2), so Vulkan compiles nothing on
the device for the library's shaders with UI FSR 1 on.

    python -X utf8 -B tools/check_shaderlib_format.py
"""
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import build_shaderlib as L  # noqa: E402

SPIRV_MAGIC = 0x07230203


def c_side():
    text = (ROOT / "android/render/s2r_shaderlib.c").read_text(encoding="utf-8")
    fmt = int(re.search(r"#define LIB_FORMAT (\d+)u", text).group(1))
    sizes = [int(v) for v in re.search(r"record_size\[S_COUNT\] = \{([^}]*)\}", text).group(1).split(",")]
    return fmt, sizes


def main():
    fmt, sizes = c_side()
    py_sizes = [struct.calcsize(L.SHADER_RECORD), 6, 1, 4, struct.calcsize(L.BUILTIN_RECORD)]
    assert fmt == L.FORMAT, f"format: reader {fmt}, writer {L.FORMAT}"
    assert sizes == py_sizes, f"record sizes: reader {sizes}, writer {py_sizes}"
    if not L.LIBRARY.is_file():
        print(f"PASS format {fmt}, record sizes {sizes} (no generated library to read)")
        return
    data = L.LIBRARY.read_bytes()
    assert data[:8] == L.MAGIC, "magic"
    got_fmt, count = struct.unpack_from("<II", data, 8)
    assert (got_fmt, count) == (fmt, len(sizes)), f"header format {got_fmt}, {count} sections"
    sections = []
    for s in range(count):
        offset, n, size = struct.unpack_from("<III", data, 48 + 12 * s)
        assert offset + size <= len(data) and n * sizes[s] == size and offset % 4 == 0, f"section {s}"
        sections.append((offset, n))
    text_at, text_n = sections[2]
    words_at, words_n = sections[3]
    words = struct.unpack_from(f"<{words_n}I", data, words_at)

    def spirv(at, n, what):
        assert at + n <= words_n and n > 5 and words[at] == SPIRV_MAGIC, what

    shaders, pixel, with_ui = sections[0][1], 0, 0
    for i in range(shaders):
        r = struct.unpack_from(L.SHADER_RECORD, data, sections[0][0] + i * sizes[0])
        is_pixel, glsl, spv, spv_n, name, ui, ui_n = r[3], r[8], r[9], r[10], r[-3], r[-2], r[-1]
        assert glsl < text_n and name < text_n, f"shader {i}: text offsets"
        spirv(spv, spv_n, f"shader {i}: SPIR-V")
        if is_pixel:
            pixel += 1
            if ui_n:
                spirv(ui, ui_n, f"shader {i}: UI FSR 1 SPIR-V")
                with_ui += 1
        else:
            assert not ui_n, f"vertex shader {i} carries a UI SPIR-V"
    builtin_ui = 0
    for i in range(sections[4][1]):
        name, gles, spv, spv_n, ui, ui_n = struct.unpack_from(L.BUILTIN_RECORD, data, sections[4][0] + i * sizes[4])
        spirv(spv, spv_n, f"builtin {i}: SPIR-V")
        if ui_n:
            spirv(ui, ui_n, f"builtin {i}: UI FSR 1 SPIR-V")
            builtin_ui += 1
    assert with_ui == pixel, f"{pixel - with_ui} pixel shaders without a UI FSR 1 SPIR-V"
    assert builtin_ui == 1, "the fixed-function pixel shader needs its UI FSR 1 SPIR-V"
    print(f"PASS format {fmt}: {shaders} game shaders, UI FSR 1 SPIR-V for all {pixel} pixel shaders and the "
          f"fixed-function pixel shader, {len(data) / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
