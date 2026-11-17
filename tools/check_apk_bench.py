"""Check uncapping and restoration after a failed benchmark, without a device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASH = Path(r'C:\Program Files\Git\bin\bash.exe')


def main():
    with tempfile.TemporaryDirectory(prefix='sims2-bench-') as directory:
        base = Path(directory)
        (base / 'tools').mkdir()
        shutil.copyfile(ROOT / 'tools/s2_apk_bench.sh', base / 'tools/s2_apk_bench.sh')
        (base / 'tools/s2.sh').write_text('echo stopped > "$BENCH_STOP_FILE"\n')
        (base / 'tools/s2_bench.sh').write_text('''python - <<'PY'
import os, xml.etree.ElementTree as ET
root=ET.parse(os.environ['BENCH_PROPS']).getroot()
caps=[e.text for e in root if e.get('key')=='s2FrameRateLimit']
assert caps==['0'],caps
assert root.find("./AnyUint32[@key='s2UIScale']").text=='200'
PY
exit 23
''')
        props = base / 'props.xml'
        original = ('<cGZPropertySetString><AnyUint32 key="s2FrameRateLimit" type="0xeb61e4f7">60</AnyUint32>'
                    '<AnyUint32 key="s2UIScale" type="0xeb61e4f7">200</AnyUint32>'
                    '<AnyUint32 type="0xeb61e4f7" key="s2FrameRateLimit">90</AnyUint32></cGZPropertySetString>')
        props.write_text(original)
        fake = base / 'fake-adb.cmd'
        fake.write_text('@python "%~dp0fake-adb.py" %*\n')
        (base / 'fake-adb.py').write_text('''import os, shutil, sys
from pathlib import Path
path=Path(os.environ['BENCH_PROPS']); backup=path.with_suffix('.backup')
command=sys.argv[4]
if command.startswith('dumpsys '): print('ACTIVITY org.sims2.port/.GameActivity')
elif command.startswith('test ! -e '): shutil.copyfile(path,backup)
elif command.startswith('cat > '): path.write_text(sys.stdin.read())
elif command.startswith('cat '): print(path.read_text(),end='')
elif command.startswith('mv -f '): shutil.move(backup,path)
else: raise AssertionError(command)
''')
        env = dict(os.environ, ADB=fake.as_posix(), S2_SERIAL='test-device',
                   BENCH_PROPS=str(props), BENCH_STOP_FILE=(base / 'stopped').as_posix())
        result = subprocess.run([str(BASH), str(base / 'tools/s2_apk_bench.sh'), 'nhood'],
                                env=env, text=True, capture_output=True)
        assert result.returncode == 23, result.stdout + result.stderr
        assert props.read_text() == original, 'original properties were not restored'
        assert (base / 'stopped').is_file(), 'app cleanup was skipped'
        assert not props.with_suffix('.backup').exists(), 'backup was left after restoration'
        print('s2_apk_bench: duplicate caps removed, unrelated values preserved, failure restore/stop PASS')


if __name__ == '__main__':
    main()
