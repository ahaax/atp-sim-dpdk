/* 阅读约定：Worker 为训练发送端，PS 为参数服务器；所有成员集合均使用全局 Worker 位图。
 * tick 是仿真时间单位，不是毫秒；报文字节格式由 atp_wire.c 显式编码。
 * 本次仅添加中文注释和排版，不改变任何非注释代码标记。 */
#include "atp_sim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>

/* 比较 job、iteration、tensor、seq 四个字段，判断是否为同一分片。 */
bool atp_key_equal(atp_key a, atp_key b) {
    return a.job==b.job && a.iteration==b.iteration && a.tensor==b.tensor && a.seq==b.seq;
}
/* 将分片标识映射到槽下标；capacity 为槽数，返回范围 [0, capacity)，容量为 0 时返回 0。 */
uint32_t atp_hash(atp_key k, uint32_t capacity) {
    /* h 为混合哈希状态；uint32 无符号回绕是计算的一部分。 */
    uint32_t h=k.job;
    h=(h^k.iteration)*UINT32_C(16777619);
    h=(h^k.tensor)*UINT32_C(16777619);
    h=(h^k.seq)*UINT32_C(16777619);
    return capacity ? h%capacity : 0;
}
/* 初始化交换机并分配 capacity 个槽；expected 是默认 Worker 位图，level 只能为 1 或 2；失败返回 false。 */
bool atp_switch_init(atp_switch *sw,uint32_t capacity,uint32_t expected,unsigned level) {
    if(!sw || !capacity || !expected || (level!=1 && level!=2)) return false;
    memset(sw,0,sizeof(*sw)); sw->slots=calloc(capacity,sizeof(*sw->slots));
    if(!sw->slots) return false;
    sw->capacity=capacity; sw->expected=expected; sw->job_members=expected;
    sw->level=level; return true;
}
/* 释放槽内存并清零交换机；允许 sw 为 NULL。 */
void atp_switch_destroy(atp_switch *sw) {
    if(sw) { free(sw->slots); memset(sw,0,sizeof(*sw)); }
}
/* 查询分片是否已关闭；关闭记录（墓碑）用于阻止迟到包重新创建聚合状态。 */
static bool is_closed(const atp_switch *sw,atp_key key) {
    for(unsigned i=0;i<sw->closed_count;++i)
        if(atp_key_equal(sw->closed[i],key)) return true;
    return false;
}
/* 登记关闭分片；墓碑表满后停止接纳新聚合，转为旁路，避免遗忘旧分片造成重复聚合。 */
static void close_key(atp_switch *sw, atp_key key) {
    if(is_closed(sw, key)) return;
    if(sw->closed_count < ATP_FRAGMENTS) sw->closed[sw->closed_count ++] = key;
    else sw->admission_disabled = true; 
}
/* 逐元素求和并合并贡献位图和标志；调用者必须先保证贡献不重叠、格式一致且数值合法。 */
static void merge(atp_packet *dst,const atp_packet *src) {
    for(unsigned v=0;v<dst->length;++v) dst->value[v]+=src->value[v];
    dst->contributors |= src->contributors;
    dst->ecn |= src->ecn; dst->collision |= src->collision;
}
/* 在 count 个配置中按作业编号 id 查找；返回配置指针，未找到返回 NULL。 */
static atp_job *find_job(atp_job *jobs,unsigned count,uint32_t id) {
    for(unsigned i=0;i<count;++i) 
        if(jobs[i].job==id) 
            return &jobs[i];
    return NULL;
}
/* 将映射种子混入临时 seq 后计算槽下标；不会修改报文的真实分片编号。 */
static uint32_t packet_index(const atp_packet *p,uint32_t capacity) {
    atp_key key=p->key;key.seq ^= p->route_seed;return atp_hash(key,capacity);
}
/* 检查量化倍数、向量长度和映射种子/版本是否一致；分片标识和贡献位图另行检查。 */
static bool same_contract(const atp_packet *a,const atp_packet *b) {
    return a->scale==b->scale && a->length==b->length &&
           a->route_seed==b->route_seed && a->route_version==b->route_version;
}

/* 处理一个输入包：now 为仿真时刻，congestion 为本次拥塞信号。
 * 返回 FORWARD 时 output/out 才可用于发送；CONSUMED 表示本地吸收，INVALID 表示拒绝。
 * 同一交换机状态须串行访问；此函数不收发网络包，也不读取真实时钟。 */
