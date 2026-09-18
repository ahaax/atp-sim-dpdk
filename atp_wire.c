/* Version 1 ATP-sim wire payload (not the original ATP/P4 wire format).
 * Explicit big-endian fields; caller supplies Ethernet/IP/UDP and integrity. */
#include "atp_sim.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static unsigned popcount(uint32_t x) {
    unsigned n=0; while(x) { x &= x-1; ++n; } return n;
}
bool atp_packet_valid(const atp_packet *p,uint32_t expected) {
    if(!p || !p->key.job || (p->type!=ATP_DATA && p->type!=ATP_ACK) ||
       !p->contributors || (p->contributors & ~expected) || !p->scale ||
       !p->length || p->length>ATP_VALUES) return false;
    if(p->type==ATP_DATA && (p->next_seed || p->next_version)) return false;
    if(p->type==ATP_ACK && (p->contributors!=expected || p->next_version<p->route_version || p->resend)) return false;
    unsigned n=popcount(p->contributors);
    for(unsigned v=0;v<p->length;++v)
        if(p->value[v]<(int64_t)INT32_MIN*n || p->value[v]>(int64_t)INT32_MAX*n) return false;
    return true;
}
static void put32(uint8_t *p,uint32_t x) {
    for(unsigned i=0;i<4;++i) p[i]=(uint8_t)(x>>(24-8*i));
}
static uint32_t get32(const uint8_t *p) {
    uint32_t x=0;for(unsigned i=0;i<4;++i) x=(x<<8)|p[i];return x;
}
static void put64(uint8_t *p,int64_t x) {
    uint64_t u=(uint64_t)x;
    for(unsigned i=0;i<8;++i) p[i]=(uint8_t)(u>>(56-8*i));
}
static int64_t get64(const uint8_t *p) {
    uint64_t u=0;for(unsigned i=0;i<8;++i) u=(u<<8)|p[i];
    if(u<=INT64_MAX) return (int64_t)u;
    return -1-(int64_t)(UINT64_MAX-u); /* Avoid implementation-defined unsigned cast. */
}
size_t atp_packet_encode(const atp_packet *p,uint8_t *buffer,size_t capacity) {
    if(!buffer || !p || !atp_packet_valid(p,p->contributors)) return 0;
    size_t n=ATP_WIRE_HEADER+8u*p->length;if(capacity<n) return 0;
    put32(buffer,UINT32_C(0x41545031));buffer[4]=1;buffer[5]=(uint8_t)p->type;
    buffer[6]=(uint8_t)((p->resend?1:0)|(p->collision?2:0)|(p->ecn?4:0)|(p->bypass?8:0));
    buffer[7]=(uint8_t)p->length;
    put32(buffer+8,p->key.job);put32(buffer+12,p->key.iteration);
    put32(buffer+16,p->key.tensor);put32(buffer+20,p->key.seq);
    put32(buffer+24,p->contributors);put32(buffer+28,p->route_seed);
    put32(buffer+32,p->route_version);put32(buffer+36,p->next_seed);
    put32(buffer+40,p->next_version);put32(buffer+44,p->scale);
    for(unsigned i=0;i<p->length;++i) put64(buffer+ATP_WIRE_HEADER+8*i,p->value[i]);
    return n;
}
bool atp_packet_decode(atp_packet *out,const uint8_t *b,size_t size) {
    if(!out || !b || size<ATP_WIRE_HEADER || get32(b)!=UINT32_C(0x41545031) ||
       b[4]!=1 || b[5]>ATP_ACK || (b[6]&0xf0u) || !b[7] || b[7]>ATP_VALUES ||
       size!=ATP_WIRE_HEADER+8u*b[7]) return false;
    atp_packet p={0};p.type=(atp_type)b[5];p.length=b[7];
    p.resend=(b[6]&1)!=0;p.collision=(b[6]&2)!=0;p.ecn=(b[6]&4)!=0;p.bypass=(b[6]&8)!=0;
    p.key=(atp_key){get32(b+8),get32(b+12),get32(b+16),get32(b+20)};
    p.contributors=get32(b+24);p.route_seed=get32(b+28);p.route_version=get32(b+32);
    p.next_seed=get32(b+36);p.next_version=get32(b+40);p.scale=get32(b+44);
    for(unsigned i=0;i<p.length;++i) p.value[i]=get64(b+ATP_WIRE_HEADER+8*i);
    if(!atp_packet_valid(&p,p.contributors)) return false;
    *out=p;return true; /* On failure, caller's output remains untouched. */
}
bool atp_quantize(atp_input *out,const atp_real_input *input,uint32_t members,
                  unsigned length,uint32_t scale) {
    if(!out || !input || !members || !scale || !length || length>ATP_VALUES) return false;
    atp_input result={0};
    for(unsigned w=0;w<ATP_WORKERS;++w) if(members&(UINT32_C(1)<<w))
        for(unsigned v=0;v<length;++v) {
            double scaled=input->value[w][v]*(double)scale;
            if(!isfinite(scaled)) return false;
            double rounded=round(scaled); /* nearest, halfway away from zero */
            if(rounded<INT32_MIN || rounded>INT32_MAX) return false;
            result.value[w][v]=(int32_t)rounded;
        }
    *out=result;return true;
}
