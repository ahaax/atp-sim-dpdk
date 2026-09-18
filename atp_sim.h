/* 阅读约定：Worker 为训练发送端，PS 为参数服务器；所有成员集合均使用全局 Worker 位图。
 * tick 是仿真时间单位，不是毫秒；报文字节格式由 atp_wire.c 显式编码。
 * 本次仅添加中文注释和排版，不改变任何非注释代码标记。 */
#ifndef ATP_SIM_H
#define ATP_SIM_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>


#define ATP_WORKERS 32u /* Worker 位图最多 32 位 */
#define ATP_RACKS 4u /* 最大机架数 */
#define ATP_VALUES 16u /* 单分片最多向量元素数 */
#define ATP_JOBS 16u /* 最大作业数 */
#define ATP_FLOWS 64u /* 最大活跃流数 */
#define ATP_WIRE_HEADER 48u /* v1 报文固定头部字节数 */
#define ATP_WIRE_MAX (ATP_WIRE_HEADER + 8u * ATP_VALUES) /* 最大编码字节数：头部加 int64 向量 */
#define ATP_FRAGMENTS 64u /* 同时保存的分片上限，也是墓碑表容量 */
#define ATP_EVENTS 4096u /* 待投递事件表容量 */
/* 分片唯一标识；不同作业、迭代或张量不能混合聚合。 */

typedef struct {
    uint32_t job, /* 作业编号，0 非法 */
             iteration, /* 训练迭代编号 */
             tensor, /* 张量编号 */
             seq; /* 张量内分片序号 */
} atp_key;

/* 整数输入：每个 Worker 提供一个向量。 */
typedef struct {
    int32_t value[ATP_WORKERS][ATP_VALUES]; /* 按 [Worker 编号][元素下标] 保存 int32 输入 */
} atp_input;
/* 量化前的实数输入，仅供主机侧预处理使用。 */   //⭐❓
typedef struct {
    double value[ATP_WORKERS][ATP_VALUES]; /* 按 [Worker 编号][元素下标] 保存实数输入 */
} atp_real_input;
typedef enum {
    ATP_DATA, /* 向 PS 发送的原始数据或部分和 */
    ATP_ACK /* 向 Worker 返回最终结果和反馈 */
} atp_type;
/* 协议的内存对象，不可直接作为网络字节布局发送。 */
typedef struct {
    atp_key key; /* 本包所属分片 */
    atp_type type; /* DATA 数据或 ACK 结果确认 */
    uint32_t contributors; /* 全局 Worker 贡献位图，第 w 位表示 Worker w；各层都不改为机架编号 */
    uint32_t scale, /* 实数转整数时的量化倍数，同一作业统一 */
             route_seed, /* 本分片冻结的槽映射种子 */
             route_version, /* 本分片冻结的映射版本 */
             next_seed, /* ACK 携带的后续映射种子建议 */
             next_version; /* ACK 携带的后续映射版本建议 */
    unsigned length; /* 有效向量元素数，范围 1..ATP_VALUES */
    int64_t value[ATP_VALUES]; /* DATA 的部分和或 ACK 的最终和，使用 int64 累加 */
    bool resend, /* 是否为重传数据 */
         collision, /* 路径上是否遇到槽冲突 */
         ecn, /* 路径上累计的拥塞标记 */
         bypass; /* 继续转发而不新建聚合状态 */
} atp_packet;

/* 交换机的一个聚合槽；发送聚合结果后仍保留到 ACK、重传或过期。 */
typedef struct {
    bool occupied, /* 槽是否占用 */
         result_sent; /* 本槽完整聚合结果是否已发出，不代表收到 ACK */
    atp_packet aggregate; /* 缓存的分片描述、贡献位图和部分和 */
    uint64_t last_progress; /* 最近一次增加新贡献的 tick，重复包不刷新 */
} atp_slot;

/* 作业成员和数值配置，以及拒绝旧迭代的回收标记。 */
typedef struct {
    uint32_t job, /* 作业编号 */
             members, /* 作业全部 Worker 的全局位图 */
             local, /* 本节点负责的成员位图，L1 是机架内成员，L2 是全部成员 */
             scale, /* 该作业统一的量化倍数 */
             retired_iteration; /* 已回收迭代的最大编号，包含此编号 */
    bool retired; /* 是否已启用 retired_iteration 检查 */
} atp_job;

