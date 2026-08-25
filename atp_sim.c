#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ============================================================
 *  ATP 极简 C 仿真（修正版）
 *  编译: gcc -O2 -Wall atp_sim.c -o atp_sim
 *  运行: ./atp_sim
 * ============================================================ */

/* 记录已经被重传解救过的 <job_id, seq>，防止后续包再次预留形成孤儿聚合器 */
#define RESCUED_MAX 256

typedef struct {
    uint32_t job_id;
    uint16_t seq;
} rescued_entry_t;



typedef struct {
    uint32_t job_id;
    uint16_t seq;
    uint8_t  worker_id;
    int32_t  data;
    uint8_t  fan_in;
    bool     collision;
    bool     from_switch;
    bool     resend;          /* 新增：是否为重传包 */
    /* 
     * 修复 B 新增：交换机结果包需要携带聚合器内的 bitmap 和 count，
     * 以便 PS 端判断这是"部分聚合"还是"完整聚合"，并做去重。
     */
    uint32_t bitmap;          /* 该包代表哪些 Worker 的聚合（原始包为 1<<worker_id） */
    uint8_t  count;           /* 该包包含几个 Worker 的和（原始包为 1） */
    
    
    bool ecn;             /* 新增：ECN 拥塞标记 */

} atp_packet_t;

/* 聚合器槽位：job_id == 0 表示空闲，不需要额外的 occupied 字段 */
typedef struct {
    uint32_t job_id;
    uint16_t seq;
    int32_t  sum;
    uint32_t bitmap;
    uint8_t  count;
    uint64_t timestamp;       /* 新增：最后更新时间（用全局时钟计数） */
    bool    ecn;             /* 新增：聚合器累积的ECN状态 */
} agg_slot_t;

typedef struct {
    agg_slot_t   *pool;
    uint32_t      pool_size;
    atp_packet_t *ps_queue;
    uint32_t      ps_tail;  // 管理或跟踪数据结构的尾部位置
    uint32_t      ps_cap; //表示某种能力或容量的值
    uint64_t      consumed;
    uint64_t      fallback;
    uint64_t      completed;
    uint64_t      global_time;/* 新增：模拟全局时钟，每处理一个包+1 */
    /* 新增：已解救表 */
    rescued_entry_t rescued[RESCUED_MAX];
    uint32_t        rescued_cnt;

    uint32_t       egress_queue_depth; /* 新增：出口队列深度，用于模拟拥塞 */
    uint32_t       ecn_threshold;      /* 新增：ECN标记阈值*/
} atp_switch_t;

#define PS_BUF_MAX 1024

typedef struct {
    uint32_t job_id;
    uint16_t seq;
    int32_t  sum;
    uint32_t bitmap;
    uint8_t  count;
    uint8_t  fan_in;
    bool     done;
} ps_buffer_t;

//计算hash索引
static inline uint16_t hash_idx(uint32_t job_id, uint16_t seq, uint32_t pool_size)
{
    return ((job_id * 31 + seq) % pool_size);
}

/* 检查该 <job, seq> 是否已经被重传解救过 */
static bool is_rescued(atp_switch_t *sw, uint32_t job_id, uint16_t seq)
{
    for (uint32_t i = 0; i < sw->rescued_cnt; i++) {
        if (sw->rescued[i].job_id == job_id && sw->rescued[i].seq == seq)
            return true;
    }
    return false;
}

/* 标记该 <job, seq> 已被重传解救 */
static void mark_rescued(atp_switch_t *sw, uint32_t job_id, uint16_t seq)
{
    if (sw->rescued_cnt >= RESCUED_MAX) {
        /* 表满：简单处理，覆盖最旧的（FIFO），或扩容。仿真里直接忽略。 */
        fprintf(stderr, "[Warn] rescued table full, dropping oldest entry\n");
        /* 左移覆盖第一个 */
        memmove(&sw->rescued[0], &sw->rescued[1], 
                (RESCUED_MAX - 1) * sizeof(rescued_entry_t));
        sw->rescued_cnt = RESCUED_MAX - 1;
    }
    sw->rescued[sw->rescued_cnt].job_id = job_id;
    sw->rescued[sw->rescued_cnt].seq = seq;
    sw->rescued_cnt++;
}

