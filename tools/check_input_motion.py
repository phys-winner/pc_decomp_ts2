"""Real input motion: a touch click must not generate three identical hover events."""
from audio_probe import ROOT, compile_probe
from check_virtual_query import function

source = (ROOT / 'android/host/input.c').read_text()
harness = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static int input_lock,locked,mouse_dx,mouse_dy,cursor_x=10,cursor_y=20,screen_w=100,screen_h=80;
static unsigned messages,events,mouse_ring;
static void pthread_mutex_lock(int *p){assert(p==&input_lock&&!locked);locked=1;}
static void pthread_mutex_unlock(int *p){assert(p==&input_lock&&locked);locked=0;}
static uint32_t mouse_key_state(void){return 0;}
static void ring_push(unsigned *ring,unsigned axis,uint32_t value){assert(ring==&mouse_ring&&(axis==0||axis==4)&&value);++events;}
static void pend(unsigned kind,uint32_t key,uint32_t point){assert(kind==0x200&&!key&&point==((uint32_t)cursor_y<<16|(uint32_t)cursor_x));++messages;}
'''
harness += '\n'.join(function(source, signature) for signature in ('void void_input_mouse_move(', 'void void_input_mouse_warp('))
harness += r'''
int main(void){
 void_input_mouse_move(0,0);assert(!messages&&!events&&!locked);
 void_input_mouse_warp(40,30);assert(messages==1&&!events&&!mouse_dx&&!mouse_dy);
 void_input_mouse_move(0,0);void_input_mouse_move(0,0);assert(messages==1&&!events);
 void_input_mouse_move(5,-3);assert(messages==2&&events==2&&cursor_x==45&&cursor_y==27&&mouse_dx==5&&mouse_dy==-3);
 void_input_mouse_move(-200,0);assert(messages==3&&events==3&&!cursor_x&&cursor_y==27&&!locked);
 void_input_mouse_warp(40,30);assert(messages==4); /* An explicit warp still refreshes a changed window/tool. */
 puts("PASS actual input motion: click hover once, zero movement silent, real motion/relative state and explicit warp preserved");
 return 0;
}
'''
compile_probe('input-motion', harness, {})
