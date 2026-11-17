"""Assert that every IDA switch above 256 entries retains all original targets."""
import json
import re
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
import pefile
from build_android import ROOT
from lift_sims2 import load_tables, make_decoder, module_spec
from liftlib import analyze_pe


def main():
    module = module_spec('Sims2EP9')
    path = ROOT/module['file']
    pe = pefile.PE(str(path), fast_load=True)
    image, base = pe.get_memory_mapped_image(), pe.OPTIONAL_HEADER.ImageBase
    switches = [s for s in json.loads((ROOT/module['ida_switches']).read_text())['switches']
                if s['element_size'] == 4 and s['count'] > 256]
    decoder = make_decoder(path.read_bytes(), analyze_pe(str(path)), load_tables(module))
    cs = Cs(CS_ARCH_X86, CS_MODE_32)
    cs.detail = True
    found = {}
    for source in (ROOT/'generated/sims2-x86').glob('*.c'):
        text = source.read_text()
        for switch in switches:
            jump, count, table = switch['jump'], switch['count'], switch['table']
            match = re.search(r'^L_' + f'{jump:08X}' + r':\n\s+\{ _itail_tgt[^\n]+goto _ljump;', text, re.M)
            if not match:
                continue
            start = text.rfind('\nvoid sub_', 0, match.start()) + 1
            end = text.index('\n}', match.start()) + 2
            body = text[start:end]
            targets = set(struct.unpack_from('<' + 'I'*count, image, table-base))
            labels = {int(s, 16) for s in re.findall(r'^L_([0-9A-F]{8}):', body, re.M)}
            assert targets <= labels, (hex(jump), count, sorted(targets-labels))
            instruction = next(cs.disasm(pe.get_data(jump-base, 7), jump))
            assert targets == set(decoder.jump_table_targets(instruction)), hex(jump)
            comparisons = re.findall(r'/\* 0x([0-9A-F]{8}): cmp [^,]+, (0x[0-9a-f]+|[0-9]+) \*/', body[:match.start()-start])
            bound = [(a, int(n, 0)) for a, n in comparisons if int(n, 0) == count-1]
            assert bound, (hex(jump), 'no original count-1 comparison')
            compare = int(bound[-1][0], 16)
            instructions = list(cs.disasm(pe.get_data(compare-base, jump-compare), compare))
            assert instructions[0].mnemonic == 'cmp' and any(i.mnemonic == 'ja' for i in instructions)
            found[jump] = dict(jump=hex(jump), table=hex(table), entries=count,
                               bound_compare=hex(compare), unique_targets=len(targets),
                               source=source.relative_to(ROOT).as_posix(), all_targets_retained=True)
    assert len(found) == len(switches) == 5, len(found)
    out = ROOT/'build/test/large-switches'
    out.mkdir(parents=True, exist_ok=True)
    (out/'result.json').write_text(json.dumps(list(found.values()), indent=2)+'\n')
    print('PASS five original cmp/ja bounds, complete tables and generated labels:',
          ', '.join(str(s['entries']) for s in found.values()))


if __name__ == '__main__':
    main()
