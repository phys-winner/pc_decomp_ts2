"""Tie measured ARM leaf bodies to a production object, including relocations."""
import argparse
import hashlib
import io
import json
from pathlib import Path

from elftools.elf.elffile import ELFFile
from build_android import ROOT


def body(path, name):
    elf = ELFFile(io.BytesIO(path.read_bytes()))
    symbol = next(s for s in elf.get_section_by_name('.symtab').iter_symbols() if s.name == name)
    start, size, index = symbol['st_value'], symbol['st_size'], symbol['st_shndx']
    data = elf.get_section(index).data()[start:start + size]
    relocations = []
    for section in elf.iter_sections():
        if section['sh_type'] not in ('SHT_RELA', 'SHT_REL') or section['sh_info'] != index:
            continue
        symbols = elf.get_section(section['sh_link'])
        for r in section.iter_relocations():
            if start <= r['r_offset'] < start + size:
                relocations.append([r['r_offset'] - start, r['r_info_type'],
                                    symbols.get_symbol(r['r_info_sym']).name,
                                    r.entry.get('r_addend', 0)])
    return {'size': size, 'bytes': data.hex(), 'relocations': relocations}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measured', type=Path, default=ROOT / 'build/test/native-render-leaves/native.o')
    parser.add_argument('--production', type=Path, default=ROOT / 'build/arm64/obj/host-app/sims2_fast.o')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    pairs = (('native_material', 'sub_A0ADFAE0'), ('native_attribute', 'sub_A052F900'))
    checks = []
    for measured_name, production_name in pairs:
        measured, production = body(args.measured, measured_name), body(args.production, production_name)
        assert measured == production, f'{production_name}: measured/production instructions or relocations differ'
        checks.append({'measured_symbol': measured_name, 'production_symbol': production_name, **production})
    result = {'status': 'PASS', 'scope': 'Whole native leaf instruction sections and relocation offsets/types/symbols/addends; not in-game FPS.',
              'sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (args.measured, args.production)},
              'checks': checks}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PASS measured/production native leaves: ' + ', '.join(f"{c['production_symbol']} {c['size']} bytes" for c in checks))


if __name__ == '__main__':
    main()
