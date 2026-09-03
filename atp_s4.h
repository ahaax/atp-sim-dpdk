/* atp_s4.h —— Theseus 热切换策略评估 */
#ifndef ATP_S4_H
#define ATP_S4_H

/* 这些变量在 atp_sim.c 中定义，此处仅声明 */
extern agg_policy_t g_policy_id;
extern const policy_params_t POLICY_TABLE[POLICY_MAX];
/*
 * 根据平滑的出口队列深度（EWMA）、阈值和驻留/滞后规则，
 * 动态选择下一个 Theseus聚合策略（FULL_AGG / EARLY_RELEASE / BYPASS），
 * 并做一步步的安全迁移以防抖动。
 */
static agg_policy_t evaluate_policy(atp_switch_t *sw)
{
    //把瞬时队列深度平滑为长期度量。
    sw->qdepth_ewma = (7 * sw->qdepth_ewma + sw->egress_queue_depth) / 8;
    //最小驻留（dwell time）保护： 禁止频繁切换
    if (sw->global_time - sw->last_switch_time < sw->dwell_time)
        return g_policy_id;

    agg_policy_t cur = g_policy_id;
    uint32_t q = sw->qdepth_ewma;
    uint32_t thr = sw->ecn_threshold;
    agg_policy_t nxt = cur;

    if (cur == POLICY_FULL_AGG) {
        if (q > thr * 4)       nxt = POLICY_BYPASS;// ①拥塞严重，直接跳到 BYPASS
        else if (q > thr)      nxt = POLICY_EARLY_RELEASE;// ②轻度拥塞，切换到 EARLY_RELEASE
    } else if (cur == POLICY_EARLY_RELEASE) {
        if (q > thr * 4)       nxt = POLICY_BYPASS;
        else if (q < thr / 2)  nxt = POLICY_FULL_AGG;//负载回落
    } else {
        if (q < thr)           nxt = POLICY_EARLY_RELEASE;
    }

    if (nxt != cur && abs((int)nxt - (int)cur) > 1) {
        nxt = (nxt > cur) ? (agg_policy_t)(cur + 1) : (agg_policy_t)(cur - 1);
    }

    return nxt;
}

#endif /* ATP_S4_H */