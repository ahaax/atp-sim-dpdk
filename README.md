# ATP 软件数据平面原型：第一阶段 C 仿真

当前版本：**第 2 次修改，第一阶段协议仿真验收版**。修改记录追加在 `../record.md`，讨论记录追加在 `../manu.md`。

目标是后续迁移到 DPDK 可编程软件数据平面。当前程序可以模拟 Worker、两级交换机、PS 和返回 ACK，但没有真实网卡 I/O，也不能据此宣称线速、多核或训练加速。

## 文件和目录说明

```text
atp-sim-dpdk/
  atp_sim.c                 交换机协议核、端点和事件仿真、演示入口
  atp_sim.h                 公共类型、上限、接口声明
  atp_wire.c                显式大端编码/解码、量化输入检查
  README.md                 本说明
  tests/
    test_atp.c              原有回归测试和公共检查函数
    phase1_cases.inc        本次新增验收场景，被 test_atp.c 包含
    run_tests.py           编译、测试、静态分析和日志保存
  test-atp/atp_sim.c        用户的历史版本，不参与本版构建
../work/
  modification-001/         第 1 次原版备份、差异、执行日志
  modification-002/
    before/                 本次修改前源码、测试和记录的快照
    verification/<时间>/    各次运行生成的程序、目标文件、日志
    ...                     差异、哈希清单和记录生成脚本
../record.md                修改日志，按修改次数追加
../manu.md                  讨论日志，按讨论次数追加
```

`.h` 是供 C 文件共享的声明；`.c` 会被编译；`.inc` 是包含进测试源文件的测试片段，不单独编译。Python 只负责自动化，不参与 ATP 协议运行。`work/` 中的 exe、o 文件是构建产物；备份、diff、日志用于追溯，保留不覆盖。

## 运行方式

在 `atp-sim-dpdk` 目录执行：

```sh
gcc -std=c11 -O2 -Wall -Wextra -Werror atp_sim.c atp_wire.c -lm -o atp_sim
./atp_sim
python tests/run_tests.py
```

Windows 下可输出/运行 `atp_sim.exe`。`-lm` 用于量化函数的数学运算。测试脚本需要 GCC 和 Python 3，不需要第三方 Python 包。

脚本分别构建 Debug（`-O0 -g`）、Release（`-O2 -DNDEBUG`），启用严格告警，执行 160 个场景并运行 GCC `-fanalyzer`。每次创建带 UTC 时间的新目录，避免覆盖历史日志。`CHECK` 不受 NDEBUG 影响，遇到错误返回非零退出码并打印位置。

## 测试在检查什么

测试从所有参与 Worker 的原始整数向量直接求参考和，逐元素比较 PS 以及每个 Worker 实际收到的参数。还检查完整成员集合、应用只交付一次、in-flight 归零、队列排空及槽位释放。

160 个场景中包括 120 组可复现的随机故障运行；另有确定性路径丢包、部分重叠、重复 ACK、快速重传、异构成员、碰撞重映射、数值边界、序列号边界等。长期回收用例在不重建 session 的情况下处理 1,025 个片段。解码器检查逐字节截断、非法字段，并接收 10,000 个随机字节缓冲区作为健壮性冒烟检查。

这些是功能回归和静态分析，不是形式化验证、模糊测试覆盖率证明，也没有真实 DPDK 性能含义。

## 协议核与仿真器的边界

`atp_switch_process(sw, packet, now, congestion, output)` 只处理状态和协议动作，返回 CONSUMED、FORWARD 或 INVALID。包处理路径不分配内存、不读取 PS、不自行取时钟或操作网卡。同一上下文必须由单个所有者串行处理。

正常聚合保持槽位到 ACK；L1 重传解救部分和；L2 重传丢弃旧状态并转发；PS 持久去重和缓存结果；Worker 保留原始输入直到确认和显式回收。全局 Worker bitmap 全程不改变含义，完成条件是 `seen == members`。

事件表、端点数组及控制屏障属于仿真器。每个模拟链路都执行 `atp_packet_encode` / `atp_packet_decode`，但不创建 Ethernet/IP/UDP 头或模拟 DMA。

## 本次补齐的机制