/* 单个交换机状态；同一实例必须由单线程串行处理或外部同步。 */
typedef struct {
    atp_slot *slots; /* 动态分配的聚合槽数组 */
    uint32_t capacity, /* 槽数组长度 */
             expected, /* 未使用作业表时默认的本地成员位图 */
             job_members; /* 未使用作业表时默认的全局成员位图，供 ACK 校验 */
    unsigned level; /* 聚合层级：1 为 L1，2 为 L2 */
    atp_job jobs[ATP_JOBS]; /* 本节点的作业配置表 */
    unsigned job_count; /* 有效作业配置数量 */
    atp_key closed[ATP_FRAGMENTS]; /* 已关闭分片的墓碑表，阻止迟到包重新聚合 */
    unsigned closed_count; /* 已保存墓碑数量 */
    bool admission_disabled; /* 墓碑溢出后禁止新建聚合状态，仍可旁路转发 */
    uint64_t fallbacks, /* 槽被其他分片占用而旁路的次数 */
             duplicates, /* 普通聚合路径吸收的完整重复包数 */
             emitted, /* 完整聚合结果发出次数 */
             expired, /* 超时回收的槽数 */
             invalid; /* 交换机拒绝的非法包数 */
} atp_switch;

typedef enum {
    ATP_CONSUMED, /* 本地吸收，不发送 output */
    ATP_FORWARD, /* output 为待转发包 */
    ATP_INVALID /* 非法输入，拒绝处理 */
} atp_action;

/* 比较 job、iteration、tensor、seq 四个字段，判断是否为同一分片。 */
bool atp_key_equal(atp_key a, atp_key b);
/* 将分片标识映射到槽下标；capacity 为槽数，返回范围 [0, capacity)，容量为 0 时返回 0。 */
uint32_t atp_hash(atp_key key, uint32_t capacity);
/* 初始化交换机并分配 capacity 个槽；expected 是默认 Worker 位图，level 只能为 1 或 2；失败返回 false。 */
bool atp_switch_init(atp_switch *sw, uint32_t capacity, uint32_t expected,
                     unsigned level);
/* 释放槽内存并清零交换机；允许 sw 为 NULL。 */
void atp_switch_destroy(atp_switch *sw);
/* 处理一个输入包：now 为仿真时刻，congestion 为本次拥塞信号。
 * 返回 FORWARD 时 output/out 才可用于发送；CONSUMED 表示本地吸收，INVALID 表示拒绝。
 * 同一交换机状态须串行访问；此函数不收发网络包，也不读取真实时钟。 */
atp_action atp_switch_process(atp_switch *sw, const atp_packet *input,
                             uint64_t now, bool congestion, atp_packet *output);
/* 关闭超过 timeout 个 tick 没有新增贡献的槽；只回收状态，可靠恢复由 Worker 重传驱动。 */
void atp_switch_expire(atp_switch *sw, uint64_t now, uint64_t timeout);
/* 校验报文类型、成员位图、长度和数值边界；expected 是允许成员位图，ACK 必须覆盖全部成员。 */
bool atp_packet_valid(const atp_packet *packet, uint32_t expected);
/* 在 buffer 中编码显式大端字节格式；capacity 单位为字节；返回写入长度，失败返回 0。实现位于 atp_wire.c。 */
size_t atp_packet_encode(const atp_packet *packet, uint8_t *buffer, size_t capacity);
/* 从长度为 length 字节的 buffer 解码；失败返回 false 且不修改 packet。实现位于 atp_wire.c。 */
bool atp_packet_decode(atp_packet *packet, const uint8_t *buffer, size_t length);
/* 将 members 指定的 Worker 实数乘 scale 后四舍五入到 int32；length 为有效元素数，溢出/非有限值返回 false。 */
bool atp_quantize(atp_input *out, const atp_real_input *input, uint32_t members,
                  unsigned length, uint32_t scale);

/* 一个流中一个 Worker 的发送及拥塞控制状态；窗口单位是分片数。 */
typedef struct {
    unsigned window, /* 允许同时在途的分片数 */
             ssthresh, /* 慢启动转拥塞避免的窗口阈值 */
             inflight, /* 已首次发送但尚未确认、也未终止失败的分片数 */
             credit; /* 拥塞避免阶段累计的有效确认数 */
    uint32_t feedback_version, /* 此 Worker 已收到的最新映射建议版本 */
             feedback_seed; /* 对应建议的种子 */
    uint64_t recovery_until, /* 恢复期结束 tick，此前不重复减窗也不增窗 */
             reductions; /* 累计减窗次数 */
} atp_sender;
/* 由 job/iteration/tensor 标识的流；不同流独立维护发送窗口和映射反馈。 */
typedef struct {
    bool used; /* 流表项是否占用 */
    atp_key id; /* 流标识，仅比较前三个字段，忽略 seq */
    uint32_t last_seq, /* 最近成功提交的分片序号 */
             seed, /* 已共同确认、供新分片使用的种子 */
             version, /* 已共同确认的映射版本 */
             proposed_seed, /* PS 当前提出的映射种子 */
             proposed_version; /* PS 当前提出的映射版本 */
    bool has_seq; /* 是否提交过分片，避免把合法 seq=0 当作未初始化 */
    atp_sender worker[ATP_WORKERS]; /* 按全局 Worker 编号保存各自发送状态 */
} atp_flow;