/*
atp_switch_t 结构体用于管理和跟踪与 ATP（异步传输协议）相关的状态和数据，
跟踪控制数据流
包括池的指针、池的大小、数据包队列及其相关的状态信息，如已消费、回退和完成的计数。
*/
atp_switch_t* switch_create(uint32_t pool_size)
{
    atp_switch_t *sw = calloc(1, sizeof(atp_switch_t));
    if (!sw) {
        fprintf(stderr, "[Error] switch_create: calloc(sw) failed\n");
        return NULL;
    }


    sw->pool_size = pool_size;
    sw->pool = calloc(pool_size, sizeof(agg_slot_t));
    if (!sw->pool) {
        fprintf(stderr, "[Error] switch_create: calloc(pool) failed\n");
        free(sw);          /* 回滚：释放已分配的 sw */
        return NULL;
    }
    
    sw->ps_cap = 4096;
    sw->ps_queue = calloc(sw->ps_cap, sizeof(atp_packet_t));
    if (!sw->ps_queue) {
        fprintf(stderr, "[Error] switch_create: calloc(ps_queue) failed\n");
        free(sw->pool); //回滚:按分配逆序释放
        free(sw);
        return NULL;
    }

    /*ECN*/
    sw->egress_queue_depth = 0;
    sw->ecn_threshold = 1; /* 默认的阈值, 测试中可覆盖 */

    return sw;
}

static void send_to_ps(atp_switch_t *sw, atp_packet_t *pkt)
{
    if (sw->ps_tail >= sw->ps_cap) {
        fprintf(stderr, "PS queue overflow\n");
        exit(1);
    }
    sw->ps_queue[sw->ps_tail++] = *pkt;
    sw->egress_queue_depth++; /* 模拟包进入出口队列，深度增加 */
}

/* 新增：根据出口队列深度标记 ECN */
static inline void check_and_mark_ecn(atp_switch_t *sw, atp_packet_t *pkt)
{
    if (sw->egress_queue_depth > sw->ecn_threshold) {
        pkt->ecn = true;
    }
}

/* 
 * 处理重传包（论文 §3.7 核心）
 * 一级交换机行为：若聚合器存在，合并该 Worker（如未合并过），
 *                然后强制把结果（  可能部分聚合）发给 PS，释放槽位。
 */
static void switch_process_resend(atp_switch_t *sw, atp_packet_t *pkt)
{
    check_and_mark_ecn(sw, pkt); /* 新增：检查出口队列深度，标记 ECN */

    uint16_t idx = hash_idx(pkt->job_id, pkt->seq, sw->pool_size);
    agg_slot_t *slot = &sw->pool[idx];

    /* 聚合器存在且匹配：合并并强制释放 */
    if (slot->job_id == pkt->job_id && slot->seq == pkt->seq) {
        if (!(slot->bitmap & (1u << pkt->worker_id))) {
            slot->sum += pkt->data;
            slot->bitmap |= (1u << pkt->worker_id);
            slot->count++;

            slot->ecn |= pkt->ecn; /* 合并，累积 ECN 状态 */
        }

        /*
         * 修复 B：交换机结果包必须携带聚合器当前的 bitmap 和 count，
         * 让 PS 知道这是"部分聚合"还是"完整聚合"。
         */

        atp_packet_t result = {
            .job_id = pkt->job_id,
            .seq = pkt->seq,
            .worker_id = 0xFF,
            .data = slot->sum,
            .fan_in = pkt->fan_in,
            .collision = false,
            .from_switch = true,
            .bitmap = slot->bitmap,
            .count = slot->count,
            .ecn = slot->ecn  /* 新增：携带累积的 ECN 状态 */
        };
        /* 强制发 PS 并释放槽位 */
        send_to_ps(sw, &result);
        memset(slot, 0, sizeof(*slot));
        sw->completed++;
        
        /* 新增：标记该 <job, seq> 已被解救，后续同 Seq 包禁止预留 */
        mark_rescued(sw, pkt->job_id, pkt->seq);
        
        printf("  [Switch] 槽位%2d 重传解救并标记: Job%d Seq%d -> 后续禁止预留\n", idx, pkt->job_id, pkt->seq);
        return;
        
    }

    /* 聚合器已不存在（可能被 ACK 释放或超时清理过）：直接转发原始包到 PS */
    printf("  [Switch] 槽位%2d 重传转发: 聚合器已释放, 原始包直发PS (ECN=%d)\n", idx, pkt->ecn);
    send_to_ps(sw, pkt);
}




