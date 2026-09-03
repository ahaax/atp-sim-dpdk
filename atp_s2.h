/* atp_s2.h —— On-Host Closure 函数实现 */
#ifndef ATP_S2_H
#define ATP_S2_H

static void ovf_push(atp_switch_t *sw, atp_packet_t *pkt)
{
    overflow_ring_t *r = &sw->ovf_ring;
    if (r->count >= r->cap) {
        atp_packet_t old = r->ring[r->head];
        r->head = (r->head + 1) % r->cap;
        r->count--;
        send_to_ps(sw, &old);
        sw->fallback++;
        printf("  [Switch] 溢出环满，挤出旧包 Job%dSeq%d 直发 PS\n",
               old.job_id, old.seq);
    }
    r->ring[r->tail] = *pkt;
    r->tail = (r->tail + 1) % r->cap;
    r->count++;
}

static void slot_flush(atp_switch_t *sw)
{
    while (sw->ovf_ring.count > 0) {
        atp_packet_t *front = &sw->ovf_ring.ring[sw->ovf_ring.head];
        uint16_t idx = hash_idx(front->job_id, front->seq, sw->pool_size);
        agg_slot_t *slot = &sw->pool[idx];
        uint32_t sender_bit = (front->edgeSwitchIdentifier == 0)
                              ? front->bitmap0 : front->bitmap1;
        bool progressed = false;

        if (slot->job_id == 0 && !is_rescued(sw, front->job_id, front->seq)) {
            slot->job_id = front->job_id;
            slot->seq = front->seq;
            slot->sum = front->data;
            slot->bitmap = sender_bit;
            slot->count = front->count;
            slot->timestamp = sw->global_time;
            progressed = true;
        }
        else if (slot->job_id == front->job_id && slot->seq == front->seq) {
            if (!(slot->bitmap & sender_bit)) {
                slot->sum += front->data;
                slot->bitmap |= sender_bit;
                slot->count += front->count;
                slot->timestamp = sw->global_time;
                progressed = true;
            } else {
                progressed = true;
                sw->consumed++;
            }
        }

        if (!progressed) break;

        sw->ovf_ring.head = (sw->ovf_ring.head + 1) % sw->ovf_ring.cap;
        sw->ovf_ring.count--;

        uint32_t b = slot->bitmap;
        uint8_t popcnt = 0;
        while (b) { popcnt++; b &= b - 1; }
        uint8_t need = (front->edgeSwitchIdentifier == 0)
                       ? front->fanInDegree0 : front->fanInDegree1;

        if (popcnt >= need) {
            atp_packet_t result = {
                .job_id = front->job_id, .seq = front->seq,
                .worker_id = 0xFF, .data = slot->sum,
                .fan_in = front->fan_in, .collision = false,
                .from_switch = true, .bitmap = slot->bitmap,
                .count = slot->count, .ecn = slot->ecn,
                .edgeSwitchIdentifier = 1,
                .bitmap0 = front->bitmap0,
                .bitmap1 = (1u << sw->switch_id),
                .fanInDegree0 = front->fanInDegree0,
                .fanInDegree1 = front->fanInDegree1
            };
            send_to_ps(sw, &result);
            memset(slot, 0, sizeof(*slot));
            sw->completed++;
            printf("  [Switch] 溢出环重试完成 Job%dSeq%d -> PS\n",
                   front->job_id, front->seq);
        } else {
            printf("  [Switch] 溢出环重试预留 Job%dSeq%d (%d/%d)\n",
                   front->job_id, front->seq, popcnt, need);
            break;
        }
    }
}

#endif /* ATP_S2_H */