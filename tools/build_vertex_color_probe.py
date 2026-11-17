"""Build the Android GPU vertex-color regression; no device actions.

    python tools/build_vertex_color_probe.py
    adb -s abb24fae push build/test/vertex-color/vertex_color_probe /data/local/tmp/vertex_color_probe
    adb -s abb24fae shell chmod 755 /data/local/tmp/vertex_color_probe
    adb -s abb24fae shell /data/local/tmp/vertex_color_probe

The test assembles the game's exact UI shader and requires 105 pixel readbacks
to match logical RGBA across D3DCOLOR, UBYTE4N, FLOAT4 and shader models 1–3.
Run chmod after every push, including a replacement executable.
"""
from concurrent.futures import ThreadPoolExecutor
import subprocess

import build_android as android

OUT = android.ROOT / 'build/test/vertex-color'


def build_probe(source='android/tools/vertex_color_probe.c', output=OUT, name='vertex_color_probe'):
    output.mkdir(parents=True, exist_ok=True)
    sources = [('probe', source, android.HOST_FLAGS)]
    sources += [(f'mojo_{i}', f'{android.MOJO_DIR}/{name}', android.MOJO_FLAGS)
                for i, name in enumerate(android.MOJO_SOURCES)]

    def compile_source(source):
        name, path, flags = source
        obj = output / f'{name}.o'
        subprocess.run([str(android.CLANG), *flags, '-fdata-sections', '-c', path, '-o', str(obj)],
                       cwd=android.ROOT, check=True)
        return obj

    with ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile_source, sources))
    exe = output / name
    subprocess.run([str(android.CLANG), *android.COMMON, '-Wl,--gc-sections',
                    '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384',
                    *map(str, objects), '-lEGL', '-lGLESv3', '-lm', '-o', str(exe)],
                   cwd=android.ROOT, check=True)
    print(f'Android GPU probe built: {exe}')
    print('GPU execution not run; root owns the adb device.')


def main():
    build_probe()


if __name__ == '__main__':
    main()
