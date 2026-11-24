/*
 * test_merge.c - merge rule mem_upsert (issue #7, spec 7.2, decision D3).
 * All time is injected through `now_ms`; nothing here sleeps.
 */
#include <string.h>

#include "check.h"
#include "membership.h"
#include "util.h"

#define SELF_ID 1000000u

static member_t mk(uint32_t id, uint8_t state, uint32_t inc, uint32_t hb)
{
    member_t m;
    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(INADDR_LOOPBACK);
    m.port = (uint16_t)(9000u + id % 1000u);
    m.role = ROLE_OVEN;
    m.state = state;
    m.incarnation = inc;
    m.heartbeat = hb;
    return m;
}

static void fresh(node_t *n)
{
    memset(n, 0, sizeof *n);
    member_t self = mk(SELF_ID, STATE_ALIVE, 1, 1);
    mem_init(n, &self);
}

/* Puts one known member into the table: id 4, given state/inc/hb, last_update = 100. */
static member_t *seed(node_t *n, uint8_t state, uint32_t inc, uint32_t hb)
{
    fresh(n);
    member_t m = mk(4, state, inc, hb);
    m.last_update_ms = 100;
    CHECK(mem_insert(n, &m) == 0);
    return mem_find(n, 4);
}

static void test_self_ignored(void)
{
    static node_t n;
    fresh(&n);
    member_t e = mk(SELF_ID, STATE_DEAD, 9, 9999); /* even with a higher incarnation */
    CHECK(mem_upsert(&n, &e, 50) == MEM_IGNORED);
    CHECK_EQ_U64(n.count, 0);
    CHECK_EQ_U64(n.self.incarnation, 1);
    CHECK_EQ_U64(n.self.heartbeat, 1);
    CHECK(n.self.state == STATE_ALIVE);
}

static void test_unknown_member(void)
{
    static node_t n;
    fresh(&n);
    static const struct { uint8_t in, out; } cases[] = {
        {STATE_ALIVE, STATE_ALIVE},
        {STATE_SUSPECT, STATE_ALIVE},
        {STATE_DEAD, STATE_DEAD},
        {STATE_LEFT, STATE_LEFT},
    };
    for (uint32_t i = 0; i < 4; i++) {
        member_t e = mk(10 + i, cases[i].in, 3, 7);
        CHECK(mem_upsert(&n, &e, 1234) == MEM_ADDED);
        const member_t *m = mem_find(&n, 10 + i);
        CHECK(m != NULL);
        if (m != NULL) {
            CHECK(m->state == cases[i].out);
            CHECK_EQ_U64(m->incarnation, 3);
            CHECK_EQ_U64(m->heartbeat, 7);
            CHECK_EQ_U64(m->last_update_ms, 1234);
        }
    }
    CHECK_EQ_U64(n.count, 4);

    /* Full table (D14): nothing to evict -> rejected. */
    fresh(&n);
    for (uint32_t id = 1; id < MAX_NODES; id++) {
        member_t e = mk(id, STATE_ALIVE, 1, 1);
        CHECK(mem_upsert(&n, &e, 10) == MEM_ADDED);
    }
    member_t extra = mk(SELF_ID - 1, STATE_ALIVE, 1, 1);
    CHECK(mem_upsert(&n, &extra, 11) == MEM_REJECTED);
    CHECK_EQ_U64(n.count, MAX_NODES - 1);
}