atp_action atp_switch_process(atp_switch *sw, const atp_packet *input,
                             uint64_t now,bool congestion,atp_packet *out) {
    if(!sw || !sw->slots || !out || !input) return ATP_INVALID;
    atp_job *job=find_job(sw->jobs,sw->job_count,input->key.job);
    /* expected 是本节点待聚齐的成员集合；members 是整个作业的集合，供返回 ACK 校验。 */
    uint32_t expected=job ? job->local : sw->expected;
    uint32_t members=job ? job->members : sw->job_members;
    if((sw->job_count && !job) ||
       (job && (input->scale!=job->scale || (job->retired && input->key.iteration<=job->retired_iteration))) ||
       !atp_packet_valid(input,input->type==ATP_ACK ? members : expected)) {
        ++sw->invalid;return ATP_INVALID;
    }
    *out=*input;
    if(out->type==ATP_DATA) out->ecn |= congestion;
    /* s 为映射目标槽；match 同时要求槽已占用且属于同一分片。 */
    atp_slot *s = &sw->slots[packet_index(out, sw->capacity)];
    bool match = s->occupied && atp_key_equal(s->aggregate.key,out->key);
    if(match && !same_contract(&s->aggregate,out)) { ++ sw->invalid;return ATP_INVALID; }
    /* ACK 释放匹配槽并留下墓碑；即使槽未命中也继续向 Worker 转发。 */
    if(out->type==ATP_ACK) {  
        if(match) memset(s,0,sizeof(*s));
        close_key(sw,out->key); 
        return ATP_FORWARD;
    }
    /* 重传恢复：L1 可取回已缓存的贡献；L2 丢弃旧聚合，后续由 PS 完成去重汇总。 */
    if(out->resend) {
        if(match) {
            if(sw->level==1) {
                /* overlap 为已缓存贡献和来包贡献的交集，非 Worker 数量。 */
                /* 交集等于来包位图表示完整重复；仅部分重叠时不能安全累加。 */
                uint32_t overlap = s->aggregate.contributors & out->contributors;
                if(!overlap) merge(&s->aggregate,out);
                /* 部分重叠，无法拆出单 Worker 值：清槽并旁路原包，交由重传恢复。 */
                else if(overlap != out->contributors) {
                    memset(s,0,sizeof(*s)); close_key(sw,out->key);
                    out->bypass = true; return ATP_FORWARD;
                }
                s->aggregate.ecn |= out->ecn; //拥塞标记，只要有一个包携带拥塞标志就记录为拥塞
                *out = s->aggregate;
            }
            
            memset(s,0,sizeof(*s));
        }
        close_key(sw,out->key); 
        out->resend = true; 
        out->bypass = true;
        return ATP_FORWARD;
    }
    /* 旁路包直接前进，避免在后续层重新创建聚合依赖。 */
    if(out->bypass) return ATP_FORWARD;
    if(!match && (is_closed(sw,out->key) || sw->admission_disabled)) {
        out->bypass=true; return ATP_FORWARD;
    }
    /* 槽冲突：保护原槽，给来包标记碰撞并旁路到 PS。 */
    if(s->occupied && !match) {
        ++sw->fallbacks; out->collision=true; out->bypass=true; return ATP_FORWARD;
    }
    if(!match) {
        s->occupied=true; s->aggregate=*out; s->last_progress=now;
    } else {
        /* 交集等于来包位图表示完整重复；仅部分重叠时不能安全累加。 */
        uint32_t overlap=s->aggregate.contributors & out->contributors;
        s->aggregate.ecn |= out->ecn;
        if(overlap) {
            if(overlap==out->contributors) { ++sw->duplicates; return ATP_CONSUMED; }
            out->bypass=true; return ATP_FORWARD; 
        }
        merge(&s->aggregate,out); s->last_progress=now;
    }
    /* 成员凑齐只发一次；不能立即清槽，否则 ACK 丢失后的恢复会丢失缓存状态。 */
    if(s->aggregate.contributors==expected && !s->result_sent) {
        *out=s->aggregate; s->result_sent=true; ++sw->emitted;
        return ATP_FORWARD; 
    }
    return ATP_CONSUMED;
}
/* 关闭超过 timeout 个 tick 没有新增贡献的槽；只回收状态，可靠恢复由 Worker 重传驱动。 */
void atp_switch_expire(atp_switch *sw,uint64_t now,uint64_t timeout) {
    if(!sw || !sw->slots) return;
    for(uint32_t i=0;i<sw->capacity;++i) {
        atp_slot *s=&sw->slots[i];
        if(s->occupied && now>=s->last_progress && now-s->last_progress>=timeout) {
            close_key(sw,s->aggregate.key); memset(s,0,sizeof(*s)); ++sw->expired;
        }
    }
}


