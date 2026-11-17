"""Test the production DXT/ATI block copy against its original pixel scatter.

    python tools/check_texture_decode.py
    python tools/check_texture_decode.py --benchmark

Extracts the actual production DXT/ATI decoder, compares its output-copy
change against the original scatter and an independent Python reference, and checks the same C
on AArch64 under Unicorn. Optional timings are desktop CPU microbenchmarks;
they do not measure Android loading time. Artifacts live under build/test.
"""
import argparse
import ctypes
import io
import json
from pathlib import Path
import random
import struct
import subprocess
import time

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn import arm64_const as A

from build_android import CLANG, COMMON, ROOT

OUT = ROOT / 'build/test/texture-decode'
FAST_COPY = '''            if (bx * 4 + 4 <= width && by * 4 + 4 <= height &&
                (channels == 1 || channels == 2 || channels == 4)) {
                uint8_t *d = dst + ((size_t)(by * 4) * width + bx * 4) * channels;
                for (uint32_t y = 0; y < 4; ++y) {
                    uint8_t *row_out = d + (size_t)y * width * channels;
                    if (channels == 4) memcpy(row_out, texels + y * 4, 16);
                    else if (channels == 2) {
                        for (uint32_t x = 0; x < 4; ++x)
                            memcpy(row_out + x * 2, texels[y * 4 + x], 2);
                    } else {
                        for (uint32_t x = 0; x < 4; ++x) row_out[x] = texels[y * 4 + x][0];
                    }
                }
                continue;
            }
'''
PRELUDE = '''typedef __SIZE_TYPE__ size_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
void *memcpy(void *, const void *, size_t);
enum { CONV_NONE, CONV_DXT1, CONV_DXT3, CONV_DXT5, CONV_ATI1, CONV_ATI2 };
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
'''
WRAPPERS = '''EXPORT void decode(int candidate, int conv, const uint8_t *src, uint32_t w,
    uint32_t h, uint32_t pitch, uint8_t *dst, uint32_t channels) {
    if (candidate) decode_blocks_candidate(conv,src,w,h,pitch,dst,channels);
    else decode_blocks_baseline(conv,src,w,h,pitch,dst,channels);
}
EXPORT void repeat(int candidate, int conv, const uint8_t *src, uint32_t w,
    uint32_t h, uint32_t pitch, uint8_t *dst, uint32_t channels, uint32_t count) {
    for (uint32_t i=0;i<count;++i) decode(candidate,conv,src,w,h,pitch,dst,channels);
}
'''


def function(source, signature):
    at = source.index(signature)
    return source[at:source.index('\n}\n', at) + 3]


