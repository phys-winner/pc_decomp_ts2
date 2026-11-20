"""Actual ARM64 owner command/FIFO plus original lifted slider integer API.

Only window notifications, references and OS waits are boundary fixtures. The
original QI/getter/setter and actual core guest-call/register helpers execute.
"""
import hashlib
import argparse
import io
import json
import struct
import subprocess

from elftools.elf.elffile import ELFFile
import pefile
from unicorn import Uc, UC_ARCH_ARM64, UC_ARCH_X86, UC_MODE_ARM, UC_MODE_32, UC_HOOK_CODE
from unicorn import arm64_const as A
from unicorn import x86_const as X

import build_android as B
import check_touch_barrier as T
from check_virtual_query import function

OUT = B.ROOT / 'build/test/cas-slider-abi'
EXTRA = r'''
static unsigned refs,adds,releases,setters,changed,finished,notifies[2],value_reads,step_mode;
static unsigned focus_calls,focus_events,focus_mode,old_refs;
static volatile unsigned trap_calls,first_trap_va,last_trap_va;
static unsigned last_event_id;
static unsigned mono_ns,time_fail,read_only_stack;
static uint64_t seen_progress;
static uint32_t guest_handler_order;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx,g_icall_count;
void (*recomp_yield_hook)(void);
static void thunk_add(void){++refs;++adds;g_eax=refs;g_esp+=4;}
static void thunk_release(void){if(!refs)bad=50;else --refs;++releases;g_eax=refs;g_esp+=4;}
static void thunk_old_add(void){g_eax=++old_refs;g_esp+=4;}
static void thunk_old_release(void){if(!old_refs)bad=62;else --old_refs;g_eax=old_refs;g_esp+=4;}
static void thunk_member(void){if(ARG(0)!=0x13000 && ARG(0)!=0x18000)bad=63;g_eax=ARG(0)==0x18000 || focus_mode!=2;g_esp+=8;}
static void thunk_accept(void){if(ARG(0)!=0x8000)bad=64;g_eax=focus_mode!=1 && !(focus_mode==10 && focus_events);g_esp+=8;}
static void thunk_focus_event(void){
    if(setters || changed || finished || lock_depth || g_ecx!=win_manager)bad=65;
    unsigned event=ARG(1);++focus_events;
    if(event==0x11){
        if(focus_mode==3)context=S2_TOUCH_CAS|S2_TOUCH_MODAL;
        if(focus_mode==4)MEM32(win_manager+0x34)=0x13000;
        if(focus_mode==6)MEM32(0x13104)=101;
        if(focus_mode==7)context=S2_TOUCH_LIVE;
        if(focus_mode==8){float value=.6f;memcpy(GPTR(0x13108),&value,4);}
        if(focus_mode==11){MEM32(0x13908)=0;} /* RTTI disappears during focus gain. */
        if(focus_mode==12)win_manager=0;
        if(focus_mode==13){uint32_t root=0x12000;void_call_guest_this(0xA0142080,win_manager+0x38,1,&root);}
    }else if(event!=0x10)bad=66;
    g_eax=1;g_esp+=24;
}
static void thunk_geometry(void){g_eax=1;g_esp+=4;}
static void thunk_notify(void){
    uint32_t event=ARG(0);
    if(lock_depth || g_ecx!=0x18000 || MEM32(event)!=3 || MEM32(event+8)!=MEM32(0x13010) || MEM32(event+12)!=0)bad=52;
    unsigned msg=MEM32(event+4);last_event_id=MEM32(event+8);
    if(changed+finished<2)notifies[changed+finished]=msg;
    if(msg==0x887113A3){
        ++changed;
        if(step_mode==5){context=S2_TOUCH_MODAL|S2_TOUCH_CAS;MEM32(win_manager+0x38)=0;}
        if(step_mode==8){
            unsigned serial=pick_serial;
            if(s2_touch_pick(1,2) || s2_touch_slider_step(700,500,1)!=S2_SLIDER_BUSY)bad=53;
            s2_touch_request_pick(1,2);
            if(serial!=pick_serial)bad=54;
            seen_progress=s2_touch_slider_status(slider_serial);
            s2_touch_barrier(slider_serial); /* duplicate nested dispatch */
        }
    }else if(msg==0x887113A4){
        if(changed!=1 || finished)bad=55;
        ++finished;
        if(step_mode==6){float value=.8f;memcpy(GPTR(0x13108),&value,4);}
    }else bad=56;
    g_eax=1;g_esp+=8;
}
static void thunk_invalidate(void){g_eax=1;g_esp+=4;}
static void thunk_picker(void){
    ++picker_calls;
    if(g_ecx!=0x12000 || ARG(0)!=700 || ARG(1)!=500 || lock_depth || handler_active || message_depth)bad=57;
    g_eax=picker_calls>1 && focus_mode==5?0x16000:picker_calls>1 && focus_mode==9?0:0x13000;g_esp+=12;
}
static void thunk_window(void){
    ++guest_calls;
    if(ARG(1)!=0x202 && ARG(1))bad=58;
    handler_active=1;
    if(nested){nested=0;drive();if(picker_calls || !touch_barrier_deferred)bad=59;}
    guest_handler_order=1;handler_active=0;g_eax=0xCAFEBABE;g_esp+=20;
}
static void thunk_bad(void){bad=60;g_esp+=4;}
void sub_A0957FE3(void);
void sub_A0957FA3(void);
void sub_A0958268(void);
void sub_A09734CB(void);
void sub_A0142080(void);
void sub_A0149CF0(void);
void sub_A09328CD(void);
void sub_A09328AB(void);
static void original_focus(void){++focus_calls;if(setters || changed || finished || refs<2)bad=67;sub_A09734CB();}
static void original_value(void){
    ++value_reads;sub_A0957FA3();
    if(step_mode==4 && value_reads==1)context=S2_TOUCH_CAS|S2_TOUCH_OPTIONS;
}
static void original_set(void){++setters;sub_A0958268();}
static void minimum(void){g_eax=MEM32(g_ecx+8);g_esp+=4;}
static void maximum(void){g_eax=MEM32(g_ecx+12);g_esp+=4;}
recomp_func_t void_resolve(uint32_t va){
    switch(va){
    case 0xA0957FE3:return sub_A0957FE3;
    case 0xA0957FA3:return original_value;
    case 0xA0958268:return original_set;
    case 0xA09734CB:return original_focus;
    case 0xA0142080:return sub_A0142080;
    case 0xA0149CF0:return sub_A0149CF0;
    case 0xA09328CD:return sub_A09328CD;
    case 0xA09328AB:return sub_A09328AB;
    case 0xA0046390:return minimum;
    case 0xA0145770:return maximum;
    case 0x701:return thunk_add;
    case 0x702:return thunk_release;
    case 0x704:return thunk_notify;
    case 0x705:return thunk_invalidate;
    case 0x706:return thunk_geometry;
    case 0x707:return thunk_member;
    case 0x708:return thunk_focus_event;
    case 0x709:return thunk_old_add;
    case 0x70A:return thunk_old_release;
    case 0x70B:return thunk_accept;
    case 0x13572468:return thunk_picker;
    case 0x12340000:return thunk_window;
    default:return thunk_bad;
    }
}
void void_lift_trap(uint32_t va,const char *s){if(!trap_calls)first_trap_va=va;++trap_calls;last_trap_va=va;bad=61;}
'''
CHECK = r'''
uint32_t cases,steps;
static void setup(int value){
    reset();context=S2_TOUCH_CAS;slider_state=SLIDER_IDLE;slider_serial=0;
    query_zero_logs=slider_reject_logs=0;diag_step_lines=diag_query_lines=diag_slider_lines=0;last_stage[0]=0;
    mono_ns=time_fail=read_only_stack=0;refs=1;adds=releases=setters=changed=finished=value_reads=0;
    notifies[0]=notifies[1]=0;step_mode=1;seen_progress=0;guest_handler_order=0;
    focus_calls=focus_events=focus_mode=0;old_refs=1;
    trap_calls=first_trap_va=last_trap_va=last_event_id=0;
    MEM32(win_manager)=0x17000;MEM32(0x17098)=0xA09734CB;MEM32(0x17058)=0x707;MEM32(0x17028)=0x708;
    MEM32(0x12204)=0x709;MEM32(0x12208)=0x70A;
    typed(0x13000,0x13200,0x13800,0x13900,".?AVcGZWinSlider@@");
    MEM32(win_manager+0x38)=0x13000;MEM32(win_manager+0x34)=0;
    MEM32(0x13200)=0xA0957FE3;MEM32(0x13204)=0x701;MEM32(0x13208)=0x702;
    MEM32(0x13338)=0xA0149CF0;MEM32(0x13498)=0xA09328CD;MEM32(0x1349C)=0xA09328AB;
    MEM32(0x133A4)=0x705;MEM32(0x134B8)=0x706;
    MEM32(0x13348)=0x70B;
    MEM32(0x1305C)=0x18000;MEM32(0x130F8)=0x13700;
    MEM32(0x13004)=win_manager;MEM32(0x13010)=3;MEM32(0x18000)=0x18200;MEM32(0x1820C)=0x704;
    MEM32(0x13710)=0xA0957FA3;MEM32(0x13714)=0xA0958268;
    MEM32(0x1371C)=0xA0046390;MEM32(0x13724)=0xA0145770;
    MEM32(0x13100)=0;MEM32(0x13104)=100;MEM8(0x1312C)=0;
    float v=(float)value/100.f;memcpy(GPTR(0x13108),&v,4);
    MEM32(0xA0D9C694u)=0x3F000000u;
}
static int status(uint64_t r){return r&255;}
static int before(uint64_t r){return((r>>8)&255)-101;}
static int after(uint64_t r){return((r>>16)&255)-101;}
static int normal(int minimum,int value,int delta){
    setup(value);void_regs first={0},last={0};void_regs_save(&first);
    MEM32(0x13100)=(uint32_t)minimum;
    float normalized=(float)(value-minimum)/(100-minimum);memcpy(GPTR(0x13108),&normalized,4);
    uint64_t receipt=s2_touch_slider_step(700,500,delta);void_regs_save(&last);
    if(memcmp(&first,&last,sizeof first) || bad || lock_depth || refs!=1 || adds!=1 || releases!=1 || focus_calls || focus_events)return 1;
    int expected=value+delta;if(expected<minimum)expected=minimum;if(expected>100)expected=100;
    if(before(receipt)!=value || after(receipt)!=expected || !((unsigned)(receipt>>32)))return 2;
    if(expected==value){if(status(receipt)!=S2_SLIDER_LIMIT || setters || changed || finished)return 3;}
    else if(status(receipt)!=S2_SLIDER_CHANGED || setters!=1 || changed!=1 || finished!=1 ||
        notifies[0]!=0x887113A3 || notifies[1]!=0x887113A4)return 4;
    if(s2_touch_slider_status((unsigned)(receipt>>32))!=receipt)return 5;
    s2_touch_barrier((unsigned)(receipt>>32));if(changed!=(expected!=value) || finished!=changed)return 6;
    ++steps;return 0;
}
uint32_t selftest(void){
    for(int minimum=0;minimum>=-100;minimum-=100)
      for(int value=minimum;value<=100;++value)for(int delta=-1;delta<=1;delta+=2)REQUIRE(!normal(minimum,value,delta));++cases;
    setup(50);mode=0;uint64_t r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_CANCELLED);
    REQUIRE(before(r)==-101 && after(r)==-101);
    mode=1;drive();REQUIRE(!setters && !picker_calls && !changed && !finished && !bad);++cases;
    setup(50);step_mode=9;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CANCELLED && !setters && !picker_calls && !bad);++cases;
    setup(50);nested=1;pend(0x202,0,0);r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && guest_handler_order && guest_calls==1 && !bad);++cases;
    setup(50);thread=2;r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_CANCELLED);
    thread=1;drive();REQUIRE(!picker_calls && !setters && !bad);++cases;
    setup(50);for(unsigned i=0;i<512;++i)pend(0,0,0);r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CANCELLED && !waits && !setters && !bad);++cases;
    setup(50);for(unsigned i=0;i<1024;++i)queue_push(1,1,0,0,0);r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CANCELLED && !setters && !bad);++cases;
    setup(50);filter=2;pend(0x202,0,0);r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CANCELLED && !setters && !picker_calls);
    filter=0;drive();REQUIRE(!setters && !picker_calls && !bad);++cases;
    unsigned blocked[]={0,S2_TOUCH_LIVE,S2_TOUCH_CAS|S2_TOUCH_MODAL,S2_TOUCH_CAS|S2_TOUCH_OPTIONS,
      S2_TOUCH_CAS|S2_TOUCH_CHOOSER,S2_TOUCH_CAS|S2_TOUCH_LOADING,S2_TOUCH_CAS|S2_TOUCH_LOT_INFO,S2_TOUCH_CAS|S2_TOUCH_BUILD};
    for(unsigned i=0;i<sizeof blocked/sizeof blocked[0];++i){setup(50);context=blocked[i];
      r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !changed && !finished && !bad);}++cases;
    setup(50);MEM32(win_manager+0x38)=0;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && focus_calls==1 && focus_events==1 && refs==2 && adds-releases==1 && !bad);++cases;
    setup(50);MEM32(win_manager+0x34)=0x13000;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && !adds && !setters && !bad);++cases;
    const char *wrong[]={".?AVcGZWinScrollbar2@@",".?AVcGZWinSpinner@@",".?AVcGZWinSliderExtra@@",".?AVcGZWinBtn@@"};
    for(unsigned i=0;i<4;++i){setup(50);typed(0x13000,0x13200,0x13800,0x13900,wrong[i]);
      r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !changed && !finished && !bad);}++cases;
    setup(50);MEM32(0x13104)=101;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && adds==1 && releases==1 && !setters && !bad);++cases;
    setup(50);MEM32(0x13714)=0;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && adds==1 && releases==1 && !setters && !bad);++cases;
    setup(50);MEM8(0x1312C)=1;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_FAILED && setters==1 && !changed && !finished && refs==1 && !bad);++cases;
    setup(50);step_mode=4;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && refs==1 && !bad);++cases;
    setup(50);step_mode=5;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && changed==1 && finished==1 && refs==1 && !bad);++cases;
    setup(50);step_mode=6;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_FAILED && before(r)==50 && after(r)==80 && changed==1 && finished==1 && !bad);++cases;
    setup(50);step_mode=8;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && status(seen_progress)==S2_SLIDER_IN_PROGRESS && changed==1 && finished==1 && !bad);++cases;
    setup(50);time_fail=1;r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_REJECTED && !pending_count);++cases;
    setup(50);read_only_stack=1;r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !adds);++cases;
    setup(50);REQUIRE(s2_touch_slider_step(700,500,0)==S2_SLIDER_REJECTED && !pending_count);++cases;
    setup(50);pick_serial=0xFFFFFFFFu;r=s2_touch_slider_step(700,500,1);
    REQUIRE((unsigned)(r>>32)==1 && status(r)==S2_SLIDER_CHANGED && !s2_touch_slider_status(7) && !bad);++cases;
    REQUIRE(slider_receipt(7,S2_SLIDER_FAILED,500,-101)==((uint64_t)7<<32|S2_SLIDER_FAILED));
    REQUIRE(slider_receipt(7,S2_SLIDER_CHANGED,-100,100)==((uint64_t)7<<32|S2_SLIDER_CHANGED|1u<<8|201u<<16));
    REQUIRE(slider_receipt(7,S2_SLIDER_CHANGED,-1,0)==((uint64_t)7<<32|S2_SLIDER_CHANGED|100u<<8|101u<<16));++cases;
    setup(50);MEM32(win_manager+0x38)=0x12000;void_regs first={0},last={0};void_regs_save(&first);
    r=s2_touch_slider_step(700,500,1);void_regs_save(&last);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && before(r)==50 && after(r)==51 && picker_calls==2 &&
      focus_calls==1 && focus_events==2 && changed==1 && finished==1 && refs==2 && old_refs==0 &&
      adds==3 && releases==2 && !memcmp(&first,&last,sizeof first) && !bad);++cases;
    for(unsigned f=1;f<=7;++f){
      setup(50);focus_mode=f;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,1);
      REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !changed && !finished && !bad);
      if(f<=2)REQUIRE(refs==1 && adds==releases && old_refs==1);
      else REQUIRE(refs==2 && adds-releases==1 && old_refs==0);
    }++cases;
    setup(50);focus_mode=8;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && before(r)==60 && after(r)==61 && changed==1 && finished==1 && !bad);++cases;
    setup(50);MEM32(win_manager+0x38)=0x12000;
    typed(0x12000,0x12200,0x12800,0x12900,".?AVcGZWinTextEdit@@");
    r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && focus_calls==1 && !(s2_touch_context()&S2_TOUCH_FOCUSED_TEXT) && !bad);++cases;
    for(unsigned f=9;f<=13;++f){
      setup(50);focus_mode=f;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,1);
      REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !changed && !finished && !bad);
      if(f==13)REQUIRE(refs==1 && old_refs==1 && adds==releases);
      else REQUIRE(refs==2 && old_refs==0 && adds-releases==1);
    }++cases;
    setup(50);MEM32(win_manager+0x38)=0x12000;void_regs probe_before={0},probe_after={0};void_regs_save(&probe_before);
    s2_touch_request_pick(700,500);drive();void_regs_save(&probe_after);
    REQUIRE(diag_slider_lines==1 && value_reads==1 && !focus_calls && !setters && !changed && !finished && refs==1 && adds==releases &&
      !memcmp(&probe_before,&probe_after,sizeof probe_before) && !bad);
    REQUIRE(diag_words[0]==0x13000 && diag_words[4]==0x130F8 && diag_words[9]==7 && diag_words[10]==50 &&
      !diag_words[11] && diag_words[12]==100 && diag_words[13]==0x12000 && diag_words[14]==1 && diag_words[15]==1);++cases;
    setup(50);MEM32(0x13100)=(uint32_t)-100;s2_touch_request_pick(700,500);drive();
    REQUIRE(diag_slider_lines==1 && (int)diag_words[11]==-100 && diag_words[12]==100 && diag_words[10]==0 &&
      !focus_calls && !setters && refs==1 && !bad);++cases;
    setup(50);MEM32(0x13100)=(uint32_t)-50;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && !strcmp(last_stage,"rangeBeforeFocus") && diag_step_lines==1 && !focus_calls && !setters && !bad);++cases;
    setup(50);focus_mode=2;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && !strcmp(last_stage,"FocusReturn") && !setters && refs==1 && !bad);++cases;
    setup(50);focus_mode=5;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_REJECTED && !strcmp(last_stage,"postPick") && !setters && refs==2 && !bad);++cases;
    setup(50);context=0;for(unsigned n=0;n<20;++n)REQUIRE((s2_touch_slider_step(700,500,1)&255)==S2_SLIDER_REJECTED);
    REQUIRE(diag_step_lines==16 && slider_reject_logs==16 && !setters && !bad);++cases;
    setup(50);mode=0;for(unsigned n=0;n<20;++n)REQUIRE(!s2_touch_pick(700,500));
    REQUIRE(diag_query_lines==16 && query_zero_logs==16);mode=1;drive();REQUIRE(!picker_calls && !setters && !bad);++cases;
    setup(50);context=0;REQUIRE((s2_touch_pick(700,500)&3)==1);
    REQUIRE(diag_query_lines==0 && pick_owner_context==0 && pick_owner_root==0x12000 && pick_owner_hit==0x13000 && !bad);++cases;
    const int unsupported_min[]={-101,-99,-1,1,50};
    for(unsigned n=0;n<5;++n){setup(50);MEM32(0x13100)=(uint32_t)unsupported_min[n];r=s2_touch_slider_step(700,500,1);
      REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !focus_calls && refs==1 && !bad);}++cases;
    setup(50);MEM32(0x13100)=(uint32_t)-100;MEM32(win_manager+0x38)=0x12000;
    r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && before(r)==0 && after(r)==1 && changed==1 && finished==1 &&
      focus_calls==1 && refs==2 && !bad);++cases;
    setup(50);MEM32(0x13100)=(uint32_t)-100;float too_low=-.01f;memcpy(GPTR(0x13108),&too_low,4);
    r=s2_touch_slider_step(700,500,1);REQUIRE(status(r)==S2_SLIDER_REJECTED && !setters && !changed && !finished && !bad);++cases;
    setup(0);MEM32(0x13100)=(uint32_t)-100;MEM32(win_manager+0x38)=0x12000;r=s2_touch_slider_step(700,500,-1);
    REQUIRE(status(r)==S2_SLIDER_LIMIT && before(r)==-100 && after(r)==-100 && focus_calls==1 && refs==2 &&
      !setters && !changed && !finished && !bad);++cases;
    setup(50);MEM32(0x13010)=0x88020000;r=s2_touch_slider_step(700,500,1);
    REQUIRE(status(r)==S2_SLIDER_CHANGED && last_event_id==0x88020000 && !trap_calls && !bad);++cases;
    return 0;
}
'''


