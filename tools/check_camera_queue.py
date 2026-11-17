"""Actual ARM64 camera queue/FIFO: keep MOVE events arriving during an owner command."""
import contextlib
import io
import json

import check_object_rotate_queue as Q
from check_virtual_query import function

OUT=Q.B.ROOT/'build/test/camera-queue'
APPLY=r'''
static unsigned camera_steps,camera_reenter;
static int touch_camera_apply_at(float dx,float dy,float radians,float zoom,float pitch,unsigned epoch,uint64_t size,float from_x,float from_y){
 if(!touch_layout_matches(s2_touch_context(),size,epoch))return 0;
 if(lock_depth || message_depth || handler_active || ui_command_started!=S2_UI_CAMERA ||
    epoch!=7 || size!=((uint64_t)540<<32|1212) || dy || radians || zoom || pitch || from_y!=.2f)bad=301;
 if(!s2_touch_camera_hover_active())bad=306;
 ++camera_steps;
 if(camera_steps==1){
  if(__builtin_fabsf(dx-.05f)>.00001f || from_x!=.1f)bad=302;
  if(camera_reenter){
   unsigned previous=pick_serial;
   if(!s2_touch_pan(.15f,.2f,.19f,.2f) || !s2_touch_pan(.19f,.2f,.22f,.2f))bad=303;
   if(pick_serial==previous || !pick_pending || ui_command_kind!=S2_UI_CAMERA ||
      __builtin_fabsf(camera_request.from_x-.15f)>.00001f || __builtin_fabsf(camera_request.dx-.07f)>.00001f)bad=304;
  }
 }else if(camera_steps!=2 || __builtin_fabsf(dx-.07f)>.00001f || from_x!=.15f)bad=305;
 g_eax=17;g_ecx=18;g_flag_a=19;
 return 1;
}
'''
CHECK=r'''
static volatile uint64_t last_rotation_receipt;
static void camera_setup(void){rot_setup();s2_touch_camera_contact(0);s2_touch_camera_pointer_intent();s2_touch_camera_hover_enable(1);camera_steps=camera_reenter=0;}
uint32_t selftest(void){
 camera_setup();REQUIRE(!s2_touch_camera_hover_active());++rotation_queue_cases;
 s2_touch_camera_contact(1);REQUIRE(s2_touch_camera_hover_active());s2_touch_camera_contact(0);
 REQUIRE(!s2_touch_camera_hover_active());++rotation_queue_cases;
 ui_command_kind=S2_UI_CAMERA;REQUIRE(s2_touch_camera_hover_active());ui_command_kind=S2_UI_CATALOG;
 REQUIRE(!s2_touch_camera_hover_active());ui_command_kind=0;++rotation_queue_cases;
 ui_command_started=S2_UI_CAMERA;REQUIRE(s2_touch_camera_hover_active());ui_command_started=S2_UI_CHEAT;
 REQUIRE(!s2_touch_camera_hover_active());ui_command_started=0;++rotation_queue_cases;
 s2_touch_camera_contact(1);s2_touch_camera_hover_enable(0);
 REQUIRE(!s2_touch_camera_hover_active());++rotation_queue_cases;
 s2_touch_camera_contact(0);ui_command_kind=S2_UI_CAMERA;
 REQUIRE(!s2_touch_camera_hover_active());ui_command_kind=0;ui_command_started=S2_UI_CAMERA;
 REQUIRE(!s2_touch_camera_hover_active());ui_command_started=0;++rotation_queue_cases;
 camera_setup();camera_reenter=1;
 REQUIRE(s2_touch_pan(.1f,.2f,.15f,.2f));unsigned first=pick_serial;last_rotation_receipt=first;
 drive();REQUIRE(camera_steps==1 && !ui_command_started && pick_pending && pick_serial!=first &&
  ui_command_kind==S2_UI_CAMERA && pending_count==2 && s2_touch_camera_hover_active() && !bad && !lock_depth);++rotation_queue_cases;
 unsigned next=pick_serial;s2_touch_barrier(first);REQUIRE(camera_steps==1 && pick_pending);++rotation_queue_cases;
 drive();REQUIRE(camera_steps==2 && !pick_pending && !ui_command_kind && !ui_command_started && s2_touch_camera_hover_active() && !bad);++rotation_queue_cases;
 s2_touch_camera_contact(0);REQUIRE(s2_touch_camera_hover_active());++rotation_queue_cases;
 s2_touch_camera_pointer_intent();REQUIRE(!s2_touch_camera_hover_active());++rotation_queue_cases;
 s2_touch_barrier(next);REQUIRE(camera_steps==2 && !bad);++rotation_queue_cases;
 for(unsigned kind=1;kind<=8;++kind){
  if(kind==S2_UI_CAMERA)continue;
  camera_setup();ui_command_started=kind;
  REQUIRE(!s2_touch_pan(.1f,.2f,.15f,.2f) && !pending_count && !pick_pending && !camera_navigation_intent && !bad);++rotation_queue_cases;
 }
 camera_setup();ui_command_started=S2_UI_CAMERA;
 REQUIRE(!s2_touch_ui_command(S2_UI_SCALE,100) && !pending_count);++rotation_queue_cases;
 float pose[6]={1,2,0,0,35,.15f};REQUIRE(!s2_touch_camera_pose(pose) && !pending_count);++rotation_queue_cases;
 REQUIRE(!s2_touch_pick(700,500) && !pending_count);++rotation_queue_cases;
 REQUIRE(s2_touch_slider_step(700,500,1)==S2_SLIDER_BUSY && !pending_count);++rotation_queue_cases;
 camera_setup();ui_command_started=S2_UI_CAMERA;REQUIRE(s2_touch_pan(.1f,.2f,.15f,.2f));
 ui_command_started=0;atomic_fetch_add(&cas_pan_epoch,1);drive();
 REQUIRE(!camera_steps && !pick_pending && !ui_command_started && !bad);++rotation_queue_cases;
 camera_setup();ui_command_started=S2_UI_CAMERA;REQUIRE(s2_touch_pan(.1f,.2f,.15f,.2f));
 ui_command_started=0;context=S2_TOUCH_LIVE;drive();
 REQUIRE(!camera_steps && !pick_pending && !ui_command_started && !bad);++rotation_queue_cases;
 camera_setup();REQUIRE(s2_touch_pan(.1f,.2f,.15f,.2f) && camera_navigation_intent);++rotation_queue_cases;
 s2_touch_camera_pointer_intent();REQUIRE(!camera_navigation_intent && s2_touch_camera_hover_active());++rotation_queue_cases;
 drive();REQUIRE(camera_steps==1 && !camera_navigation_intent && !s2_touch_camera_hover_active() && !bad);++rotation_queue_cases;
 camera_setup();REQUIRE(s2_touch_camera_pose(pose) && camera_navigation_intent);++rotation_queue_cases;
 camera_setup();pending_count=512;
 REQUIRE(!s2_touch_pan(.1f,.2f,.15f,.2f) && !camera_navigation_intent && !pick_pending);++rotation_queue_cases;
 pending_count=0;pose[4]=0;REQUIRE(!s2_touch_camera_pose(pose) && !camera_navigation_intent);++rotation_queue_cases;
 return 0;
}
'''

def main():
    original=Q.source
    def source():
        text=original().rsplit(Q.CHECK,1)[0]
        old=function(text,'static int touch_camera_apply_at(')
        text=text.replace(old,APPLY,1)
        setup=Q.CHECK[:Q.CHECK.index('uint32_t selftest(void){')]
        return text+setup+CHECK
    Q.source=source;Q.OUT=OUT
    with contextlib.redirect_stdout(io.StringIO()):Q.main()
    path=OUT/'results.json';result=json.loads(path.read_text())
    result['scope']='Actual ARM64 queue/input FIFO/DispatchMessage/owner barrier, full register preservation and atomic contact/pending/apply/navigation-intent hover state. Accepted requests latch navigation intent; UP retains it, genuine pointer intent clears it, and a later camera apply cannot rearm it. Camera apply is a strict boundary that queues two MOVE segments during the running command; projection/surface math and original setters are checked separately.'
    path.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
