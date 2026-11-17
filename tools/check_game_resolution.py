"""Build the standalone actual-C UI-resolution probe for 4/16 KiB Android.

This verifies ELF loading constraints locally. Only an isolated on-device
execution with exit0 establishes that the probe itself passes on hardware.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from elftools.elf.elffile import ELFFile

import build_android as B

OUT = B.ROOT / 'build/test/ui-scale'


def layout(path):
    with path.open('rb') as source:
        elf = ELFFile(source)
        return [dict(offset=int(p['p_offset']), address=int(p['p_vaddr']),
                     file_bytes=int(p['p_filesz']), memory_bytes=int(p['p_memsz']),
                     alignment=int(p['p_align']), flags=int(p['p_flags']))
                for p in elf.iter_segments() if p['p_type'] == 'PT_LOAD']


def require_16k(loads):
    assert loads, 'no ELF PT_LOAD segments'
    end = 0
    for p in loads:
        assert p['alignment'] >= 16384, p
        assert p['offset'] % 16384 == p['address'] % 16384, p
        begin = p['address'] & ~16383
        assert begin >= end, 'PT_LOAD segments share a virtual 16 KiB page'
        end = (p['address'] + p['memory_bytes'] + 16383) & ~16383


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    binary = OUT / 'check-game-resolution'
    before = OUT / 'check-game-resolution.before-4k'
    if binary.exists() and not before.exists():
        shutil.copyfile(binary, before)
    command = [str(B.CLANG), *B.HOST_FLAGS, '-Wl,--gc-sections',
               '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
               str(B.ROOT / 'tools/test_game_resolution.c'), '-o', str(binary)]
    sources = [B.ROOT / name for name in ('tools/test_game_resolution.c', 'android/host/sims2_settings.c', 'android/host/sims2_settings.h')]
    hashes = {str(p.relative_to(B.ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    build = subprocess.run(command, cwd=B.ROOT, capture_output=True, text=True)
    (OUT / 'build-game-resolution.log').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        raise RuntimeError('probe compilation failed: ' + build.stderr)
    assert hashes == {str(p.relative_to(B.ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}, 'probe sources changed during compilation; rebuild after owner freeze'
    loads = layout(binary)
    require_16k(loads)
    old_loads = layout(before) if before.exists() else None
    old_valid = None
    if old_loads:
        try:
            require_16k(old_loads)
            old_valid = True
        except AssertionError:
            old_valid = False
    with binary.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    result = dict(binary=str(binary), bytes=binary.stat().st_size, sha256=digest, source_sha256=hashes,
                  build_command=command, elf_16k_layout_pass=True, load_segments=loads,
                  previous_binary=str(before) if before.exists() else None,
                  previous_16k_layout_pass=old_valid, previous_load_segments=old_loads,
                  executed_on_device=False, hardware_result='pending isolated execution and exit status',
                  scope='test-only probe build; no production source or device operation')
    (OUT / 'game-resolution-build.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('PASS local ELF alignment/congruence/non-overlap checks; on-device execution remains pending:', binary)


if __name__ == '__main__':
    main()
