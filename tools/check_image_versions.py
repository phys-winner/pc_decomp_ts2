"""Check runtime PE pins and build a probe that rejects altered images before mapping."""
import hashlib
import json
import re
import subprocess
import zlib
from pathlib import Path

import build_android as B
from check_virtual_query import function


def main():
    text = (B.ROOT/'android/host/pe.c').read_text()
    declarations = text[text.index('typedef struct { const char *name; const char *file;'):
                        text.index('static const char *pseudo_names[]')]
    modules = json.loads((B.ROOT/'port.json').read_text())['modules']
    pins = re.findall(r'\{"([^"]+)", "[^"]+", 0x[0-9A-F]+u, [01], 0, ([0-9]+)u, 0x([0-9A-F]+)u\}', declarations)
    assert len(pins) == len(modules) == 2
    for name, size, crc in pins:
        module = next(m for m in modules if name == Path(m['file']).name)
        raw = (B.ROOT/module['file']).read_bytes()
        assert len(raw) == int(size) and zlib.crc32(raw) == int(crc, 16)
        assert hashlib.sha256(raw).hexdigest() == module['sha256']
    assert text.index('if (!image_matches(spec, file, file_size))') < text.index('uint32_t lfanew =')
    prelude = '#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <zlib.h>\n'
    checks = r'''
int main(int argc,char **argv){
 assert(argc==3);
 for(int i=0;i<2;++i){
  FILE *f=fopen(argv[i+1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
  uint8_t *data=malloc((size_t)n);assert(data&&fread(data,1,n,f)==(size_t)n);assert(!fclose(f));
  assert(image_matches(&lifted[i],data,n));
  assert(!image_matches(&lifted[i],data,n-1)&&!image_matches(&lifted[i],data,n+1));
  assert(!image_matches(&lifted[1-i],data,n));
  long positions[]={0,n/2,n-1};
  for(unsigned j=0;j<3;++j){data[positions[j]]^=1;assert(!image_matches(&lifted[i],data,n));data[positions[j]]^=1;}
  assert(image_matches(&lifted[i],data,n));free(data);
 }
 puts("PASS original runtime PE pins; wrong size/module and changes at start/middle/end rejected before PE parsing; input files read only");return 0;
}
'''
    out = B.ROOT/'build/test/image-versions'
    out.mkdir(parents=True, exist_ok=True)
    source, binary = out/'check.c', out/'check-image-versions'
    source.write_text(prelude + declarations + function(text, 'static int image_matches(') + checks)
    subprocess.run([str(B.CLANG), *B.HOST_FLAGS, '-Wl,-z,max-page-size=16384', str(source), '-lz',
                    '-o', str(binary)], cwd=B.ROOT, check=True)
    print('PASS size/CRC32 pins match SHA-256-pinned originals; selected-device probe:', binary)


if __name__ == '__main__':
    main()
