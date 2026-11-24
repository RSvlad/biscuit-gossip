/*
 * failure.h - failure detector (spec 7.3, F5; decisions D3, D4, D10, D11, D18).
 *
 * Time-driven state transitions measured ONLY by the local silence
 * `now - last_update_ms`.  Pure function over node_t with injected time: the unit
 * test does not sleep.  Recovery (SUSPECT -> ALIVE, DEAD -> ALIVE with a new
 * incarnation) is NOT here: it is part of mem_upsert (#7).
 */
#ifndef FAILURE_H
#define FAILURE_H

#include <stdint.h>

#include "common.h"

/*
 * Checks every member of n->table[] (never self):
 *   ALIVE   and silence > cfg.suspect_ms -> SUSPECT
 *   SUSPECT and silence > cfg.fail_ms    -> DEAD
 *   DEAD    and silence > cfg.cleanup_ms -> removed from the table (D4)
 *   LEFT    and silence > cfg.cleanup_ms -> removed from the table (D4)
 * Thresholds are strict (`>`).  If the silence already exceeds several of them
 * (e.g. after a long stall), all the transitions happen in one call and each one
 * is logged through mem_log_transition (D18).  The caller does not call this
 * while n->frozen (D10).  Thresholds are read from n->cfg; their order
 * (suspect < fail < cleanup) is validated at start-up (#12), not here.
 */
void failure_check(node_t *n, uint64_t now_ms);

#endif /* FAILURE_H */
