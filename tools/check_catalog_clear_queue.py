"""Production owner FIFO: catalog clearing waits for Hand and shares UI command exclusion."""
import contextlib
import io
import json
import check_object_rotate_queue as Q

OUT=Q.B.ROOT/'build/test/catalog-clear-queue'
CHECK=r'''
static void hand_setup(void){
 rot_setup();MEM32(0x15054)=0;MEM32(0x1505C)=0;s2_touch_pump();
 clear_calls=0;clear_ticket_value=17;
}
uint32_t selftest(void){
 hand_setup();REQUIRE(touch_object_kind(0x13000)==1);
 REQUIRE(s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0) && !clear_calls);
 unsigned serial=pick_serial;drive();REQUIRE(clear_calls==1 && !bad && !lock_depth);++rotation_queue_cases;
 s2_touch_barrier(serial);REQUIRE(clear_calls==1);++rotation_queue_cases;
 hand_setup();clear_ticket_value=0;REQUIRE(!s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0) && !pending_count && !clear_calls);++rotation_queue_cases;
 hand_setup();slider_state=SLIDER_STARTED;REQUIRE(!s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0) && !clear_calls);++rotation_queue_cases;
 hand_setup();ui_command_kind=S2_UI_CAMERA;ui_command_expiry.tv_sec=11;
 REQUIRE(!s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0) && !clear_calls);++rotation_queue_cases;
 hand_setup();ui_command_started=1;REQUIRE(!s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0) && !clear_calls);++rotation_queue_cases;
 hand_setup();REQUIRE(s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0));
 MEM32(0x15054)=1;MEM32(0x1505C)=0x19000;drive();REQUIRE(!clear_calls && !bad);++rotation_queue_cases;
 hand_setup();REQUIRE(s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0));
 MEM32(0x13000)=0xA0DE83FC+4;drive();REQUIRE(!clear_calls && !bad);++rotation_queue_cases;
 hand_setup();REQUIRE(s2_touch_ui_command(S2_UI_CLEAR_CATALOG,0));
 mono_ns=2000000000;drive();REQUIRE(!clear_calls && !bad);++rotation_queue_cases;
 return 0;
}
'''

def main():
    original_source=Q.source
    def source():
        text=original_source().rsplit(Q.CHECK,1)[0]
        text=text.replace('uint32_t s2_catalog_clear_request(void){return 1;}',
                          'static unsigned clear_calls,clear_ticket_value;uint32_t s2_catalog_clear_request(void){return clear_ticket_value;}')
        text=text.replace('int s2_catalog_clear_owner(uint32_t serial){return serial==1;}',
                          'int s2_catalog_clear_owner(uint32_t serial){if(serial!=17||lock_depth)bad=221;++clear_calls;return 1;}')
        setup=Q.CHECK[:Q.CHECK.index('uint32_t selftest(void){')]
        return text+setup+CHECK
    Q.source=source;Q.OUT=OUT
    with contextlib.redirect_stdout(io.StringIO()):Q.main()
    path=OUT/'results.json';result=json.loads(path.read_text())
    result['scope']='Production ARM64 owner FIFO/DispatchMessage and original PE object-tool RTTI; deferred clear, once-only dispatch, no-cache/busy/expiry rejection, Hand required. Catalog validation/original ClearSelection dispatch are checked separately in check_catalog_bound.py.'
    path.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))

if __name__=='__main__':main()