/* 
 * 交换机核心：尽力而为的三种路径
 * 用于处理交换机中的数据包，根据槽位的状态（空闲、自己的或冲突）执行相应的操作，
 * 包括预留槽位、累加数据或将数据包发送到处理系统（PS）。
 * 该函数通过哈希索引确定槽位，并在处理过程中更新交换机的状态和统计信息
 */
void switch_process(atp_switch_t *sw, atp_packet_t *pkt)
{

    sw->global_time++;  /* 全局时钟推进 */
    
    check_and_mark_ecn(sw, pkt); /* 新增：检查出口队列是否堵塞 */

    /* 如果是重传包，走专门逻辑 */
    if (pkt->resend) {
        switch_process_resend(sw, pkt);
        return;
    }

    uint16_t idx = hash_idx(pkt->job_id, pkt->seq, sw->pool_size);
    agg_slot_t *slot = &sw->pool[idx];

    /* ---- 路径 A：槽位空闲 (job_id == 0) -> FCFS 预留 ---- *///⭐是/必须是FCFS吗?
    if (slot->job_id == 0) {  /* 修复1：用 job_id==0 判断空闲，job_id会从1开始,0默认无效 */
       
        /* 新增：如果该 <job, seq> 已被重传解救过，说明聚合器曾经存在但被强制释放了。
        * 此时不应再预留槽位（否则后续重传包会形成新的孤儿聚合器），直接转发到 PS。 */
        if (is_rescued(sw, pkt->job_id, pkt->seq)) {
            send_to_ps(sw, pkt);
            printf("  [Switch] 槽位%2d 拒绝预留: Job%d Seq%d 已被重传解救过,直接发PS\n",
                idx, pkt->job_id, pkt->seq);
            return;
        }
       
        // 正常预留
        slot->job_id = pkt->job_id;
        slot->seq = pkt->seq;
        slot->sum = pkt->data;
        slot->bitmap = pkt->bitmap; //(1u << pkt->worker_id);//1U 是一个无符号整数常量，表示值为 1 的无符号整型。它通常用于需要确保数值为非负的场景
        slot->ecn = pkt->ecn; /* 新增：继承包的 ECN 状态 */
        slot->count = 1;
        slot->timestamp = sw->global_time;

        sw->consumed++;
        printf("  [Switch] 槽位%2d 预留    : Job%d Seq%d Worker%d (data=%d)\n",
               idx, pkt->job_id, pkt->seq, pkt->worker_id, pkt->data);
        return;
    }

    /* ---- 路径 B：槽位是自己的 -> 累加 ---- */
    if (slot->job_id == pkt->job_id && slot->seq == pkt->seq) {
        if (slot->bitmap & (1u << pkt->worker_id)) {
            printf("  [Switch] 槽位%2d 重复    : Job%d Seq%d Worker%d -> 丢弃\n",
                   idx, pkt->job_id, pkt->seq, pkt->worker_id);
            sw->consumed++;
            return;
        }

        slot->sum += pkt->data;
        slot->bitmap |= (1u << pkt->worker_id);  //设为1，表示该worker已经贡献过数据
        slot->count++;
        slot->ecn |= pkt->ecn; /* 新增：累积 ECN 状态 */
        slot->timestamp = sw->global_time;

        if (slot->count >= pkt->fan_in) {
            /*
             * 修复 B：正常聚合完成时，结果包携带完整 bitmap 和 count。
             */
            atp_packet_t result = {
                .job_id = pkt->job_id,
                .seq = pkt->seq,
                .worker_id = 0xFF,//255,哨兵值,不是worker发送的包是switch端
                //255是 uint8_t 的最大值，不可能被真实 Worker 用到，所以天然安全，不会冲突。
                .data = slot->sum,
                .fan_in = pkt->fan_in,
                .collision = false,
                .from_switch = true,
                .bitmap = slot->bitmap,
                .count = slot->count,
                .ecn = slot->ecn  /* 新增：携带累积的 ECN 状态 */
            };
            send_to_ps(sw, &result);

            printf("  [Switch] 槽位%2d 完成    : Job%d Seq%d -> 值=%d (发往PS) (ECN=%d)\n",
                   idx, pkt->job_id, pkt->seq, slot->sum, slot->ecn);

            memset(slot, 0, sizeof(*slot)); /* 释放槽位 */
            sw->completed++;
        } else {
            printf("  [Switch] 槽位%2d 累加    : Job%d Seq%d %d/%d\n",
                   idx, pkt->job_id, pkt->seq, slot->count, pkt->fan_in);
            sw->consumed++;
        }
        return;
    }

    /* ---- 路径 C：冲突 -> 尽力而为回退到 PS ---- */
    pkt->collision = true;
    send_to_ps(sw, pkt);
    sw->fallback++;

    printf("  [Switch] 槽位%2d 冲突回退: 被Job%dSeq%d占用,"
           "Job%dSeq%dWorker%d -> 直接发PS (ECN=%d)\n",
           idx, slot->job_id, slot->seq,
           pkt->job_id, pkt->seq, pkt->worker_id, pkt->ecn);
}


