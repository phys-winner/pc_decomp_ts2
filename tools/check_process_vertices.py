"""Emit deterministic GPU regression fixtures using a captured preprocessing VS3.

    python tools/check_process_vertices.py
    python tools/check_process_vertices.py --actual-dir build/test/pv-results

The default call_1 capture is the game's four-bone preprocessing shader. Its
original shader/declarations are preserved; zeroed validation-probe streams
are replaced with known inputs and poses. A GPU replay should start from each
destination.before.bin, then save the whole destination as CASE.bin in the
actual directory. Comparison checks raw POSITION/NORMAL/UV and untouched bytes.
Build/device commands, including chmod after every executable push, are in
tools/build_process_probe.py. Use serial abb24fae and /data/local/tmp/sims2.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
CAPTURE = ROOT / 'build/test/processvertices/captured-abb24fae/call_1.bin'
OUT = ROOT / 'build/test/processvertices/regression'
F32_REGISTERS = 8192
ELEMENT = struct.Struct('<HHBBBB')
MATRICES = (
    ((1, 0, 0, 10), (0, 2, 0, -4), (0, 0, 3, 2)),
    ((0, -1, 0, -3), (1, 0, 0, 6), (0, 0, 2, -8)),
    ((-2, 0, 0, 1), (0, 0.5, 0, 2), (0, 0, 1, 3)),
    ((1, 0, 0.25, -2), (0, 1, 0.5, 4), (0.125, 0, 1, 7)),
)


def capture(path):
    data = path.read_bytes()
    header = struct.unpack_from('<10I', data)
    assert header[0] == 0x31565053, 'expected SPV1 capture'
    p = 40
    declarations = []
    for count in header[7:9]:
        assert count <= 64
        declarations.append([ELEMENT.unpack_from(data, p + i * 8) for i in range(count)])
        p += count * 8
    shader = data[p:p + header[9]]
    assert len(shader) == header[9] and shader[:4] == b'\x00\x03\xfe\xff'
    p += header[9] + 256 * 16 + 16 * 16 + 16 * 4
    streams = []
    for _ in range(16):
        stride, offset, size = struct.unpack_from('<3I', data, p)
        p += 12
        assert p + size <= len(data)
        streams.append((stride, offset))
        p += size
    assert p == len(data)
    return declarations, shader, streams


def shader_semantics(shader):
    words = struct.unpack('<' + 'I' * (len(shader) // 4), shader)
    result = {'inputs': {}, 'outputs': {}}
    p = 1
    while p < len(words):
        token = words[p]
        if token == 0xFFFF:
            break
        opcode = token & 0xFFFF
        count = (token >> 16) & 0x7FFF if opcode == 0xFFFE else (token >> 24) & 15
        assert p + count < len(words)
        if opcode == 31:  # DCL: semantic, register
            semantic, register = words[p + 1:p + 3]
            regtype = ((register >> 28) & 7) | ((register >> 8) & 24)
            kind = 'inputs' if regtype == 1 else 'outputs' if regtype == 6 else None
            if kind:
                result[kind][register & 0x7FF] = (semantic & 15, (semantic >> 16) & 15)
        p += 1 + count
    assert p < len(words) and words[p] == 0xFFFF
    return result


def expected_vertex(position, normal, uv, weights, matrices):
    # The real shader supplies the fourth weight as 1 - x - y - z.
    weights = (*weights, 1 - sum(weights))
    rows = [[sum(weight * matrix[row][col] for weight, matrix in zip(weights, matrices))
             for col in range(4)] for row in range(3)]
    transformed = [sum(row[i] * position[i] for i in range(3)) + row[3] for row in rows]
    direction = [sum(row[i] * normal[i] for i in range(3)) for row in rows]
    return (*transformed, *direction, *uv)


def fixture(name, declarations, shader, strides, bones, weights, source_start=0, dest_index=0, offsets=(0, 0), count=1):
    path = OUT / name
    path.mkdir(parents=True, exist_ok=True)
    position, normal, uv = (1.25, -2.0, 0.75), (0.2, 0.6, -0.5), (0.125, 0.875)
    input_decl, output_decl = declarations
    constants = bytearray(F32_REGISTERS * 16)
    for bone, matrix in zip(bones, MATRICES):
        if bone == 255:
            continue
        for row, values in enumerate(matrix):
            struct.pack_into('<4f', constants, (90 + 3 * bone + row) * 16, *values)
    streams = [bytearray(b'\xA5' * (offsets[i] + (source_start + count) * strides[i])) for i in range(2)]
    expected = bytearray()
    for vertex in range(count):
        pos = (position[0] + vertex, position[1], position[2])
        tex = (uv[0], uv[1] - vertex * 0.25)
        values = {(0, 0): pos, (3, 0): normal, (5, 0): tex, (1, 0): weights, (2, 0): bones}
        for stream, offset, kind, method, usage, index in input_decl:
            if (usage, index) not in values:
                continue
            assert method == 0 and stream < 2
            at = offsets[stream] + (source_start + vertex) * strides[stream] + offset
            fmt = {1: '<2f', 2: '<3f', 5: '<4B'}[kind]
            struct.pack_into(fmt, streams[stream], at, *values[(usage, index)])
        matrices = [MATRICES[i] if bone != 255 else ((0, 0, 0, 0),) * 3 for i, bone in enumerate(bones)]
        expected += struct.pack('<8f', *expected_vertex(pos, normal, tex, weights, matrices))
    destination = bytearray(b'\xCD' * ((dest_index + count) * 32 + 16))
    after = bytearray(destination)
    after[dest_index * 32:(dest_index + count) * 32] = expected
    files = {'shader.vs.bin': shader, 'input.decl.bin': b''.join(ELEMENT.pack(*e) for e in input_decl),
             'output.decl.bin': b''.join(ELEMENT.pack(*e) for e in output_decl), 'constants.f32.bin': constants,
             'stream0.bin': streams[0], 'stream1.bin': streams[1], 'destination.before.bin': destination,
             'expected.buffer.bin': after, 'expected.vertices.bin': expected}
    # The standalone Android replay consumes the same fixture without a JSON
    # dependency. PVR1 followed by first/dest/count/flags/stride/size, stream
    # stride+offset pairs, and the float-register count (all little-endian u32).
    files['case.params.bin'] = struct.pack('<12I', 0x31525650, source_start, dest_index, count, 0x800,
                                          32, len(destination), strides[0], offsets[0], strides[1], offsets[1],
                                          F32_REGISTERS)
    for filename, data in files.items():
        (path / filename).write_bytes(data)
    manifest = {'case': name, 'shader_sha256': hashlib.sha256(shader).hexdigest(),
                'source_start': source_start, 'destination_index': dest_index, 'count': count, 'flags': 0x800,
                'float_register_count': F32_REGISTERS, 'destination_stride': 32, 'destination_size': len(destination),
                'streams': [{'stride': strides[i], 'offset': offsets[i], 'file': f'stream{i}.bin'} for i in range(2)],
                'expected_vertices': [struct.unpack_from('<8f', expected, i * 32) for i in range(count)]}
    (path / 'case.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return path, manifest


def compare(path, manifest, actual):
    data = actual.read_bytes()
    expected = (path / 'expected.buffer.bin').read_bytes()
    assert len(data) == len(expected), (manifest['case'], 'destination size', len(data), len(expected))
    begin = manifest['destination_index'] * 32
    end = begin + manifest['count'] * 32
    assert data[:begin] == expected[:begin] and data[end:] == expected[end:], (manifest['case'], 'untouched bytes changed')
    for i, want in enumerate(struct.unpack('<' + 'f' * ((end - begin) // 4), expected[begin:end])):
        got = struct.unpack_from('<f', data, begin + i * 4)[0]
        assert math.isclose(got, want, rel_tol=2e-5, abs_tol=2e-5), (manifest['case'], i, got, want)


def main():
    global OUT
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--capture', type=Path, default=CAPTURE)
    parser.add_argument('--out', type=Path, default=OUT)
    parser.add_argument('--actual-dir', type=Path)
    args = parser.parse_args()
    OUT = args.out
    declarations, shader, streams = capture(args.capture)
    semantics = shader_semantics(shader)
    assert set(semantics['inputs'].values()) == {(0, 0), (3, 0), (2, 0), (1, 0), (5, 0)}
    assert set(semantics['outputs'].values()) == {(0, 0), (3, 0), (5, 0)}
    assert declarations[1] == [(0, 0, 2, 0, 0, 0), (0, 12, 2, 0, 3, 0), (0, 24, 1, 0, 5, 0)]
    raw = expected_vertex((1.25, -2, 0.75), (0.2, 0.6, -0.5), (0.125, 0.875), (1, 0, 0), MATRICES)
    assert raw == (11.25, -8, 4.25, 0.2, 1.2, -1.5, 0.125, 0.875)
    # An implied fourth weight exercises the last pose rows, c267..c269.
    high = expected_vertex((1.25, -2, 0.75), (0.2, 0.6, -0.5), (0.125, 0.875), (0, 0, 0), MATRICES)
    assert all(math.isclose(a, b) for a, b in zip(high, (-0.5625, 2.375, 7.90625, 0.075, 0.35, -0.475, 0.125, 0.875)))
    cases = [('blend', (0, 1, 2, 3), (0.2, 0.3, 0.1), {}),
             ('high-bone', (55, 57, 58, 59), (0, 0, 0), {}),
             ('unused-bones', (0, 255, 255, 255), (1, 0, 0), {}),
             ('offsets', (55, 57, 58, 59), (0.2, 0.3, 0.1),
              {'source_start': 2, 'dest_index': 3, 'offsets': (16, 8), 'count': 2})]
    for name, bones, weights, options in cases:
        path, manifest = fixture(name, declarations, shader, [s[0] for s in streams[:2]], bones, weights, **options)
        compare(path, manifest, path / 'expected.buffer.bin')
        if args.actual_dir:
            compare(path, manifest, args.actual_dir / f'{name}.bin')
    print(f'ProcessVertices reference/fixtures: {len(cases)} blend, high-bone, unused-bone and offset cases PASS')
    print(f'Fixtures: {OUT}; ' + ('actual GPU destination comparisons PASS' if args.actual_dir else 'GPU execution not run'))


if __name__ == '__main__':
    main()
