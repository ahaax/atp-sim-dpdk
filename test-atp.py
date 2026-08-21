class ATPSwitch:
    def __init__(self, pool_size):
        self.pool = AggregatorPool(pool_size)
        self.to_ps = []  # 模拟"发往 PS 的链路"，用列表代替网络队列
    
    def process(self, pkt):
        """处理一个 Worker 发来的梯度包"""
        idx = self.pool.hash(pkt.job_id, pkt.seq)
        slot = self.pool.slots[idx]
        
        # ==================== 路径 A：柜子是空的 -> 预留 ====================
        if slot is None:
            self.pool.slots[idx] = {
                'job_id': pkt.job_id,
                'seq': pkt.seq,
                'sum': pkt.data,               # 第一个值先存进来
                'bitmap': 1 << pkt.worker_id,  # 用位图记录谁到了
                'count': 1
            }
            print(f"  [Switch] 槽位{idx:2d} 预留: Job{pkt.job_id} Seq{pkt.seq} Worker{pkt.worker_id}")
            return  # 包被交换机"吃掉"，不进入 to_ps，不占用出口带宽
        
        # ==================== 路径 B：柜子是自己的 -> 累加 ====================
        if slot['job_id'] == pkt.job_id and slot['seq'] == pkt.seq:
            # 防重复：这个 Worker 是不是已经来过了？
            if slot['bitmap'] & (1 << pkt.worker_id):
                print(f"  [Switch] 槽位{idx:2d} 重复包，丢弃")
                return
            
            slot['sum'] += pkt.data
            slot['bitmap'] |= (1 << pkt.worker_id)
            slot['count'] += 1
            
            # 检查：人齐了吗？
            if slot['count'] >= pkt.fan_in:
                # 凑齐了！把聚合结果发给 PS，然后清空柜子
                result = ATPPacket(pkt.job_id, pkt.seq, -1, slot['sum'], pkt.fan_in)
                self.to_ps.append(result)
                print(f"  [Switch] 槽位{idx:2d} 完成: Job{pkt.job_id} Seq{pkt.seq} 值={slot['sum']}")
                self.pool.slots[idx] = None  # 释放柜子（简化：假设 ACK 瞬间到）
            else:
                print(f"  [Switch] 槽位{idx:2d} 累加: {slot['count']}/{pkt.fan_in}")
            return
        
        # ==================== 路径 C：柜子被别的 Job 占了 -> 尽力而为回退！ ====================
        pkt.collision = True
        self.to_ps.append(pkt)
        print(f"  [Switch] 槽位{idx:2d} 冲突! 被Job{slot['job_id']}占用，"
              f"Job{pkt.job_id}Seq{pkt.seq}Worker{pkt.worker_id} -> 回退PS")