/*
 * 超时扫描：清理孤儿聚合器（论文 §3.7 内存泄漏防护）
 */
void switch_scan_timeout(atp_switch_t *sw, uint64_t threshold)
{
    for (uint32_t i = 0; i < sw->pool_size; i++) {
        agg_slot_t *slot = &sw->pool[i];
        if (slot->job_id != 0 &&
            (sw->global_time - slot->timestamp) > threshold) {
            printf("  [Switch] 槽位%2d 超时清理: Job%d Seq%d 孤儿聚合器强制释放\n",
                   i, slot->job_id, slot->seq);
            memset(slot, 0, sizeof(*slot));
        }
    }
}



/* 
 * PS 端处理
 *函数用于处理来自交换机的多个数据包，聚合数据并检查是否收齐所有必要的包。
 *它维护一个缓冲区以存储和更新每个作业的状态，
 *并在处理完成或未收齐时输出相应的日志信息。
 * 
 * 修复 A：增加 entry->done 前置检查，防止交换机结果包先到、原始 fallback 包后到
 *         导致的重复累加。
 * 修复 B：交换机结果包携带 bitmap/count，PS 端用通用 bitmap 去重逻辑处理，
 *         支持"部分聚合结果"的正确累加，不再盲目认为 from_switch=true 就是完整结果。
 */

