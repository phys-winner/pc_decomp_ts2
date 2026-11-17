"""Verify pinned source submodules and apply the port's reviewed patches once."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def git(folder, *args):
    return subprocess.check_output(['git', '-C', str(folder), *args], text=True).strip()


def prepare(*names):
    dependencies = json.loads((ROOT / 'dependencies.json').read_text(encoding='utf-8'))
    for name in names or [n for n, d in dependencies.items() if d.get('submodule')]:
        spec = dependencies[name]
        folder = ROOT / spec['directory']
        if not (folder / '.git').exists():
            raise SystemExit(f'{name}: run git submodule update --init --recursive')
        if Path(git(folder, 'rev-parse', '--show-toplevel')).resolve() != folder.resolve():
            raise SystemExit(f'{name}: expected a source submodule at {folder}')
        if git(folder, 'rev-parse', 'HEAD') != spec['revision']:
            raise SystemExit(f'{name}: submodule revision differs from dependencies.json')
        expected = spec.get('patched_files', {})
        matches = all(hashlib.sha256((folder / p).read_bytes().replace(b'\r\n', b'\n')).hexdigest() == h
                      for p, h in expected.items())
        changed = set(git(folder, 'diff', 'HEAD', '--name-only', '--ignore-submodules=none').splitlines())
        if changed - expected.keys() or (changed and not matches):
            raise SystemExit(f'{name}: unexpected local source changes; preserve them before preparing dependencies')
        if not matches:
            patch = str(ROOT / spec['patch'])
            subprocess.run(['git', '-C', str(folder), 'apply', '--check', patch], check=True)
            subprocess.run(['git', '-C', str(folder), 'apply', patch], check=True)
            if not all(hashlib.sha256((folder / p).read_bytes().replace(b'\r\n', b'\n')).hexdigest() == h
                       for p, h in expected.items()):
                raise SystemExit(f'{name}: patched source hash mismatch')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('names', nargs='*')
    prepare(*parser.parse_args().names)
    print('Pinned submodules and port patches verified.')
