"""Build the DXT->ATC transcoder check (android/tools/atc_check.c with the
production android/host/d3d9_atc.c) and, with --serial, run it on a device:
the GPU's ATC decode must equal the transcoder's model texel for texel, and the
colour error against the old RGBA8 decode stays within the printed bounds.
    python -X utf8 tools/check_atc.py [--serial SERIAL]
Without a device it only builds (the GPU half needs real ATC hardware, Adreno)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import build_android as B

ADB = os.environ.get('ADB', str(Path(os.environ.get('ANDROID_SDK', Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk'))
                                / 'platform-tools/adb.exe'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default=os.environ.get('S2_SERIAL'))
    args = parser.parse_args()
    out = B.ROOT / 'build/test/atc'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'atc_check'
    sources = ['android/tools/atc_check.c', 'android/host/d3d9_atc.c', 'android/host/d3d9_atc.h']
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-std=gnu11', '-Wall', '-Werror', '-Wno-unused-function',
                    '-Iandroid/runtime', '-Iandroid/host', '-Wl,-z,max-page-size=16384', 'android/tools/atc_check.c',
                    'android/host/d3d9_atc.c', '-o', str(binary), '-lEGL', '-lGLESv3', '-lm'], cwd=B.ROOT, check=True)
    result = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  sources={n: hashlib.sha256((B.ROOT / n).read_bytes()).hexdigest() for n in sources})
    if args.serial:
        remote = '/data/local/tmp/sims2/atc_check'
        subprocess.run([ADB, '-s', args.serial, 'push', str(binary), remote], check=True, capture_output=True)
        run = subprocess.run([ADB, '-s', args.serial, 'shell', f'chmod 755 {remote} && {remote}; echo exit=$?'],
                             capture_output=True, text=True)
        print(run.stdout)
        result['device'] = run.stdout.strip().splitlines()
        result['passed'] = run.stdout.rstrip().endswith('exit=0')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.serial and not result['passed']:
        raise SystemExit('ATC check failed on the device')
    print('PASS' if args.serial else 'built (run with --serial on an Adreno device)')


if __name__ == '__main__':
    main()
