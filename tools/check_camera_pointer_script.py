"""Execute actual script pointer branches and verify intent precedes enqueue."""
from audio_probe import CLANG, compile_probe
from build_android import ROOT


def branch(source, action):
    marker = '} else if (!strcmp(e->action, "' + action + '")) {'
    assert source.count(marker) == 1
    start = source.index('{', source.index(marker))
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start + 1:end - 1]


def main():
    source = (ROOT / 'android/host/script.c').read_text()
    actions = ('click', 'cursor', 'move', 'warp', 'bdown', 'bup', 'wheel', 'touchpick')
    prelude = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct {int a,b,c;} script_event;
static char order[32];static unsigned n,resumes,packets,cases;
static void log_call(char c){assert(n<31);order[n++]=c;order[n]=0;}
static void resume(void){++resumes;log_call('R');}
static void (*s2_touch_camera_pointer_intent)(void)=resume;
static void void_input_cursor(uint32_t *x,uint32_t *y){*x=10;*y=20;log_call('C');}
static void void_input_mouse_move(int x,int y){log_call('M');packets+=(x!=0||y!=0);}
static void void_input_mouse_warp(int x,int y){(void)x;(void)y;log_call('W');++packets;}
static void void_input_mouse_button(int b,int down){log_call(b<0||b>7?'X':down?'D':'U');packets+=(b>=0&&b<=7);}
static void void_input_mouse_wheel(int delta){log_call('L');packets+=(delta!=0);}
static void s2_touch_request_pick(int x,int y){(void)x;(void)y;log_call('T');}
static void hold(double t){(void)t;log_call('H');}
typedef void (*run_fn)(const script_event *);
static void check(run_fn f,script_event e,const char *calls,unsigned r,unsigned p){
 n=resumes=packets=0;order[0]=0;f(&e);assert(!strcmp(order,calls)&&resumes==r&&packets==p);++cases;
}
'''
    code = prelude
    for action in actions:
        code += 'static void run_' + action + '(const script_event *e){\n' + branch(source, action) + '\n}\n'
    code += r'''
int main(void){
 check(run_cursor,(script_event){10,20,0},"CRM",1,0);
 check(run_cursor,(script_event){11,20,0},"CRM",1,1);
 check(run_warp,(script_event){10,20,0},"RW",1,1);
 check(run_move,(script_event){0,0,0},"M",0,0);
 check(run_move,(script_event){0,1,0},"RM",1,1);
 check(run_move,(script_event){-1,0,0},"RM",1,1);
 check(run_bdown,(script_event){0,0,0},"RD",1,1);
 check(run_bdown,(script_event){7,0,0},"RD",1,1);
 check(run_bdown,(script_event){-1,0,0},"X",0,0);
 check(run_bdown,(script_event){8,0,0},"X",0,0);
 check(run_bup,(script_event){0,0,0},"U",0,1);
 check(run_wheel,(script_event){0,0,0},"L",0,0);
 check(run_wheel,(script_event){120,0,0},"RL",1,1);
 check(run_touchpick,(script_event){10,20,0},"T",0,0);
 check(run_click,(script_event){10,20,0},"CRMHRDHU",2,2);
 check(run_click,(script_event){10,20,-1},"CRMHXHX",1,0);
 s2_touch_camera_pointer_intent=0;
 check(run_cursor,(script_event){10,20,0},"CM",0,0);
 check(run_warp,(script_event){10,20,0},"W",0,1);
 check(run_move,(script_event){1,0,0},"M",0,1);
 check(run_bdown,(script_event){0,0,0},"D",0,1);
 check(run_wheel,(script_event){120,0,0},"L",0,1);
 check(run_click,(script_event){10,20,0},"CMHDHU",0,2);
 printf("PASS script pointer intent: %u actual branches/cases; absolute-zero resume, relative-zero and UP ignore, valid enqueue ordering, absent weak helper\n",cases);
 return 0;
}
'''
    compile_probe('camera-pointer-script', code, {}, compiler_override=CLANG)


if __name__ == '__main__':
    main()
