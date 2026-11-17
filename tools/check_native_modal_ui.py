"""Actual owner-fenced modal UI classification using the original PE button type."""
import hashlib,json
import check_town_callout as C
OUT=C.B.ROOT/'build/test/native-modal-ui-v15d'
CHECK=r'''
uint32_t cases,queries;
static void setup(void){
 reset();context=S2_TOUCH_BUY|S2_TOUCH_MODAL;slider_state=SLIDER_IDLE;ui_command_kind=ui_command_started=0;
 memset(GPTR(0x18000),0,0x3000);MEM32(0x18000)=0xA0E53EEC;MEM32(0x18010)=0xACB50200;
 MEM32(0x18058)=0x19000;MEM32(0x180E8)=3;
 typed(0x19000,0x19100,0x19200,0x19300,".?AVcGZWinFlatRect@@");MEM32(0x19058)=0x1A000;
 typed(0x1A000,0x1A100,0x1A200,0x1A300,".?AVcTSWinSceneGraph@@");MEM32(0x1A058)=0x12000;
 picked=0x18000;scene_window=0x1A000;
}
static int pick(void){++queries;return s2_touch_pick(700,500);}
uint32_t selftest(void){
 setup();REQUIRE(pick()==1 && !bad && picker_calls==1);++cases;
 setup();MEM32(win_manager+0x34)=0x18000;REQUIRE(pick()==1 && !bad);++cases;
 setup();context=S2_TOUCH_BUILD|S2_TOUCH_MODAL;REQUIRE(pick()==1 && !bad);++cases;
 for(unsigned flags=0;flags<3;++flags){setup();MEM32(0x180E8)=flags;REQUIRE(!pick() && !bad);++cases;}
 setup();picked=0x19000;REQUIRE(!pick() && !bad);++cases;
 setup();picked=scene_window;REQUIRE(!pick() && !bad);++cases;
 setup();picked=0x12000;REQUIRE(!pick() && !bad);++cases;
 setup();MEM32(0x18000)=0xA0E53EF0;REQUIRE(!pick() && !bad);++cases;
 setup();context=0;REQUIRE(pick()==1 && !bad);++cases;
 setup();context=S2_TOUCH_OPTIONS;REQUIRE(pick()==1 && !bad);++cases;
 setup();context|=S2_TOUCH_LOADING;REQUIRE(!pick() && !bad && !picker_calls);++cases;
 setup();thread=2;REQUIRE(!pick() && !bad && !picker_calls);++cases;
 return 0;
}
'''
def main():
 pe=C.pefile.PE(str(C.B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True)
 original=C.source
 C.source=lambda path,current:original(path,current).rsplit(C.CHECK,1)[0]+CHECK
 result=C.run_arm(C.B.ROOT/'android/host/sims2_touch.c',OUT,True,pe.get_memory_mapped_image(),pe.OPTIONAL_HEADER.ImageBase)
 result.update(pass_=True,production_sha256=hashlib.sha256((C.B.ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),limits='Actual C FIFO/owner dispatch/pick classifier and original RTTI/vtable. Root picker/window callbacks are boundaries; physical native X remains device acceptance pending.')
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
