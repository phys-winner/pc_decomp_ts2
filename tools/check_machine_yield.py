"""Build an actual-pthread ARM64 regression for the guest machine hand-over.

Covers the FIFO return of a yield, the main thread's slice, and the audio
producer's urgency: a waiter that pokes the holder's back-edge counter and is
served within the main thread's slice, an urgent holder that keeps its own
slice, and the owner of a mutex the urgent thread waits for (boosted_tid).

Run the resulting binary on the selected device; building alone is not a pass.
"""
import subprocess
from build_android import ROOT, CLANG
from check_virtual_query import function

PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef struct { unsigned regs; int depth; uint32_t id; } void_thread;
static __thread void_thread local = {0,1,0}, *t_self;
static __thread uint64_t t_lock_wait_ns, t_held_since;
static __thread int t_main_thread;
static pthread_mutex_t mlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mcond = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t ticket_next, ticket_serving;
static _Atomic int machine_sleepers;
static unsigned guest_register;
static struct { uint32_t m_backedges; } g_m;
#define g_backedges (g_m.m_backedges)
#define RECOMP_YIELD_EVERY 0x10000u
#define VS_MACHINE 0
static _Atomic uint64_t machine_waited;
static void void_stat_add(int kind, uint64_t ns, int count) { (void)kind; (void)count; atomic_fetch_add(&machine_waited, ns); }
static void void_regs_save(unsigned *out) { *out=guest_register; }
static void void_regs_load(const unsigned *in) { guest_register=*in; }
#define void_fatal(s) abort()
#define MAIN_SLICE_NS 2000000ull
#define URGENT_POKE_NS 250000L
static _Atomic int machine_urgent;
static __thread int t_urgent;
static _Atomic uint32_t boosted_tid;
#define ICALL_TRACE_SIZE 128
static uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE], g_icall_trace_idx, g_cur_func;
static const char *void_thunk_name(uint32_t va) { (void)va; return NULL; }
static unsigned reports;
static void void_log(const char *format, ...) { (void)format; ++reports; }
#define URGENT_REPORT_NS 20000000ull
#define URGENT_REPORTS 40
static _Atomic int holder_host_tid;
static void hold_sample_request(void) {}
static void hold_sample_text(char *out, size_t size) { (void)size; out[0] = 0; }
static void holder_stack_publish(void) {}
static _Atomic uint32_t holder_tid;
static unsigned urgent_reports;
static _Atomic uint64_t producer_ticket;
static _Atomic uint32_t machine_loans;
enum { LOAN_NONE, LOAN_OFFERED, LOAN_RETURNED, LOAN_TAKEN };
#define LOAN_OFFER_NS 2000000L
enum { BORROW_NONE, BORROW_PRODUCER, BORROW_OWNER };
#define PARKED_SLOTS 8
static _Atomic int loan_state;
static _Atomic uint64_t parked_tickets[PARKED_SLOTS];
static _Atomic uint32_t boosted_waiting;
static __thread int t_parked_slot = -1, t_borrowing;
static __thread uint64_t t_parked;
#define WAIT_SLOTS 64u
static struct { uint64_t ticket; pthread_cond_t *cond; } wait_slots[WAIT_SLOTS];
static __thread pthread_cond_t t_machine_cond;
static __thread int t_machine_cond_ready;
#define HANDOFF_REPORT_NS 10000000000ull
static _Atomic uint64_t released_ns, released_ticket;
static uint32_t handoff_count, handoff_slow[3];
static uint64_t handoff_max_ns, handoff_report_at;
'''
TEST = r'''
static int is_parked(uint64_t ticket) {
    for (int i = 0; i < PARKED_SLOTS; ++i) if (atomic_load(&parked_tickets[i]) == ticket) return 1;
    return 0;
}
static int any_parked(void) { for (int i = 0; i < PARKED_SLOTS; ++i) if (atomic_load(&parked_tickets[i])) return 1; return 0; }
static void *waiting(void *unused) {
    (void)unused; t_self=&local;
    machine_acquire();
    /* The yielding owner already reserved its return, before we can run. */
    assert(machine_contended()); guest_register=222;
    yield_hook();
    assert(guest_register==222); machine_release(); return NULL;
}
static void *stress(void *unused) {
    (void)unused; t_self=&local;
    for (unsigned n=0; n<2000; ++n) {
        machine_acquire(); guest_register=n;
        yield_hook(); assert(guest_register==n); machine_release();
    }
    return NULL;
}
/* The audio producer: urgent, it pokes until the main thread hands over. */
static _Atomic int producer_served, producer_waiting;
static void *producer(void *unused) {
    (void)unused; t_self=&local; local.id=0x114; t_urgent=1;
    atomic_store(&producer_waiting,1);
    machine_acquire();
    assert(!atomic_load(&machine_urgent));
    atomic_store(&producer_served,1);
    guest_register=333;
    /* Within its own slice the producer keeps the machine from a plain waiter. */
    while (!machine_contended()) sched_yield();
    uint64_t before=atomic_load(&ticket_serving);
    yield_hook(); assert(atomic_load(&ticket_serving)==before && guest_register==333);
    machine_release(); return NULL;
}
static void *plain(void *unused) { (void)unused; t_self=&local; machine_acquire(); machine_release(); return NULL; }
static _Atomic int plain_served;
static void *plain_counted(void *unused) {
    (void)unused; t_self=&local; machine_acquire(); atomic_fetch_add(&plain_served,1); machine_release(); return NULL;
}
/* A sound thread owning what the producer waits for is boosted. */
static void *owner(void *unused) {
    (void)unused; t_self=&local; local.id=0x110;
    atomic_store(&boosted_tid,0x110);
    assert(thread_urgent());
    machine_acquire();                 /* urgent: served within the main thread's slice */
    t_held_since=now_ns();
    while (!machine_contended()) sched_yield();
    uint64_t before=atomic_load(&ticket_serving);
    yield_hook(); assert(atomic_load(&ticket_serving)==before);   /* keeps its slice */
    atomic_store(&boosted_tid,0);
    assert(!thread_urgent());
    machine_release(); return NULL;
}
/* The producer waiting for a loan: it renders, then hands the machine back. */
static void *producer_loan(void *unused) {
    (void)unused; t_self=&local; local.id=0x118; t_urgent=1;
    machine_acquire();
    atomic_store(&producer_served,1);
    guest_register=777;
    machine_release(); return NULL;
}
/* The owner of the producer's mutex: asleep for the machine, boosted, it borrows
 * the lender's machine until it releases the mutex, then waits for its turn. */
