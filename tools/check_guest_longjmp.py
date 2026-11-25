"""Run the production jump bridge with the original lifted MSVC CRT and Lua protection on Android."""
import argparse
import re
import subprocess

import build_android as B


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    args = parser.parse_args()
    out = B.ROOT / 'build/test/guest-longjmp'
    out.mkdir(parents=True, exist_ok=True)
    names = {'sims2_A0C48000.c': ['A0C49D20', 'A0C49D03', 'A0C49DC0'],
             'sims2_A0C78000.c': ['A0C7F740', 'A0C7F8B0']}
    source = b'#include "recomp_types.h"\nvoid void_lift_trap(uint32_t, const char *);\n'
    for filename, entries in names.items():
        original = (B.ROOT / 'generated/sims2-x86' / filename).read_bytes()
        for entry in entries:
            body = re.search(rb'\nvoid sub_' + entry.encode() + rb'\(void\) \{.*?\n\}', original, re.S)
            assert body is not None, entry
            source += body.group(0) + b'\n'
    declarations = b'\n'.join(b'void ' + name + b'(void);' for name in sorted(set(re.findall(rb'\bsub_[A-F0-9]+\b', source))))
    original = b'#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n' + declarations + b'\n' + source
    patched = b'#define RECOMP_GENERATED_CODE\n' + B.msvc_jumps(original)
    assert patched.count(b'RECOMP_MSVC_SETJMP(') == 1
    assert patched.count(b'void_guest_jmp_try_resume();') == 2
    for local in (False, True):
        crt = out / ('crt-local.c' if local else 'crt-global.c')
        crt.write_bytes(b'#define RECOMP_LOCAL_REGS\n' + B.localize_registers(patched) if local else patched)
        binary = out / ('check-local' if local else 'check-global')
        subprocess.run([str(B.CLANG), *B.COMMON, *B.EMULATED_TLS, '-O2', '-std=gnu11',
                        '-Iandroid/runtime', '-Iandroid/host', str(crt), 'android/host/guest_jmp.c',
                        'tools/fixtures/guest_longjmp.c', '-o', str(binary)], cwd=B.ROOT, check=True)
        remote = '/data/local/tmp/sims2-check-jump-' + ('local' if local else 'global')
        adb = ['adb', '-s', args.serial]
        subprocess.run(adb + ['push', str(binary), remote], capture_output=True, check=True)
        subprocess.run(adb + ['shell', 'chmod 755 ' + remote], check=True)
        result = subprocess.run(adb + ['shell', remote], capture_output=True, text=True)
        (out / (binary.name + '.log')).write_text(result.stdout + result.stderr)
        print(result.stdout, end='')
        assert result.returncode == 0, result.stderr
        stale = subprocess.run(adb + ['shell', remote + ' stale'], capture_output=True, text=True)
        assert stale.returncode == 73 and 'without a live lifted caller' in stale.stderr, stale
        subprocess.run(adb + ['shell', 'rm ' + remote], check=True)
    # The same real Lua throw must fail without the bridge; otherwise this test misses the bug.
    baseline = out / 'crt-before.c'
    baseline.write_bytes(original)
    binary = out / 'check-before'
    subprocess.run([str(B.CLANG), *B.COMMON, *B.EMULATED_TLS, '-O2', '-std=gnu11',
                    '-Iandroid/runtime', '-Iandroid/host', str(baseline), 'android/host/guest_jmp.c',
                    'tools/fixtures/guest_longjmp.c', '-o', str(binary)], cwd=B.ROOT, check=True)
    remote = '/data/local/tmp/sims2-check-jump-before'
    subprocess.run(adb + ['push', str(binary), remote], capture_output=True, check=True)
    subprocess.run(adb + ['shell', 'chmod 755 ' + remote], check=True)
    before = subprocess.run(adb + ['shell', remote], capture_output=True, text=True)
    (out / 'check-before.log').write_text(before.stdout + before.stderr)
    assert before.returncode != 0 and 'PASS' not in before.stdout, before
    subprocess.run(adb + ['shell', 'rm ' + remote], check=True)
    print('PASS negative control: the original unbridged Lua throw fails')
    # Pin the integration to every original site, including non-Lua libraries.
    sites = tails = 0
    for path in (B.ROOT / 'generated/sims2-x86').glob('*.c'):
        original = path.read_bytes()
        updated = B.msvc_jumps(original)
        count = original.count(b'RECOMP_CALL(sub_A0C49D20)')
        assert updated.count(b'RECOMP_MSVC_SETJMP(') == count, path
        sites += count
        tails += updated.count(b'void_guest_jmp_try_resume();')
    assert sites == 34 and tails == 2, (sites, tails)
    print(f'PASS integration: {sites} original MSVC setjmp sites, {tails} longjmp epilogue')


if __name__ == '__main__':
    main()
