"""Check the production GLES base-vertex policy and the separate timer guard."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT
from check_virtual_query import function


def main():
    source = (ROOT / 'android/render/s2r_gles.c').read_text(encoding='utf-8')
    policy = '\n'.join(function(source, signature) for signature in (
        'static int mali_g51(', 'static int use_base_vertex_attrib(', 'static int mali_g51_r18('))
    checks = r'''
int main(void)
{
    assert(use_base_vertex_attrib("Mali-G51", "", NULL));
    assert(use_base_vertex_attrib("Mali-G51 MP4", "", "1"));
    assert(use_base_vertex_attrib("Mali-G51 (Panfrost)", "", "1"));
    assert(use_base_vertex_attrib("ANGLE (ARM, Mali-G51, OpenGL ES 3.2)", "", "1"));
    assert(use_base_vertex_attrib("Mali-G51", "", "invalid"));
    assert(!use_base_vertex_attrib("Mali-G51", "", "0"));
    assert(use_base_vertex_attrib("Mali-T830", "", "0"));
    assert(!use_base_vertex_attrib("Mali-G510", "", "1"));
    assert(!use_base_vertex_attrib("Mali-G510 MC2", "", "1"));
    assert(!use_base_vertex_attrib("Mali-G76 MC4", "", "1"));
    assert(!use_base_vertex_attrib("Mali-G615 MC5", "", "1"));
    assert(!use_base_vertex_attrib("Adreno (TM) 650", "", "1"));
    assert(!use_base_vertex_attrib(NULL, "", "1"));
    assert(!use_base_vertex_attrib("Mali-G51", "draw", "1"));
    assert(use_base_vertex_attrib("Mali-G51", "attrib", "0"));
    assert(mali_g51_r18("Mali-G51 MP4", "OpenGL ES 3.2 v1.r18p0-01rel0"));
    assert(!mali_g51_r18("Mali-G51", "OpenGL ES 3.2 v1.r19p0"));
    assert(!mali_g51_r18("Mali-G51", NULL));
    puts("PASS GLES: Mali-G51 across renderer names, setting on/off/default, Mali-T, exact GPU distinction, debug modes and r18-only timer guard");
    return 0;
}
'''
    prelude = '#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n'
    compile_probe('gles-base-vertex', prelude + policy + checks, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
