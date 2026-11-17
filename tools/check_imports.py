"""Check the shipped ELF shim table against every pinned game/codec PE import."""
import hashlib
import io
import json
import struct

from elftools.elf.elffile import ELFFile
import pefile
from build_android import ROOT


def main():
    library = ROOT/'build/arm64/libsims2.so'
    elf = ELFFile(io.BytesIO(library.read_bytes()))
    table = elf.get_section_by_name('void_shims')
    raw = table.data()
    assert len(raw) % 32 == 0
    segments = [(s['p_vaddr'], s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    relocations = {r['r_offset']: r['r_addend'] for r in elf.get_section_by_name('.rela.dyn').iter_relocations()
                   if r['r_info_type'] == 1027}
    strings = {}
    def string(address):
        if not address:
            return None
        if address not in strings:
            for start, data in segments:
                if start <= address < start + len(data):
                    strings[address] = data[address-start:address-start+256].split(b'\0')[0].decode()
                    break
            else:
                raise AssertionError(f'unmapped shim string {address:#x}')
        return strings[address]
    named, ordinal = set(), set()
    for offset in range(0, len(raw), 32):
        address = table['sh_addr'] + offset
        dll = string(relocations.get(address, struct.unpack_from('<Q', raw, offset)[0]))
        name = string(relocations.get(address+8, struct.unpack_from('<Q', raw, offset+8)[0]))
        number = struct.unpack_from('<I', raw, offset+16)[0]
        if name:
            named.add((dll.lower(), name))
        if number:
            ordinal.add((dll.lower(), number))
    rows = []
    for module in json.loads((ROOT/'port.json').read_text())['modules']:
        path = ROOT/module['file']
        assert hashlib.sha256(path.read_bytes()).hexdigest() == module['sha256']
        pe = pefile.PE(str(path))
        for group in pe.DIRECTORY_ENTRY_IMPORT:
            dll = group.dll.decode().lower()
            for entry in group.imports:
                name = entry.name.decode() if entry.name else None
                implemented = (dll, name) in named if name else (dll, entry.ordinal) in ordinal
                rows.append(dict(module=path.name, dll=dll, name=name, ordinal=entry.ordinal,
                                 implemented=implemented))
    missing = [r for r in rows if not r['implemented']]
    out = ROOT/'build/test/imports'
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').write_text(json.dumps(rows, indent=2)+'\n')
    assert not missing, missing
    print(f'PASS all {len(rows)} pinned PE imports resolve by name/ordinal in the production ELF; API error semantics require separate tests')


if __name__ == '__main__':
    main()