def color(block, four):
    a, b, bits = struct.unpack('<HHI', block)
    def rgb(c):
        r,g,b = c >> 11, (c >> 5) & 63, c & 31
        return [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255]
    p = [rgb(a), rgb(b)]
    if a > b or four:
        p += [[(2*p[0][i]+p[1][i]+1)//3 for i in range(3)]+[255],
              [(p[0][i]+2*p[1][i]+1)//3 for i in range(3)]+[255]]
    else:
        p += [[(p[0][i]+p[1][i])//2 for i in range(3)]+[255], [0]*4]
    return [list(p[(bits >> (2*i)) & 3]) for i in range(16)]


def alpha(block):
    a,b = block[:2]; bits = int.from_bytes(block[2:], 'little'); p = [a,b]
    if a > b: p += [((7-i)*a+i*b+3)//7 for i in range(1,7)]
    else: p += [((5-i)*a+i*b+2)//5 for i in range(1,5)] + [0,255]
    return [p[(bits >> (3*i)) & 7] for i in range(16)]


def reference(conv, data, width, height, pitch, channels):
    out = bytearray(width*height*channels); block_bytes = 8 if conv in (1,4) else 16
    for by in range((height+3)//4):
        for bx in range((width+3)//4):
            at=by*pitch+bx*block_bytes; b=data[at:at+block_bytes]
            if conv <= 3:
                pixels=color(b[:8] if conv==1 else b[8:],conv!=1)
                if conv==2:
                    bits=int.from_bytes(b[:8],'little')
                    for i in range(16): pixels[i][3]=((bits>>(4*i))&15)*17
                elif conv==3:
                    for p,a in zip(pixels,alpha(b[:8])):p[3]=a
            elif conv==4:pixels=[[a] for a in alpha(b)]
            else:pixels=[list(p) for p in zip(alpha(b[8:]),alpha(b[:8]))]
            for y in range(min(4,height-by*4)):
                for x in range(min(4,width-bx*4)):
                    at=((by*4+y)*width+bx*4+x)*channels
                    out[at:at+channels]=bytes(pixels[y*4+x][:channels])
    return bytes(out)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--benchmark',action='store_true'); args=parser.parse_args()
    OUT.mkdir(parents=True,exist_ok=True)
    production=(ROOT/'android/host/d3d9_format.c').read_text()
    helpers='\n'.join(function(production,s) for s in ['static void rgb565(',
        'static void decode_color_block(', 'static void decode_alpha_block('])
    candidate=function(production,'static void decode_blocks(')
    assert candidate.count(FAST_COPY)==1, 'production block-copy path changed; review the baseline extraction'
    baseline=candidate.replace(FAST_COPY,'')
    anchor='            for (uint32_t y = 0; y < 4; ++y) {'
    assert baseline.count(anchor)==1
    source=PRELUDE+helpers+baseline.replace('decode_blocks(','decode_blocks_baseline(',1)
    source+=candidate.replace('decode_blocks(','decode_blocks_candidate(',1)+WRAPPERS
    code=OUT/'check.c';code.write_text(source)
    definition=OUT/'crt.def';definition.write_text('LIBRARY msvcrt.dll\nEXPORTS\nmemcpy\n')
    library=OUT/'crt.lib'
    subprocess.run([str(CLANG.with_name('llvm-dlltool.exe')),'-m','i386:x86-64','-d',str(definition),'-l',str(library)],check=True)
    obj=OUT/'check.obj';dll_path=OUT/'check.dll'
    subprocess.run([str(CLANG),'--target=x86_64-pc-windows-msvc','-O2','-fno-stack-protector','-c',str(code),'-o',str(obj)],check=True)
    subprocess.run([str(CLANG.with_name('ld.lld.exe')),'-flavor','link','/dll','/noentry','/out:'+str(dll_path),str(obj),str(library)],check=True)
    dll=ctypes.CDLL(str(dll_path));signature=[ctypes.c_int,ctypes.c_int,ctypes.c_void_p,
        ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_void_p,ctypes.c_uint32]
    dll.decode.argtypes=signature;dll.repeat.argtypes=signature+[ctypes.c_uint32]
    rng=random.Random(0x445854);cases=[]; guard=b'\xD3'*32
    for conv in range(1,6):
        channels=4 if conv<=3 else conv-3
        for i in range(100):
            w,h=((1,1),(3,5),(4,4),(8,8),(7,4),(13,17),(64,64))[i] if i<7 else (rng.randint(1,67),rng.randint(1,49))
            pitch=((w+3)//4)*(8 if conv in (1,4) else 16)+rng.randrange(0,4)*8
            data=rng.randbytes(pitch*((h+3)//4));expected=guard+reference(conv,data,w,h,pitch,channels)+guard
            cases.append((conv,w,h,pitch,channels,data,expected))
            src=ctypes.create_string_buffer(data)
            for mode in (0,1):
                dst=ctypes.create_string_buffer(guard+b'\xCC'*(w*h*channels)+guard)
                dll.decode(mode,conv,src,w,h,pitch,ctypes.byref(dst,32),channels)
                assert dst.raw[:-1]==expected,(conv,w,h,pitch,mode)
    print('PASS: actual C, 500 cases x baseline/candidate vs independent reference; all formats, edges, pitches, canaries')
    # Link identical optimized C for AArch64. A separate memcpy fallback only
    # supplies variable-length copies; fixed-size copies retain clang builtins.
    memory=OUT/'memory.c';memory.write_text(PRELUDE+'''void *memcpy(void *d,const void *s,size_t n) {
        volatile uint8_t *p=d; const volatile uint8_t *q=s;
        for(size_t i=0;i<n;++i)p[i]=q[i]; return d; }\n''')
    elf_path=OUT/'check.elf'
    subprocess.run([str(CLANG),*COMMON,'-O2','-fno-stack-protector','-nostdlib',
        '-Wl,-e,decode',str(code),str(memory),'-o',str(elf_path)],check=True)
    elf=ELFFile(io.BytesIO(elf_path.read_bytes()));symbols={s.name:s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    base,arena,stack,stop=0x10000000,0x20000000,0x30000000,0x40000000
    arm=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for at,size in [(base,0x100000),(arena,0x100000),(stack,0x10000),(stop,0x1000)]:arm.mem_map(at,size)
    for segment in elf.iter_segments():
        if segment['p_type']=='PT_LOAD':arm.mem_write(base+segment['p_vaddr'],segment.data())
    relocations=elf.get_section_by_name('.rela.dyn')
    if relocations:
        for r in relocations.iter_relocations():
            assert r['r_info_type']==1027;arm.mem_write(base+r['r_offset'],struct.pack('<Q',base+r['r_addend']))
    regs=[A.UC_ARM64_REG_X0,A.UC_ARM64_REG_X1,A.UC_ARM64_REG_X2,A.UC_ARM64_REG_X3,
          A.UC_ARM64_REG_X4,A.UC_ARM64_REG_X5,A.UC_ARM64_REG_X6,A.UC_ARM64_REG_X7]
    instructions=[0];arm.hook_add(UC_HOOK_CODE,lambda *unused:instructions.__setitem__(0,instructions[0]+1))
    counts={0:0,1:0};checked=0;arm_rows=[]
    for case in [c for i,c in enumerate(cases) if i%100<7]:
        conv,w,h,pitch,channels,data,expected=case;src=arena;dst=arena+0x20000
        arm.mem_write(src,data);row_counts={}
        for mode in (0,1):
            arm.mem_write(dst,guard+b'\xCC'*(w*h*channels)+guard);instructions[0]=0
            for reg,value in zip(regs,[mode,conv,src,w,h,pitch,dst+32,channels]):arm.reg_write(reg,value)
            arm.reg_write(A.UC_ARM64_REG_SP,stack+0xF000);arm.reg_write(A.UC_ARM64_REG_LR,stop)
            arm.emu_start(base+symbols['decode'],stop,count=2000000)
            assert arm.reg_read(A.UC_ARM64_REG_PC)==stop
            assert bytes(arm.mem_read(dst,len(expected)))==expected,(conv,w,h,'ARM',mode)
            counts[mode]+=instructions[0]
            row_counts[mode]=instructions[0]
        arm_rows.append({'conv':conv,'width':w,'height':h,'instructions':row_counts})
        checked+=1
    print(f'PASS: actual AArch64, {checked} cases x baseline/candidate vs reference/canaries; instructions {counts}')
    report={'host_cases':len(cases),'arm_cases':checked,'arm_instructions':counts,'arm_rows':arm_rows,'benchmark':[]}
    for row in arm_rows:
        if row['width']==64:print('ARM64 64x64 instruction counts:',row)
    if args.benchmark:
        for conv in (1,2,3,4,5):
            channels=4 if conv<=3 else conv-3
            w=h=512;pitch=(w//4)*(8 if conv in (1,4) else 16);src=ctypes.create_string_buffer(rng.randbytes(pitch*(h//4)));dst=ctypes.create_string_buffer(w*h*channels)
            timings={0:[],1:[]}
            for iteration in range(8):
                for mode in ((0,1) if iteration%2==0 else (1,0)):
                    start=time.perf_counter_ns();dll.repeat(mode,conv,src,w,h,pitch,dst,channels,40)
                    timings[mode].append((time.perf_counter_ns()-start)/40/1e6)
            medians={mode:sorted(v)[len(v)//2] for mode,v in timings.items()}
            row={'conv':conv,'desktop_ms_per_512x512':medians,'candidate_over_baseline':medians[1]/medians[0]}
            report['benchmark'].append(row);print('DESKTOP ONLY:',row)
    (OUT/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print(OUT)


if __name__=='__main__':main()
