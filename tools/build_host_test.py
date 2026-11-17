"""Link a focused Android test with the exact objects selected by the native build.

The test can include a production C file to reach its internal contract; that
object is excluded, and all other runtime objects come from link.rsp.
"""
import argparse
from pathlib import Path
import re
import subprocess
import build_android as B

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('test',type=Path)
    p.add_argument('--objects',type=Path,default=B.ROOT/'build/arm64/obj')
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--define',action='append',default=[])
    p.add_argument('--wrap',action='append',default=[])
    a=p.parse_args()
    objects_root=a.objects.resolve()
    response=objects_root.parent/'link.rsp'
    included={Path(n).stem for n in re.findall(r'#include "\.\./android/host/([a-z0-9_]+\.c)"',a.test.read_text())}
    objects=[Path(line.strip().strip('"')) for line in response.read_text().splitlines() if line.strip()]
    assert all(o.resolve().is_relative_to(objects_root) and o.is_file() for o in objects)
    objects=[o for o in objects if o.stem not in included]
    a.output.parent.mkdir(parents=True,exist_ok=True)
    rsp=a.output.with_suffix('.rsp')
    rsp.write_text('\n'.join('"'+o.as_posix()+'"' for o in objects),encoding='utf-8')
    flags=B.HOST_FLAGS+['-DVOID_ANDROID_APP']+['-D'+d for d in a.define]
    cmd=[str(B.CLANG),*flags,str(a.test),'@'+str(rsp),'-o',str(a.output),
         '-Wl,--gc-sections','-Wl,-z,max-page-size=16384','-Wl,--build-id=sha1',
         '-lm','-lz','-ldl','-llog','-landroid','-lEGL','-lGLESv3','-lOpenSLES','-laaudio']
    cmd += ['-Wl,--wrap='+name for name in a.wrap]
    result = subprocess.run(cmd, cwd=B.ROOT, capture_output=True, text=True)
    if result.stdout or result.stderr:
        print((result.stdout + result.stderr).replace(str(Path.home()), '<USER_HOME>')
              .replace(Path.home().as_posix(), '<USER_HOME>'))
    if result.returncode:
        raise SystemExit(result.returncode)
    print(f'{a.output}: {len(objects)} exact runtime objects, excludes {sorted(included)}')

if __name__=='__main__':main()
