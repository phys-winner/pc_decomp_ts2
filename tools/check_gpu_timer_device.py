"""Build an isolated API29 EGL pbuffer timer/pixel probe; optional explicit device run."""
import argparse
import json
import subprocess
import build_android as B


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    out = B.ROOT / 'build/test/gpu-timer-device'
    out.mkdir(parents=True, exist_ok=True)
    source = B.ROOT / 'android/tools/gpu_timer_check.c'
    binary = out / 'probe'
    command = [str(B.CLANG), '--target=aarch64-linux-android29', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
               str(source), '-o', str(binary), '-lEGL', '-lGLESv3']
    subprocess.run(command, cwd=B.ROOT, check=True)
    receipt = {'status': 'BUILT', 'api': 29, 'command': command, 'source_sha256': B.sha_file(source), 'binary_sha256': B.sha_file(binary),
               'scope': 'Separate-process 64x64 EGL pbuffer, core/EXT timer APIs, 32/64-bit results, monotonic upper bound, exact alternating pixels; no game or Android window acceptance.'}
    if args.serial:
        adb = ['adb', '-s', args.serial]
        remote = '/data/local/tmp/sims2-gpu-timer-check'
        subprocess.run(adb + ['push', str(binary), remote], check=True)
        subprocess.run(adb + ['shell', 'chmod', '755', remote], check=True)
        run = subprocess.run(adb + ['shell', remote], text=True, capture_output=True, timeout=60)
        (out / 'device.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        assert run.returncode == 0 and 'DONE pixel_checks=16' in run.stdout, run.stdout + run.stderr
        receipt.update(status='CAPTURED', serial=args.serial)
        print(run.stdout)
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