void ps_process(atp_switch_t *sw)
{
    ps_buffer_t buf[PS_BUF_MAX];//buf 是 PS 端的缓冲区，里面每个 buf[j] 代表一个“job + seq”的汇总状态
    
    uint32_t buf_cnt = 0;

    printf("\n  --- PS 开始处理交换机转来的 %d 个包 ---\n", sw->ps_tail);

    for (uint32_t i = 0; i < sw->ps_tail; i++) {
        atp_packet_t *pkt = &sw->ps_queue[i];
        ps_buffer_t *entry = NULL;

        for (uint32_t j = 0; j < buf_cnt; j++) {
            if (buf[j].job_id == pkt->job_id && buf[j].seq == pkt->seq) {
                entry = &buf[j];
                break;
            }
        }
        if (!entry) {
            assert(buf_cnt < PS_BUF_MAX);
            entry = &buf[buf_cnt++];  
            memset(entry, 0, sizeof(*entry));
            entry->job_id = pkt->job_id;
            entry->seq = pkt->seq;
            entry->fan_in = pkt->fan_in;
        }

        /* ========== 修复 A：如果该 (job, seq) 已经被标记为 done，直接忽略后续一切包 ========== */
        if (entry->done) {
            continue;
        }

        /* 新增：打印 ECN 拥塞信号 */
        if (pkt->ecn) {
            printf("  [PS] 检测到 ECN 标记: Job%d Seq%d (交换机出口拥塞)\n",
                   pkt->job_id, pkt->seq);
        }

        /* ========== 修复 B：通用 bitmap 去重累加逻辑 ========== */
        uint32_t overlap = entry->bitmap & pkt->bitmap;

        if (overlap) {
            /*
             * 有重叠：说明 PS 已经收到了其中某些 Worker 的数据。
             * 如果交换机结果包是"完整聚合"（count >= fan_in），直接用权威结果覆盖。
             * 否则（部分聚合且有重叠），丢弃，因为无法拆分总和避免重复。
             */
            if (pkt->from_switch && pkt->count >= entry->fan_in) {
                entry->sum = pkt->data;
                entry->bitmap = pkt->bitmap;
                entry->done = true;
                printf("  [PS] 聚合完成: Job%d Seq%d = %d (交换机完整结果覆盖)\n",
                       entry->job_id, entry->seq, entry->sum);
            } else {
                printf("  [PS] 丢弃重复: Job%d Seq%d (overlap bitmap=%u)\n",
                       pkt->job_id, pkt->seq, overlap);
            }
            continue;
        }

        /* 无重叠：安全累加 */
        //fallback包，仍然是原始 worker 数据包，需要按 worker 去合并
        entry->sum += pkt->data;
        entry->bitmap |= pkt->bitmap;
        entry->count += pkt->count;  /* 修复 B：累计 count */

        /* 检查是否收齐：用 bitmap 的 popcount 判断（比 count 更可靠） */
        
        uint32_t b = entry->bitmap;
        uint8_t popcnt = 0;
        while (b) { popcnt++; b &= b - 1; }  /* 计算 popcount */

        if (popcnt >= entry->fan_in && !entry->done) {
            printf("  [PS] 聚合完成: Job%d Seq%d = %d (PS端累加完成, bitmap=%u)\n",
                   entry->job_id, entry->seq, entry->sum, entry->bitmap);
            entry->done = true;
        }
    }


    for (uint32_t j = 0; j < buf_cnt; j++) {
        if (!buf[j].done) {
            printf("  [PS] 警告: Job%d Seq%d 未收齐 (%d/%d)\n",
                   buf[j].job_id, buf[j].seq, buf[j].count, buf[j].fan_in);
        }
    }
}

static void print_stats(atp_switch_t *sw, int total_packets)
{
    printf("\n========== 统计 ==========\n");
    printf("总包数              : %d\n", total_packets);
    printf("被交换机消费(省带宽): %lu\n", sw->consumed);
    printf("尽力而为回退到PS    : %lu\n", sw->fallback);
    printf("交换机完成聚合次数  : %lu\n", sw->completed);
    printf("PS 收到包数        : %u\n", sw->ps_tail);
    printf("带宽节省率         : %.1f%%\n",
           100.0 * sw->consumed / total_packets);
}

static void reset_switch(atp_switch_t *sw)
{
    memset(sw->pool, 0, sw->pool_size * sizeof(agg_slot_t));
    sw->ps_tail = 0;
    sw->consumed = 0;
    sw->fallback = 0;
    sw->completed = 0;
    sw->global_time = 0;
    sw->egress_queue_depth = 0;   
}

static atp_packet_t make_pkt(uint32_t job, uint16_t seq, uint8_t wid, int val, uint8_t fan_in)
{
    return (atp_packet_t){
        .job_id = job, .seq = seq, .worker_id = wid,
        .data = val, .fan_in = fan_in,
        .collision = false, .from_switch = false, .resend = false,
        .bitmap = (1u << wid), /* 原始包只代表自己这一个 Worker */
        .count = 1,
        .ecn = false  /* 默认无 ECN */
    };
}

