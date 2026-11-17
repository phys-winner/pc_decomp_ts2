"""Build the program-cache housekeeping check: program_cache_housekeeping and its
helpers are cut out of android/host/d3d9_shader.c unchanged into
build/test/program-housekeeping/housekeeping.inc and compiled with
android/tools/progcache_check.c; with --serial it runs on the device in a
scratch folder under /data/local/tmp/sims2, which it removes afterwards.
--mutate NAME runs it on a deliberately broken copy (no-wipe, follow-links,
no-limit) to show that the check catches that break.
    python -X utf8 tools/check_program_housekeeping.py [--serial SERIAL] [--mutate NAME]"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

import build_android as B

ADB = os.environ.get('ADB', str(Path(os.environ.get('ANDROID_SDK', Path(os.environ.get('LOCALAPPDATA', '')) / 'Android/Sdk'))
                                / 'platform-tools/adb.exe'))
BEGIN = "/* program_cache_path's names"
END = '\nint d3d_program_cache_on(void)\n'
MUTATIONS = {
    'no-wipe': ('int wipe = known && strcmp(seen, want) != 0;', 'int wipe = 0;'),
    'follow-links': ('fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) ||\n            !S_ISREG(st.st_mode)',
                     'fstatat(dirfd(d), e->d_name, &st, 0)'),
    'no-limit': ('if (kept > PROGRAM_FOLDER_LIMIT) {', 'if (0) {'),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', default=os.environ.get('S2_SERIAL'))
    parser.add_argument('--mutate', choices=sorted(MUTATIONS))
    args = parser.parse_args()
    out = B.ROOT / 'build/test/program-housekeeping' / (args.mutate or 'production')
    out.mkdir(parents=True, exist_ok=True)
    source = (B.ROOT / 'android/host/d3d9_shader.c').read_text(encoding='utf-8').replace('\r\n', '\n')
    block = source[source.index(BEGIN):source.index(END)]
    if args.mutate:
        old, new = MUTATIONS[args.mutate]
        if old not in block:
            raise SystemExit(f'mutation {args.mutate}: its target text is not in the source any more')
        block = block.replace(old, new)
    (out / 'housekeeping.inc').write_text(block, encoding='utf-8')
    binary = out / 'progcache_check'
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-std=gnu11', '-Wall', '-Werror', f'-I{out}',
                    '-Wl,-z,max-page-size=16384', 'android/tools/progcache_check.c', '-o', str(binary)],
                   cwd=B.ROOT, check=True)
    result = dict(binary=str(binary.relative_to(B.ROOT)), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  mutation=args.mutate,
                  sources={n: hashlib.sha256((B.ROOT / n).read_bytes()).hexdigest()
                           for n in ('android/tools/progcache_check.c', 'android/host/d3d9_shader.c')})
    if args.serial:
        remote, folder = '/data/local/tmp/sims2/progcache_check', '/data/local/tmp/sims2/progcache-test'
        subprocess.run([ADB, '-s', args.serial, 'push', str(binary), remote], check=True, capture_output=True)
        run = subprocess.run([ADB, '-s', args.serial, 'shell',
                              f'rm -rf {folder} {folder}.target; chmod 755 {remote} && {remote} {folder}; echo exit=$?; '
                              f'rm -rf {folder} {folder}.target {remote}'],
                             capture_output=True, text=True, timeout=300)
        print(run.stdout)
        result['device'] = run.stdout.strip().splitlines()
        result['passed'] = run.stdout.rstrip().endswith('exit=0')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if args.serial and not result['passed'] and not args.mutate:
        raise SystemExit('program cache housekeeping check failed on the device')
    if args.serial and args.mutate:
        print('mutation caught' if not result['passed'] else 'MUTATION NOT CAUGHT')
        if result['passed']:
            raise SystemExit(1)
    if not args.serial:
        print('built (run with --serial on a device)')


if __name__ == '__main__':
    main()
