/*
 * test_failure.c - failure detector failure_check (issue #10, spec 7.3).
 * Fake clock: all time is injected through `now_ms`; nothing here sleeps.
 */
#include <string.h>

#include "check.h"
#include "failure.h"
#include "membership.h"

#define SELF_ID 1000000u

static member_t mk(uint32_t id, uint8_t state, uint32_t inc, uint32_t hb, uint64_t last_update)
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
    m.last_update_ms = last_update;
    return m;
}

static void fresh(node_t *n)
{
    memset(n, 0, sizeof *n);
    member_t self = mk(SELF_ID, STATE_ALIVE, 1, 1, 0);
    mem_init(n, &self);
    n->cfg.suspect_ms = DEF_SUSPECT_MS;
    n->cfg.fail_ms = DEF_FAIL_MS;
    n->cfg.cleanup_ms = DEF_CLEANUP_MS;
}

static void add(node_t *n, uint32_t id, uint8_t state, uint32_t inc, uint32_t hb, uint64_t last_update)
{
    member_t m = mk(id, state, inc, hb, last_update);
    CHECK(mem_insert(n, &m) == 0);
}

/* State of member `id`, or -1 when it is not in the table. */
static int state_of(node_t *n, uint32_t id)
{
    const member_t *m = mem_find(n, id);
    return m == NULL ? -1 : (int)m->state;
}

static void test_boundaries_from_alive(void)
{
    static node_t n;
    static const struct { uint64_t now; int expect; } cases[] = {
        {0, STATE_ALIVE},      {5000, STATE_ALIVE},  {5001, STATE_SUSPECT},
        {10000, STATE_SUSPECT}, {10001, STATE_DEAD}, {30000, STATE_DEAD},
        {30001, -1 /* removed */},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fresh(&n);
        add(&n, 4, STATE_ALIVE, 1, 10, 0);
        failure_check(&n, cases[i].now);
        CHECK_EQ_U64(state_of(&n, 4) + 1, cases[i].expect + 1);
    }
}

static void test_boundaries_stepwise(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 4, STATE_ALIVE, 1, 10, 0);
    static const struct { uint64_t now; int expect; } steps[] = {
        {5000, STATE_ALIVE}, {5001, STATE_SUSPECT}, {10000, STATE_SUSPECT},
        {10001, STATE_DEAD}, {30000, STATE_DEAD},   {30001, -1},
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        failure_check(&n, steps[i].now);
        CHECK_EQ_U64(state_of(&n, 4) + 1, steps[i].expect + 1);
    }
    CHECK_EQ_U64(n.count, 0);
}

static void test_suspect_recovers_and_timer_restarts(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 4, STATE_ALIVE, 1, 10, 0);
    failure_check(&n, 6000);
    CHECK(state_of(&n, 4) == STATE_SUSPECT);

    member_t e = mk(4, STATE_ALIVE, 1, 11, 0); /* larger heartbeat = progress */
    CHECK(mem_upsert(&n, &e, 8000) == MEM_UPDATED);
    CHECK(state_of(&n, 4) == STATE_ALIVE);

    failure_check(&n, 8000 + 4999); /* the timer starts from the new last_update */
    CHECK(state_of(&n, 4) == STATE_ALIVE);
    failure_check(&n, 8000 + 5001);
    CHECK(state_of(&n, 4) == STATE_SUSPECT);
}

static void test_dead_needs_new_incarnation(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 4, STATE_ALIVE, 1, 10, 0);
    failure_check(&n, 12000);
    CHECK(state_of(&n, 4) == STATE_DEAD);

    member_t late = mk(4, STATE_ALIVE, 1, 40, 0); /* same incarnation, larger heartbeat */
    CHECK(mem_upsert(&n, &late, 13000) == MEM_IGNORED);
    CHECK(state_of(&n, 4) == STATE_DEAD);
    failure_check(&n, 14000);
    CHECK(state_of(&n, 4) == STATE_DEAD);

    member_t restarted = mk(4, STATE_ALIVE, 2, 0, 0);
    CHECK(mem_upsert(&n, &restarted, 15000) == MEM_UPDATED);
    CHECK(state_of(&n, 4) == STATE_ALIVE);
    failure_check(&n, 15000 + 5000);
    CHECK(state_of(&n, 4) == STATE_ALIVE);
}