1. **逐流端点状态**：流由 job、iteration、tensor 标识，各流、各 Worker 独立维护窗口、慢启动阈值、在途量、加性增长信用及减窗抑制期。每作业可选择不同 Worker 成员集合和量化 scale；物理 rack_of 映射由 session 固定。
2. **快速重传**：最早的已发送未确认片段之后，已有三个不同片段被 ACK 时触发一次快速重传。重复 ACK 和其他流的 ACK 不计数。快速重传丢失后仍可由 RTO 恢复。
3. **拥塞反馈**：ECN 经聚合、PS 和 ACK 到 Worker；按已确认连续前缀发放增长信用；慢启动逐 ACK 增长，拥塞避免按窗口累计增长；一个恢复周期内避免对同一拥塞突发反复减窗。参数可配置，默认窗口 4、阈值 16、步长 1、RTO 80 ticks，适合小规模功能测试，不是 ATP 原论文的 100Gbps 参数。
4. **版本化碰撞反馈**：PS 根据 collision 提出新 seed/version，随结果 ACK 返回。所有成员收到最新建议后提交；以后注册的新片段使用新版本。在途片段和重传始终使用提交时的不可变映射描述，防止 Worker 在不同时间改变同一片段的位置。旧 ACK 不回退版本。此处是 ATP 思想的软件变体，不是原论文逐索引表的逐行复现。
5. **长期状态回收**：调用 `atp_sim_retire(sim, job, through_iteration)`，只有受影响片段全部成功确认、无相关在途事件和槽位时才成功。先向端点和交换机安装永久退休迭代下界，再回收原始输入、PS 结果、流状态和墓碑；任意迟到的旧 DATA/ACK 都被拒绝。一次最多 64 个活动/待回收片段，但累计可以超过 64。
6. **失败回收**：`atp_sim_abort` 用于明确接受已终止的失败并回收；不会取消仍在运行的片段，也不能把失败伪装为成功。`aborted_fragments` 单独累计。应用需先读取/保存结果或错误，然后调用 retire/abort。
7. **报文和数值契约**：每片段 1—16 个元素，int32 输入，int64 精确求和，支持正整数 scale。`atp_quantize` 检查浮点输入，按最近值、半值远离零取整；非有限值或超出 int32 范围时返回 false 且不写入部分输出。PS 结果除以 scale 得到量化域结果；这不等于未经量化的精确浮点和。

## 外部调用规则

- `atp_sim_create(workers, racks, pool_size)`：建立物理仿真环境，最多 32 Worker、4 个机架。
- `atp_sim_configure_job`：预配置作业成员 mask 和 scale。若未配置，第一次注册片段时使用全部 Worker、scale=1。已有 job 的成员/scale 不允许原地改变；用新 job 标识建立新实例。
- `atp_sim_add_vector`：注册不可变片段描述；同一流内 seq 必须严格递增。同一个 job/iteration/tensor/seq 不可复用。`atp_sim_add` 是长度 16 的便利接口。
- `atp_sim_run`：推进虚拟时间；true 表示本次存留片段全部成功、ACK 完成且队列/槽位清理；false 可能是给定 ticks 不足或已终止失败，查 fragment.failed 区分。
- 应用检查 `fragments[]` 的结果后，再调用 retire/abort；回收会压缩数组，使原片段指针/下标失效。
- iteration 的退休下界只增加，不回绕。同一 job 用尽 uint32 iteration 时要换作业实例标识。最多配置 16 个 job；这是部署配置预算，不能无限注册新 job。
- 主动回收前必须确认相应结果已被应用保存。控制屏障在本 C 仿真中是原子的；部署到多机器时需可靠控制消息与确认后再释放内存，不能直接照搬同时修改所有上下文的函数。

## 字节格式：ATP-sim v1

这是本原型的 UDP payload 候选格式，不与原 ATP/P4 报文互通。所有多字节整数使用大端，禁止直接发送 C struct。

| 偏移 | 字节数 | 内容 |
|---:|---:|---|
| 0 | 4 | magic，ASCII ATP1 |
| 4 | 1 | version=1 |
| 5 | 1 | type，DATA=0 / ACK=1 |
| 6 | 1 | flags：bit0 resend、bit1 collision、bit2 ECN、bit3 bypass；其余须为 0 |
| 7 | 1 | 元素数，1—16 |
| 8、12、16、20 | 各 4 | job、iteration、tensor、seq |
| 24 | 4 | 全局 contributors |
| 28、32 | 各 4 | 此片段固定 route_seed、route_version |
| 36、40 | 各 4 | ACK 推荐的 next_seed、next_version；DATA 须为 0 |
| 44 | 4 | 正整数 scale |
| 48 | 8 × 元素数 | 有符号 int64 部分和/结果 |

解码检查总长度精确匹配、magic/version/type/flags、元素数、scale、贡献数及数值范围；接收协议上下文继续检查作业成员、退休下界、固定映射和长度契约。网络完整性校验、来源认证及外层协议属于适配层，不由本 payload 编解码器提供。

## 第一阶段验收边界

在**有界活动窗口、每作业固定成员、显式受控回收、整数/定点向量**模型下，第一阶段的可靠闭环、拥塞反馈、快速重传、碰撞反馈与状态生命周期均已实现并有回归验证。它不是原 ATP ASIC 实现的逐行复刻。

第一阶段有意采用的软件设计是：全局贡献集合、宽累加器、不可变片段映射版本、退休迭代下界。它们目前是工程正确性设计；单独使用这些常见技术不能声称论文创新。后续 CPU 成本/收益感知准入仍需文献查重与性能实验。

下一阶段才实现 DPDK 的 mbuf 所有权、RX/TX 短发、Ethernet/IP/UDP、真实拥塞信号、NUMA、跨核分派以及分布式控制屏障。当前队列和超时使用有界扫描，不能当作高性能实现；永久失联报告失败，任意无限丢包不保证完成。