static void test_ordering_boundaries(void)
{
    static node_t n;
    const member_t *m = seed(&n, STATE_ALIVE, 1, 999);

    member_t e = mk(4, STATE_ALIVE, 2, 0); /* (2,0) > (1,999) */
    CHECK(mem_upsert(&n, &e, 500) == MEM_UPDATED);
    CHECK_EQ_U64(m->incarnation, 2);
    CHECK_EQ_U64(m->heartbeat, 0);
    CHECK_EQ_U64(m->last_update_ms, 500);

    e = mk(4, STATE_ALIVE, 1, 5000); /* lower incarnation loses whatever the heartbeat */
    CHECK(mem_upsert(&n, &e, 600) == MEM_IGNORED);
    CHECK_EQ_U64(m->incarnation, 2);
    CHECK_EQ_U64(m->last_update_ms, 500);

    m = seed(&n, STATE_ALIVE, 1, 5);
    e = mk(4, STATE_ALIVE, 1, 5); /* identical: nothing changes, last_update stays */
    CHECK(mem_upsert(&n, &e, 700) == MEM_IGNORED);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_ALIVE, 1, 4); /* older heartbeat */
    CHECK(mem_upsert(&n, &e, 700) == MEM_IGNORED);
    CHECK_EQ_U64(m->heartbeat, 5);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_ALIVE, 1, 6); /* newer heartbeat */
    CHECK(mem_upsert(&n, &e, 700) == MEM_UPDATED);
    CHECK_EQ_U64(m->heartbeat, 6);
    CHECK_EQ_U64(m->last_update_ms, 700);
}

static void test_late_message_does_not_revive(void)
{
    static node_t n;
    const member_t *m = seed(&n, STATE_DEAD, 1, 37);

    member_t e = mk(4, STATE_ALIVE, 1, 40); /* delayed heartbeat of the same incarnation */
    CHECK(mem_upsert(&n, &e, 900) == MEM_IGNORED);
    CHECK(m->state == STATE_DEAD);
    CHECK_EQ_U64(m->heartbeat, 37);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_ALIVE, 1, 30);
    CHECK(mem_upsert(&n, &e, 900) == MEM_IGNORED);
    CHECK(m->state == STATE_DEAD);
    CHECK_EQ_U64(m->last_update_ms, 100);
}

static void test_restart_revives(void)
{
    static node_t n;
    member_t *m = seed(&n, STATE_DEAD, 1, 37);

    member_t e = mk(4, STATE_ALIVE, 2, 0); /* restart: new incarnation, new address */
    e.port = 7777;
    e.ip.s_addr = htonl(0x7F000002u);
    CHECK(mem_upsert(&n, &e, 900) == MEM_UPDATED);
    CHECK(m->state == STATE_ALIVE);
    CHECK_EQ_U64(m->incarnation, 2);
    CHECK_EQ_U64(m->heartbeat, 0);
    CHECK_EQ_U64(m->port, 7777);
    CHECK_EQ_U64(ntohl(m->ip.s_addr), 0x7F000002u);
    CHECK_EQ_U64(m->last_update_ms, 900);

    /* The same holds after LEFT. */
    m = seed(&n, STATE_LEFT, 1, 37);
    e = mk(4, STATE_ALIVE, 2, 0);
    CHECK(mem_upsert(&n, &e, 901) == MEM_UPDATED);
    CHECK(m->state == STATE_ALIVE);
}

static void test_suspect_recovery(void)
{
    static node_t n;
    const member_t *m = seed(&n, STATE_SUSPECT, 1, 10);

    member_t e = mk(4, STATE_ALIVE, 1, 10); /* no progress: stays suspected */
    CHECK(mem_upsert(&n, &e, 800) == MEM_IGNORED);
    CHECK(m->state == STATE_SUSPECT);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_ALIVE, 1, 11); /* progress: ALIVE, the timer restarts from now */
    CHECK(mem_upsert(&n, &e, 800) == MEM_UPDATED);
    CHECK(m->state == STATE_ALIVE);
    CHECK_EQ_U64(m->last_update_ms, 800);
}

static void test_remote_opinion_not_adopted(void)
{
    static node_t n;
    static const uint8_t remote[] = {STATE_SUSPECT, STATE_DEAD};
    for (size_t i = 0; i < sizeof remote; i++) {
        const member_t *m = seed(&n, STATE_ALIVE, 1, 10);
        member_t e = mk(4, remote[i], 1, 12); /* progress is adopted, the verdict is not */
        CHECK(mem_upsert(&n, &e, 300) == MEM_UPDATED);
        CHECK(m->state == STATE_ALIVE);
        CHECK_EQ_U64(m->heartbeat, 12);

        m = seed(&n, STATE_ALIVE, 1, 10);
        e = mk(4, remote[i], 1, 10); /* no progress: nothing at all */
        CHECK(mem_upsert(&n, &e, 300) == MEM_IGNORED);
        CHECK(m->state == STATE_ALIVE);
        CHECK_EQ_U64(m->last_update_ms, 100);
    }
}