typedef enum {
    ATP_TO_L1, /* DATA 到一级交换机 */
    ATP_TO_L2, /* DATA 到二级交换机 */
    ATP_TO_PS, /* DATA 到参数服务器 PS */
    ATP_ACK_L2, /* ACK 返回二级交换机 */
    ATP_ACK_L1, /* ACK 返回一级交换机 */
    ATP_ACK_WORKER, /* ACK 返回 Worker */
    ATP_DEST_COUNT /* 目的类型数量，用于数组边界，不是有效目的 */
} atp_dest;
/* 一个待投递的仿真网络事件，保存独立报文副本。 */
typedef struct {
    bool used; /* 事件表项是否占用 */
    atp_packet packet; /* 待投递报文副本 */
    atp_dest dest; /* 处理阶段/目的类型 */
    unsigned endpoint; /* L1 方向为机架编号，ACK_WORKER 为 Worker 编号，L2/PS 使用 0 */
    uint64_t due; /* 允许投递的最早 tick */
} atp_event;
/* 一个活跃分片的主机侧输入、PS 去重结果及所有 Worker 确认状态。 */
typedef struct {
    atp_key key; /* 完整分片标识 */
    uint32_t members, /* 本分片参与 Worker 位图 */
             scale, /* 提交时固定的量化倍数 */
             route_seed, /* 提交时固定的槽映射种子 */
             route_version, /* 提交时固定的映射版本 */
             next_seed, /* PS 完成时缓存的 ACK 种子建议 */
             next_version; /* PS 完成时缓存的 ACK 版本建议 */
    unsigned length, /* 本分片有效元素数 */
             flow; /* 所属 flows 数组下标 */
    int32_t input[ATP_WORKERS][ATP_VALUES]; /* 各 Worker 的原始整数输入，供首次发送和重传 */
    int64_t sum[ATP_VALUES], /* PS 已接受贡献的逐元素累计和 */
            received[ATP_WORKERS][ATP_VALUES]; /* 各 Worker 收到的最终结果，用于仿真验证 */
    uint32_t seen, /* PS 已累计的贡献位图 */
             sent, /* 已尝试首次发送的 Worker 位图 */
             acked; /* 已收到结果 ACK 的 Worker 位图 */
    unsigned attempts[ATP_WORKERS]; /* 各 Worker 累计尝试次数，包含首次发送 */
    uint64_t last_send[ATP_WORKERS]; /* 各 Worker 最近一次尝试发送的 tick */
    bool done, /* PS 已凑齐全部贡献，不代表所有 Worker 都收到 ACK */
         failed, /* 达到尝试上限后的终止失败标记 */
         ecn, /* PS 接收路径累计拥塞标记 */
         collision; /* PS 接收路径累计槽冲突标记 */
    unsigned deliveries; /* PS 首次完成结果的交付次数，正确执行应最多为 1 */
    uint32_t fast_requested, /* 等待执行快速重传的 Worker 位图 */
             fast_done, /* 已请求过一次快速重传的 Worker 位图 */
             cc_applied; /* 已将本分片 ACK 计入拥塞控制的 Worker 位图 */
} atp_fragment;

