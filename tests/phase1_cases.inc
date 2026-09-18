/* Extra phase-one acceptance tests. Included by test_atp.c to share CHECK/oracle. */
#include <math.h>
static atp_packet fragment_packet(const atp_fragment *f,uint32_t mask) {
    atp_packet p={0};p.key=f->key;p.type=ATP_DATA;p.contributors=mask;
    p.length=f->length;p.scale=f->scale;p.route_seed=f->route_seed;p.route_version=f->route_version;
    for(unsigned w=0;w<ATP_WORKERS;++w) if(mask&(UINT32_C(1)<<w))
        for(unsigned v=0;v<f->length;++v) p.value[v]+=f->input[w][v];
    return p;
}
static atp_packet fragment_ack(const atp_fragment *f) {
    atp_packet p=fragment_packet(f,f->members);p.type=ATP_ACK;
    p.next_seed=f->next_seed;p.next_version=f->next_version;p.ecn=f->ecn;return p;
}
static void independent_flows(void) {
    atp_sim *s=atp_sim_create(4,2,32);CHECK(s);s->ecn_threshold=UINT32_MAX;
    CHECK(atp_sim_configure_job(s,10,3,1));CHECK(atp_sim_configure_job(s,20,12,10));
    CHECK(!atp_sim_configure_job(s,10,15,1));
    atp_input in={0};for(unsigned w=0;w<4;++w) for(unsigned v=0;v<ATP_VALUES;++v) in.value[w][v]=(int32_t)(w+v+1);
    CHECK(atp_sim_add_vector(s,(atp_key){10,1,0,0},&in,3));
    CHECK(atp_sim_add_vector(s,(atp_key){20,1,0,0},&in,7));
    atp_packet p=fragment_packet(&s->fragments[0],3);p.ecn=true;
    CHECK(atp_sim_inject(s,ATP_TO_PS,0,&p));
    CHECK(atp_sim_run(s,5000));verify(s);
    CHECK(s->flows[0].worker[0].window==2 && s->flows[0].worker[1].window==2);
    CHECK(s->flows[1].worker[2].window==5 && s->flows[1].worker[3].window==5);
    CHECK(s->fragments[0].sent==3 && s->fragments[1].sent==12);
    CHECK(s->flows[1].worker[2].reductions==0);
    atp_sim_destroy(s);++tests;
    /* Same job, different tensor gets independent congestion state. */
    s=atp_sim_create(2,1,32);CHECK(s);s->ecn_threshold=UINT32_MAX;
    add_fragment(s,(atp_key){1,1,0,0},0);add_fragment(s,(atp_key){1,1,1,0},0);
    p=fragment_packet(&s->fragments[0],3);p.ecn=true;CHECK(atp_sim_inject(s,ATP_TO_PS,0,&p));
    CHECK(atp_sim_run(s,5000));verify(s);
    CHECK(s->flows[0].worker[0].window==2 && s->flows[1].worker[0].window==5);
    atp_sim_destroy(s);++tests;
}
static void fast_retransmission(void) {
    atp_sim *s=atp_sim_create(2,1,64);CHECK(s);s->max_delay=0;s->rto=1000;s->ecn_threshold=UINT32_MAX;
    for(unsigned i=0;i<4;++i) add_fragment(s,key(i),0);
    s->drop_next[ATP_TO_L1]=1;
    CHECK(atp_sim_run(s,100));verify(s);CHECK(s->fast_retries>0 && s->now<s->rto);
    CHECK(s->flows[0].worker[0].reductions==1);
    uint64_t fast=s->fast_retries,reductions=s->flows[0].worker[0].reductions;
    unsigned window=s->flows[0].worker[0].window;
    atp_packet ack=fragment_ack(&s->fragments[3]);
    for(unsigned i=0;i<20;++i) CHECK(atp_sim_inject(s,ATP_ACK_WORKER,0,&ack));
    CHECK(atp_sim_run(s,100));verify(s);
    CHECK(s->fast_retries==fast && s->flows[0].worker[0].reductions==reductions &&
          s->flows[0].worker[0].window==window);
    atp_sim_destroy(s);++tests;
    /* ACKs from another job cannot count as out-of-order ACKs of the gap. */
    s=atp_sim_create(2,1,64);CHECK(s);s->max_delay=0;s->rto=1000;s->ecn_threshold=UINT32_MAX;
    add_fragment(s,key(0),0);
    for(unsigned i=0;i<4;++i) add_fragment(s,(atp_key){2,1,0,i},0);
    s->drop_next[ATP_TO_L1]=1;
    CHECK(!atp_sim_run(s,100));CHECK(s->fast_retries==0);
    CHECK(atp_sim_run(s,5000));verify(s);atp_sim_destroy(s);++tests;
}
static void versioned_rehash(void) {
    atp_sim *s=atp_sim_create(4,2,2);CHECK(s);s->max_delay=0;
    for(unsigned i=0;i<6;++i) add_fragment(s,key(i),0);
    CHECK(atp_sim_run(s,10000));verify(s);CHECK(s->rehashes>0);
    atp_flow *flow=&s->flows[0];CHECK(flow->version>0);
    for(unsigned w=0;w<4;++w) CHECK(flow->worker[w].feedback_version==flow->version);
    uint32_t version=flow->version,seed=flow->seed;
    for(unsigned i=0;i<6;++i) CHECK(s->fragments[i].route_version==0 && s->fragments[i].route_seed==0);
    add_fragment(s,key(100),0);CHECK(s->fragments[6].route_version==version && s->fragments[6].route_seed==seed);
    atp_packet wrong=fragment_packet(&s->fragments[6],1);wrong.route_seed^=1;
    CHECK(atp_sim_inject(s,ATP_TO_L1,0,&wrong));
    CHECK(atp_sim_run(s,5000));verify(s);CHECK(s->invalid>0);
    atp_packet old_ack=fragment_ack(&s->fragments[0]);
    CHECK(atp_sim_inject(s,ATP_ACK_WORKER,0,&old_ack));CHECK(atp_sim_run(s,100));
    CHECK(flow->version>=version && flow->worker[0].feedback_version>=version);
    atp_sim_destroy(s);++tests;
    /* Lost mapping feedback reaches all senders through cached ACK repair. */
    s=atp_sim_create(2,1,1);CHECK(s);s->drop_next[ATP_ACK_WORKER]=2;
    for(unsigned i=0;i<5;++i) add_fragment(s,key(i),0);
    CHECK(atp_sim_run(s,10000));verify(s);CHECK(s->rehashes>0 && s->retries>0);
    CHECK(s->flows[0].worker[0].feedback_version==s->flows[0].worker[1].feedback_version);
    atp_sim_destroy(s);++tests;
}
static void reclamation(void) {
    atp_sim *s=atp_sim_create(2,1,8);CHECK(s);add_fragment(s,key(0),0);
    CHECK(!atp_sim_retire(s,1,1));CHECK(atp_sim_run(s,5000));verify(s);
    atp_packet late=fragment_packet(&s->fragments[0],1),out;
    CHECK(atp_sim_inject(s,ATP_TO_L1,0,&late));CHECK(!atp_sim_retire(s,1,1));
    CHECK(atp_sim_run(s,100));CHECK(atp_sim_retire(s,1,1));
    CHECK(s->count==0 && s->retired_fragments==1 && s->l1[0].closed_count==0);
    CHECK(!atp_sim_inject(s,ATP_TO_L1,0,&late));
    CHECK(atp_switch_process(&s->l1[0],&late,s->now,false,&out)==ATP_INVALID);
    late.type=ATP_ACK;late.contributors=3;late.value[0]=3;
    CHECK(atp_switch_process(&s->l2,&late,s->now,false,&out)==ATP_INVALID);
    atp_input in={0};CHECK(!atp_sim_add(s,key(1),&in));CHECK(!atp_sim_retire(s,1,1));
    /* 1,024 further fragments, with constant-size active tables and no session reset. */
    for(unsigned round=2;round<130;++round) {
        for(unsigned i=0;i<8;++i) add_fragment(s,(atp_key){1,round,0,i},0);
        CHECK(atp_sim_run(s,10000));verify(s);CHECK(atp_sim_retire(s,1,round));
        CHECK(s->count==0 && s->l2.closed_count==0 && !s->l2.admission_disabled);
    }
    CHECK(s->retired_fragments==1025);atp_sim_destroy(s);++tests;
    /* Targeted retirement leaves other jobs' state/results intact. */
    s=atp_sim_create(2,1,8);CHECK(s);add_fragment(s,key(0),0);add_fragment(s,(atp_key){2,1,0,0},0);
    CHECK(atp_sim_run(s,5000));CHECK(atp_sim_retire(s,1,1));
    CHECK(s->count==1 && s->fragments[0].key.job==2);verify(s);
    CHECK(atp_sim_retire(s,2,1));atp_sim_destroy(s);++tests;
    /* Failed result must not be silently committed by the retirement API. */
    s=atp_sim_create(2,1,8);CHECK(s);add_fragment(s,key(0),0);s->offline=2;s->max_attempts=1;
    CHECK(!atp_sim_abort(s,1,1));
    CHECK(!atp_sim_run(s,1000));CHECK(!atp_sim_retire(s,1,1));
    CHECK(atp_sim_abort(s,1,1));CHECK(s->aborted_fragments==1 && s->count==0);
    s->offline=0;s->max_attempts=30;add_fragment(s,(atp_key){1,2,0,0},0);
    CHECK(atp_sim_run(s,5000));verify(s);atp_sim_destroy(s);++tests;
}
static void serialization(void) {
    atp_packet p=packet(0,3,INT32_MIN);p.length=3;p.scale=100;p.key.job=UINT32_C(0x01020304);
    p.value[1]=2*(int64_t)INT32_MAX;p.value[2]=-1;p.route_seed=0x10203040;p.route_version=7;p.resend=true;p.ecn=true;
    uint8_t bytes[ATP_WIRE_MAX+1],again[ATP_WIRE_MAX];atp_packet decoded={0};
    size_t n=atp_packet_encode(&p,bytes,sizeof(bytes));CHECK(n==72);
    CHECK(bytes[0]=='A' && bytes[1]=='T' && bytes[2]=='P' && bytes[3]=='1');
    CHECK(bytes[8]==1 && bytes[9]==2 && bytes[10]==3 && bytes[11]==4);
    CHECK(atp_packet_decode(&decoded,bytes,n));CHECK(decoded.value[2]==-1 && decoded.value[1]==p.value[1]);
    CHECK(decoded.route_seed==p.route_seed && decoded.scale==100 && decoded.resend);
    CHECK(atp_packet_encode(&decoded,again,sizeof(again))==n && memcmp(bytes,again,n)==0);
    for(size_t i=0;i<n;++i) { decoded.scale=999;CHECK(!atp_packet_decode(&decoded,bytes,i));CHECK(decoded.scale==999); }
    CHECK(!atp_packet_decode(&decoded,bytes,n+1));CHECK(!atp_packet_encode(&p,again,n-1));
    for(unsigned i=4;i<=7;++i) { uint8_t save=bytes[i];bytes[i]=255;CHECK(!atp_packet_decode(&decoded,bytes,n));bytes[i]=save; }
    uint8_t save=bytes[0];bytes[0]=0;CHECK(!atp_packet_decode(&decoded,bytes,n));bytes[0]=save;
    p.scale=0;CHECK(!atp_packet_encode(&p,again,sizeof(again)));p.scale=1;
    p.value[0]=INT64_MAX;CHECK(!atp_packet_encode(&p,again,sizeof(again)));
    CHECK(!atp_packet_decode(NULL,bytes,n));CHECK(!atp_packet_decode(&decoded,NULL,n));++tests;
    /* Decoder rejects random malformed buffers without writing invalid output. */
    unsigned rng=7;
    for(unsigned i=0;i<10000;++i) {
        for(unsigned b=0;b<ATP_WIRE_MAX;++b) { rng=rng*1664525u+1013904223u;bytes[b]=(uint8_t)(rng>>24); }
        if(atp_packet_decode(&decoded,bytes,i%(ATP_WIRE_MAX+1))) CHECK(atp_packet_valid(&decoded,decoded.contributors));
    }
    ++tests;
}
static void quantization(void) {
    atp_input quantized={0};atp_real_input real={0};
    real.value[0][0]=1.25;real.value[1][0]=-1.25;real.value[0][1]=2.0;real.value[1][1]=3.0;
    CHECK(atp_quantize(&quantized,&real,3,2,2));
    CHECK(quantized.value[0][0]==3 && quantized.value[1][0]==-3);
    atp_sim *s=atp_sim_create(2,1,4);CHECK(s);CHECK(atp_sim_configure_job(s,1,3,2));
    CHECK(atp_sim_add_vector(s,key(0),&quantized,2));CHECK(atp_sim_run(s,5000));verify(s);
    CHECK((double)s->fragments[0].sum[1]/s->fragments[0].scale==5.0);atp_sim_destroy(s);
    atp_input before=quantized;real.value[0][0]=NAN;
    CHECK(!atp_quantize(&quantized,&real,3,2,2));CHECK(memcmp(&before,&quantized,sizeof(before))==0);
    real.value[0][0]=INFINITY;CHECK(!atp_quantize(&quantized,&real,3,2,2));
    real.value[0][0]=(double)INT32_MAX+1;CHECK(!atp_quantize(&quantized,&real,3,2,1));
    CHECK(!atp_quantize(&quantized,&real,3,0,2));CHECK(!atp_quantize(&quantized,&real,3,2,0));++tests;
}
static void window_rules(void) {
    for(unsigned slow=0;slow<2;++slow) {
        atp_sim *s=atp_sim_create(1,1,64);CHECK(s);s->max_delay=0;s->ecn_threshold=UINT32_MAX;
        s->initial_ssthresh=slow ? 16 : 4;
        for(unsigned i=0;i<4;++i) add_fragment(s,key(i),0);
        CHECK(atp_sim_run(s,5000));verify(s);CHECK(s->flows[0].worker[0].window==(slow ? 8u : 5u));
        atp_sim_destroy(s);++tests;
    }
    atp_sim *s=atp_sim_create(1,1,64);CHECK(s);s->max_delay=0;s->rto=1000;
    for(unsigned i=0;i<3;++i) add_fragment(s,key(i),0);
    s->drop_next[ATP_TO_L1]=1;CHECK(!atp_sim_run(s,20));CHECK(s->fragments[2].done);
    atp_packet ack=fragment_ack(&s->fragments[2]);
    for(unsigned i=0;i<20;++i) CHECK(atp_sim_inject(s,ATP_ACK_WORKER,0,&ack));
    CHECK(!atp_sim_run(s,20));CHECK(s->fast_retries==0);
    CHECK(atp_sim_run(s,5000));verify(s);atp_sim_destroy(s);++tests;
}
static void heterogeneous_faults(void) {
    for(unsigned seed=1;seed<=20;++seed) {
        atp_sim *s=atp_sim_create(8,3,seed%3+1);CHECK(s);s->rng=seed*31;
        s->loss_per_mille=100;s->duplicate_per_mille=200;s->max_delay=12;
        CHECK(atp_sim_configure_job(s,1,0x55,7));CHECK(atp_sim_configure_job(s,2,0xaa,11));
        atp_input input={0};
        for(unsigned w=0;w<8;++w) for(unsigned v=0;v<ATP_VALUES;++v)
            input.value[w][v]=(int32_t)(seed*(w+3)*(v+7))*(v%2 ? -1:1);
        for(unsigned i=0;i<8;++i)
            CHECK(atp_sim_add_vector(s,(atp_key){i%2+1,seed,0,i},&input,(seed+i)%ATP_VALUES+1));
        CHECK(atp_sim_run(s,20000));verify(s);
        CHECK(atp_sim_retire(s,1,seed));CHECK(atp_sim_retire(s,2,seed));CHECK(s->count==0);
        atp_sim_destroy(s);++tests;
    }
}
static void phase1_acceptance(void) {
    independent_flows();fast_retransmission();versioned_rehash();reclamation();serialization();quantization();window_rules();heterogeneous_faults();
}
