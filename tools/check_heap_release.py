"""Build the guest-heap check (android/tools/heap_check.c, which compiles the
production android/host/memory.c into itself) and, with --serial, run it on a
device: guest_sys_free returns every allocation dlmalloc unmaps, in each shape
(a top trimmed across allocations, a segment glued from neighbours, a range
into free pages refused); a heap whose segments dlmalloc glued holds nothing
beyond its footprint after trims and returns everything on HeapDestroy; a
random HeapAlloc/HeapReAlloc/HeapFree load keeps every live block's bytes,
dlmalloc's own corruption checks stay silent, freed large blocks give their
pages back and destroying the heap returns every page and reservation.
--memory FILE builds against another memory.c (one before a fix) to show that
the checks fail on it.
    python -X utf8 tools/check_heap_release.py [--serial SERIAL] [--memory FILE]"""
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
    parser.add_argument('--memory', type=Path, help='memory.c to build against instead of android/host/memory.c')
    args = parser.parse_args()
    out = B.ROOT / ('build/test/heap-release-override' if args.memory else 'build/test/heap-release')
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'heap_check'
    memory = args.memory.resolve() if args.memory else B.ROOT / 'android/host/memory.c'
    # A memory.c outside android/host still finds host.h and dlmalloc through -Iandroid/host.
    override = [f'-DMEMORY_C="{memory.as_posix()}"'] if args.memory else []
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-std=gnu11', '-Wall', '-Wno-unused-function',
                    '-Wno-unused-variable', '-Wno-unused-but-set-variable', '-Iandroid/runtime', '-Iandroid/host',
                    *override, '-Wl,-z,max-page-size=16384', 'android/tools/heap_check.c', '-o', str(binary)],
                   cwd=B.ROOT, check=True)
    sources = {'android/tools/heap_check.c': B.ROOT / 'android/tools/heap_check.c',
               'android/host/memory.c' if not args.memory else str(memory): memory,
               'android/host/host.h': B.ROOT / 'android/host/host.h'}
    result = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  sources={n: hashlib.sha256(p.read_bytes()).hexdigest() for n, p in sources.items()})
    if args.serial:
        remote = '/data/local/tmp/sims2/heap_check'
        subprocess.run([ADB, '-s', args.serial, 'push', str(binary), remote], check=True, capture_output=True)
        run = subprocess.run([ADB, '-s', args.serial, 'shell', f'chmod 755 {remote} && {remote}; echo exit=$?'],
                             capture_output=True, text=True, timeout=900)
        print(run.stdout[-3000:])
        result['device'] = run.stdout.strip().splitlines()[-12:]
        result['passed'] = run.stdout.rstrip().endswith('exit=0')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.serial and not result['passed']:
        raise SystemExit('heap check failed on the device')
    print('PASS' if args.serial else 'built (run with --serial on a device)')


if __name__ == '__main__':
    main()
