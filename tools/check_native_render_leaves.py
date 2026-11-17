"""Build/run an API29 ARM64 differential check and paired CPU microbenchmark.

Without --serial only builds the executable; the original x86 oracle remains
check_native_lookup.py. This measures two kernels, not game FPS.
"""
import argparse
import json
import re
import subprocess

import build_android as B

OUT = B.ROOT / 'build/test/native-render-leaves'
NAMES = ('sub_A0ADFAE0', 'sub_A052F900')

HARNESS = r'''
#include "host.h"
#include <assert.h>
#include <sys/mman.h>
#include <time.h>
recomp_machine_t g_m;
void sub_A0ADFAE0(void),sub_A052F900(void),native_material(void),native_attribute(void);
static uint32_t seed=0xADFAE052u;
static uint32_t random_word(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static uint32_t flags(void){return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1)&0x8D5;}
static void setup(unsigned kind,unsigned which){
 memset(&g_m,0,sizeof g_m);
 uint32_t *r=&g_m.m_eax;for(unsigned i=0;i<8;++i)r[i]=random_word();
 g_ecx=0x11000;g_esp=0x80000;MEM32(g_esp)=0x60000000;
 if(!kind){
  MEM8(0x11004)=which%7?1:0;MEM32(0x11028)=0x12000;MEM32(0x1102C)=0x12010;
  MEM32(g_esp+4)=which%9;
  for(unsigned i=0;i<4;++i){MEM32(0x12000+4*i)=i%3?0x14000:0;}
  MEM32(0x14014)=0x18000;MEM32(0x14028)=which%2?15:16;
 }else MEM32(g_esp+4)=which%117;
}
static uint64_t now(void){struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
typedef void (*function)(void);
static uint64_t measure(function f,unsigned kind,unsigned n){
 setup(kind,1);uint32_t checksum=0;uint64_t start=now();
 for(unsigned i=0;i<n;++i){g_ecx=0x11000;g_esp=0x80000;MEM32(0x80004)=i%16;f();checksum+=g_eax;}
 uint64_t elapsed=now()-start;printf("%u ",checksum);return elapsed;
}
int main(void){
 void *p=mmap((void *)(VOID_GUEST_BASE+0x10000),0xF0000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
 assert(p==(void *)(VOID_GUEST_BASE+0x10000));
 for(unsigned i=0;i<0x10000;i+=4)MEM32(0x10000+i)=random_word();
 function original[2]={sub_A0ADFAE0,sub_A052F900},native[2]={native_material,native_attribute};
 for(unsigned kind=0;kind<2;++kind)for(unsigned i=0;i<4096;++i){
  setup(kind,i);recomp_machine_t before=g_m;
  original[kind]();recomp_machine_t expected=g_m;uint32_t expected_flags=flags();
  g_m=before;native[kind]();
  assert(!memcmp(&g_m,&expected,32)&&flags()==expected_flags);
 }
 puts("PASS 8192 ARM64 original/native cases: all GPRs and arithmetic flags");
 for(unsigned kind=0;kind<2;++kind)for(unsigned pair=0;pair<8;++pair){
  unsigned first=pair&1;
  uint64_t a,b;
  printf("pair %u kind %u order %s checksums ",pair,kind,first?"BA":"AB");
  if(!first){a=measure(original[kind],kind,2000000);b=measure(native[kind],kind,2000000);}
  else{b=measure(native[kind],kind,2000000);a=measure(original[kind],kind,2000000);}
  printf("original_ns %llu native_ns %llu\n",(unsigned long long)a,(unsigned long long)b);
 }
 return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    bodies = []
    for name in NAMES:
        filename = 'sims2_' + f'{int(name[4:], 16) & ~0x7FFF:08X}' + '.c'
        source = (B.ROOT / 'generated/sims2-x86' / filename).read_bytes()
        match = re.search(rb'void ' + name.encode() + rb'\(void\) \{.*?\n\}', source, re.S)
        assert match, name
        bodies.append(match.group() + b'\n')
    generated = OUT / 'original.c'
    generated.write_bytes(b'#include "recomp_types.h"\n' + B.localize_registers(b''.join(bodies)))
    harness = OUT / 'probe.c'
    harness.write_text(HARNESS, encoding='utf-8')
    common = ['--target=aarch64-linux-android29', '-O2', '-fno-strict-aliasing', '-fwrapv',
              '-ffp-contract=off', '-fno-math-errno', '-fPIC', '-ffunction-sections', '-fdata-sections',
              '-Iandroid/runtime', '-Iandroid/host']
    profile = B.ROOT / 'build/pgo/abb24fae-20261022.profdata'
    profile_flags = ['-fprofile-instr-use=' + profile.as_posix(), '-Wno-profile-instr-unprofiled'] if profile.is_file() else []
    commands = [
        [str(B.CLANG), *common, '-DRECOMP_GENERATED_CODE', '-DRECOMP_LOCAL_REGS', '-DRECOMP_PLAIN_MEMORY',
         *profile_flags, '-w', '-c', str(generated), '-o', str(OUT / 'original.o')],
        [str(B.CLANG), *common, '-DS2_NATIVE_LEAF_BENCHMARK', '-Dsub_A0ADFAE0=native_material', '-Dsub_A052F900=native_attribute',
         '-c', 'android/host/sims2_fast.c', '-o', str(OUT / 'native.o')],
        [str(B.CLANG), *common, str(harness), str(OUT / 'original.o'), str(OUT / 'native.o'),
         '-Wl,--gc-sections', '-o', str(OUT / 'probe')],
    ]
    for command in commands:
        subprocess.run(command, cwd=B.ROOT, check=True)
    result = {'status': 'BUILT', 'api': 29, 'commands': commands,
              'default_native': ['sub_A0ADFAE0', 'sub_A052F900'], 'diagnostic_only': [],
              'binary_sha256': B.sha_file(OUT / 'probe'), 'native_source_sha256': B.sha_file(B.ROOT / 'android/host/sims2_fast.c'),
              'original_source_sha256': B.sha_file(generated), 'pgo_sha256': B.sha_file(profile) if profile_flags else None,
              'limits': 'Two isolated integer kernels; no game FPS, RAM or renderer speedup is established.'}
    if args.serial:
        adb = ['adb', '-s', args.serial]
        remote = '/data/local/tmp/sims2-native-leaves'
        subprocess.run(adb + ['push', str(OUT / 'probe'), remote], check=True)
        subprocess.run(adb + ['shell', 'chmod', '755', remote], check=True)
        run = subprocess.run(adb + ['shell', remote], text=True, capture_output=True, timeout=90)
        (OUT / 'device.log').write_text(run.stdout + run.stderr, encoding='utf-8')
        assert run.returncode == 0, run.stdout + run.stderr
        assert 'PASS 8192 ARM64' in run.stdout
        measurements = [tuple(map(int, row)) for row in re.findall(r'kind (\d).*original_ns (\d+) native_ns (\d+)', run.stdout)]
        assert len(measurements) == 16, run.stdout
        result.update(status='PASS', serial=args.serial, cases=8192, pairs=measurements)
        print(run.stdout)
    (OUT / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