/* 更新可复现的 xorshift32 随机状态；用于丢包、重复和延迟注入，不用于密码学。 */
static unsigned random_next(atp_sim *sim) {
    /* x 是本轮伪随机状态；避免 xorshift 停留在全零状态。 */
    uint32_t x=sim->rng ? sim->rng : 1;
    x^=x<<13;x^=x>>17;x^=x<<5;sim->rng=x;return x;
}
/* 判断分片迭代号是否落入已回收区间；回收下界生效后拒绝迟到包。 */
static bool stale(const atp_job *job,atp_key key) {
    return job && job->retired && key.iteration<=job->retired_iteration;
}
/* 将报文复制到有界事件队列，设置目的节点和到达时刻；队列满则记一次丢包并返回 false。 */
static bool enqueue(atp_sim *sim,atp_dest dest,unsigned endpoint,const atp_packet *p) {
    if(sim->queue_count>=sim->queue_limit) { ++sim->drops;return false; }
    for(unsigned i=0;i<ATP_EVENTS;++i) if(!sim->events[i].used) {
        atp_event *e=&sim->events[i];e->used=true;e->packet=*p;e->dest=dest;e->endpoint=endpoint;
        e->due=sim->now+1+(sim->max_delay ? random_next(sim)%sim->max_delay : 0);
        ++sim->queue_count;return true;
    }
    ++sim->drops;return false;
}
/* 模拟一跳发送：校验、编解码、丢包/重复注入，再入队；true 仅表示原包成功入队，不保证交付。
 * endpoint：L1 方向为机架编号，ACK_WORKER 为 Worker 编号，L2/PS 调用处使用 0。 */
bool atp_sim_inject(atp_sim *sim, atp_dest dest, unsigned endpoint, const atp_packet *p) {
    if(!sim || !p || (unsigned)dest>=ATP_DEST_COUNT) return false;
    atp_job *job=find_job(sim->jobs,sim->job_count,p->key.job);
    if(stale(job,p->key)) { ++sim->stale;return false; }
    if(!job || p->scale!=job->scale || !atp_packet_valid(p,job->members) ||
       ((dest<=ATP_TO_PS)!=(p->type==ATP_DATA)) ||
       ((dest==ATP_TO_L1 || dest==ATP_ACK_L1) && endpoint>=sim->racks) ||
       (dest==ATP_ACK_WORKER && (endpoint>=sim->workers || !(job->members&(UINT32_C(1)<<endpoint))))) return false;
    
    /* wire 为临时字节缓冲区，decoded 为往返编解码得到的独立报文对象。 */
    uint8_t wire[ATP_WIRE_MAX];atp_packet decoded;
    /* n 是实际编码字节数，0 表示编码失败。 */
    size_t n=atp_packet_encode(p,wire,sizeof(wire));
    if(!n || !atp_packet_decode(&decoded,wire,n)) { ++sim->invalid;return false; }
    ++sim->tx_attempts;
    if(sim->drop_next[dest]) { --sim->drop_next[dest];++sim->drops;return false; }
    if(random_next(sim)%1000<sim->loss_per_mille) { ++sim->drops;return false; }
    bool ok=enqueue(sim,dest,endpoint,&decoded);
    if(ok && random_next(sim)%1000<sim->duplicate_per_mille) (void)enqueue(sim,dest,endpoint,&decoded);
    return ok;
}
/* 创建 workers 个 Worker、racks 个 L1 和一个 L2；每台交换机有 pool_size 个槽；失败返回 NULL。 */
atp_sim *atp_sim_create(unsigned workers,unsigned racks,uint32_t pool_size) {
    if(!workers || workers>ATP_WORKERS || !racks || racks>ATP_RACKS || racks>workers || !pool_size) return NULL;
    atp_sim *sim=calloc(1,sizeof(*sim));if(!sim) return NULL;
    sim->workers=workers;sim->racks=racks;
    sim->expected=workers==32 ? UINT32_MAX : (UINT32_C(1)<<workers)-1;
    /* masks 保存各机架成员位图；Worker 按 w % racks 分布。 */
    uint32_t masks[ATP_RACKS]={0};
    for(unsigned w=0;w<workers;++w) { sim->rack_of[w]=w%racks;masks[w%racks]|=UINT32_C(1)<<w; }
    for(unsigned r=0;r<racks;++r)
        if(!atp_switch_init(&sim->l1[r],pool_size,masks[r],1)) { atp_sim_destroy(sim);return NULL; }
    for(unsigned r=0;r<racks;++r) sim->l1[r].job_members=sim->expected;
    if(!atp_switch_init(&sim->l2,pool_size,sim->expected,2)) { atp_sim_destroy(sim);return NULL; }
    sim->queue_limit=ATP_EVENTS;sim->service_budget=64;sim->ecn_threshold=32;
    sim->rng=1;sim->max_delay=3;sim->rto=80;sim->slot_timeout=300;sim->max_attempts=30;
    sim->initial_window=4;sim->initial_ssthresh=16;sim->ai_step=1;return sim;
}
/* 释放仿真器及所有交换机槽；允许 sim 为 NULL。 */
void atp_sim_destroy(atp_sim *sim) {
    if(!sim) return;
    for(unsigned r=0;r<ATP_RACKS;++r) atp_switch_destroy(&sim->l1[r]);
    atp_switch_destroy(&sim->l2);free(sim);
}
/* 为作业配置全局成员位图 members 和量化倍数 scale，并下发各层本地成员；已有配置仅接受相同参数。 */
bool atp_sim_configure_job(atp_sim *sim,uint32_t id,uint32_t members,uint32_t scale) {
    if(!sim || !id || !members || (members&~sim->expected) || !scale) return false;
    atp_job *old=find_job(sim->jobs,sim->job_count,id);
    if(old) return old->members==members && old->scale==scale;
    if(sim->job_count>=ATP_JOBS) return false;
    atp_job job={0};job.job=id;job.members=members;job.local=members;job.scale=scale;
    sim->jobs[sim->job_count++]=job;
    sim->l2.jobs[sim->l2.job_count++]=job;
    for(unsigned r=0;r<sim->racks;++r) {
        job.local=0;
        for(unsigned w=0;w<sim->workers;++w)
            if(sim->rack_of[w]==r) job.local |= members&(UINT32_C(1)<<w);
        sim->l1[r].jobs[sim->l1[r].job_count++]=job;
    }
    return true;
}
/* 比较作业、迭代、张量三个字段；seq 不参与流身份，同一流中的分片共享发送窗口。 */
static bool same_flow(atp_key a,atp_key b) {
    return a.job==b.job && a.iteration==b.iteration && a.tensor==b.tensor;
}
/* 提交一个有效长度为 length 的分片并复制输入；同一流 seq 必须递增；容量/参数不合法返回 false。
 * 提交时冻结映射描述；后续 ACK 的映射反馈只影响以后提交的分片。 */
