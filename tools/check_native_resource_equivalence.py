"""Tie the resource probe to production code and verify the unchanged OFF route."""
import argparse
import hashlib
import json
from pathlib import Path
import re

import build_android as B
from check_native_leaf_equivalence import body


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measured', type=Path, default=B.ROOT / 'build/test/native-resource-enumeration/normal')
    parser.add_argument('--production-helper', type=Path, default=B.OUT / 'obj/host-app/sims2_resource.o')
    parser.add_argument('--production-clone', type=Path, default=B.OUT / 'obj/native-generated/sims2_resource_enum.o')
    parser.add_argument('--original', type=Path, default=B.OUT / 'obj/sims2-x86/sims2_A09A8000.o')
    parser.add_argument('--original-before', type=Path, default=B.ROOT / 'build/20261109-poco/resource-original-v9.o')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    comparisons = []
    for measured, production, symbol in (
        (args.measured / 'skip.o', args.production_helper, 's2_resource_skip'),
        (args.measured / 'skip.o', args.production_helper, 'sims2_resource_enumerate'),
        (args.measured / 'patched.o', args.production_clone, 'sub_A09AC040_native'),
    ):
        expected, actual = body(measured, symbol), body(production, symbol)
        assert expected == actual, symbol + ': production instructions or relocations differ from the probe'
        comparisons.append({'symbol': symbol, 'bytes': actual['size'],
                            'measured_object': str(measured), 'production_object': str(production),
                            'measured_sha256': digest(measured), 'production_sha256': digest(production)})
    assert args.original.read_bytes() == args.original_before.read_bytes(), 'OFF original object changed'
    assert 'sub_A09AC040' not in B.NATIVE_OVERRIDES, 'OFF must retain its original PGO symbol'
    direct = re.compile(rb'RECOMP_CALL\s*\(\s*sub_A09AC040\s*\)')
    direct_sources = [str(p.relative_to(B.ROOT)) for p in B.generated_sources()
                      if p.name != 'recomp_dispatch.c' and direct.search(p.read_bytes())]
    assert not direct_sources, 'unrouted direct calls: ' + ', '.join(direct_sources)
    dispatch_source = B.GENERATED / 'dispatch/recomp_dispatch.c'
    dispatch, _ = B.override_source(dispatch_source, True)
    content = dispatch.read_bytes()
    assert content.count(b'{0xA09AC040u, sims2_resource_enumerate}') == 1
    assert b'{0xA09AC040u, sub_A09AC040}' not in content
    result = {
        'status': 'PASS', 'comparisons': comparisons,
        'original_object_sha256': digest(args.original),
        'original_before_object': str(args.original_before),
        'original_function_bytes': body(args.original, 'sub_A09AC040')['size'],
        'direct_generated_calls': direct_sources,
        'guest_va_dispatch': '0xA09AC040 -> sims2_resource_enumerate',
        'limits': 'Object/relocation identity and generated dispatch routing; physical route counters and cold whole-game timings remain separate evidence.',
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PASS resource probe/production code, unchanged OFF object and guest dispatch route')


if __name__ == '__main__':
    main()
