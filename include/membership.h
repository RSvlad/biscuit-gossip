/*
 * membership.h - membership table (spec section 10, decisions D1, D14, D15, D18).
 *
 * Pure module: no network, no clock.  All time arrives as a `now_ms` parameter,
 * so unit tests never sleep.  D1: `n->self` is NOT part of `n->table[]`; the
 * table holds only the other members, sorted by node_id (capacity MAX_NODES - 1).
 * "View of the network" (list, send sample) = self + table.
 * Everything goes through `node_t *`; there is no global state and no malloc.
 */
#ifndef MEMBERSHIP_H
#define MEMBERSHIP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

/* Result of mem_upsert() (merge rule, issue #7). */
enum {
    MEM_REJECTED = -1, /* unknown member and the table is full (D14) */
    MEM_IGNORED = 0,   /* nothing changed */
    MEM_ADDED = 1,     /* new member inserted */
    MEM_UPDATED = 2,   /* progress (incarnation, heartbeat) accepted */
    MEM_LEFT = 3       /* member is now STATE_LEFT */
};

/* Zeroes the table, copies `self` (D1).  Does not touch cfg/stats/socket. */
void mem_init(node_t *n, const member_t *self);

/* Binary search by node_id.  Returns the table entry or NULL.  Never finds self. */
member_t *mem_find(node_t *n, uint32_t id);

/*
 * Merge of one received record (spec 7.2, D3).
 * STUB until #7: only inserts an unknown member (state ALIVE, or DEAD/LEFT as
 * received, last_update_ms = now_ms); records about self or about known members
 * are ignored.  #7 replaces this with the full rule table.
 */
int mem_upsert(node_t *n, const member_t *e, uint64_t now_ms);

/* extension */
/*
 * Adds a member that is not in the table yet, keeping the table sorted.
 * The caller sets e->last_update_ms.  Returns 0, or -1 when
 * the id is already present or equals self (D1), or when the table is full and
 * nothing can be evicted.  Full table (D14): the DEAD/LEFT member with the
 * smallest last_update_ms is evicted first; if there is none, the new record is
 * dropped with a WARN and stats.dropped_table_full is incremented.
 */
int mem_insert(node_t *n, const member_t *e);

/* Removes a member by id (shifts the array down).  An unknown id is not an error. */
void mem_remove(node_t *n, uint32_t id);

/*
 * Sample for sending (D15).  out[0] is always self.  If the others fit into
 * max - 1 slots, all of them are copied (table order); otherwise a random
 * subset (partial Fisher-Yates on a stack index array) with priority for
 * non-DEAD members.  LEFT members are included (so that LEAVE spreads on).
 * include_dead == false leaves DEAD members out entirely.  max is clamped to
 * MAX_ENTRIES_MSG.  No duplicates.  Returns the number of records written
 * (0 if max == 0).
 */
size_t mem_sample(const node_t *n, member_t *out, size_t max, bool include_dead);

/*
 * Picks up to k gossip targets: random, without repetition, never self, only
 * ALIVE or SUSPECT (SUSPECT stays a candidate so that a falsely suspected node
 * can recover).  With fewer than k candidates, all of them are returned.
 */
size_t mem_pick_peers(const node_t *n, member_t *out, size_t k);

/* ---- helpers ----------------------------------------------------------------- */

/* extension */
const char *state_name(uint8_t state); /* "ALIVE", "SUSPECT", "DEAD", "LEFT"; unknown -> "?" */
/* extension */
const char *role_name(uint8_t role);   /* "MIXER" ... "MONITOR"; unknown -> "?" */

/* extension */
/*
 * The single place that logs a state change (D18), at INFO:
 *   member 4 OVEN: ALIVE -> SUSPECT (silence 5.2s)
 * from/to are NEW, ALIVE, SUSPECT, DEAD, LEFT or REMOVED.
 */
void mem_log_transition(const member_t *m, const char *from, const char *to, uint64_t silence_ms);

#endif /* MEMBERSHIP_H */
