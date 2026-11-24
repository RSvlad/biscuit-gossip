/*
 * failure.c - failure detector (issue #10, spec 7.3).  See failure.h.
 * No now_ms()/time() here: time arrives only through the parameter.
 */
#include "failure.h"

#include "membership.h"

/* Guarded: last_update_ms <= now_ms always holds, but never underflow. */
static uint64_t silence_of(const member_t *m, uint64_t now_ms)
{
    return now_ms >= m->last_update_ms ? now_ms - m->last_update_ms : 0;
}

void failure_check(node_t *n, uint64_t now_ms)
{
    /*
     * Walk backwards: mem_remove shifts the tail down, so the members that are
     * still to be visited (lower indices) never move and none is skipped.
     */
    for (size_t i = n->count; i-- > 0;) {
        member_t *m = &n->table[i];
        uint64_t silence = silence_of(m, now_ms);

        if (m->state == STATE_ALIVE && silence > n->cfg.suspect_ms) {
            mem_log_transition(m, "ALIVE", "SUSPECT", silence);
            m->state = STATE_SUSPECT;
        }
        if (m->state == STATE_SUSPECT && silence > n->cfg.fail_ms) {
            mem_log_transition(m, "SUSPECT", "DEAD", silence);
            m->state = STATE_DEAD;
        }
        if ((m->state == STATE_DEAD || m->state == STATE_LEFT) && silence > n->cfg.cleanup_ms) {
            mem_log_transition(m, state_name(m->state), "REMOVED", silence);
            mem_remove(n, m->node_id);
        }
    }
}
