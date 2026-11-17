"""Replay a traced skinned draw (android/host/d3d9_gl.c trace_dump_skinned)
on the CPU with RendererD3D's skinning: the palette from c56, three
registers a bone (rows 0 and 1 of the rotation, the translation; row 2 is
their cross product), weights from BLENDWEIGHT with the last one 1 - sum,
indices from BLENDINDICES. Draws the bind pose and the skinned result
side by side (front view, Z up) and prints palette sanity checks.
    python tools/skin_replay.py DRAW.bin [OUT.png]
"""
import struct
import sys

from PIL import Image, ImageDraw

TYPE_SIZE = {0: 4, 1: 8, 2: 12, 3: 16, 4: 4, 5: 4, 6: 4, 7: 8, 8: 4}


def read(path):
    data = open(path, "rb").read()
    magic, count, base_vertex, index_count = struct.unpack_from("<4I", data, 0)
    assert magic == 0x31444B53, "not a skinned draw dump"
    off = 16
    elements = []
    for _ in range(count):
        stream, etype, usage, usage_index, o0, o1 = struct.unpack_from("<6B", data, off)
        elements.append({"stream": stream, "type": etype, "usage": usage, "index": usage_index, "offset": o0 | o1 << 8})
        off += 8
    streams = []
    for s in range(4):
        present, stride, offset = struct.unpack_from("<3I", data, off)
        streams.append({"present": present, "stride": stride, "offset": offset})
        off += 12
    indices = list(struct.unpack_from("<%dI" % index_count, data, off))
    off += 4 * index_count
    lo, hi = struct.unpack_from("<2I", data, off)
    off += 8
    for s in range(4):
        if not streams[s]["present"]:
            continue
        size = struct.unpack_from("<I", data, off)[0]
        off += 4
        streams[s]["bytes"] = data[off:off + size]
        off += size
    constants = [struct.unpack_from("<4f", data, off + 16 * r) for r in range(256)]
    return elements, streams, indices, lo, hi, constants


def fetch(element, streams, vertex):
    s = streams[element["stream"]]
    at = vertex * s["stride"] + element["offset"]
    blob = s["bytes"][at:at + TYPE_SIZE.get(element["type"], 16)]
    t = element["type"]
    if t <= 3:
        values = list(struct.unpack("<%df" % (t + 1), blob))
        return values + [0.0] * (3 - t) + ([1.0] if t < 3 else [])
    if t == 5:                                  # UBYTE4
        return [float(b) for b in blob]
    if t == 4:                                  # D3DCOLOR: bytes B, G, R, A read as RGBA
        b, g, r, a = blob
        return [r / 255.0, g / 255.0, b / 255.0, a / 255.0]
    raise ValueError("element type %d" % t)


def find(elements, usage, index=0):
    for e in elements:
        if e["usage"] == usage and e["index"] == index:
            return e
    return None


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def bone(constants, i):
    r0 = constants[56 + 3 * i][:3]
    r1 = constants[57 + 3 * i][:3]
    t = constants[58 + 3 * i][:3]
    return r0, r1, cross(r0, r1), t


def apply(m, p):
    r0, r1, r2, t = m
    return (sum(a * b for a, b in zip(r0, p)) + t[0], sum(a * b for a, b in zip(r1, p)) + t[1],
            sum(a * b for a, b in zip(r2, p)) + t[2])


def main():
    elements, streams, indices, lo, hi, constants = read(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else sys.argv[1].replace(".bin", ".png")
    pos_e, w_e, i_e = find(elements, 0), find(elements, 1), find(elements, 2)
    print("elements:", [(e["stream"], e["type"], e["usage"], e["index"], e["offset"]) for e in elements])
    print("streams:", [(s["present"], s["stride"], s["offset"], len(s.get("bytes", b""))) for s in streams])
    print("vertices %d..%d, %d indices" % (lo, hi, len(indices)))
    bind, skinned, used, outside = [], [], set(), 0
    for v in range(0, hi - lo + 1):
        p = fetch(pos_e, streams, v)[:3]
        w = fetch(w_e, streams, v)
        idx = fetch(i_e, streams, v)
        if i_e["type"] == 4:
            idx = [round(c * 255.0) for c in idx]
        weights = [w[0], w[1], w[2], 1.0 - w[0] - w[1] - w[2]] if w_e["type"] >= 2 else (
            [w[0], w[1], 0.0, 1.0 - w[0] - w[1]] if w_e["type"] == 1 else [w[0], 0.0, 0.0, 1.0 - w[0]])
        q = [0.0, 0.0, 0.0]
        for k in range(4):
            if weights[k] == 0.0:
                continue
            b = int(idx[k])
            if 58 + 3 * b > 255:                    # past the register file: GL reads nothing useful
                outside += 1
                if outside <= 8:
                    print("v%d weight %.3f on bone %d (index slot %d) past c255: weights %s idx %s" % (
                        v, weights[k], b, k, [round(x, 3) for x in weights], [int(x) for x in idx]))
                continue
            used.add(b)
            r = apply(bone(constants, b), p)
            q = [q[j] + weights[k] * r[j] for j in range(3)]
        bind.append(p)
        skinned.append(q)
        if v < 6:
            print("v%d pos %s weights %s idx %s -> %s" % (v, [round(x, 3) for x in p], [round(x, 3) for x in weights],
                                                        [int(x) for x in idx], [round(x, 3) for x in q]))
    print("bones used:", sorted(used), "; weights on bones past c255:", outside)
    for b in sorted(used):
        r0, r1, r2, t = bone(constants, b)
        n0 = sum(x * x for x in r0) ** 0.5
        n1 = sum(x * x for x in r1) ** 0.5
        d = sum(x * y for x, y in zip(r0, r1))
        flag = "" if abs(n0 - 1) < 0.02 and abs(n1 - 1) < 0.02 and abs(d) < 0.02 else "  <-- not a rotation"
        print("bone %3d |r0| %.3f |r1| %.3f r0.r1 %+.3f t %s%s" % (b, n0, n1, d, [round(x, 3) for x in t], flag))
    # The bind pose front on (x across, z up), then the skinned result in
    # three orthographic views (x-z, y-z, x-y), each fitted to its own box.
    size = 400
    views = [(bind, 0, 2), (skinned, 0, 2), (skinned, 1, 2), (skinned, 0, 1)]
    image = Image.new("RGB", (len(views) * size, size), (255, 255, 255))
    draw = ImageDraw.Draw(image)
    for panel, (points, ax, ay) in enumerate(views):
        us = [p[ax] for p in points]
        vs = [p[ay] for p in points]
        span = max(max(us) - min(us), max(vs) - min(vs), 1e-6)
        su = lambda u: panel * size + 12 + (u - min(us)) / span * (size - 24)
        sv = lambda v: size - 12 - (v - min(vs)) / span * (size - 24)
        for t in range(0, len(indices) - 2, 3):
            a, b, c = (points[indices[t + k] - lo] for k in range(3))
            draw.line([(su(a[ax]), sv(a[ay])), (su(b[ax]), sv(b[ay])), (su(c[ax]), sv(c[ay])), (su(a[ax]), sv(a[ay]))],
                      fill=(40, 40, 160) if panel == 0 else (160, 40, 40))
        draw.text((panel * size + 6, 4), "bind x-z" if panel == 0 else "skinned " + "xyz"[ax] + "-" + "xyz"[ay], fill=(0, 0, 0))
    image.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