static void test_left_is_terminal(void)
{
    static node_t n;
    const member_t *m = seed(&n, STATE_LEFT, 1, 10);

    member_t e = mk(4, STATE_ALIVE, 1, 20); /* same incarnation, higher heartbeat */
    CHECK(mem_upsert(&n, &e, 400) == MEM_IGNORED);
    CHECK(m->state == STATE_LEFT);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_LEFT, 1, 10); /* LEFT again: nothing new */
    CHECK(mem_upsert(&n, &e, 400) == MEM_IGNORED);
    CHECK_EQ_U64(m->last_update_ms, 100);

    e = mk(4, STATE_ALIVE, 2, 0); /* new incarnation brings it back */
    CHECK(mem_upsert(&n, &e, 400) == MEM_UPDATED);
    CHECK(m->state == STATE_ALIVE);
}

static void test_left_from_owner(void)
{
    static node_t n;

    /* Rule 4: same incarnation, LEFT, whatever the heartbeat. */
    const member_t *m = seed(&n, STATE_ALIVE, 1, 10);
    member_t e = mk(4, STATE_LEFT, 1, 5);
    CHECK(mem_upsert(&n, &e, 450) == MEM_LEFT);
    CHECK(m->state == STATE_LEFT);
    CHECK_EQ_U64(m->heartbeat, 10);
    CHECK_EQ_U64(m->last_update_ms, 450); /* D4: the cleanup timer starts now */

    m = seed(&n, STATE_SUSPECT, 1, 10);
    e = mk(4, STATE_LEFT, 1, 10);
    CHECK(mem_upsert(&n, &e, 451) == MEM_LEFT);
    CHECK(m->state == STATE_LEFT);

    /* Rule 3b: LEFT with a higher heartbeat is progress and LEFT at once. */
    m = seed(&n, STATE_ALIVE, 1, 10);
    e = mk(4, STATE_LEFT, 1, 20);
    CHECK(mem_upsert(&n, &e, 452) == MEM_LEFT);
    CHECK(m->state == STATE_LEFT);
    CHECK_EQ_U64(m->heartbeat, 20);

    /* A DEAD record is replaced by the owner's own LEFT, with any heartbeat. */
    m = seed(&n, STATE_DEAD, 1, 37);
    e = mk(4, STATE_LEFT, 1, 40);
    CHECK(mem_upsert(&n, &e, 453) == MEM_LEFT);
    CHECK(m->state == STATE_LEFT);
    m = seed(&n, STATE_DEAD, 1, 37);
    e = mk(4, STATE_LEFT, 1, 30);
    CHECK(mem_upsert(&n, &e, 454) == MEM_LEFT);
    CHECK(m->state == STATE_LEFT);

    /* LEFT of an older incarnation is stale. */
    m = seed(&n, STATE_ALIVE, 2, 1);
    e = mk(4, STATE_LEFT, 1, 99);
    CHECK(mem_upsert(&n, &e, 455) == MEM_IGNORED);
    CHECK(m->state == STATE_ALIVE);
}

static void test_idempotent(void)
{
    static node_t n;
    fresh(&n);
    member_t e = mk(7, STATE_ALIVE, 1, 3);
    CHECK(mem_upsert(&n, &e, 10) == MEM_ADDED);
    CHECK(mem_upsert(&n, &e, 20) == MEM_IGNORED);
    CHECK_EQ_U64(n.count, 1);
    CHECK_EQ_U64(mem_find(&n, 7)->last_update_ms, 10);

    e.heartbeat = 4;
    CHECK(mem_upsert(&n, &e, 30) == MEM_UPDATED);
    CHECK(mem_upsert(&n, &e, 40) == MEM_IGNORED);
    CHECK_EQ_U64(mem_find(&n, 7)->last_update_ms, 30);
}

