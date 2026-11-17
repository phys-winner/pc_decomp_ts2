"""Build the actual-C Android camera/catalog probe; device exit0 is a separate gate."""
import hashlib
import json
import subprocess

import build_android as B


def main():
    out = B.ROOT / 'build/20261028-ui'
    out.mkdir(parents=True, exist_ok=True)
    binary = out / 'test-touch-camera'
    command = [str(B.CLANG), *B.HOST_FLAGS, '-Wl,--gc-sections', '-Wl,-z,max-page-size=16384',
               'tools/test_touch_camera.c', '-lm', '-o', str(binary)]
    sources = ['android/host/sims2_touch.c', 'android/host/sims2_settings.h', 'tools/test_touch_camera.c']
    hashes = {name: hashlib.sha256((B.ROOT / name).read_bytes()).hexdigest() for name in sources}
    subprocess.run(command, cwd=B.ROOT, check=True)
    assert hashes == {name: hashlib.sha256((B.ROOT / name).read_bytes()).hexdigest() for name in sources}
    result = dict(binary=str(binary), sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  sources=hashes, built=True, device_execution='pending')
    (out / 'camera-catalog-build.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__': main()