static _Atomic int owner_ran, owner_done;
static void *owner_borrower(void *unused) {
    (void)unused; t_self=&local; local.id=0x130;
    machine_acquire();
    atomic_store(&owner_ran, 1);
    assert(t_borrowing == BORROW_OWNER);
    guest_register=4242;
    void_machine_owner_done();
    assert(guest_register==4242 && !t_borrowing);
    atomic_store(&owner_done, 1);
    machine_release(); return NULL;
}
/* ...and wants the machine again at once: its parked ticket, then its turn. */
static void *producer_twice(void *unused) {
    (void)unused; t_self=&local; local.id=0x11C; t_urgent=1;
    machine_acquire(); atomic_store(&producer_served,1); guest_register=888; machine_release();
    machine_acquire(); atomic_store(&producer_served,2); machine_release(); return NULL;
}
int main(void) {
    alarm(15); t_self=&local; local.id=0x104;
    machine_acquire(); guest_register=111;
    uint64_t before=atomic_load(&ticket_next);
    yield_hook(); assert(guest_register==111 && atomic_load(&ticket_next)==before);
    pthread_t other; assert(!pthread_create(&other,NULL,waiting,NULL));
    while(atomic_load(&ticket_next)<2) sched_yield();
    t_main_thread=1; t_held_since=now_ns();
    yield_hook(); assert(atomic_load(&ticket_serving)==0);
    t_held_since=now_ns()-MAIN_SLICE_NS; yield_hook();
    assert(guest_register==111); machine_release(); assert(!pthread_join(other,NULL));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving));
    pthread_t workers[4];
    for(unsigned i=0;i<4;++i) assert(!pthread_create(&workers[i],NULL,stress,NULL));
    for(unsigned i=0;i<4;++i) assert(!pthread_join(workers[i],NULL));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !atomic_load(&machine_sleepers));
    /* Urgent: the main thread inside its slice hands over at its next back-edge
     * (the producer set the counter to the threshold), then gets it back. */
    machine_acquire(); guest_register=444; t_held_since=now_ns();
    g_backedges=0;
    pthread_t audio; assert(!pthread_create(&audio,NULL,producer,NULL));
    while(!atomic_load(&machine_urgent)) sched_yield();
    while(g_backedges!=RECOMP_YIELD_EVERY) sched_yield();
    assert(!atomic_load(&producer_served));
    yield_hook();                       /* still within the main slice: the producer goes first */
    assert(atomic_load(&producer_served) && guest_register==444);
    pthread_t waiter; assert(!pthread_create(&waiter,NULL,plain,NULL));
    machine_release(); assert(!pthread_join(audio,NULL)); assert(!pthread_join(waiter,NULL));
    /* Boosted: urgent while the producer waits on its mutex. */
    machine_acquire(); t_held_since=now_ns();
    atomic_fetch_add(&machine_urgent,1);   /* the producer, blocked in wait_core */
    pthread_t sound; assert(!pthread_create(&sound,NULL,owner,NULL));
    while(atomic_load(&ticket_next)-atomic_load(&ticket_serving)<2) sched_yield();
    yield_hook();                       /* hands over within the slice */
    atomic_fetch_sub(&machine_urgent,1);
    pthread_t waiter2; assert(!pthread_create(&waiter2,NULL,plain,NULL));
    machine_release(); assert(!pthread_join(sound,NULL)); assert(!pthread_join(waiter2,NULL));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !atomic_load(&machine_sleepers));
    assert(!atomic_load(&machine_urgent) && !atomic_load(&boosted_tid));
    /* A loan from a host call goes to the producer; the queue waits for the lender. */
    machine_acquire(); guest_register=555;
    assert(!void_machine_lend_urgent());                          /* nobody waits */
    uint64_t s=atomic_load(&ticket_serving);
    atomic_store(&producer_served,0);
    pthread_t lender; assert(!pthread_create(&lender,NULL,producer_loan,NULL));
    while(atomic_load(&producer_ticket)!=s+2u) sched_yield();
    assert(void_machine_lend_urgent() && atomic_load(&producer_served) && guest_register==555);
    assert(!pthread_join(lender,NULL) && !atomic_load(&producer_ticket) && !atomic_load(&loan_state));
    assert(atomic_load(&ticket_serving)==s && is_parked(s+1u));   /* its ticket is parked */
    machine_release();                                            /* and skipped */
    assert(atomic_load(&ticket_serving)==s+2u && atomic_load(&ticket_next)==s+2u && !any_parked());
    /* One thread ahead of the producer, one behind: the loan still goes to the
     * producer, and both wait for the lender, in their order. */
    machine_acquire(); guest_register=555; s=atomic_load(&ticket_serving);
    atomic_store(&producer_served,0); atomic_store(&plain_served,0);
    pthread_t first; assert(!pthread_create(&first,NULL,plain_counted,NULL));
    while(atomic_load(&ticket_next)!=s+2u) sched_yield();
    pthread_t lender2; assert(!pthread_create(&lender2,NULL,producer_loan,NULL));
    while(atomic_load(&producer_ticket)!=s+3u) sched_yield();
    pthread_t behind; assert(!pthread_create(&behind,NULL,plain_counted,NULL));
    while(atomic_load(&ticket_next)!=s+4u) sched_yield();
    assert(void_machine_lend_urgent() && atomic_load(&producer_served) && guest_register==555 && !atomic_load(&plain_served));
    assert(!pthread_join(lender2,NULL) && is_parked(s+2u) && atomic_load(&ticket_serving)==s);
    machine_release();
    assert(!pthread_join(first,NULL) && !pthread_join(behind,NULL) && atomic_load(&plain_served)==2);
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !any_parked());
    /* The producer wants the machine again before its parked ticket comes up: it takes it back. */
    machine_acquire(); guest_register=666; s=atomic_load(&ticket_serving);
    atomic_store(&producer_served,0);
    pthread_t twice; assert(!pthread_create(&twice,NULL,producer_twice,NULL));
    while(atomic_load(&producer_ticket)!=s+2u) sched_yield();
    assert(void_machine_lend_urgent() && guest_register==666);
    while(atomic_load(&producer_served)!=1 || atomic_load(&producer_ticket)!=s+2u) sched_yield();
    assert(!any_parked() && atomic_load(&ticket_next)==s+2u);
    machine_release(); assert(!pthread_join(twice,NULL) && atomic_load(&producer_served)==2);
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !any_parked());
    /* The owner of the producer's mutex borrows, returns at its release, then gets its own turn. */
    machine_acquire(); guest_register=777; s=atomic_load(&ticket_serving);
    pthread_t ob; assert(!pthread_create(&ob,NULL,owner_borrower,NULL));
    while(atomic_load(&ticket_next)!=s+2u) sched_yield();
    while(!wait_slots[(s+1u)%WAIT_SLOTS].cond) sched_yield();       /* asleep on its own condition */
    machine_boost(0x130);
    while(atomic_load(&boosted_waiting)!=0x130) sched_yield();
    assert(void_machine_lend_urgent() && atomic_load(&owner_ran) && guest_register==777);
    assert(!atomic_load(&boosted_tid) && !atomic_load(&owner_done));
    while(!wait_slots[(s+1u)%WAIT_SLOTS].cond) sched_yield();       /* its parked ticket, taken back */
    assert(!any_parked() && atomic_load(&ticket_next)==s+2u);
    machine_release(); assert(!pthread_join(ob,NULL) && atomic_load(&owner_done));
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !any_parked() && !atomic_load(&machine_sleepers));
    /* An offer nobody takes is withdrawn: the machine never left the lender. */
    machine_acquire(); guest_register=999; s=atomic_load(&ticket_serving);
    uint32_t loans=atomic_load(&machine_loans);
    atomic_store(&producer_ticket,s+77u);
    assert(!void_machine_lend_urgent() && atomic_load(&loan_state)==LOAN_NONE && atomic_load(&machine_loans)==loans);
    atomic_store(&producer_ticket,0);
    assert(atomic_load(&ticket_serving)==s && guest_register==999);
    machine_release();
    assert(atomic_load(&ticket_next)==atomic_load(&ticket_serving) && !atomic_load(&machine_sleepers));
    puts("PASS machine yield: reserved FIFO return, register preservation, main slice, 8000 contended turns, "
         "urgent producer poked through the main slice and keeping its own, boosted mutex owner, loans to the producer anywhere in "
         "the queue with its ticket parked, skipped or taken back, the producer's mutex owner borrowing until it "
         "releases, an untaken offer withdrawn, sleepers on their own conditions");
    return 0;
}
'''

def main():
    source=(ROOT/'android/host/threads.c').read_text(encoding='utf-8')
    signatures=('static uint64_t now_ns(', 'static int thread_urgent(', 'static void machine_poke(',
                'static int park_ticket(', 'static void unpark_ticket(', 'static int skip_parked(',
                'static void machine_boost(', 'static void handoff_note(', 'static void machine_wait(',
                'static uint64_t take_ticket(', 'static void machine_acquire(', 'static void machine_release(', 'static int machine_contended(',
                'int void_machine_lend_urgent(', 'static void yield_hook(',
                'void void_blocking_begin(', 'void void_blocking_end(', 'static void void_machine_owner_done(')
    out=ROOT/'build/test/machine-yield'
    out.mkdir(parents=True,exist_ok=True)
    fixture=out/'probe.c'
    fixture.write_text(PRELUDE+'\n'.join(function(source,s) for s in signatures)+TEST,encoding='utf-8')
    subprocess.run([str(CLANG),'--target=aarch64-linux-android30','-std=gnu11','-O2','-Wall','-Wextra','-Werror',
                    '-Wl,-z,max-page-size=16384',str(fixture),'-o',str(out/'check-machine-yield')],check=True)
    print(out/'check-machine-yield')

if __name__=='__main__': main()
