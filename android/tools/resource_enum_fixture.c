/* Shared modeled allocation/vector callbacks; original/patched code is real. */
#include "host.h"
recomp_machine_t g_m;
uint32_t g_icall_trace[ICALL_TRACE_SIZE],g_icall_from[ICALL_TRACE_SIZE],g_icall_trace_idx,g_icall_count;
uint32_t g_enter_trace[1024],g_enter_idx;
void (*recomp_yield_hook)(void);
void sub_A01F3D30(void),sub_A09AC040(void),sub_A09AC040_native(void);
void sims2_resource_enumerate(void);
void s2_resource_skip_enable(int);
static uint32_t heap_at;
unsigned diag_active,retarget,mutation,event_count,yield_count;
int fixture_count_thunks,fixture_trace_n;
static unsigned record_enabled=1;
typedef struct {uint32_t kind;recomp_machine_t machine;uint32_t stack[32];uint32_t icall[2];} event;
event events[256];
void *memcpy(void *to,const void *from,size_t n){for(size_t i=0;i<n;++i)((unsigned char *)to)[i]=((const unsigned char *)from)[i];return to;}
static void record(uint32_t kind){
 if(!record_enabled)return;
 if(event_count>=256)__builtin_trap();
 event *e=&events[event_count++];e->kind=kind;e->machine=g_m;
 for(unsigned i=0;i<32;++i)e->stack[i]=MEM32(g_esp-64+4*i);
 e->icall[0]=g_icall_trace[127];e->icall[1]=g_icall_from[127];
}
int void_vm_readable(uint32_t at,uint32_t length){
 uint64_t end=(uint64_t)at+length;
 return (at>=0x10000&&end<=0xA0000)||(end<=0x1000)||(at>=0xFFFFF000u&&end<=0x100000000ull);
}
int void_vm_writable(uint32_t at,uint32_t length){return void_vm_readable(at,length);}
static void unknown(void);
void_fn void_dispatch_find(uint32_t va){
 if(va==0xA01F3D30u)return retarget?unknown:sub_A01F3D30;
 if(va==0xA09AC040u)return sims2_resource_enumerate;
 return NULL;
}
void void_log(const char *fmt,...){(void)fmt;}
static void finish(uint32_t words,uint32_t tag){
 g_eax=0xE1000000u|tag;g_ecx=0xC1000000u|tag;g_edx=0xD1000000u|tag;
 g_flag_k=FK_TEST;g_flag_a=g_flag_b=tag;g_flag_cf=0;g_esp+=4+words*4;
}
static uint32_t allocate(uint32_t bytes){
 uint32_t raw=heap_at;heap_at+=(bytes+63u)&~31u;
 if(heap_at>=0x7A000)__builtin_trap();
 uint32_t p=raw+32;MEM32(p-4)=raw;return p;
}
void sub_A09A5500(void){
 record(1);uint32_t vec=g_ecx,count=MEM32(MEM32(g_esp+4)),base=allocate(count*8);
 MEM32(vec)=MEM32(vec+4)=base;MEM32(vec+8)=base+count*8;finish(1,1);
}
void sub_A09A37D0(void){
 record(2);uint32_t vec=g_ecx,pair=MEM32(g_esp+8),base=MEM32(vec),end=MEM32(vec+4),cap=MEM32(vec+8),n=(end-base)/8;
 if(end==cap){
  uint32_t capacity=n?2*n:2,new_base=allocate(capacity*8);
  for(uint32_t i=0;i<n*8;i+=4)MEM32(new_base+i)=MEM32(base+i);
  base=new_base;MEM32(vec)=base;MEM32(vec+8)=base+capacity*8;
 }
 MEM32(base+n*8)=MEM32(pair);MEM32(base+n*8+4)=MEM32(pair+4);MEM32(vec+4)=base+(n+1)*8;finish(2,2);
}
void sub_A09A5E20(void){
 record(3);
 /* A deterministic permutation exercises subsequent materialization. */
 uint32_t first=MEM32(g_esp+4),n=MEM32(g_esp+12);
 for(uint32_t i=0;i<n/2;++i){
  uint32_t a=first+i*8,b=first+(n-1-i)*8,x=MEM32(a),y=MEM32(a+4);
  MEM32(a)=MEM32(b);MEM32(a+4)=MEM32(b+4);MEM32(b)=x;MEM32(b+4)=y;
 }
 finish(0,3);
}
void sub_A09B2FB0(void){
 record(4);uint32_t vec=g_ecx,count=MEM32(g_esp+4),base=MEM32(vec),n=(MEM32(vec+4)-base)/16;
 if(MEM32(vec+8)-base<count*16){
  uint32_t replacement=allocate(count*16);
  for(uint32_t i=0;i<n*16;i+=4)MEM32(replacement+i)=MEM32(base+i);
  base=replacement;MEM32(vec)=base;MEM32(vec+8)=base+count*16;
 }
 MEM32(vec+4)=base+count*16;finish(1,4);
}
void sub_A0824624(void){record(5);finish(0,5);}
void sub_A01443B0(void){__builtin_trap();}
void sub_A082A797(void){__builtin_trap();}
void void_lift_trap(uint32_t at,const char *why){(void)at;(void)why;__builtin_trap();}
static void unknown(void){
 uint32_t p=MEM32(g_esp+4),group=MEM32(p+4),wanted=MEM32(g_ecx+8);
 g_eax=(group&0xFFFFFF00u)|((group==wanted)?1:0);g_flag_k=FK_CMP;g_flag_a=group;g_flag_b=wanted;g_flag_cf=group<wanted;g_esp+=8;
}
recomp_func_t void_resolve(uint32_t va){
 if(diag_active)++g_dispatch_gen;
 if(va==0xA01F3D30u)return retarget?unknown:sub_A01F3D30;
 if(va==0xA01F3D31u)return unknown;
 __builtin_trap();
}
static void yield_callback(void){
 record(6);++yield_count;
 if(mutation==1)MEM32(0x17008)^=1;
 if(mutation==2)MEM32(0x1800C)=0xA01F3D31u;
 if(mutation==3){g_edx^=0;MEM32(0x16000)^=0x12345678u;}
 if(mutation==4)g_backedges=RECOMP_YIELD_EVERY-5;
}
void reset(void){
 fixture_count_thunks=diag_active==1;fixture_trace_n=diag_active==2;
 record_enabled=1;
 heap_at=0x40000;event_count=yield_count=0;
 for(unsigned i=0;i<sizeof(events);++i)((unsigned char *)events)[i]=0;
 for(unsigned i=0;i<ICALL_TRACE_SIZE;++i)g_icall_trace[i]=g_icall_from[i]=0;
 g_icall_trace_idx=g_icall_count=g_enter_idx=0;
 recomp_yield_hook=mutation?yield_callback:0;
}
void benchmark_reset(void){
 record_enabled=0;heap_at=0x40000;event_count=yield_count=0;recomp_yield_hook=0;
 MEM32(0x32000)=MEM32(0x32004)=MEM32(0x32008)=0;
}
uint32_t run_original(void){sub_A09AC040();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}
uint32_t run_patched(void){sims2_resource_enumerate();return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1);}