bool atp_sim_add_vector(atp_sim *sim,atp_key key,const atp_input *values,unsigned length) {
    if(!sim || !values || !key.job || !length || length>ATP_VALUES || sim->count>=ATP_FRAGMENTS ||
       !sim->initial_window || sim->initial_window>ATP_FRAGMENTS || !sim->initial_ssthresh ||
       !sim->ai_step || sim->ai_step>ATP_FRAGMENTS) return false;
    atp_job *job=find_job(sim->jobs,sim->job_count,key.job);
    if(!job) {
        if(!atp_sim_configure_job(sim,key.job,sim->expected,1)) return false;
        job=find_job(sim->jobs,sim->job_count,key.job);
    }
    if(!job) return false;
    if(stale(job,key)) return false;
    /* index 是流表下标；ATP_FLOWS 作为尚未找到可用表项的哨兵值。 */
    unsigned index=ATP_FLOWS;
    for(unsigned i=0;i<ATP_FLOWS;++i)
        if(sim->flows[i].used && same_flow(sim->flows[i].id,key)) { index=i;break; }
    if(index==ATP_FLOWS)
        for(unsigned i=0;i<ATP_FLOWS;++i) if(!sim->flows[i].used) {
            index=i;atp_flow *flow=&sim->flows[i];memset(flow,0,sizeof(*flow));flow->used=true;flow->id=key;
            for(unsigned w=0;w<sim->workers;++w) {
                flow->worker[w].window=sim->initial_window;
                flow->worker[w].ssthresh=sim->initial_ssthresh;
            }
            break;
        }
    if(index==ATP_FLOWS) return false;
    atp_flow *flow=&sim->flows[index];
    if(flow->has_seq && key.seq<=flow->last_seq) return false;
    flow->has_seq=true;flow->last_seq=key.seq;
    atp_fragment *f=&sim->fragments[sim->count++];memset(f,0,sizeof(*f));
    f->key=key, f->members=job->members, f->scale=job->scale, f->length=length, f->flow=index;
    
    /* 提交时复制已共同确认的映射；活跃分片不会随后续反馈改变槽位置。 */
    f->route_seed=flow->seed;f->route_version=flow->version;
    memcpy(f->input,values->value,sizeof(f->input));return true;
}
/* 以 ATP_VALUES 个元素提交分片，是 atp_sim_add_vector 的固定长度便捷入口。 */
bool atp_sim_add(atp_sim *sim,atp_key key,const atp_input *values) {
    return atp_sim_add_vector(sim,key,values,ATP_VALUES);
}
/* 按完整分片标识查找主机侧状态；未找到返回 NULL。 */
static atp_fragment *find_fragment(atp_sim *sim,atp_key key) {
    for(unsigned i = 0; i < sim->count; ++ i) 
        if(atp_key_equal(sim->fragments[i].key, key)) return &sim->fragments[i];
    return NULL;
}
/* 生成携带分片身份、长度、量化倍数和映射版本的空包；调用者再填写类型、贡献和数据。 */
static atp_packet descriptor(const atp_fragment *f) {
    atp_packet p={0};
    p.key = f->key;
    p.length = f->length;
    p.scale = f->scale;
    p.route_seed = f->route_seed;
    p.route_version = f->route_version;
    return p;
}
/* 将 PS 缓存的最终和、拥塞/碰撞反馈放入 ACK，经 L2 返回；发送失败等待后续重传触发重发。 */
static void send_ack(atp_sim *sim,const atp_fragment *f) {
    atp_packet p=descriptor(f);p.type=ATP_ACK;p.contributors=f->members;
    p.ecn=f->ecn;p.collision=f->collision;p.next_seed=f->next_seed;p.next_version=f->next_version;
    memcpy(p.value,f->sum,sizeof(p.value));(void)atp_sim_inject(sim,ATP_ACK_L2,0,&p);
}
/* PS 接收端去重并累计互不重叠的贡献；凑齐成员后仅交付一次结果，并缓存结果用于重复回复 ACK。 */
static void ps_process(atp_sim *sim,atp_fragment *f,const atp_packet *p) {
    if(f->failed) return;
    if(f->done) { send_ack(sim,f);return; }
    f->ecn |= p->ecn;f->collision |= p->collision;
    /* PS 已收贡献与本包的交集；任何重叠都不能直接加，缺失贡献由后续重传补齐。 */
    uint32_t overlap=f->seen&p->contributors;
    if(overlap) { if(overlap!=p->contributors) ++sim->overlaps;return; }
    for(unsigned v=0;v<f->length;++v) f->sum[v]+=p->value[v];
    f->seen |= p->contributors;
    if(f->seen==f->members) {
        f->done=true;++f->deliveries;atp_flow *flow=&sim->flows[f->flow];
        if(f->collision && flow->proposed_version<UINT32_MAX) {
            ++flow->proposed_version;flow->proposed_seed+=UINT32_C(0x9e3779b9);
        }
        f->next_seed=flow->proposed_seed;f->next_version=flow->proposed_version;
        send_ack(sim,f);
    }
}
/* 拥塞或重传时将窗口减半（至少 1）；一个 RTO 恢复期内避免重复减窗。 */
static void decrease(atp_sim *sim,atp_sender *sender) {
    if(sim->now<sender->recovery_until) return;
    sender->window=sender->window>1 ? sender->window/2 : 1;
    sender->ssthresh=sender->window;sender->credit=0;
    sender->recovery_until=sim->now+sim->rto;++sender->reductions;
}
/* 仅对本流连续已确认前缀计入拥塞控制，避免乱序 ACK 提前增长窗口；每个分片每个 Worker 只计一次。 */
static void congestion_ack(atp_sim *sim,atp_flow *flow,unsigned w) {
    uint32_t bit=UINT32_C(1)<<w;atp_sender *sender=&flow->worker[w];
    
    for(unsigned i=0;i<sim->count;++i) {
        atp_fragment *f=&sim->fragments[i];
        if(!same_flow(flow->id,f->key) || f->failed || (f->cc_applied&bit)) continue;
        if(!(f->acked&bit)) break;
        f->cc_applied|=bit;
        if(f->ecn) decrease(sim,sender);
        else if(sim->now>=sender->recovery_until) {
            /* add 是本次窗口增长量；慢启动逐确认增长，拥塞避免累计满一窗口确认后增长。 */
            unsigned add=0;
            if(sender->window<sender->ssthresh) add=sim->ai_step;
            else if(++sender->credit>=sender->window) { add=sim->ai_step;sender->credit=0; }
            sender->window=add>ATP_FRAGMENTS-sender->window ? ATP_FRAGMENTS : sender->window+add;
        }
    }
}
/* 若最早未确认分片之后已有至少 3 个不同分片被确认，请求一次快速重传；重复 ACK 不增加计数。 */
static void gap_detection(atp_sim *sim,unsigned flow_id,unsigned w) {
    /* bit 标识当前 Worker；missing 为最早缺 ACK 的已发送分片；higher 为其后不同已确认分片数。 */
    uint32_t bit=UINT32_C(1)<<w;atp_fragment *missing=NULL;unsigned higher=0;
    for(unsigned i=0;i<sim->count;++i) {
        atp_fragment *f=&sim->fragments[i];
        if(f->flow!=flow_id || f->failed || !(f->sent&bit)) continue;
        if(!(f->acked&bit) && !missing) missing=f;
        else if(missing && (f->acked&bit)) ++higher;
    }
    if(missing && higher>=3 && !(missing->fast_done&bit)) {
        missing->fast_requested|=bit;missing->fast_done|=bit;
    }
}
/* 收集各成员收到的最新映射建议；所有成员确认当前建议后才提交新版本。
 * 这里依靠单进程共享状态协调；未来 DPDK/分布式部署仍需实现实际控制消息与同步。 */
