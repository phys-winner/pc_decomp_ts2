"""Exercise deployment preflight and pack selection with a fake adb; no device."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BASH = Path(r'C:\Program Files\Git\bin\bash.exe')


def main():
    with tempfile.TemporaryDirectory(prefix='sims2-deploy-') as directory:
        base = Path(directory)
        (base / 'tools').mkdir()
        shutil.copyfile(ROOT / 'tools/deploy_apk.sh', base / 'tools/deploy_apk.sh')
        (base / 'build/android-game').mkdir(parents=True)
        with zipfile.ZipFile(base / 'build/android-game/sims2.apk', 'w') as archive:
            archive.writestr('classes.dex', b'test')
        binaries = base / 'game/EP9/TSBin'
        binaries.mkdir(parents=True)
        for name in ('Sims2EP9.exe', 'ijl15.dll'):
            (binaries / name).write_bytes(b'PE')
        for pack, size in [('Base', 2 * 1024**3), ('EP9', 1024**2)]:
            path = base / f'game/{pack}/TSData/data.package'
            path.parent.mkdir(parents=True)
            with path.open('wb') as stream:
                stream.truncate(size)
        fake = base / 'fake-adb.cmd'
        fake.write_text('@python "%~dp0fake-adb.py" %*\n', encoding='utf-8')
        (base / 'fake-adb.py').write_text('''
import json, os, sys
with open(os.environ['ADB_CALL_LOG'], 'a') as log:
    log.write(json.dumps(sys.argv[1:])+'\\n')
args=sys.argv[3:]
if args[:1]==['shell'] and args[1]=='df -Pk /sdcard':
    print('Filesystem 1024-blocks Used Available Capacity Mounted on')
    print('mock 999999999 0 '+os.environ['ADB_FREE_KIB']+' 0% /sdcard')
elif args[:1]==['shell'] and args[1].startswith('find '):
    print('2147483648 /sdcard/Sims2/game/Base/TSData/data.package')
else:
    print('Success')
''', encoding='utf-8')
        env = dict(os.environ, ADB=fake.as_posix(), S2_GAME=(base / 'game').as_posix(),
                   ADB_CALL_LOG=str(base / 'calls.jsonl'), ADB_FREE_KIB='1024')
        def run(*packs):
            (base / 'calls.jsonl').write_text('', encoding='utf-8')
            result = subprocess.run([str(BASH), str(base / 'tools/deploy_apk.sh'), 'test-device', *packs],
                                    env=env, text=True, capture_output=True)
            calls = [json.loads(line) for line in (base / 'calls.jsonl').read_text().splitlines()]
            return result, calls
        result, calls = run('Base', 'EP9')
        assert result.returncode and 'insufficient storage' in result.stderr, result
        assert calls and all(c[2] == 'shell' and (c[3].startswith('df ') or c[3].startswith('find ')) for c in calls), calls
        env['ADB_FREE_KIB'] = str(1024**2 + 4096)
        result, calls = run('Base', 'EP9')
        assert result.returncode == 0, result.stdout + result.stderr
        pushed = [c for c in calls if c[2] == 'push']
        assert len(pushed) == 4, pushed
        assert all('/lifted' not in ' '.join(c) for c in calls)
        assert any('/EP9/TSBin/Sims2EP9.exe' in ' '.join(c) for c in pushed)
        assert any('/Base/TSData' in c[-2] for c in pushed), pushed
        assert any('/EP9/TSData' in c[-2] for c in pushed), pushed
        result, calls = run('EP0')
        assert result.returncode and 'unknown pack' in result.stderr and not calls, result
        result, calls = run('EP1')
        assert result.returncode and 'missing game data' in result.stderr and not calls, result
        print('deploy_apk: insufficient storage, existing files, subset and invalid inputs PASS')


if __name__ == '__main__':
    main()