uint64_t resource_stats[2];
void s2_resource_skip_stats(uint64_t *,uint64_t *);
void get_resource_stats(void){s2_resource_skip_stats(resource_stats,resource_stats+1);}

#ifdef S2_RESOURCE_DEVICE_PROBE
/* Standalone fixture: real original/clone/predicate/helper, modeled vector ABI. */
#define _GNU_SOURCE
#include "host.h"
#include <assert.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
extern const unsigned char fixtures_begin[],fixtures_end[];
extern unsigned diag_active,retarget,mutation,event_count,yield_count;
extern uint32_t g_icall_trace[128],g_icall_from[128],g_icall_trace_idx;
extern event events[256];
void reset(void),benchmark_reset(void),sub_A09AC040(void),sub_A09AC040_native(void);
void s2_resource_skip_enable(int enabled),s2_resource_skip_stats(uint64_t *,uint64_t *);
static unsigned page_bytes;
static uint32_t get(const unsigned char *p){uint32_t x;memcpy(&x,p,4);return x;}
static uint32_t flags(void){return recomp_eflags(g_flag_k,g_flag_a,g_flag_b,g_flag_cf,1)&0x8D5;}
static void setup(const unsigned char *record,unsigned which){
 memset(GPTR(0x10000),(int)(which&255),0x90000);memset(GPTR(0),(int)(which&255),page_bytes);
 memset(GPTR(0u-page_bytes),(int)(which&255),page_bytes);
 memcpy(&g_m,record+32,sizeof(g_m));
 const unsigned char *writes=record+32+sizeof(g_m);
 for(unsigned i=0;i<get(record+20);++i)MEM32(get(writes+8*i))=get(writes+8*i+4);
 mutation=get(record+4);diag_active=get(record+12);retarget=get(record+16);
 reset();s2_resource_skip_enable((int)get(record+8));
}
static void differential(void){
 assert(get(fixtures_begin)==0x31525053&&get(fixtures_begin+8)==sizeof(g_m));
 unsigned cases=get(fixtures_begin+4);assert(cases==416);
 size_t memory_bytes=0x90000+2*page_bytes;
 unsigned char *expected=malloc(memory_bytes);event *expected_events=malloc(sizeof(events));assert(expected&&expected_events);
 unsigned callbacks=0,yields=0;const unsigned char *record=fixtures_begin+16;
 for(unsigned i=0;i<cases;++i){
  assert(record+32+sizeof(g_m)<=fixtures_end&&get(record)>=32+sizeof(g_m)&&record+get(record)<=fixtures_end);
  assert(get(record)==32+sizeof(g_m)+get(record+20)*8);
  setup(record,i);sub_A09AC040();recomp_machine_t machine=g_m;uint32_t f=flags();
  unsigned n=event_count,y=yield_count,idx=g_icall_trace_idx;uint32_t icalls[128],from[128];
  memcpy(icalls,g_icall_trace,sizeof(icalls));memcpy(from,g_icall_from,sizeof(from));memcpy(expected_events,events,sizeof(events));
  memcpy(expected,GPTR(0x10000),0x90000);memcpy(expected+0x90000,GPTR(0),page_bytes);memcpy(expected+0x90000+page_bytes,GPTR(0u-page_bytes),page_bytes);
  setup(record,i);sims2_resource_enumerate();
  if(memcmp(&machine,&g_m,sizeof(g_m))||f!=flags()){fprintf(stderr,"FAIL case=%u machine/flags\n",i);abort();}
  if(memcmp(expected,GPTR(0x10000),0x90000)||memcmp(expected+0x90000,GPTR(0),page_bytes)||
     memcmp(expected+0x90000+page_bytes,GPTR(0u-page_bytes),page_bytes)){fprintf(stderr,"FAIL case=%u guest memory\n",i);abort();}
  if(n!=event_count||y!=yield_count||idx!=g_icall_trace_idx||memcmp(expected_events,events,sizeof(events))||
     memcmp(icalls,g_icall_trace,sizeof(icalls))||memcmp(from,g_icall_from,sizeof(from))){fprintf(stderr,"FAIL case=%u boundaries\n",i);abort();}
  callbacks+=n;yields+=y;record+=get(record);
 }
 assert(record==fixtures_end);free(expected);free(expected_events);
 uint64_t rows,batches;s2_resource_skip_stats(&rows,&batches);assert(rows>0&&batches>0);
 printf("PASS cases=%u callbacks=%u yields=%u skipped_rows=%llu batches=%llu full_machine_xmm_flags_memory_boundaries\n",
  cases,callbacks,yields,(unsigned long long)rows,(unsigned long long)batches);
}
typedef struct {const char *name;unsigned rejected,buckets,per_bucket,calls,pairs,unknown;} bench_case;
static const bench_case benchmarks[]={
 {"mixed90",90,10,256,800,8,0},{"mixed99",99,100,64,320,8,0},
 {"all-matching",0,10,64,3200,2,0},{"unknown",99,100,64,320,2,1}
};
static void benchmark_setup(unsigned kind){
 const bench_case *c=&benchmarks[kind];
 memset(GPTR(0x10000),0,0x90000);memset(&g_m,0,sizeof(g_m));diag_active=retarget=mutation=0;reset();s2_resource_skip_enable(1);
 unsigned buckets=c->buckets,per_bucket=c->per_bucket;
 MEM32(0x11004)=0x14000;MEM32(0x11008)=0x14000+buckets*per_bucket*16;MEM32(0x1101C)=0x12000;MEM32(0x11028)=buckets;
 MEM32(0x30000)=0x31000;MEM32(0x30004)=1;MEM32(0x30008)=7;MEM32(0x3100C)=c->unknown?0xA01F3D31u:0xA01F3D30u;
 for(unsigned i=0;i<buckets;++i){
  MEM32(0x12000+i*16)=0x12340000+i;MEM32(0x12004+i*16)=c->rejected&&i?9:7;MEM32(0x12008+i*16)=i*per_bucket;
  for(unsigned j=0;j<per_bucket;++j){uint32_t row=0x14000+(i*per_bucket+j)*16;MEM32(row)=j;MEM32(row+4)=i;MEM32(row+8)=0x12345678;MEM32(row+12)=0;}
 }
 MEM32(0x12008+buckets*16)=buckets*per_bucket;
 MEM32(0x80800)=0x60000000;MEM32(0x80804)=0x32000;MEM32(0x80808)=0x30000;
}
static uint64_t now(void){struct timespec t;assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
typedef void (*kernel)(void);
typedef struct {uint64_t ns,rows,batches;uint32_t checksum;} measure_result;
static measure_result measure(kernel f,unsigned kind,unsigned calls){
 benchmark_setup(kind);uint64_t r0,b0,r1,b1;s2_resource_skip_stats(&r0,&b0);uint32_t sum=0;uint64_t start=now();
 for(unsigned i=0;i<calls;++i){
  benchmark_reset();g_ecx=0x11000;g_esp=0x80800;f();sum+=g_eax+g_backedges;
  uint32_t first=MEM32(0x32000),end=MEM32(0x32004);
  for(uint32_t at=first;at!=end;at+=4)sum+=MEM32(at);
 }
 uint64_t elapsed=now()-start;s2_resource_skip_stats(&r1,&b1);
 return (measure_result){elapsed,r1-r0,b1-b0,sum};
}
int main(void){
 setvbuf(stdout,NULL,_IOLBF,0);long p=sysconf(_SC_PAGESIZE);assert(p==4096||p==16384);page_bytes=(unsigned)p;
 const uint32_t at[3]={0x10000,0,0u-page_bytes},length[3]={0x90000,page_bytes,page_bytes};
 for(unsigned i=0;i<3;++i){void *wanted=(void *)(VOID_GUEST_BASE+at[i]);assert(mmap(wanted,length[i],PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==wanted);}
 differential();
 for(unsigned kind=0;kind<4;++kind){
  const bench_case *c=&benchmarks[kind];unsigned calls=c->calls,buckets=c->buckets,per_bucket=c->per_bucket;
  measure_result a=measure(sub_A09AC040,kind,8),b=measure(sims2_resource_enumerate,kind,8);assert(a.checksum==b.checksum);
  for(unsigned pair=0;pair<c->pairs;++pair){
   if(!(pair&1)){a=measure(sub_A09AC040,kind,calls);b=measure(sims2_resource_enumerate,kind,calls);}
   else{b=measure(sims2_resource_enumerate,kind,calls);a=measure(sub_A09AC040,kind,calls);}
   assert(a.checksum==b.checksum&&!a.rows&&!a.batches);
   uint64_t expected_batches=c->rejected&&!c->unknown&&per_bucket>=4?(uint64_t)calls*(buckets-1):0;
   assert(b.batches==expected_batches&&b.rows==expected_batches*per_bucket);
   printf("pair=%u scenario=%s rejected_pct=%u predicate=%s order=%s calls=%u rows_per_call=%u matching_rows_per_call=%u original_ns=%llu native_ns=%llu checksum=%u/%u skipped_rows=%llu batches=%llu\n",
    pair,c->name,c->rejected,c->unknown?"unknown":"known",pair&1?"BA":"AB",calls,buckets*per_bucket,c->rejected?per_bucket:buckets*per_bucket,(unsigned long long)a.ns,(unsigned long long)b.ns,a.checksum,b.checksum,(unsigned long long)b.rows,(unsigned long long)b.batches);
  }
 }
 puts("DONE isolated enumeration kernel with shared modeled vector helpers; no game FPS result");return 0;
}

#endif
