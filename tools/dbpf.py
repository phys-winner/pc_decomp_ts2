"""Read and write The Sims 2 DBPF packages (version 1.1, index 7.1/7.2) and
their QFS (RefPack) compression, for overlaying UI resources without
touching the original game files.

    python tools/dbpf.py list PACKAGE [--type 0x00000000]
    python tools/dbpf.py grep PACKAGE TEXT          resources whose (decompressed) bytes contain TEXT
    python tools/dbpf.py get PACKAGE TYPE GROUP INSTANCE [RESOURCE] OUT
"""
import struct
import sys
from pathlib import Path

DIR_TYPE = 0xE86B1EEF   # the compressed-resource directory


def qfs_decompress(data):
    """QFS/RefPack: 9-byte header (size:u32 le, 0x10FB, 24-bit be size) then
    the command stream."""
    if len(data) < 9 or data[4:6] != b"\x10\xfb":
        raise ValueError("not QFS")
    size = (data[6] << 16) | (data[7] << 8) | data[8]
    out = bytearray()
    i = 9
    while i < len(data):
        c0 = data[i]
        if c0 < 0x80:
            c1 = data[i + 1]
            plain = c0 & 3
            count = ((c0 & 0x1C) >> 2) + 3
            offset = ((c0 & 0x60) << 3) + c1 + 1
            i += 2
        elif c0 < 0xC0:
            c1, c2 = data[i + 1], data[i + 2]
            plain = (c1 & 0xC0) >> 6
            count = (c0 & 0x3F) + 4
            offset = ((c1 & 0x3F) << 8) + c2 + 1
            i += 3
        elif c0 < 0xE0:
            c1, c2, c3 = data[i + 1], data[i + 2], data[i + 3]
            plain = c0 & 3
            count = ((c0 & 0x0C) << 6) + c3 + 5
            offset = ((c0 & 0x10) << 12) + (c1 << 8) + c2 + 1
            i += 4
        elif c0 < 0xFC:
            plain = ((c0 & 0x1F) << 2) + 4
            count = 0
            offset = 0
            i += 1
        else:
            plain = c0 & 3
            count = 0
            offset = 0
            i += 1
        out += data[i:i + plain]
        i += plain
        for _ in range(count):
            out.append(out[-offset])
        if c0 >= 0xFC:
            break
    if len(out) != size:
        raise ValueError(f"QFS size {len(out)} != {size}")
    return bytes(out)


def qfs_compress_stored(data):
    """A valid QFS stream without back-references (literal runs only): the
    game decompresses it like any other, which is all an overlay needs."""
    out = bytearray(struct.pack("<I", 0) + b"\x10\xfb" + bytes([(len(data) >> 16) & 0xFF, (len(data) >> 8) & 0xFF,
                                                              len(data) & 0xFF]))
    i = 0
    while len(data) - i >= 4:
        n = min(112, (len(data) - i) & ~3)
        out.append(0xE0 + (n - 4) // 4)
        out += data[i:i + n]
        i += n
    rest = len(data) - i
    out.append(0xFC + rest)
    out += data[i:]
    struct.pack_into("<I", out, 0, len(out))
    return bytes(out)


class Package:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        if d[:4] != b"DBPF":
            raise ValueError(f"{path}: not DBPF")
        self.major, self.minor = struct.unpack_from("<II", d, 4)
        self.index_major, self.index_count, self.index_offset, self.index_size = struct.unpack_from("<IIII", d, 32)
        self.index_minor = struct.unpack_from("<I", d, 60)[0] if self.minor >= 1 else 0
        entry_size = self.index_size // self.index_count if self.index_count else 20
        self.entries = []
        for k in range(self.index_count):
            at = self.index_offset + k * entry_size
            if entry_size >= 24:
                t, g, i, r, off, size = struct.unpack_from("<IIIIII", d, at)
            else:
                t, g, i, off, size = struct.unpack_from("<IIIII", d, at)
                r = 0
            self.entries.append({"type": t, "group": g, "instance": i, "resource": r, "offset": off, "size": size})
        self.compressed = {}
        for e in self.entries:
            if e["type"] == DIR_TYPE:
                raw = d[e["offset"]:e["offset"] + e["size"]]
                step = 20 if entry_size >= 24 else 16
                for at in range(0, len(raw) - step + 1, step):
                    if step == 20:
                        t, g, i, r, usize = struct.unpack_from("<IIIII", raw, at)
                    else:
                        t, g, i, usize = struct.unpack_from("<IIII", raw, at)
                        r = 0
                    self.compressed[(t, g, i, r)] = usize

    def key(self, e):
        return (e["type"], e["group"], e["instance"], e["resource"])

    def raw(self, e):
        return self.data[e["offset"]:e["offset"] + e["size"]]

    def read(self, e):
        raw = self.raw(e)
        if self.key(e) in self.compressed and raw[4:6] == b"\x10\xfb":
            return qfs_decompress(raw)
        return raw


def write_package(path, resources, index_minor=2):
    """resources: [((type, group, instance, resource), bytes, compress)]."""
    header = bytearray(96)
    body = bytearray()
    index = []
    directory = []
    for (t, g, i, r), data, compress in resources:
        blob = qfs_compress_stored(data) if compress else data
        if compress:
            directory.append(struct.pack("<IIIII", t, g, i, r, len(data)))
        index.append((t, g, i, r, 96 + len(body), len(blob)))
        body += blob
    if directory:
        blob = b"".join(directory)
        index.append((DIR_TYPE, 0xE86B1EEF, 0x286B1F03, 0, 96 + len(body), len(blob)))
        body += blob
    table = b"".join(struct.pack("<IIIIII", *e) for e in index)
    header[0:4] = b"DBPF"
    struct.pack_into("<II", header, 4, 1, 1)
    struct.pack_into("<IIII", header, 32, 7, len(index), 96 + len(body), len(table))
    struct.pack_into("<I", header, 60, index_minor)
    Path(path).write_bytes(bytes(header) + bytes(body) + table)


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    p = Package(path)
    if cmd == "list":
        want = int(sys.argv[4], 0) if len(sys.argv) > 4 and sys.argv[3] == "--type" else None
        for e in p.entries:
            if want is None or e["type"] == want:
                print(f"{e['type']:08X} {e['group']:08X} {e['instance']:08X} {e['resource']:08X} {e['size']:8d}"
                      f"{' z' if p.key(e) in p.compressed else ''}")
    elif cmd == "grep":
        needle = sys.argv[3].encode()
        for e in p.entries:
            if e["type"] == DIR_TYPE:
                continue
            try:
                data = p.read(e)
            except ValueError:
                continue
            if needle in data:
                print(f"{e['type']:08X} {e['group']:08X} {e['instance']:08X} {e['resource']:08X} {len(data)}")
    elif cmd == "get":
        t, g, i = (int(x, 0) for x in sys.argv[3:6])
        r = int(sys.argv[6], 0) if len(sys.argv) > 7 else 0
        out = sys.argv[-1]
        for e in p.entries:
            if (e["type"], e["group"], e["instance"]) == (t, g, i) and (len(sys.argv) <= 7 or e["resource"] == r):
                Path(out).write_bytes(p.read(e))
                print(f"wrote {out}")
                return
        raise SystemExit("not found")


if __name__ == "__main__":
    main()