def build_source(touch_source=None):
    paths = {n: (B.ROOT / ('android/host/' + n + '.c')).read_text(encoding='utf-8')
             for n in ('input', 'user32', 'sims2_touch', 'core', 'threads')}
    if touch_source: paths['sims2_touch'] = touch_source.read_text(encoding='utf-8')
    u, i, touch, core, threads = (paths[n] for n in ('user32', 'input', 'sims2_touch', 'core', 'threads'))
    state = u[u.index('typedef struct { uint32_t hwnd, msg, wparam, lparam, time, x, y; } qmsg_t;'):
              u.index('static void timers_post_due(void);')]
    state += i[i.index('typedef struct { uint32_t msg, wparam, lparam; } pending_t;'):i.index('void void_input_screen_size(')]
    state += 'static __thread unsigned message_depth, touch_barrier_deferred, modal_message_depth;\n'
    prelude = T.PRELUDE.replace('static void drive(void);',
        'static void drive(void);\nstatic unsigned mono_ns,time_fail,read_only_stack;')
    prelude += '\nuint64_t void_monotonic_ns(void){return mono_ns;}\n'
    prelude = prelude.replace('t->tv_sec=10;t->tv_nsec=0;return 0;', 't->tv_sec=10;t->tv_nsec=c==CLOCK_MONOTONIC?mono_ns:0;return time_fail;')
    prelude = prelude.replace('pthread_mutex_unlock(p);drive();pthread_mutex_lock(p);return 1;',
        'pthread_mutex_unlock(p);if(step_mode==9)mono_ns=80000000;drive();pthread_mutex_lock(p);return 1;')
    prelude = prelude.replace('static uint32_t lock_depth,', 'static unsigned step_mode;\nstatic uint32_t lock_depth,')
    prelude = prelude.replace('return void_vm_readable(a,n);}', 'return !read_only_stack && void_vm_readable(a,n);}')
    prelude = prelude.replace('void void_log(const char *s,...){ }', r'''
static unsigned diag_step_lines,diag_query_lines,diag_slider_lines,diag_words[16];
static char last_stage[64];
void void_log(const char *s,...){
 va_list ap;va_start(ap,s);
 if(strstr(s,"[touch-step]")){++diag_step_lines;const char *p=va_arg(ap,const char *);unsigned n=0;while(p[n] && n<63){last_stage[n]=p[n];++n;}last_stage[n]=0;}
 if(strstr(s,"[touch-query]"))++diag_query_lines;
 if(strstr(s,"[touch] slider")){++diag_slider_lines;for(unsigned n=0;n<16;++n)diag_words[n]=(n>=10 && n<=12) || n>=14 ? (uint32_t)va_arg(ap,int):va_arg(ap,unsigned);}
 va_end(ap);
}
''')
    actual = ''.join(function(core, s) for s in ('void void_regs_save(', 'void void_regs_load('))
    actual += '\n'.join(line for line in touch.splitlines() if not line.startswith('#include'))
    actual += ''.join(function(u, s) for s in ('static queue_t *queue_for(', 'static void queue_push(',
        'void void_post_message(', 'static int matches(', 'static int queue_take(', 'static void store_msg(',
        'static void message_returned(', 'static uint32_t send_message(', 'static uint32_t dispatch_message('))
    actual += ''.join(function(i, s) for s in ('static void pend(', 'int void_input_touch_barrier(', 'void void_input_pump('))
    actual += ''.join(function(threads, s) for s in ('uint32_t void_call_guest(', 'uint32_t void_call_guest_this('))
    lifted = ''
    for chunk, name in [('A0950000', 'A0957FE3'), ('A0950000', 'A0957FA3'), ('A0958000', 'A0958268'),
                        ('A0140000', 'A0142080'), ('A0970000', 'A09734CB'),
                        ('A0148000', 'A0149CF0'), ('A0930000', 'A0931C2C'),
                        ('A0930000', 'A09328AB'), ('A0930000', 'A09328CD')]:
        code = (B.ROOT / f'generated/sims2-x86/sims2_{chunk}.c').read_text()
        lifted += function(code, f'void sub_{name}(void) {{')
    names = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
    lifted = '\n'.join(f'#define {n} g_{n}' for n in names) + '\n#define _xmm g_xmm\n' + lifted
    lifted += '\n'.join(f'\n#undef {n}' for n in names) + '\n#undef _xmm\n'
    utilities = T.CHECK[:T.CHECK.index('uint32_t void_call_guest(')]
    reset = function(T.CHECK, 'static void reset(')
    extra = EXTRA.replace('static unsigned mono_ns,time_fail,read_only_stack;', '')
    forward = 'static void queue_push(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);\n'
    forward += 'static void message_returned(void);\nstatic uint32_t send_message(uint32_t,uint32_t,uint32_t,uint32_t);\n'
    return prelude + state + forward + actual + extra + lifted + utilities + reset + CHECK


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--expect-v14d-failure',action='store_true');args=parser.parse_args()
    output=OUT/'before-repro' if args.expect_v14d_failure else OUT
    output.mkdir(parents=True, exist_ok=True)
    cfile, binary = output / 'check.c', output / 'check.elf'
    before_source=OUT/'before-v14d/android/host/sims2_touch.c' if args.expect_v14d_failure else None
    cfile.write_text(build_source(before_source))
    subprocess.run([str(B.CLANG), *B.COMMON, '-O2', '-mno-outline-atomics', '-fno-builtin', '-fno-stack-protector', '-nostdlib',
        '-Iandroid/host', '-Iandroid/runtime', '-Wl,--gc-sections,-e,selftest', str(cfile), '-o', str(binary)], check=True)
    elf = ELFFile(io.BytesIO(binary.read_bytes()))
    symbols = {s.name: s['st_value'] for s in elf.get_section_by_name('.symtab').iter_symbols()}
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, guest, stack, stop = 0x10000000, 0x2000000000, 0x50000000, 0x60000000
    uc.mem_map(base, 0x1000000); uc.mem_map(guest + 0x10000, 0x20000)
    uc.mem_map(guest + 0xA0D9C000, 4096); uc.mem_map(stack, 0x10000); uc.mem_map(stop, 0x1000)
    for segment in elf.iter_segments():
        if segment['p_type'] == 'PT_LOAD': uc.mem_write(base + segment['p_vaddr'], segment.data())
    relocations = elf.get_section_by_name('.rela.dyn')
    if relocations:
        for r in relocations.iter_relocations():
            assert r['r_info_type'] == 1027
            uc.mem_write(base + r['r_offset'], (base + r['r_addend']).to_bytes(8, 'little'))
    uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, stack + 0x100)
    uc.reg_write(A.UC_ARM64_REG_SP, stack + 0xF000); uc.reg_write(A.UC_ARM64_REG_LR, stop)
    uc.emu_start(base + symbols['selftest'], stop, count=100000000)
    assert uc.reg_read(A.UC_ARM64_REG_PC) == stop
    failure=uc.reg_read(A.UC_ARM64_REG_X0)
    traps={name:int.from_bytes(uc.mem_read(base+symbols[name],4),'little') for name in ('trap_calls','first_trap_va','last_trap_va')}
    if args.expect_v14d_failure:
        assert failure and traps['first_trap_va']==0xA0149CF0 and traps['last_trap_va']==0xA09328CD, (failure,traps)
        receipt=dict(expected_regression_failure=True,C_assertion_line=failure,**traps,
            frozen_source_sha256=hashlib.sha256(before_source.read_bytes()).hexdigest(),
            scope='Actual lifted GetID/5arg notification and recipient dispatcher detect both wrong host call arities; strict guest-call checks unchanged')
        (output/'results.json').write_text(json.dumps(receipt,indent=2)+'\n');print('EXPECTED v14d ABI FAILURE:',receipt);return
    assert failure == 0, ('C line',failure,traps)
    counts = {name: int.from_bytes(uc.mem_read(base + symbols[name], 4), 'little') for name in ('cases', 'steps')}
    pe = pefile.PE(str(B.ROOT / 'generated/input/Sims2EP9.exe'), fast_load=True)
    image = pe.get_memory_mapped_image()
    original = Uc(UC_ARCH_X86, UC_MODE_32)
    original.mem_map(pe.OPTIONAL_HEADER.ImageBase, (len(image) + 65535) & ~65535)
    original.mem_write(pe.OPTIONAL_HEADER.ImageBase, image)
    original.mem_map(0, 4096); original.mem_map(0x10000, 0x20000); original.mem_map(stop, 4096)
    trace = []

    def boundary(cpu, address, size, user):
        arg_counts = {0x706: 0, 0x704: 1, 0x705: 0, 0x707: 1}
        if address not in arg_counts: return
        sp = cpu.reg_read(X.UC_X86_REG_ESP)
        if address == 0x704:
            event=int.from_bytes(cpu.mem_read(sp+4,4),'little')
            trace.append((cpu.reg_read(X.UC_X86_REG_ECX),*struct.unpack('<4I',bytes(cpu.mem_read(event,16)))))
        cpu.reg_write(X.UC_X86_REG_EAX, 1)
        cpu.reg_write(X.UC_X86_REG_EIP, int.from_bytes(cpu.mem_read(sp, 4), 'little'))
        cpu.reg_write(X.UC_X86_REG_ESP, sp + 4 + 4 * arg_counts[address])

    original.hook_add(UC_HOOK_CODE, boundary)
    oracle_steps = 0
    for minimum in (0, -100):
      for value in range(minimum, 101):
        for delta in (-1, 1):
            memory = bytearray(0x20000)
            def put(address, number): struct.pack_into('<I', memory, address - 0x10000, number)
            put(0x13000, 0x13200); put(0x130F8, 0x13700); put(0x1305C, 0x18000)
            put(0x13710, 0xA0957FA3); put(0x134B8, 0x706); put(0x13338, 0xA0149CF0)
            put(0x13498, 0xA09328CD);put(0x1349C,0xA09328AB);put(0x13004,0x11000);put(0x11000,0x17000);put(0x17058,0x707)
            put(0x18000,0x18200);put(0x1820C,0x704);put(0x13010,3)
            put(0x133A4, 0x705); put(0x13100, minimum & 0xffffffff); put(0x13104, 100)
            struct.pack_into('<f', memory, 0x3108, (value - minimum) / (100 - minimum))
            expected = min(100, max(minimum, value + delta))
            put(0x28000, stop); put(0x28004, expected & 0xffffffff)
            original.mem_write(0x10000, bytes(memory)); trace.clear()
            if expected != value:
                original.reg_write(X.UC_X86_REG_ECX, 0x130F8); original.reg_write(X.UC_X86_REG_ESP, 0x28000)
                original.emu_start(0xA0958268, stop, count=10000)
                assert original.reg_read(X.UC_X86_REG_EIP) == stop
                assert original.reg_read(X.UC_X86_REG_EAX) & 255 == 1
                assert trace == [(0x18000, 3, 0x887113A3, 3, 0)]
            original.reg_write(X.UC_X86_REG_ECX, 0x130F8); original.reg_write(X.UC_X86_REG_ESP, 0x28000)
            original.emu_start(0xA0957FA3, stop, count=1000)
            assert original.reg_read(X.UC_X86_REG_EIP) == stop
            assert original.reg_read(X.UC_X86_REG_EAX) == expected & 0xffffffff
            oracle_steps += 1
    original.mem_write(0x13010,struct.pack('<I',0x88020000));original.mem_write(0x28000,struct.pack('<2I',stop,0xCAFEBABE))
    original.reg_write(X.UC_X86_REG_ECX,0x13000);original.reg_write(X.UC_X86_REG_ESP,0x28000)
    original.emu_start(0xA0149CF0,stop,count=100)
    assert original.reg_read(X.UC_X86_REG_ESP)==0x28004 and original.reg_read(X.UC_X86_REG_EAX)==0x88020000
    for trailing in (0,0xDEADBEEF):
        original.mem_write(0x28000,struct.pack('<6I',stop,0x18000,3,0x887113A4,0x88020000,trailing));trace.clear()
        original.reg_write(X.UC_X86_REG_ECX,0x13000);original.reg_write(X.UC_X86_REG_ESP,0x28000)
        original.emu_start(0xA09328CD,stop,count=1000)
        assert original.reg_read(X.UC_X86_REG_ESP)==0x28018
        assert trace==[(0x18000,3,0x887113A4,0x88020000,trailing)]
    result = dict(pass_=True, **counts, actual='ARM64 owner command, input FIFO/DispatchMessage, original lifted QI/getter/setter/focus and smart-reference transfer, real guest call ABI and full register/EH save/restore',
        original_x86_integer_steps=oracle_steps,
        limits='OS waits, references, geometry/invalidation and CAS notification delivery are boundary fixtures; started real-thread timeout tested separately; no device actions',
        source_sha256=hashlib.sha256((B.ROOT / 'android/host/sims2_touch.c').read_bytes()).hexdigest())
    result['actual_notification_abi']='Original GetID noarg RET; original notify5arg RET14 -> original recipient dispatcher2arg RET8 -> final CAS endpoint mock1arg RET4; all4 event words checked'
    result['original_x86_direct_ABI_cases']=3
    (output / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PASS actual ARM64 CAS slider:', counts, 'including all integer values, ordered A3/A4, clamps, cancellation/deadline, full FIFO/filter/owner guards and full ABI/EH bank')


if __name__ == '__main__': main()