/* ---- Property: the order in which records arrive must not matter ---------------- */

#define PROP_IDS 10u
#define PROP_RECORDS 200u
#define PROP_ORDERS 50u

static void shuffle(member_t *v, size_t cnt)
{
    for (size_t i = cnt; i > 1; i--) {
        size_t j = rng_below((uint32_t)i);
        member_t t = v[i - 1];
        v[i - 1] = v[j];
        v[j] = t;
    }
}

/*
 * with_left == false: records are ALIVE/SUSPECT only, so the final (inc, hb) of
 * every member is the lexicographic maximum.
 * with_left == true: LEFT is terminal within an incarnation, so the heartbeat
 * may legitimately differ between orders; the final incarnation is the maximum
 * and the state is LEFT iff some LEFT record carries that incarnation.
 * (DEAD is deliberately absent: an unknown DEAD record is a tombstone, rule 2,
 * and which of two records is "first" then decides the outcome - see issue #7.)
 */
static void run_property(bool with_left, uint64_t seed_value)
{
    static node_t n;
    static member_t recs[PROP_RECORDS];
    uint32_t best_inc[PROP_IDS + 1] = {0}, best_hb[PROP_IDS + 1] = {0};
    bool seen[PROP_IDS + 1] = {false}, left_at_best[PROP_IDS + 1] = {false};

    rng_seed(seed_value);
    for (size_t i = 0; i < PROP_RECORDS; i++) {
        uint32_t id = 1 + rng_below(PROP_IDS);
        uint32_t inc = 1 + rng_below(3);
        uint32_t hb = rng_below(20);
        uint8_t st = (uint8_t)(rng_below(2) ? STATE_ALIVE : STATE_SUSPECT);
        if (with_left && rng_below(8) == 0)
            st = STATE_LEFT;
        recs[i] = mk(id, st, inc, hb);
    }
    for (size_t i = 0; i < PROP_RECORDS; i++) {
        uint32_t id = recs[i].node_id;
        if (!seen[id] || recs[i].incarnation > best_inc[id] ||
            (recs[i].incarnation == best_inc[id] && recs[i].heartbeat > best_hb[id])) {
            best_inc[id] = recs[i].incarnation;
            best_hb[id] = recs[i].heartbeat;
        }
        seen[id] = true;
    }
    for (size_t i = 0; i < PROP_RECORDS; i++)
        if (recs[i].state == STATE_LEFT && recs[i].incarnation == best_inc[recs[i].node_id])
            left_at_best[recs[i].node_id] = true;

    for (unsigned order = 0; order < PROP_ORDERS; order++) {
        shuffle(recs, PROP_RECORDS);
        fresh(&n);
        for (size_t i = 0; i < PROP_RECORDS; i++)
            mem_upsert(&n, &recs[i], 1000 + i);
        for (uint32_t id = 1; id <= PROP_IDS; id++) {
            const member_t *m = mem_find(&n, id);
            if (!seen[id]) {
                CHECK(m == NULL);
                continue;
            }
            CHECK(m != NULL);
            if (m == NULL)
                continue;
            CHECK_EQ_U64(m->incarnation, best_inc[id]);
            if (!with_left) {
                CHECK_EQ_U64(m->heartbeat, best_hb[id]);
                CHECK(m->state == STATE_ALIVE);
            } else {
                CHECK(m->state == (left_at_best[id] ? STATE_LEFT : STATE_ALIVE));
            }
        }
    }
}

static void test_order_independence(void)
{
    run_property(false, 7);
    run_property(true, 7);
    run_property(true, 8);
}

int main(void)
{
    test_self_ignored();
    test_unknown_member();
    test_ordering_boundaries();
    test_late_message_does_not_revive();
    test_restart_revives();
    test_suspect_recovery();
    test_remote_opinion_not_adopted();
    test_left_is_terminal();
    test_left_from_owner();
    test_idempotent();
    test_order_independence();
    TEST_MAIN_END();
}
