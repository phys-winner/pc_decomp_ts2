"""Execute both actual QA toggle branches with owner-order and weak-link checks."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT


def check_toggle(action, setter):
    source = (ROOT / 'android/host/script.c').read_text(encoding='utf-8')
    marker = '} else if (!strcmp(e->action, "' + action + '")) {'
    assert source.count(marker) == 1
    start = source.index('{', source.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    branch = source[start + 1:end - 1]
    prelude = r'''
#include <assert.h>
#include <stdio.h>
typedef struct { int a; } script_event;
static unsigned owner, order, calls, enters, leaves;
static int selected;
static void (*s2_resource_skip_enable)(int);
static void void_machine_enter(void){assert(!owner && order==0);owner=1;order=1;++enters;}
static void toggle(int enabled){assert(owner && order==1);selected=enabled;order=2;++calls;}
static void void_machine_leave(void){assert(owner && order==2);owner=0;order=3;++leaves;}
'''
    checks = r'''
int main(void){
 script_event e={0};
 run(&e);assert(!owner&&!calls&&!enters&&!leaves&&!order);
 s2_resource_skip_enable=toggle;
 run(&e);assert(!owner&&calls==1&&enters==1&&leaves==1&&order==3&&selected==0);
 order=0;e.a=1;
 run(&e);assert(!owner&&calls==2&&enters==2&&leaves==2&&order==3&&selected==1);
 puts("PASS resourceskip: machine owner before mode/counter access, balanced release, OFF/ON and absent weak setter");
 return 0;
}
'''
    prelude = prelude.replace('s2_resource_skip_enable', setter)
    checks = checks.replace('s2_resource_skip_enable', setter).replace('PASS resourceskip:', 'PASS ' + action + ':')
    compile_probe(action + '-owner', prelude + 'static void run(const script_event *e){\n' +
                  branch + '\n}\n' + checks, {}, compiler_override=CLANG)


def main():
    check_toggle('resourceskip', 's2_resource_skip_enable')
    check_toggle('camerahover', 's2_touch_camera_hover_enable')


if __name__ == '__main__':
    main()
