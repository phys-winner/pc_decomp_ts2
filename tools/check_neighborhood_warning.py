"""Check the actual STR# resource: correct only the misleading Russian translation."""
from pathlib import Path
import struct
from build_overlay import GAME, patch_neighborhood_warning, rewrite_package
import dbpf

def entries(data):
    assert struct.unpack_from('<H', data, 64)[0] == 0xFFFD
    count = struct.unpack_from('<H', data, 66)[0]
    pos, rows = 68, []
    for _ in range(count):
        lang = data[pos]; pos += 1
        end = data.index(0, pos); text = data[pos:end].decode('utf-8'); pos = end + 1
        end = data.index(0, pos); comment = data[pos:end]; pos = end + 1
        rows.append((lang, text, comment))
    assert pos == len(data)
    return rows

if __name__ == '__main__':
    source = GAME / 'EP9/TSData/Res/Text/Neighborhood.package'
    package = dbpf.Package(source)
    entry = next(e for e in package.entries if (e['type'],e['group'],e['instance']) == (0x53545223,0xFFFFFFFF,205))
    original = package.read(entry); patched = patch_neighborhood_warning(original)
    before, after = entries(original), entries(patched)
    changed = [i for i in range(len(before)) if before[i] != after[i]]
    assert changed == [331] and before[331][0] == after[331][0] == 13
    assert before[331][2] == after[331][2] and original[:68] == patched[:68]
    assert 'установленных дополнений' in after[331][1] and 'еще не установлены' not in after[331][1]
    assert 'no associated neighborhoods' in after[23][1]
    for bad in (b'',patched,original+original):
        try: patch_neighborhood_warning(bad)
        except ValueError: pass
        else: raise AssertionError('missing or duplicate original accepted')
    print('PASS: original 616 localized STR# entries, exactly one Russian correction, English semantics and all other bytes preserved')
