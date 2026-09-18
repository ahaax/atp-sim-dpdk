#include "../atp_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)
static unsigned tests;
static atp_key key(unsigned seq) { return (atp_key){1,1,0,seq}; }
static atp_packet packet(unsigned seq,uint32_t mask,int64_t value) {
    atp_packet p={0}; p.key=key(seq); p.type=ATP_DATA; p.contributors=mask;p.length=ATP_VALUES;p.scale=1;
    for(unsigned v=0;v<ATP_VALUES;++v) p.value[v]=value;
    return p;
}
static void add_fragment(atp_sim *sim,atp_key k,int mode) {
    atp_input a={0};
    for(unsigned w=0;w<sim->workers;++w) for(unsigned v=0;v<ATP_VALUES;++v)
        a.value[w][v]=mode==1 ? INT32_MAX : mode==2 ? INT32_MIN : (int32_t)((w+1)*(v+1))*(v%2 ? -1:1);
    CHECK(atp_sim_add(sim,k,&a));
}
static void verify(atp_sim *sim) {
    CHECK(sim->queue_count==0);
    for(unsigned i=0;i<sim->count;++i) {
        atp_fragment *f=&sim->fragments[i];
        CHECK(f->done && !f->failed && f->deliveries==1);
        CHECK(f->seen==f->members && f->acked==f->members);
        for(unsigned v=0;v<f->length;++v) {
            int64_t ref=0;
            for(unsigned w=0;w<sim->workers;++w) if(f->members&(UINT32_C(1)<<w)) ref+=(int64_t)f->input[w][v];
            CHECK(f->sum[v]==ref);
            for(unsigned w=0;w<sim->workers;++w) if(f->members&(UINT32_C(1)<<w)) CHECK(f->received[w][v]==ref);
        }
    }
    for(unsigned i=0;i<ATP_FLOWS;++i) for(unsigned w=0;w<sim->workers;++w) CHECK(sim->flows[i].worker[w].inflight==0);
    for(unsigned r=0;r<sim->racks;++r)
        for(uint32_t i=0;i<sim->l1[r].capacity;++i) CHECK(!sim->l1[r].slots[i].occupied);
    for(uint32_t i=0;i<sim->l2.capacity;++i) CHECK(!sim->l2.slots[i].occupied);
}
static void core(void) {
    atp_switch s; atp_packet p=packet(0,1,10),out;
    CHECK(!atp_switch_init(&s,0,1,1));
    CHECK(atp_switch_init(&s,1,1,2));
    CHECK(atp_switch_process(&s,&p,1,false,&out)==ATP_FORWARD);
    CHECK(out.value[0]==10 && s.slots[0].occupied && s.slots[0].result_sent);
    CHECK(atp_switch_process(&s,&p,2,true,&out)==ATP_CONSUMED);
    p.resend=true;
    CHECK(atp_switch_process(&s,&p,3,false,&out)==ATP_FORWARD);
    CHECK(out.resend && !s.slots[0].occupied);
    p.resend=false;
    CHECK(atp_switch_process(&s,&p,4,false,&out)==ATP_FORWARD);
    CHECK(out.bypass && !s.slots[0].occupied);
    atp_switch_destroy(&s);
    CHECK(atp_switch_init(&s,1,3,1));
    p=packet(0,1,10); CHECK(atp_switch_process(&s,&p,1,false,&out)==ATP_CONSUMED);
    p=packet(0,2,20); p.resend=true;
    CHECK(atp_switch_process(&s,&p,2,true,&out)==ATP_FORWARD);
    CHECK(out.resend && out.ecn && out.contributors==3 && out.value[0]==30);
    CHECK(!s.slots[0].occupied); atp_switch_destroy(&s);
    CHECK(atp_switch_init(&s,1,3,1));
    p=packet(0,1,10); CHECK(atp_switch_process(&s,&p,1,false,&out)==ATP_CONSUMED);
    atp_packet ack=packet(1,3,30); ack.type=ATP_ACK;
    CHECK(atp_switch_process(&s,&ack,2,false,&out)==ATP_FORWARD);
    CHECK(s.slots[0].occupied); /* ACK for colliding key must not release another. */
    atp_switch_expire(&s,0,1); CHECK(s.slots[0].occupied);
    atp_switch_expire(&s,4,2); CHECK(!s.slots[0].occupied && s.expired==1);
    p.contributors=4; CHECK(atp_switch_process(&s,&p,5,false,&out)==ATP_INVALID);
    p=packet(0,1,INT64_MAX); CHECK(atp_switch_process(&s,&p,5,false,&out)==ATP_INVALID);
    atp_switch_destroy(&s);
    CHECK(atp_switch_init(&s,100000,1,1));
    bool high=false;
    for(unsigned i=0;i<10000;++i) if(atp_hash(key(i),100000)>65535) { high=true; break; }
    CHECK(high); atp_switch_destroy(&s); ++tests;
}
static void tombstones(void) {
    atp_switch s; CHECK(atp_switch_init(&s,1,1,1)); atp_packet out;
    for(unsigned i=0;i<ATP_FRAGMENTS+2;++i) {
        atp_packet p=packet(i,1,1); p.type=ATP_ACK;
        CHECK(atp_switch_process(&s,&p,i,false,&out)==ATP_FORWARD);
    }
    CHECK(s.closed_count==ATP_FRAGMENTS && s.admission_disabled);
    atp_packet p=packet(999,1,1);
    CHECK(atp_switch_process(&s,&p,100,false,&out)==ATP_FORWARD);
    CHECK(out.bypass && !s.slots[0].occupied); atp_switch_destroy(&s); ++tests;
}
static void topologies(void) {
    const unsigned n[]={1,2,3,4,7,32}, racks[]={1,1,2,2,4,4};
    for(unsigned j=0;j<6;++j) {
        atp_sim *s=atp_sim_create(n[j],racks[j],4); CHECK(s);
        add_fragment(s,key(0),0); CHECK(atp_sim_run(s,5000)); verify(s);
        /* Persistent PS and drained queues: repeat poll cannot redeliver. */
        CHECK(atp_sim_run(s,20)); verify(s);
        atp_packet late=packet(0,1,1); late.value[1]=-2;late.value[2]=3;late.value[3]=-4;
        CHECK(atp_sim_inject(s,ATP_TO_L1,0,&late));
        CHECK(atp_sim_run(s,1000)); verify(s);
        atp_sim_destroy(s); ++tests;
    }
}
static void loss_paths(void) {
    for(unsigned d=0;d<ATP_DEST_COUNT;++d) {
        atp_sim *s=atp_sim_create(4,2,2); CHECK(s); add_fragment(s,key(0),0);
        s->drop_next[d]=1; CHECK(atp_sim_run(s,8000)); CHECK(s->retries>0 && s->drops>0);
        verify(s); atp_sim_destroy(s); ++tests;
    }
}
static void partial_overlap(void) {
    atp_sim *s=atp_sim_create(3,1,1); CHECK(s); add_fragment(s,key(0),0);
    s->max_delay=0;
    atp_packet p=packet(0,1,1);for(unsigned v=0;v<ATP_VALUES;++v) p.value[v]=(int64_t)(v+1)*(v%2 ? -1:1);
    CHECK(atp_sim_inject(s,ATP_TO_PS,0,&p));
    p.contributors=3;for(unsigned v=0;v<ATP_VALUES;++v) p.value[v]=3*(int64_t)(v+1)*(v%2 ? -1:1);
    CHECK(atp_sim_inject(s,ATP_TO_PS,0,&p));
    CHECK(atp_sim_run(s,5000)); CHECK(s->overlaps>0 && s->retries>0);
    verify(s); atp_sim_destroy(s); ++tests;
}
static void bounds_and_failure(void) {
    CHECK(!atp_sim_create(33,1,1)); CHECK(!atp_sim_create(1,2,1)); CHECK(!atp_sim_create(1,1,0));
    for(int mode=1;mode<=2;++mode) {
        atp_sim *s=atp_sim_create(32,4,4); CHECK(s); add_fragment(s,key(0),mode);
        CHECK(atp_sim_run(s,5000)); verify(s); atp_sim_destroy(s); ++tests;
    }
    atp_sim *s=atp_sim_create(2,1,1); CHECK(s); add_fragment(s,key(0),0);
    s->offline=2;s->max_attempts=3;s->slot_timeout=30;
    CHECK(!atp_sim_run(s,2000)); CHECK(s->fragments[0].failed && !s->fragments[0].done);
    CHECK(s->fragments[0].deliveries==0); atp_sim_destroy(s); ++tests;
    s=atp_sim_create(2,1,1); CHECK(s); add_fragment(s,key(0),0);
    s->queue_limit=0; s->max_attempts=3;
    CHECK(!atp_sim_run(s,2000)); CHECK(s->drops>0 && s->fragments[0].failed);
    atp_sim_destroy(s); ++tests;
}
static void pressure(void) {
    atp_sim *s=atp_sim_create(4,2,1); CHECK(s); s->queue_limit=8;s->ecn_threshold=0;
    s->service_budget=4;s->slot_timeout=20;
    for(unsigned i=0;i<8;++i) add_fragment(s,(atp_key){i%2+1,1,0,i},0);
    CHECK(atp_sim_run(s,20000)); verify(s); CHECK(s->drops>0 && s->retries>0);
    CHECK(s->l1[0].fallbacks+s->l1[1].fallbacks>0);
    CHECK(s->flows[0].worker[0].window<4); atp_sim_destroy(s); ++tests;
}
static void keys_and_capacity(void) {
    atp_sim *s=atp_sim_create(2,1,4); CHECK(s);
    add_fragment(s,(atp_key){1,1,0,65535},0); add_fragment(s,(atp_key){1,1,0,65536},0);
    add_fragment(s,(atp_key){1,2,0,65535},0); add_fragment(s,(atp_key){1,1,1,65535},0);
    CHECK(atp_sim_run(s,5000)); verify(s);
    atp_input a={0};
    CHECK(!atp_sim_add(s,(atp_key){1,1,0,65535},&a)); CHECK(!atp_sim_add(s,(atp_key){0,0,0,0},&a));
    for(unsigned i=s->count;i<ATP_FRAGMENTS;++i) add_fragment(s,key(65537+i),0);
    CHECK(!atp_sim_add(s,key(999999),&a)); CHECK(atp_sim_run(s,20000)); verify(s);
    atp_sim_destroy(s); ++tests;
}
static void randomized(void) {
    for(unsigned seed=1;seed<=100;++seed) {
        atp_sim *s=atp_sim_create(4,2,seed%4+1); CHECK(s); s->rng=seed;
        s->loss_per_mille=80;s->duplicate_per_mille=150;s->max_delay=15;
        for(unsigned i=0;i<6;++i) add_fragment(s,(atp_key){i%2+1,seed,0,i},0);
        CHECK(atp_sim_run(s,15000)); verify(s); atp_sim_destroy(s); ++tests;
    }
}
static void additional_recovery(void) {
    /* Lost slot state before the sender times out: original inputs survive. */
    atp_sim *s=atp_sim_create(4,2,2); CHECK(s); add_fragment(s,key(0),0);
    s->slot_timeout=1;s->max_delay=8;
    CHECK(atp_sim_run(s,5000)); verify(s); CHECK(s->retries>0);
    atp_sim_destroy(s); ++tests;
    /* Abort before normal slot timeout must still reclaim occupied state. */
    s=atp_sim_create(2,1,1); CHECK(s); add_fragment(s,key(0),0);
    s->offline=2;s->max_attempts=1;s->slot_timeout=300;
    CHECK(!atp_sim_run(s,1000)); CHECK(s->fragments[0].failed);
    CHECK(!s->l1[0].slots[0].occupied && s->now>=300);
    atp_sim_destroy(s); ++tests;
    /* L2 retransmission forwards only the incoming contribution, not old sum. */
    atp_switch sw; CHECK(atp_switch_init(&sw,1,15,2));
    atp_packet out,p=packet(0,3,30);
    CHECK(atp_switch_process(&sw,&p,1,false,&out)==ATP_CONSUMED);
    p=packet(0,4,40);p.resend=true;
    CHECK(atp_switch_process(&sw,&p,2,false,&out)==ATP_FORWARD);
    CHECK(out.value[0]==40 && out.contributors==4 && out.resend && !sw.slots[0].occupied);
    atp_switch_destroy(&sw); ++tests;
    /* CE on a duplicate must propagate to the eventual complete result. */
    CHECK(atp_switch_init(&sw,1,3,1));p=packet(0,1,10);
    CHECK(atp_switch_process(&sw,&p,1,false,&out)==ATP_CONSUMED);
    CHECK(atp_switch_process(&sw,&p,2,true,&out)==ATP_CONSUMED);
    p=packet(0,2,20);
    CHECK(atp_switch_process(&sw,&p,3,false,&out)==ATP_FORWARD);
    CHECK(out.ecn && out.value[0]==30);
    p=out;p.type=ATP_ACK;
    CHECK(atp_switch_process(&sw,&p,4,false,&out)==ATP_FORWARD);
    CHECK(!sw.slots[0].occupied);atp_switch_destroy(&sw); ++tests;
}
#include "phase1_cases.inc"
int main(void) {
    core();tombstones();topologies();loss_paths();partial_overlap();
    bounds_and_failure();pressure();keys_and_capacity();randomized();additional_recovery();phase1_acceptance();
    printf("PASS: %u regression scenarios (including 120 seeded fault runs).\n",tests);
    return 0;
}