static void accept_feedback(atp_sim *sim,atp_fragment *f,const atp_packet *p,unsigned w) {
    atp_flow *flow=&sim->flows[f->flow];atp_sender *sender=&flow->worker[w];
    if(p->next_version>sender->feedback_version) {
        sender->feedback_version=p->next_version;sender->feedback_seed=p->next_seed;
    }
    if(flow->proposed_version<=flow->version) return;
    for(unsigned u=0;u<sim->workers;++u) if(f->members&(UINT32_C(1)<<u))
        if(flow->worker[u].feedback_version!=flow->proposed_version ||
           flow->worker[u].feedback_seed!=flow->proposed_seed) return;
    flow->version=flow->proposed_version;flow->seed=flow->proposed_seed;++sim->rehashes;
}
/* 投递一个到期事件，驱动 DATA,仿真中的下一跳节点连接：Worker→L1→L2→PS，ACK：PS→L2→L1→Worker。 */
static void dispatch(atp_sim *sim,const atp_event *e) {
    const atp_packet *p=&e->packet;
    atp_job *job = find_job(sim->jobs, sim->job_count, p->key.job);
    if(stale(job,p->key)) { ++sim->stale;return; }
    /* f 是目标主机分片；wanted 是提交时冻结的协议描述，用于拒绝不一致的来包。 */
    atp_fragment *f = find_fragment(sim,p->key);
    atp_packet wanted = {0};
    if(f) 
        wanted = descriptor(f);
    if(!f || !atp_packet_valid(p,f->members) || !same_contract(&wanted,p) ||
       ((e->dest<=ATP_TO_PS)!=(p->type==ATP_DATA))) { ++sim->invalid;return; }
    /* out 接收交换机输出；ce 用全局仿真队列近似拥塞信号，不代表真实网卡队列。 */
    atp_packet out;
    bool ce = sim->queue_count > sim->ecn_threshold;
    switch(e->dest) {
        case ATP_TO_L1:
            if(atp_switch_process(&sim->l1[e->endpoint],p,sim->now,ce,&out)==ATP_FORWARD)
                (void)atp_sim_inject(sim,ATP_TO_L2,0,&out);
            break;
        case ATP_TO_L2:
            if(atp_switch_process(&sim->l2,p,sim->now,ce,&out)==ATP_FORWARD)
                (void)atp_sim_inject(sim,ATP_TO_PS,0,&out);
            break;
        case ATP_TO_PS: ps_process(sim,f,p);break;
        case ATP_ACK_L2:
            if(atp_switch_process(&sim->l2,p,sim->now,false,&out)!=ATP_FORWARD) break;
            for(unsigned r=0;r<sim->racks;++r) {
                atp_job *rack_job=find_job(sim->l1[r].jobs,sim->l1[r].job_count,p->key.job);
                if(rack_job && rack_job->local)
                    (void)atp_sim_inject(sim,ATP_ACK_L1,r,p);
            }
            break;
        case ATP_ACK_L1:
            if(atp_switch_process(&sim->l1[e->endpoint],p,sim->now,false,&out)!=ATP_FORWARD) break;
            for(unsigned w=0;w<sim->workers;++w)
                if(sim->rack_of[w]==e->endpoint && (f->members&(UINT32_C(1)<<w)))
                    (void)atp_sim_inject(sim,ATP_ACK_WORKER,w,p);
            break;
        case ATP_ACK_WORKER: {
            unsigned w = e->endpoint;
            uint32_t bit=UINT32_C(1) << w;
            if(!(f->sent & bit) || (f->acked & bit) || f->failed) break;
            atp_flow *flow = &sim->flows[f->flow];
            f->acked |= bit, -- flow->worker[w].inflight; //在途发送
            memcpy(f->received[w], p->value, sizeof(p->value));
            accept_feedback(sim, f, p, w);
            congestion_ack(sim, flow, w);
            gap_detection(sim, f->flow, w);
            break;
        }
        default: ++sim->invalid;break;
    }
}
/* 逐流逐 Worker 调度首次发送、超时重传和快速重传；达到尝试上限则标记分片失败并释放在途配额。 */
static void worker_tick(atp_sim *sim) {
    for(unsigned i = 0; i < sim->count; ++ i) {
        atp_fragment *f = &sim->fragments[i];
        if(f -> failed) continue;
        for(unsigned w = 0; w < sim->workers; ++ w) {
            uint32_t bit = UINT32_C(1) << w; //检查第w个worker，把第w个worker对应的bit位置置为1
            if(!(f->members & bit) || (f->acked & bit)) continue;
            atp_sender *sender=&sim->flows[f->flow].worker[w];
            /* retry 表示曾发送过；fast 表示缺口检测已请求快速重传，可跳过 RTO【重传超时时间】 等待。 */
            bool retry = (f->sent & bit) != 0, fast=(f->fast_requested & bit) != 0;
            if(retry && !fast && sim->now - f->last_send[w] < sim->rto) continue;
            if(!retry && sender->inflight >= sender->window) continue;
            /* 尝试上限是整个分片的终止失败条件；同时释放所有尚未确认成员的在途配额。 */
            if(f->attempts[w]>=sim->max_attempts) {
                f->failed=true;
                for(unsigned u=0;u<sim->workers;++u)
                    if((f->sent&~f->acked)&(UINT32_C(1)<<u)) --sim->flows[f->flow].worker[u].inflight;
                break;
            }
            atp_packet p = descriptor(f);
            p.type = ATP_DATA;
            p.contributors = bit;
            p.resend = retry;
            for(unsigned v=0;v<f->length;++v) p.value[v]=f->input[w][v];
            if(!retry) { f->sent|=bit;++sender->inflight; }
            else { ++sim->retries;decrease(sim,sender);if(fast) ++sim->fast_retries; }
            f->fast_requested &= ~bit;++f->attempts[w];f->last_send[w]=sim->now;
            if(!(sim->offline&bit)) (void)atp_sim_inject(sim,ATP_TO_L1,sim->rack_of[w],&p);
        }
    }
}
/* 检查交换机是否仍有占用槽，供仿真结束条件使用。 */
static bool switch_busy(const atp_switch *sw) {
    for(uint32_t i=0;i<sw->capacity;++i) if(sw->slots[i].occupied) return true;
    return false;
}
/* 最多推进 ticks 个逻辑时钟步；全部成功且事件/槽排空才返回 true。
 * false 可能是参数错误、时间预算不足或终止失败，需结合 fragment.failed 等状态判断。 */