static void test_independent_members_one_call(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 1, STATE_ALIVE, 1, 1, 20000);  /* silence  2000 -> ALIVE   */
    add(&n, 2, STATE_ALIVE, 1, 1, 15000);  /* silence  7000 -> SUSPECT */
    add(&n, 3, STATE_SUSPECT, 1, 1, 11000); /* silence 11000 -> DEAD    */
    add(&n, 4, STATE_DEAD, 1, 1, 0);       /* silence 22000 -> DEAD    */
    add(&n, 5, STATE_LEFT, 1, 1, 0);       /* silence 22000 -> LEFT    */
    add(&n, 6, STATE_DEAD, 1, 1, 100);     /* silence 21900 -> DEAD    */
    failure_check(&n, 22000);
    CHECK(state_of(&n, 1) == STATE_ALIVE);
    CHECK(state_of(&n, 2) == STATE_SUSPECT);
    CHECK(state_of(&n, 3) == STATE_DEAD);
    CHECK(state_of(&n, 4) == STATE_DEAD);
    CHECK(state_of(&n, 5) == STATE_LEFT);
    CHECK(state_of(&n, 6) == STATE_DEAD);
    CHECK_EQ_U64(n.count, 6);
}

static void test_removal_of_consecutive_members(void)
{
    static node_t n;
    fresh(&n);
    for (uint32_t id = 1; id <= 8; id++) /* ids 1..8: removal hits adjacent indices */
        add(&n, id, (id % 2) ? STATE_DEAD : STATE_LEFT, 1, 1, id == 5 ? 29000 : 0);
    failure_check(&n, 30001);
    CHECK_EQ_U64(n.count, 1); /* only id 5 (silence 1001) survives */
    CHECK(state_of(&n, 5) == STATE_DEAD);
    for (uint32_t id = 1; id <= 8; id++)
        if (id != 5)
            CHECK(state_of(&n, id) == -1);
    for (size_t i = 1; i < n.count; i++) /* table stays sorted */
        CHECK(n.table[i - 1].node_id < n.table[i].node_id);
}

static void test_clock_jump(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 4, STATE_ALIVE, 1, 10, 0);
    failure_check(&n, 12000); /* ALIVE -> SUSPECT -> DEAD in one call */
    CHECK(state_of(&n, 4) == STATE_DEAD);
    CHECK_EQ_U64(mem_find(&n, 4)->last_update_ms, 0); /* a transition is not progress */
}

static void test_left_removed_after_cleanup(void)
{
    static node_t n;
    fresh(&n);
    add(&n, 4, STATE_LEFT, 1, 10, 2000); /* last_update = the moment of the transition (D4) */
    failure_check(&n, 2000 + 30000);
    CHECK(state_of(&n, 4) == STATE_LEFT);
    failure_check(&n, 2000 + 30001);
    CHECK(state_of(&n, 4) == -1);
}

static void test_self_never_touched(void)
{
    static node_t n;
    fresh(&n);
    n.self.last_update_ms = 0;
    failure_check(&n, 1000000);
    CHECK(n.self.state == STATE_ALIVE);
    CHECK_EQ_U64(n.self.heartbeat, 1);
    CHECK_EQ_U64(n.count, 0);
}

static void test_custom_thresholds(void)
{
    static node_t n;
    fresh(&n);
    n.cfg.suspect_ms = 1000;
    n.cfg.fail_ms = 2000;
    n.cfg.cleanup_ms = 3000;
    add(&n, 4, STATE_ALIVE, 1, 10, 0);
    failure_check(&n, 1000);
    CHECK(state_of(&n, 4) == STATE_ALIVE);
    failure_check(&n, 1001);
    CHECK(state_of(&n, 4) == STATE_SUSPECT);
    failure_check(&n, 2001);
    CHECK(state_of(&n, 4) == STATE_DEAD);
    failure_check(&n, 3000);
    CHECK(state_of(&n, 4) == STATE_DEAD);
    failure_check(&n, 3001);
    CHECK(state_of(&n, 4) == -1);
}

static void test_edge_cases(void)
{
    static node_t n;
    fresh(&n);
    failure_check(&n, 123456); /* empty table: no crash */
    CHECK_EQ_U64(n.count, 0);

    add(&n, 4, STATE_ALIVE, 1, 10, 5000); /* last_update in the "future": silence is 0 */
    failure_check(&n, 1000);
    CHECK(state_of(&n, 4) == STATE_ALIVE);

    failure_check(&n, 5000); /* idempotent: a repeated call at the same time changes nothing */
    failure_check(&n, 5000);
    CHECK(state_of(&n, 4) == STATE_ALIVE);
}

int main(void)
{
    test_boundaries_from_alive();
    test_boundaries_stepwise();
    test_suspect_recovers_and_timer_restarts();
    test_dead_needs_new_incarnation();
    test_independent_members_one_call();
    test_removal_of_consecutive_members();
    test_clock_jump();
    test_left_removed_after_cleanup();
    test_self_never_touched();
    test_custom_thresholds();
    test_edge_cases();
    TEST_MAIN_END();
}
