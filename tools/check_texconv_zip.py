"""Build and run the production converter's ZIP/folder regression on an Android device."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import zipfile

from build_texconv import OUT, ROOT, compile_unit, link, units


def u32(*values):
    return struct.pack('<' + 'I' * len(values), *values)


def text(value):
    data = value.encode('ascii')
    assert len(data) < 128
    return bytes([len(data)]) + data


def rcol(block):
    return u32(0, 1, 0) + text(block) + u32(0, 8) + text('cSGResource') + u32(0, 1)


def txtr(name, color, reference=False):
    resource = rcol('cImageData') + text(name) + u32(4, 4, 4, 1, 0, 1, 0, 1)
    pixels = struct.pack('<HHI', color, 0, 0)
    return resource + (b'\1' + text(name) if reference else b'\0' + u32(8) + pixels)


def package(resources):
    header = bytearray(96)
    header[:4] = b'DBPF'
    index, data = b'', b''
    for i, (kind, raw) in enumerate(resources):
        index += u32(kind, 0, i, 96 + len(data), len(raw))
        data += raw
    struct.pack_into('<III', header, 36, len(resources), 96 + len(data), len(index))
    return header + data + index


def fixture(root):
    probe = 'Base/TSData/Res/Sims3D/Objects06.package'
    files = {
        probe: package([(0x1C4A276C, txtr('inline', 0xF800)), (0x1C4A276C, txtr('large', 0, True))]),
        'EP1/TSData/Res/Textures.package': package([
            (0x1C4A276C, txtr('second', 0x07E0)),
            (0xED534136, rcol('cLevelInfo') + text('large') + u32(4, 4, 0, 8) + struct.pack('<HHI', 0x001F, 0, 0))]),
    }
    for name, data in files.items():
        dst = root / 'plain' / name
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(data)
    (root / 'side').mkdir(exist_ok=True)
    archive = root / 'archive.bin'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_STORED) as z:
        for name, data in files.items():
            z.writestr(name, data)
    data = archive.read_bytes()
    index = b'ZMI1' + u32(len(files)) + struct.pack('<Q', len(data)) + bytes(32)
    with zipfile.ZipFile(archive) as z:
        for item in z.infolist():
            name_size, extra_size = struct.unpack_from('<HH', data, item.header_offset + 26)
            offset = item.header_offset + 30 + name_size + extra_size
            name = item.filename.encode()
            index += struct.pack('<QQIIQH', offset, item.file_size, item.CRC, 0, 0, len(name)) + name
    (root / 'archive.zmi').write_bytes(index)


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--serial', default=os.environ.get('S2_SERIAL'), required=not os.environ.get('S2_SERIAL'))
    args = parser.parse_args()
    fixture_dir = ROOT / 'build/test/texconv-zip/fixture'
    fixture_dir.mkdir(parents=True, exist_ok=True)
    fixture(fixture_dir)
    # build_texconv.py must have prepared these production objects first.
    objects = [OUT / 'obj' / (p.parent.name + '_' + p.stem + '.o') for p, _ in units()
               if p.name != 's2texconv.cpp']
    test = compile_unit((ROOT / 'tools/test_texconv_zip.cpp', ['-Wall', '-Wextra', '-Werror']))
    program = OUT / 'test-texconv-zip'
    link(objects + [test], program, ['-pie', '-Wl,--wrap=open'])
    adb = Path(os.environ['LOCALAPPDATA']) / 'Android/Sdk/platform-tools/adb.exe'
    prefix = [str(adb), '-s', args.serial]
    remote = '/data/local/tmp/sims2/qa-texconv-zip'
    for command in ([*prefix, 'shell', 'mkdir', '-p', remote + '/side'],
                    [*prefix, 'shell', 'rm', '-f', remote + '/side/Base/TSData/Res/Sims3D/Objects06.package'],
                    [*prefix, 'push', str(fixture_dir) + '/.', remote],
                    [*prefix, 'push', str(program), remote + '/test'],
                    [*prefix, 'shell', 'chmod', '755', remote + '/test'],
                    [*prefix, 'shell', remote + '/test', remote]):
        subprocess.run(command, check=True)


if __name__ == '__main__':
    main()
