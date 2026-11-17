"""Exact Hand/Object and click-only tool classification using original PE type data."""
import hashlib,json
import check_town_callout as C
OUT=C.B.ROOT/'build/test/object-kind-v15d'
CHECK=r'''
uint32_t cases,queries,pose_calls,pose_expected;
void sub_A067D8A4_lifted(void){if(ARG(0)!=pose_expected)bad=212;++pose_calls;g_eax=1;g_esp+=8;}

static void setup(void){
 reset();MEM32(0x2E000)=80;atomic_store(&preview_object,0);context=S2_TOUCH_BUY;memset(GPTR(0x15000),0,0x2000);MEM32(tool_handler+0x20)=0x15000;
 MEM32(0x15000)=0xA0DE83FC;MEM32(0x15064)=MEM32(0x15068)=0x16000;MEM32(0x16000)=0xA0DEBB54;
}
uint32_t selftest(void){
 for(unsigned slot=0;slot<2;++slot){
  setup();MEM8(0x150FC)=slot;s2_touch_pump();REQUIRE(s2_touch_tool()==1 && (uint32_t)s2_touch_tool_token()==1 && !bad);++cases;
  uint64_t hand=s2_touch_tool_token();MEM32(0x16054)=1;MEM32(0x1605C)=0x19000;s2_touch_pump();REQUIRE(s2_touch_tool()==2 && s2_touch_tool_token()!=hand && s2_touch_object_cost()==80 && !bad);++cases;
  MEM32(0x16054)=3;s2_touch_pump();REQUIRE(s2_touch_tool()==2 && !bad);++cases;
  REQUIRE(touch_object_preview_frozen(0x15000));
  REQUIRE(!s2_touch_object_preview(s2_touch_tool_token()+(1ull<<32)));
  REQUIRE(s2_touch_object_preview(s2_touch_tool_token())&&!touch_object_preview_frozen(0x15000));++cases;
  MEM32(0x2E000)=0;MEM32(0x19000)=0xA0DD3754;MEM16(0x191CC)=751;MEM32(0x2E004)=2000;
  s2_touch_pump();REQUIRE(s2_touch_object_cost()==2000&&!bad);++cases;
  MEM32(0x19000)=0xA0DD2F0C;MEM32(0x2E004)=325;
  s2_touch_pump();REQUIRE(s2_touch_object_cost()==325&&!bad);++cases;
  MEM32(0x16054)=1;s2_touch_pump();REQUIRE(s2_touch_object_cost()==325&&!bad);++cases;
  MEM32(0x16054)=3;MEM32(0x19000)=0x2FFFF;s2_touch_pump();REQUIRE(!s2_touch_object_cost()&&!bad);++cases;
  atomic_store(&preview_object,0);MEM32(0x19000)=0xA0DD3754;MEM8(0x1607D)=1;MEM32(0x16070)=0;
  MEM32(0x16060)=0x41480000;MEM32(0x16064)=0x419C0000;MEM32(0x16068)=1;
  g_ecx=0x15000;g_esp=0x2D000;MEM32(0x2D004)=0x1A000;pose_expected=0x15074;
  unsigned calls=pose_calls;sub_A067D8A4();
  REQUIRE(!bad&&pose_calls==calls+1&&g_esp==0x2D008&&MEM32(0x2D004)==0x1A000&&!memcmp(GPTR(0x15074),GPTR(0x16060),12));++cases;
  REQUIRE(s2_touch_object_preview(s2_touch_tool_token()));
  g_ecx=0x15000;g_esp=0x2D000;pose_expected=0x1A000;sub_A067D8A4();REQUIRE(!bad&&pose_calls==calls+2);++cases;
  MEM32(0x16054)=0;MEM32(0x1605C)=0;s2_touch_pump();REQUIRE(!atomic_load(&preview_object)&&!s2_touch_object_preview(s2_touch_tool_token()));++cases;

  for(unsigned state=0;state<6;++state)for(unsigned object=0;object<2;++object){
   setup();MEM8(0x150FC)=slot;MEM32(0x16054)=state;MEM32(0x1605C)=object?0x19000:0;
   s2_touch_pump();unsigned expected=state==0&&!object?1:((state==1||state==3)&&object?2:0);
   REQUIRE(s2_touch_tool()==expected && !bad);++cases;
  }
  setup();MEM8(0x150FC)=slot;MEM32(0x15000+(slot?0x68:0x64))=0;s2_touch_pump();REQUIRE(!s2_touch_tool() && !bad);++cases;
  setup();MEM8(0x150FC)=slot;MEM32(0x16000)=0xA0DEBC7C;s2_touch_pump();REQUIRE(!s2_touch_tool() && !bad);++cases;
 }
 setup();MEM32(0x15000)=0xA0DE83FC+4;REQUIRE(!touch_object_kind(0x15000));++cases;
 setup();MEM32(0x15064)=0x2FFFF;REQUIRE(!touch_object_kind(0x15000));++cases;
 for(unsigned mode=0;mode<2;++mode){
  setup();context=mode?S2_TOUCH_BUILD:S2_TOUCH_BUY;
  MEM32(0x15000)=0xA0DEB2A4;s2_touch_pump();
  uint64_t eyedropper=s2_touch_tool_token();REQUIRE((uint32_t)eyedropper==6 && !s2_touch_fill_kind() && !bad);++cases;
  s2_touch_pump();REQUIRE(s2_touch_tool_token()==eyedropper && !bad);++cases;
  MEM32(0x15000)=0xA0DEB4E4;s2_touch_pump();
  REQUIRE(s2_touch_tool()==6 && s2_touch_tool_token()!=eyedropper && (s2_touch_tool_token()>>32)!=(eyedropper>>32) && !bad);++cases;
  /* Similar original RTTI names and an interior vtable never gain the click capability. */
  const uint32_t other[]={0xA0DEC65C,0xA0DEBBE4,0xA0DEB2A8};
  for(unsigned i=0;i<sizeof other/sizeof other[0];++i){MEM32(0x15000)=other[i];s2_touch_pump();REQUIRE(!s2_touch_tool() && !bad);++cases;}
 }
 return 0;
}
'''
def main():
 pe=C.pefile.PE(str(C.B.ROOT/'generated/input/Sims2EP9.exe'),fast_load=True);original=C.source
 C.source=lambda path,current:original(path,current).rsplit(C.CHECK,1)[0].replace("return 80;", "return MEM32(0x2E000);").replace("if(va==0xA067AD3Au)", "if(va==0xA0547400u||va==0xA052F8D0u){if(self!=0x19000||n||args)bad=211;return MEM32(0x2E004);}if(va==0xA067AD3Au)")+CHECK
 result=C.run_arm(C.B.ROOT/'android/host/sims2_touch.c',OUT,True,pe.get_memory_mapped_image(),pe.OPTIONAL_HEADER.ImageBase)
 result.update(pass_=True,production_sha256=hashlib.sha256((C.B.ROOT/'android/host/sims2_touch.c').read_bytes()).hexdigest(),limits='Actual C owner pump/classifier and original PE RTTI/vtables, exact Eyedropper/ObjectDesign kind6 and token generation. Price getter is a controlled read-only boundary; no draw/model or device tool acceptance. Other active wrapper states deliberately unknown.')
 (OUT/'results.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
