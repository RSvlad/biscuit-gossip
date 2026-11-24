#include "membership.h"

#include <string.h>

#include "util.h"

#define TABLE_CAP (MAX_NODES - 1) /* D1: self is kept outside the table */

/* First index whose node_id is >= id (the insertion point when id is absent). */
static size_t lower_bound(const node_t *n, uint32_t id)
{
    size_t lo = 0, hi = n->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (n->table[mid].node_id < id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void remove_at(node_t *n, size_t idx)
{
    memmove(&n->table[idx], &n->table[idx + 1], (n->count - idx - 1) * sizeof n->table[0]);
    n->count--;
    memset(&n->table[n->count], 0, sizeof n->table[0]); /* no stale record past count */
}

void mem_init(node_t *n, const member_t *self)
{
    memset(n->table, 0, sizeof n->table);
    n->count = 0;
    n->self = *self;
}

member_t *mem_find(node_t *n, uint32_t id)
{
    size_t i = lower_bound(n, id);
    if (i < n->count && n->table[i].node_id == id)
        return &n->table[i];
    return NULL;
}

int mem_insert(node_t *n, const member_t *e)
{
    if (e->node_id == n->self.node_id)
        return -1; /* D1: self never lives in the table */

    size_t pos = lower_bound(n, e->node_id);
    if (pos < n->count && n->table[pos].node_id == e->node_id)
        return -1; /* already present */

    if (n->count >= TABLE_CAP) { /* D14 */
        size_t victim = n->count;
        for (size_t i = 0; i < n->count; i++) {
            uint8_t st = n->table[i].state;
            if ((st == STATE_DEAD || st == STATE_LEFT) &&
                (victim == n->count || n->table[i].last_update_ms < n->table[victim].last_update_ms))
                victim = i;
        }
        if (victim == n->count) {
            LOG_W("membership: table full (%zu members), dropping record of node %u", n->count,
                  (unsigned)e->node_id);
            n->stats.dropped_table_full++;
            return -1;
        }
        const member_t *v = &n->table[victim];
        uint64_t silence = e->last_update_ms >= v->last_update_ms ? e->last_update_ms - v->last_update_ms : 0;
        mem_log_transition(v, state_name(v->state), "REMOVED", silence);
        remove_at(n, victim);
        pos = lower_bound(n, e->node_id); /* indices may have shifted */
    }

    memmove(&n->table[pos + 1], &n->table[pos], (n->count - pos) * sizeof n->table[0]);
    n->table[pos] = *e;
    n->count++;
    return 0;
}

void mem_remove(node_t *n, uint32_t id)
{
    size_t i = lower_bound(n, id);
    if (i < n->count && n->table[i].node_id == id)
        remove_at(n, i);
}

/* STUB until #7: the full merge rule replaces this. */
int mem_upsert(node_t *n, const member_t *e, uint64_t now_ms)
{
    if (e->node_id == n->self.node_id || mem_find(n, e->node_id) != NULL)
        return MEM_IGNORED;

    member_t m = *e;
    if (m.state != STATE_DEAD && m.state != STATE_LEFT)
        m.state = STATE_ALIVE;
    m.last_update_ms = now_ms;
    if (mem_insert(n, &m) != 0)
        return MEM_REJECTED;
    mem_log_transition(&m, "NEW", state_name(m.state), 0);
    return MEM_ADDED;
}

/*
 * Copies up to `room` of the table entries listed in idx[0..cnt) into out.  If
 * they all fit, table order is kept; otherwise a random subset is chosen with a
 * partial Fisher-Yates shuffle (idx is permuted in place).  Returns the count.
 */
static size_t take(const node_t *n, size_t *idx, size_t cnt, size_t room, member_t *out)
{
    size_t k = cnt <= room ? cnt : room;
    if (cnt > room) {
        for (size_t i = 0; i < k; i++) {
            size_t j = i + rng_below((uint32_t)(cnt - i));
            size_t tmp = idx[i];
            idx[i] = idx[j];
            idx[j] = tmp;
        }
    }
    for (size_t i = 0; i < k; i++)
        out[i] = n->table[idx[i]];
    return k;
}

size_t mem_sample(const node_t *n, member_t *out, size_t max, bool include_dead)
{
    if (max == 0)
        return 0;
    if (max > MAX_ENTRIES_MSG)
        max = MAX_ENTRIES_MSG;

    out[0] = n->self;
    size_t written = 1;

    size_t first[MAX_NODES], last[MAX_NODES]; /* non-DEAD first, DEAD only as filler */
    size_t nfirst = 0, nlast = 0;
    for (size_t i = 0; i < n->count; i++) {
        if (n->table[i].state != STATE_DEAD)
            first[nfirst++] = i;
        else if (include_dead)
            last[nlast++] = i;
    }

    written += take(n, first, nfirst, max - written, out + written);
    written += take(n, last, nlast, max - written, out + written);
    return written;
}

size_t mem_pick_peers(const node_t *n, member_t *out, size_t k)
{
    size_t cand[MAX_NODES];
    size_t nc = 0;
    for (size_t i = 0; i < n->count; i++) {
        if (n->table[i].state == STATE_ALIVE || n->table[i].state == STATE_SUSPECT)
            cand[nc++] = i;
    }
    return take(n, cand, nc, k, out);
}

const char *state_name(uint8_t state)
{
    static const char *const names[] = {"ALIVE", "SUSPECT", "DEAD", "LEFT"};
    return state < sizeof names / sizeof names[0] ? names[state] : "?";
}

const char *role_name(uint8_t role)
{
    static const char *const names[] = {"MIXER", "ROLLER", "CUTTER", "OVEN",
                                        "COOLER", "PACKER", "MONITOR"};
    return role < sizeof names / sizeof names[0] ? names[role] : "?";
}

void mem_log_transition(const member_t *m, const char *from, const char *to, uint64_t silence_ms)
{
    LOG_I("member %u %s: %s -> %s (silence %.1fs)", (unsigned)m->node_id, role_name(m->role), from,
          to, (double)silence_ms / 1000.0);
}