bool atp_sim_run(atp_sim *sim,uint64_t ticks) {
    if(!sim || !sim->rto || !sim->max_attempts || sim->now>UINT64_MAX-ticks ||
       sim->now+ticks>UINT64_MAX-sim->rto || sim->now+ticks>UINT64_MAX-sim->max_delay-1) return false;
    for(uint64_t t=0;t<ticks;++t) {
        ++sim->now;worker_tick(sim);
        for(unsigned n=0;n<sim->service_budget;++n) {
            /* next/due 记录本轮最早到期事件；ATP_EVENTS 表示没有可投递事件。 */
            unsigned next=ATP_EVENTS;uint64_t due=UINT64_MAX;
            for(unsigned i=0;i<ATP_EVENTS;++i)
                if(sim->events[i].used && sim->events[i].due<=sim->now && sim->events[i].due<due) {
                    next=i;due=sim->events[i].due;
                }
            if(next==ATP_EVENTS) break;
            atp_event e=sim->events[next];sim->events[next].used=false;--sim->queue_count;dispatch(sim,&e);
        }
        for(unsigned r=0;r<sim->racks;++r) atp_switch_expire(&sim->l1[r],sim->now,sim->slot_timeout);
        atp_switch_expire(&sim->l2,sim->now,sim->slot_timeout);
        /* finished 表示无仍待确认的非失败分片；success 额外要求没有终止失败。 */
        bool finished=true,success=true;
        for(unsigned i=0;i<sim->count;++i) {
            if(sim->fragments[i].failed) success=false;
            else if(sim->fragments[i].acked!=sim->fragments[i].members) finished=false;
        }
        if(finished && !sim->queue_count) {
            bool busy=switch_busy(&sim->l2);
            for(unsigned r=0;r<sim->racks;++r) busy |= switch_busy(&sim->l1[r]);
            if(!busy) return success;
        }
    }
    return false;
}
/* 判断 key 是否属于指定作业且 iteration 不大于 through（包含端点）。 */
static bool affected(atp_key key,uint32_t job,uint32_t through) {
    return key.job==job && key.iteration<=through;
}
/* 检查待回收区间内是否仍有占用槽；有则不能通过回收屏障。 */
static bool can_retire_switch(const atp_switch *sw,uint32_t job,uint32_t through) {
    for(uint32_t i=0;i<sw->capacity;++i)
        if(sw->slots[i].occupied && affected(sw->slots[i].aggregate.key,job,through)) return false;
    return true;
}
/* 设置拒绝旧迭代的持久标记并压紧墓碑表；调用者已检查该作业存在且待回收槽全部排空。 */
static void retire_switch(atp_switch *sw,uint32_t id,uint32_t through) {
    atp_job *job=find_job(sw->jobs,sw->job_count,id);
    if(!job) return; 
    job->retired=true;job->retired_iteration=through;
    /* kept 是压紧数组时保留项的写入下标；仅移除指定回收区间内的项。 */
    unsigned kept=0;
    for(unsigned i=0;i<sw->closed_count;++i)
        if(!affected(sw->closed[i],id,through)) sw->closed[kept++]=sw->closed[i];
    sw->closed_count=kept;
    
}
/* 先验证分片、事件、槽均满足回收条件，再设置旧迭代拒绝标记并回收存储；allow_failed 允许回收终止失败项。 */
static bool reclaim(atp_sim *sim,uint32_t id,uint32_t through,bool allow_failed) {
    if(!sim) return false;
    atp_job *job=find_job(sim->jobs,sim->job_count,id);
    if(!job || (job->retired && through<=job->retired_iteration)) return false;
    if(!find_job(sim->l2.jobs,sim->l2.job_count,id)) return false;
    for(unsigned r=0;r<sim->racks;++r)
        if(!find_job(sim->l1[r].jobs,sim->l1[r].job_count,id)) return false;
    for(unsigned i=0;i<sim->count;++i) {
        atp_fragment *f=&sim->fragments[i];
        bool completed=f->done && !f->failed && f->acked==f->members;
        if(affected(f->key,id,through) && !completed && !(allow_failed && f->failed)) return false;
    }
    for(unsigned i=0;i<ATP_EVENTS;++i)
        if(sim->events[i].used && affected(sim->events[i].packet.key,id,through)) return false;
    if(!can_retire_switch(&sim->l2,id,through)) return false;
    for(unsigned r=0;r<sim->racks;++r) if(!can_retire_switch(&sim->l1[r],id,through)) return false;
    
    /* 单线程回收屏障：先安装拒绝旧迭代的标记，再释放输入/PS 状态；不是已实现的分布式屏障。 */
    job->retired=true;job->retired_iteration=through;retire_switch(&sim->l2,id,through);
    for(unsigned r=0;r<sim->racks;++r) retire_switch(&sim->l1[r],id,through);
    /* kept 是压紧数组时保留项的写入下标；仅移除指定回收区间内的项。 */
    unsigned kept=0;
    for(unsigned i=0;i<sim->count;++i) {
        if(affected(sim->fragments[i].key,id,through)) {
            ++sim->retired_fragments;
            if(sim->fragments[i].failed) ++sim->aborted_fragments;
        }
        else sim->fragments[kept++]=sim->fragments[i];
    }
    memset(&sim->fragments[kept],0,(sim->count-kept)*sizeof(sim->fragments[0]));sim->count=kept;
    for(unsigned i=0;i<ATP_FLOWS;++i)
        if(sim->flows[i].used && affected(sim->flows[i].id,id,through)) memset(&sim->flows[i],0,sizeof(sim->flows[i]));
    return true;
}
/* 回收指定作业中 iteration <= through_iteration 的成功分片；要求全部 ACK 到达且相关事件/槽已排空。 */
bool atp_sim_retire(atp_sim *sim,uint32_t id,uint32_t through) {
    return reclaim(sim,id,through,false);
}
/* 显式回收终止失败分片（也可含已成功项）；不是取消正在执行的工作，同样要求相关事件/槽排空。 */
bool atp_sim_abort(atp_sim *sim,uint32_t id,uint32_t through) {
    return reclaim(sim,id,through,true);
}
/* 测试构建定义 ATP_NO_MAIN 后不编译演示入口，由 tests/test_atp.c 提供 main。 */
#ifndef ATP_NO_MAIN
/* 最小演示：4 个 Worker、2 个机架、每台交换机 2 个槽，注入 DATA/ACK 丢包后检查闭环完成情况。 */
int main(void) {
    atp_sim *sim=atp_sim_create(4,2,2);if(!sim) return EXIT_FAILURE;
    atp_input values={0};
    for(unsigned w=0;w<4;++w) for(unsigned v=0;v<ATP_VALUES;++v) values.value[w][v]=(int32_t)((w+1)*(v+1));
    bool ok=atp_sim_add(sim,(atp_key){1,1,0,0},&values);
    sim->drop_next[ATP_TO_PS]=1;sim->drop_next[ATP_ACK_WORKER]=1;ok=ok && atp_sim_run(sim,5000);
    printf("ATP closed-loop demo: %s; sum=%" PRId64 ", mask=%" PRIu32 ", retries=%" PRIu64 ", drops=%" PRIu64 "\n",
           ok ? "completed" : "FAILED",sim->fragments[0].sum[0],sim->fragments[0].seen,sim->retries,sim->drops);
    atp_sim_destroy(sim);return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