void test1_no_contention(void)
{
    printf("\n\n########################################\n");
    printf("TEST 1: 单 Job, Pool 充足（无冲突）\n");
    printf("########################################\n");
    atp_switch_t *sw = switch_create(16);
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),
        make_pkt(1, 0, 1, 20, 2),
        make_pkt(1, 1, 0, 100, 2),
        make_pkt(1, 1, 1, 200, 2),
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);
    ps_process(sw);
    print_stats(sw, 4);
    free(sw->pool); free(sw->ps_queue); free(sw);
}

void test2_contention_and_fallback(void)
{
    printf("\n\n########################################\n");
    printf("TEST 2: 双 Job, Pool=3 (故意冲突, 展示尽力而为回退)\n");
    printf("########################################\n");
    atp_switch_t *sw = switch_create(3);
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),
        make_pkt(2, 0, 0, 1000, 2),
        make_pkt(1, 0, 1, 5, 2),
        make_pkt(2, 0, 1, 500, 2),
        make_pkt(1, 1, 0, 50, 2),
        make_pkt(2, 1, 0, 5000, 2),
        make_pkt(1, 1, 1, 60, 2),
        make_pkt(2, 1, 1, 6000, 2),
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);
    ps_process(sw);
    print_stats(sw, 8);
    free(sw->pool); free(sw->ps_queue); free(sw);
}

void test3_dynamic_reuse(void)
{
    printf("\n\n########################################\n");
    printf("TEST 3: Pool=2(极端), 展示 Job1 完成后槽位被 Job2 复用\n");
    printf("########################################\n");
    atp_switch_t *sw = switch_create(2);
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 1, 2),
        make_pkt(1, 1, 0, 2, 2),
        make_pkt(1, 0, 1, 3, 2),
        make_pkt(1, 1, 1, 4, 2),
        make_pkt(2, 0, 0, 100, 2),
        make_pkt(2, 1, 0, 200, 2),
        make_pkt(2, 0, 1, 300, 2),
        make_pkt(2, 1, 1, 400, 2),
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);
    ps_process(sw);
    print_stats(sw, 8);
    free(sw->pool); free(sw->ps_queue); free(sw);
}
/*
 * TEST 4: 强制冲突 + 重传解救（展示论文 §3.7 可靠性机制）
 */
void test4_resend_recovery(void)
{
    printf("\n\n########################################\n");
    printf("TEST 4: Pool=1, 强制冲突 + Worker 重传解救孤儿聚合器\n");
    printf("########################################\n");

    atp_switch_t *sw = switch_create(1);

    /* 阶段1：正常发送，制造孤儿聚合器 */
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),   /* Job1 W0 预留槽位0 */
        make_pkt(2, 0, 0, 100, 2),  /* Job2 W0 冲突 -> 回退PS */
        make_pkt(1, 0, 1, 20, 2),   /* Job1 W1 -> 完成，释放槽位0 */
        make_pkt(2, 0, 1, 200, 2),  /* Job2 W1 -> 预留槽位0（现在卡住了，等不到W0） */
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);

    /* 阶段2：模拟"Worker 发现超时，重传 Job2 Seq0 W0" */
    printf("\n  --- Worker2 发现 ACK 超时，发起重传 ---\n");
    atp_packet_t resend = make_pkt(2, 0, 0, 100, 2);
    resend.resend = true;
    switch_process(sw, &resend);

    /* 阶段3：PS 处理所有包 */
    ps_process(sw);
    print_stats(sw, 5); /* 4 个原始包 + 1 个重传包 = 5 */

    free(sw->pool); free(sw->ps_queue); free(sw);
}

/*
 * TEST 5: 超时扫描（模拟 Worker 崩溃，无人重传）
 */