/* 主机侧离散事件仿真器，包含端系统和网络模型；不是实际 DPDK 收发循环。 */
typedef struct {
    atp_switch l1[ATP_RACKS], /* 各机架的一级聚合交换机 */
               l2; /* 汇聚各机架的二级聚合交换机 */
    unsigned workers, /* 实际 Worker 数 */
             racks, /* 实际机架数 */
             count; /* 当前保存的分片数，含尚未回收的完成项 */
    unsigned rack_of[ATP_WORKERS]; /* Worker 到机架编号的映射 */
    uint32_t expected; /* 所有物理 Worker 的位图，不等于每个作业的成员集 */
    atp_job jobs[ATP_JOBS]; /* 全局作业配置 */
    unsigned job_count; /* 有效作业数 */
    atp_flow flows[ATP_FLOWS]; /* 逐流发送与映射状态表 */
    atp_fragment fragments[ATP_FRAGMENTS]; /* 有界活跃分片表 */
    atp_event events[ATP_EVENTS]; /* 待投递事件表 */
    unsigned queue_count, /* 当前排队事件数 */
             queue_limit, /* 可配置的队列容量上限 */
             service_budget, /* 每 tick 最多处理的事件数 */
             ecn_threshold; /* 队列长度超过此值时标记拥塞 */
    unsigned drop_next[ATP_DEST_COUNT], /* 各目的阶段接下来强制丢弃的发送次数 */
             loss_per_mille, /* 随机丢包概率，千分数 */
             duplicate_per_mille; /* 随机复制包概率，千分数 */
    unsigned max_delay, /* 随机延迟范围参数；正值时延迟 1..max_delay，0 时仍延迟 1 tick */
             max_attempts, /* 每 Worker 每分片最大尝试次数，包含首次发送 */
             rng; /* 可复现伪随机状态 */
    unsigned initial_window, /* 新流各 Worker 初始窗口 */
             initial_ssthresh, /* 新流初始慢启动阈值 */
             ai_step; /* 窗口每次增长的分片数 */
    uint32_t offline; /* 模拟离线 Worker 的位图，仍推进其超时和尝试计数 */
    uint64_t now, /* 当前逻辑时刻，单位 tick */
             rto, /* 重传超时及减窗恢复期长度，单位 tick */
             slot_timeout, /* 槽无新增贡献的超时长度，单位 tick */
             tx_attempts, /* 通过发送校验及编解码后的逐跳发送尝试数 */
             drops, /* 故障注入或队列不足造成的丢包数 */
             retries, /* Worker 重传尝试数 */
             overlaps, /* PS 收到部分重叠贡献包的次数 */
             invalid; /* 仿真编解码或投递校验记录的非法包数 */
    uint64_t fast_retries, /* 已执行的快速重传次数 */
             rehashes, /* 所有成员确认后提交新映射的次数 */
             retired_fragments, /* 累计回收分片数，包含失败项 */
             aborted_fragments, /* 累计回收的终止失败分片数 */
             stale; /* 拒绝的已回收迭代迟到包数 */
} atp_sim;
/* 创建 workers 个 Worker、racks 个 L1 和一个 L2；每台交换机有 pool_size 个槽；失败返回 NULL。 */
atp_sim *atp_sim_create(unsigned workers, unsigned racks, uint32_t pool_size);
/* 释放仿真器及所有交换机槽；允许 sim 为 NULL。 */
void atp_sim_destroy(atp_sim *sim);
/* 以 ATP_VALUES 个元素提交分片，是 atp_sim_add_vector 的固定长度便捷入口。 */
bool atp_sim_add(atp_sim *sim, atp_key key, const atp_input *values);
/* 为作业配置全局成员位图 members 和量化倍数 scale，并下发各层本地成员；已有配置仅接受相同参数。 */
bool atp_sim_configure_job(atp_sim *sim, uint32_t job, uint32_t members, uint32_t scale);
/* 提交一个有效长度为 length 的分片并复制输入；同一流 seq 必须递增；容量/参数不合法返回 false。
 * 提交时冻结映射描述；后续 ACK 的映射反馈只影响以后提交的分片。 */
bool atp_sim_add_vector(atp_sim *sim, atp_key key, const atp_input *values, unsigned length);

/* 回收指定作业中 iteration <= through_iteration 的成功分片；要求全部 ACK 到达且相关事件/槽已排空。 */
bool atp_sim_retire(atp_sim *sim, uint32_t job, uint32_t through_iteration);

/* 显式回收终止失败分片（也可含已成功项）；不是取消正在执行的工作，同样要求相关事件/槽排空。 */
bool atp_sim_abort(atp_sim *sim, uint32_t job, uint32_t through_iteration);
/* 最多推进 ticks 个逻辑时钟步；全部成功且事件/槽排空才返回 true。
 * false 可能是参数错误、时间预算不足或终止失败，需结合 fragment.failed 等状态判断。 */
bool atp_sim_run(atp_sim *sim, uint64_t ticks);
/* 模拟一跳发送：校验、编解码、丢包/重复注入，再入队；true 仅表示原包成功入队，不保证交付。
 * endpoint：L1 方向为机架编号，ACK_WORKER 为 Worker 编号，L2/PS 调用处使用 0。 */
bool atp_sim_inject(atp_sim *sim, atp_dest dest, unsigned endpoint,
                    const atp_packet *packet);
#endif
