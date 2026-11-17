"""Build the standalone Android GPU ProcessVertices regression (no adb).

Build and replay (run chmod after EVERY executable push, including rebuilds):

    python tools/check_process_vertices.py
    python tools/build_process_probe.py
    adb -s abb24fae shell mkdir -p /data/local/tmp/sims2
    adb -s abb24fae push build/test/processvertices/regression /data/local/tmp/sims2/
    adb -s abb24fae push build/test/processvertices/gpu/process_vertices_probe /data/local/tmp/sims2/process_vertices_probe
    adb -s abb24fae shell chmod 755 /data/local/tmp/sims2/process_vertices_probe
    adb -s abb24fae shell /data/local/tmp/sims2/process_vertices_probe /data/local/tmp/sims2/regression /data/local/tmp/sims2/pv-results
    adb -s abb24fae pull /data/local/tmp/sims2/pv-results build/test/
    python tools/check_process_vertices.py --actual-dir build/test/pv-results

The probe compares all four fixtures itself and saves complete CPU/GPU
destination files. It requires no app restart and does not modify game data.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

import build_android as android

ROOT = android.ROOT
OUT = ROOT / 'build/test/processvertices/gpu'


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [('probe', 'android/tools/process_vertices_probe.c', android.HOST_FLAGS),
               ('gl', 'android/host/d3d9_gl.c', android.HOST_FLAGS)]
    sources += [(f'mojo_{i}', f'{android.MOJO_DIR}/{name}', android.MOJO_FLAGS)
                for i, name in enumerate(android.MOJO_SOURCES)]

    def compile_source(source):
        name, path, flags = source
        obj = OUT / f'{name}.o'
        subprocess.run([str(android.CLANG), *flags, '-fdata-sections', '-c', path, '-o', str(obj)],
                       cwd=ROOT, check=True)
        return obj

    with ThreadPoolExecutor(max_workers=6) as pool:
        objects = list(pool.map(compile_source, sources))
    exe = OUT / 'process_vertices_probe'
    subprocess.run([str(android.CLANG), *android.COMMON, '-Wl,--gc-sections',
                    *map(str, objects), '-lEGL', '-lGLESv3', '-llog', '-lm', '-o', str(exe)],
                   cwd=ROOT, check=True)
    print(f'Android GPU probe built: {exe}')
    print('GPU execution not run; root owns the adb device.')


if __name__ == '__main__':
    main()