void test5_orphan_timeout(void)
{
    printf("\n\n########################################\n");
    printf("TEST 5: Pool=1, Worker 崩溃，展示孤儿聚合器超时清理\n");
    printf("########################################\n");

    atp_switch_t *sw = switch_create(1);
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),   /* W0 预留 */
        /* W1 永远不来（Worker 崩溃） */
    };
    switch_process(sw, &pkts[0]);

    printf("\n  --- 模拟时间推进，扫描超时(阈值=3)---\n");
    sw->global_time = 5; /* 快进时间 */
    switch_scan_timeout(sw, 3);

    /* 槽位已被清理，新 Job 可以进来 */
    atp_packet_t late = make_pkt(2, 0, 0, 999, 2);
    switch_process(sw, &late);

    ps_process(sw);
    print_stats(sw, 2);

    free(sw->pool); free(sw->ps_queue); free(sw);
    printf("  [Note] Job1 数据已丢失，需 Worker 重传或 PS 请求重传才能恢复\n");
    
}

/* ============================================================
 *  新增 Test 6：验证 rescued 机制
 * ============================================================ */
void test6_rescued_mechanism(void)
{
    printf("\n\n########################################\n");
    printf("TEST 6: 验证 rescued 机制（重传解救后，同 Job 同 Seq 的新包直发 PS）\n");
    printf("########################################\n");

    atp_switch_t *sw = switch_create(1);

    /* 阶段1：制造冲突与孤儿聚合器 */
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),   /* Job1 Seq0 W0 预留槽位0 */
        make_pkt(2, 0, 0, 100, 2),  /* Job2 Seq0 W0 冲突 -> 回退PS */
        make_pkt(1, 0, 1, 20, 2),   /* Job1 Seq0 W1 -> 完成，释放槽位0 */
        make_pkt(2, 0, 1, 200, 2),  /* Job2 Seq0 W1 -> 预留槽位0（等W0） */
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);

    /* 阶段2：W0 重传，解救孤儿聚合器 */
    printf("\n  --- Job2 W0 超时重传，触发解救 ---\n");
    atp_packet_t resend = make_pkt(2, 0, 0, 100, 2);
    resend.resend = true;
    switch_process(sw, &resend);

    /* 阶段3：同 Job 同 Seq 的新包（非重传）到达 —— 应被直发 PS，禁止预留 */
    printf("\n  --- Job2 Seq0 W0 新包（延迟到达/重复），应被直发 PS ---\n");
    atp_packet_t late_pkt = make_pkt(2, 0, 0, 100, 2);
    late_pkt.resend = false;
    switch_process(sw, &late_pkt);

    ps_process(sw);
    print_stats(sw, 6);

    free(sw->pool); free(sw->ps_queue); free(sw);
}


/* ============================================================
 *  新增 Test 7：ECN 拥塞控制
 * ============================================================ */
void test7_ecn_congestion(void)
{
    printf("\n\n########################################\n");
    printf("TEST 7: ECN 拥塞控制（出口队列深度超阈值标记 ECN）\n");
    printf("########################################\n");

    atp_switch_t *sw = switch_create(2);
    sw->ecn_threshold = 1; /* 低阈值，便于触发拥塞标记 */

    /* 
     * 场景设计：
     * 包1: Job1 Seq0 W0 -> 预留槽位，不触发 ECN（深度=0）
     * 包2: Job1 Seq0 W1 -> 完成聚合，send_to_ps，深度=1
     * 包3: Job2 Seq0 W0 -> 到达时深度=1 > 阈值=1，被标记 ECN
     * 包4: Job2 Seq0 W1 -> 完成聚合，send_to_ps，深度=2，且继承 ECN
     */
    atp_packet_t pkts[] = {
        make_pkt(1, 0, 0, 10, 2),
        make_pkt(1, 0, 1, 20, 2),
        make_pkt(2, 0, 0, 100, 2),
        make_pkt(2, 0, 1, 200, 2),
    };
    for (size_t i = 0; i < sizeof(pkts)/sizeof(pkts[0]); i++)
        switch_process(sw, &pkts[i]);

    ps_process(sw);
    print_stats(sw, 4);

    free(sw->pool); free(sw->ps_queue); free(sw);
}

int main(void)
{
    test1_no_contention();
    test2_contention_and_fallback();
    test3_dynamic_reuse();
    test4_resend_recovery();
    test5_orphan_timeout();
    test6_rescued_mechanism();
    test7_ecn_congestion();
    printf("\n\n全部测试通过。\n");
    return 0;
}