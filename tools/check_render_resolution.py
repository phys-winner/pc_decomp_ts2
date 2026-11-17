"""Execute the production logical-canvas and GLES target-size formulas on the host."""
import ctypes
from pathlib import Path
import subprocess
import tempfile

from build_android import CLANG, ROOT
from check_virtual_query import function


def main():
    settings = (ROOT / 'android/host/sims2_settings.c').read_text()
    renderer = (ROOT / 'android/host/d3d9_gl.c').read_text()
    prelude = '''
typedef unsigned int uint32_t;
int _fltused;
static int phys_w,phys_h,render_scale,ui_scale;
static struct {uint32_t max_texture;} d3d_gl_limits;
'''
    source = prelude + '\n'.join(line for line in settings.splitlines() if line.startswith(
        ('int s2_render_scale(', 'void s2_physical_size(')))
    source += '\n' + function(settings, 'void s2_game_resolution(')
    source += '\n' + '\n'.join(function(renderer, signature) for signature in (
        'float d3d_screen_scale(', 'uint32_t d3d_scaled_size('))
    source += r'''
#define REQUIRE(c) do{if(!(c))return __LINE__;}while(0)
__declspec(dllexport) unsigned check(void){
 phys_w=2424;phys_h=1080;ui_scale=300;render_scale=33;
 int w,h;s2_game_resolution(&w,&h);REQUIRE(w==2424&&h==1080);
 float k=d3d_screen_scale(w);
 REQUIRE(d3d_scaled_size(w,k)==800&&d3d_scaled_size(h,k)==356);
 /* Low render sizes are not silently raised to 25% of the fitted UI canvas. */
 phys_w=320;phys_h=240;render_scale=25;s2_game_resolution(&w,&h);
 k=d3d_screen_scale(w);REQUIRE(w==800&&h==600&&k<0.101f);
 REQUIRE(d3d_scaled_size(w,k)==80&&d3d_scaled_size(h,k)==60);
 for(phys_w=320;phys_w<=4096;phys_w+=127){
  phys_h=phys_w*9/16;
  for(render_scale=25;render_scale<=200;render_scale+=25){
   for(ui_scale=100;ui_scale<=300;ui_scale+=25){
    s2_game_resolution(&w,&h);k=d3d_screen_scale(w);
    uint32_t pixels=d3d_scaled_size(w,k);
    uint32_t wanted=(uint32_t)((float)phys_w*render_scale/100.0f+0.5f);
    REQUIRE(pixels+1>=wanted&&pixels<=wanted+1&&w>=800&&h>=600);
    REQUIRE(d3d_screen_scale(0)==k);
   }
  }
 }
 /* A genuine GL texture limit still wins, for the guest and temporary devices. */
 phys_w=4096;phys_h=2160;render_scale=200;ui_scale=100;d3d_gl_limits.max_texture=2048;
 s2_game_resolution(&w,&h);k=d3d_screen_scale(w);REQUIRE(d3d_scaled_size(w,k)==2048);
 k=d3d_screen_scale(800);REQUIRE(d3d_scaled_size(800,k)==2048);
 REQUIRE(d3d_scaled_size(1,0.01f)==1);
 return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sims2-render-size-') as directory:
        base = Path(directory)
        c, obj, dll = base / 'check.c', base / 'check.obj', base / 'check.dll'
        c.write_text(source, encoding='utf-8')
        subprocess.run([str(CLANG), '--target=x86_64-pc-windows-msvc', '-O2', '-fno-builtin',
                        '-fno-stack-protector', '-c', str(c), '-o', str(obj)], check=True)
        subprocess.run([str(CLANG.with_name('ld.lld.exe')), '-flavor', 'link', '/dll', '/noentry',
                        '/out:' + str(dll), str(obj)], check=True)
        loaded = ctypes.CDLL(str(dll))
        try:
            result = loaded.check()
            assert not result, ('C line', result, source.splitlines()[result - 1])
        finally:
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(loaded._handle)
    print('PASS production logical/UI and GLES pixel formulas: render 25..200%, UI 100..300%, stable minimum, small-screen floor regression, GL limit')


if __name__ == '__main__':
    main()
