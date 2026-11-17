"""Actual Git fixture: initial patch, idempotence, dirty-source and revision guards."""
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from unittest.mock import patch
import prepare_dependencies as dependencies


with tempfile.TemporaryDirectory(prefix='s2-dependencies-') as directory:
    root = Path(directory)
    source = root / 'reference/test'
    source.mkdir(parents=True)
    subprocess.run(['git', 'init', '-q', str(source)], check=True)
    (source / 'file.txt').write_text('before\n', encoding='ascii')
    subprocess.run(['git', '-C', str(source), 'add', 'file.txt'], check=True)
    subprocess.run(['git', '-C', str(source), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                    '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Fixture'], check=True)
    revision = dependencies.git(source, 'rev-parse', 'HEAD')
    (root / 'test.patch').write_text('--- a/file.txt\n+++ b/file.txt\n@@ -1 +1 @@\n-before\n+after\n', encoding='ascii')
    spec = dict(submodule=True, directory='reference/test', revision=revision, patch='test.patch',
                patched_files={'file.txt': hashlib.sha256(b'after\n').hexdigest()})
    (root / 'dependencies.json').write_text(json.dumps({'test': spec}), encoding='utf-8')
    with patch.object(dependencies, 'ROOT', root):
        dependencies.prepare('test'); dependencies.prepare('test')
        assert (source / 'file.txt').read_text() == 'after\n'
        (source / 'file.txt').write_text('my edit\n', encoding='ascii')
        try: dependencies.prepare('test')
        except SystemExit: pass
        else: raise AssertionError('Unreviewed source edit accepted')
        assert (source / 'file.txt').read_text() == 'my edit\n'
        spec['revision'] = '0' * 40
        (root / 'dependencies.json').write_text(json.dumps({'test': spec}), encoding='utf-8')
        try: dependencies.prepare('test')
        except SystemExit: pass
        else: raise AssertionError('Wrong submodule revision accepted')
print('PASS source submodule pins, initial patch, repeat preparation and preservation of unexpected edits')
