"""Build the isolated Snowapril/AMD FSR 3.1.4 Vulkan module (loaded on demand)."""
import hashlib
import json
from pathlib import Path
import subprocess
import struct
import tarfile
import urllib.request

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'reference/fsr3-android'
REVISION='3a369b3d2d3734de0f6e7aa84084879fe7f70535'
ARCHIVE_SHA='0b439a84f3031fdafd03549a4f7f2d408524c376f2fc1f7314c2dc5d07feee6a'
ARCHIVE_URL='https://github.com/Snowapril/fsr3-android/releases/download/v0.1.0/fsr3-android-arm64-static-libs.tar.gz'
OUT=ROOT/'build/fsr3-android-sdk'

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def optical_flow_shader():
    original=SOURCE/'sdk/include/FidelityFX/gpu/opticalflow/ffx_opticalflow_compute_optical_flow_v5.h'
    if sha(original)!='642a93f8de16223e5ba0a9d604d60e7f0febfbd95d2259df3cee65c846eeb050':
        raise ValueError('Optical flow reduction preimage changed')
    text=original.read_text().replace('#define MaxWaves 2','#define MaxWaves 4')
    text=text.replace('if (ffxWaveLaneCount() == 32)','if (ffxWaveLaneCount() < 64)')
    text=text.replace('FfxInt32 waveId = iLocalIndex >> 5u;','FfxInt32 waveId = iLocalIndex / FfxInt32(ffxWaveLaneCount());')
    text=text.replace('blockSadSum += sWaveSad[waveId ^ 1];',
        'blockSadSum = 0u;\n        for (FfxUInt32 wave = 0u; wave < 64u / ffxWaveLaneCount(); ++wave) blockSadSum += sWaveSad[wave];\n        FFX_GROUP_MEMORY_BARRIER;')
    text=text.replace('min0123 = ffxMin(min0123, sWaveMin[waveId ^ 1]);',
        'for (FfxUInt32 wave = 0u; wave < 64u / ffxWaveLaneCount(); ++wave) min0123 = ffxMin(min0123, sWaveMin[wave]);\n        FFX_GROUP_MEMORY_BARRIER;')
    patched=OUT/'shaders/opticalflow'/original.name;patched.parent.mkdir(parents=True,exist_ok=True);patched.write_text(text)
    shader=SOURCE/'sdk/src/backends/vk/shaders/opticalflow/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl'
    compiler=Path.home()/'AppData/Local/Android/Sdk/ndk/27.2.12479018/shader-tools/windows-x86_64/glslc.exe';spv=OUT/'optical-flow-wave16.spv'
    subprocess.run([str(compiler),'--target-env=vulkan1.2','-fshader-stage=compute','-O','-DFFX_GLSL=1','-DFFX_GPU=1',
        '-I'+str(OUT/'shaders'),'-I'+str(SOURCE/'sdk/include/FidelityFX/gpu'),str(shader),'-o',str(spv)],check=True)
    words=struct.unpack('<'+'I'*(spv.stat().st_size//4),spv.read_bytes())
    (OUT/'optical_flow_wave16.h').write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t fpOpticalFlowWave16[]={'
        +','.join(hex(w) for w in words)+'};\n')
    return [original,patched,shader,compiler,spv,OUT/'optical_flow_wave16.h']

def main():
    actual=subprocess.check_output(['git','-C',str(SOURCE),'rev-parse','HEAD'],text=True).strip()
    if actual!=REVISION:raise ValueError('Unexpected FSR 3 source revision: '+actual)
    if subprocess.check_output(['git','-C',str(SOURCE),'status','--porcelain','--untracked-files=no'],text=True).strip():
        raise ValueError('FSR 3 reference source must remain pristine')
    OUT.mkdir(parents=True,exist_ok=True)
    shader_inputs=optical_flow_shader()
    compiler=Path.home()/'AppData/Local/Android/Sdk/ndk/27.2.12479018/shader-tools/windows-x86_64/glslc.exe'
    prepare=OUT/'prepare.spv'
    subprocess.run([str(compiler),'--target-env=vulkan1.2','-fshader-stage=compute','-O',str(ROOT/'android/fsr3/prepare.comp'),'-o',str(prepare)],check=True)
    words=struct.unpack('<'+'I'*(prepare.stat().st_size//4),prepare.read_bytes())
    (OUT/'prepare_spv.h').write_text('#pragma once\n#include <cstdint>\nstatic const uint32_t s2Prepare[]={'+','.join(hex(w) for w in words)+'};\n')
    shader_inputs += [ROOT/'android/fsr3/prepare.comp',prepare,OUT/'prepare_spv.h']

    archive=ROOT/'reference/fsr3-android-arm64-static-libs.tar.gz'
    if not archive.exists():
        cached=ROOT/'build/mobile-20261104'/archive.name
        archive.write_bytes(cached.read_bytes() if cached.exists() else urllib.request.urlopen(ARCHIVE_URL,timeout=60).read())
    if sha(archive)!=ARCHIVE_SHA:raise ValueError('FSR 3 archive checksum mismatch')
    with tarfile.open(archive) as tar:tar.extractall(OUT,filter='data')
    libraries=sorted((OUT/'fsr3-android-arm64-libs').glob('lib*.a'))
    if len(libraries)!=5:raise ValueError('Expected five pinned SDK archives')
    import os
    sdk=Path(os.environ.get('ANDROID_SDK_ROOT',Path.home()/'AppData/Local/Android/Sdk'))
    compiler=sdk/'ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/clang++.exe'
    binary=OUT/'libs2fsr3.so'
    command=[str(compiler),'--target=aarch64-linux-android35','-O2','-std=c++17','-fPIC','-shared','-fvisibility=hidden',
             '-static-libstdc++','-Wl,--exclude-libs,ALL','-Wl,--no-undefined','-Wl,-z,max-page-size=16384',
             '-Wl,-soname,libs2fsr3.so','-I'+str(SOURCE/'sdk/include'),'-I'+str(OUT),
             str(ROOT/'android/fsr3/fsr3_android.cpp'),str(ROOT/'android/fsr3/interop.cpp'),'-Wl,--start-group',*map(str,libraries),'-Wl,--end-group',
             '-lvulkan','-landroid','-llog','-o',str(binary)]
    subprocess.run(command,check=True)
    inputs=[ROOT/'android/fsr3/fsr3_api.h',ROOT/'android/fsr3/fsr3_android.cpp',ROOT/'android/fsr3/interop.cpp',Path(__file__),compiler,*libraries,*shader_inputs]
    receipt=dict(source_revision=REVISION,archive_url=ARCHIVE_URL,archive_sha256=ARCHIVE_SHA,
                 android_api=35,frame_generation=False,output=str(binary.relative_to(ROOT)),sha256=sha(binary),
                 inputs={str(p):sha(p) for p in inputs})
    (OUT/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))

if __name__=='__main__':main()
