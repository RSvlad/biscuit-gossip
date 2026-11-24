#include <string.h>

#include "check.h"
#include "membership.h"
#include "util.h"

#define SELF_ID 1000000u
#define CAP (MAX_NODES - 1) /* table capacity (D1) */

static member_t mk(uint32_t id, uint8_t state, uint64_t last_update_ms)
{
    member_t m;
    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(INADDR_LOOPBACK);
    m.port = (uint16_t)(9000u + id % 1000u);
    m.role = (uint8_t)(id % 7u);
    m.state = state;
    m.incarnation = 1;
    m.heartbeat = 1;
    m.last_update_ms = last_update_ms;
    return m;
}

static void fresh(node_t *n)
{
    memset(n, 0, sizeof *n);
    member_t self = mk(SELF_ID, STATE_ALIVE, 0);
    self.role = ROLE_MONITOR;
    mem_init(n, &self);
}

static void fill(node_t *n, size_t count) /* ids 1..count, ALIVE, last_update = 100 + id */
{
    for (size_t i = 1; i <= count; i++) {
        member_t m = mk((uint32_t)i, STATE_ALIVE, 100 + i);
        CHECK(mem_insert(n, &m) == 0);
    }
}

static bool sorted(const node_t *n)
{
    for (size_t i = 1; i < n->count; i++)
        if (n->table[i - 1].node_id >= n->table[i].node_id)
            return false;
    return true;
}

static bool unique(const member_t *v, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++)
        for (size_t j = i + 1; j < cnt; j++)
            if (v[i].node_id == v[j].node_id)
                return false;
    return true;
}

static void test_init(void)
{
    static node_t n;
    fresh(&n);
    CHECK_EQ_U64(n.count, 0);
    CHECK_EQ_U64(n.self.node_id, SELF_ID);
    CHECK(n.self.role == ROLE_MONITOR);
    CHECK(mem_find(&n, SELF_ID) == NULL); /* D1: self is not in the table */
}

static void test_insert_dup_sorted(void)
{
    static node_t n;
    fresh(&n);

    size_t total = CAP > 43 ? 40 : CAP - 3;
    uint32_t ids[40];
    for (size_t i = 0; i < total; i++)
        ids[i] = (uint32_t)(i + 1);
    rng_seed(1);
    for (size_t i = total; i > 1; i--) { /* Fisher-Yates: random insertion order */
        size_t j = rng_below((uint32_t)i);
        uint32_t t = ids[i - 1];
        ids[i - 1] = ids[j];
        ids[j] = t;
    }
    for (size_t i = 0; i < total; i++) {
        member_t m = mk(ids[i], STATE_ALIVE, 10);
        CHECK(mem_insert(&n, &m) == 0);
    }
    CHECK_EQ_U64(n.count, total);
    CHECK(sorted(&n));
    for (uint32_t id = 1; id <= total; id++) {
        const member_t *f = mem_find(&n, id);
        CHECK(f != NULL && f->node_id == id);
    }
    CHECK(mem_find(&n, 0) == NULL);
    CHECK(mem_find(&n, (uint32_t)total + 1) == NULL);

    /* A duplicate does not double the entry. */
    member_t dup = mk(5, STATE_ALIVE, 99);
    CHECK(mem_insert(&n, &dup) == -1);
    CHECK_EQ_U64(n.count, total);
    CHECK_EQ_U64(mem_find(&n, 5)->last_update_ms, 10);
    CHECK(mem_upsert(&n, &dup, 500) == MEM_IGNORED); /* stub: known members are left alone */
    CHECK_EQ_U64(n.count, total);

    /* self never enters the table (D1). */
    member_t me = mk(SELF_ID, STATE_ALIVE, 1);
    CHECK(mem_insert(&n, &me) == -1);
    CHECK(mem_upsert(&n, &me, 1) == MEM_IGNORED);

    /* upsert stub: a new member is added; DEAD/LEFT are kept, others become ALIVE. */
    member_t s = mk(900, STATE_SUSPECT, 0);
    CHECK(mem_upsert(&n, &s, 777) == MEM_ADDED);
    CHECK(mem_find(&n, 900)->state == STATE_ALIVE);
    CHECK_EQ_U64(mem_find(&n, 900)->last_update_ms, 777);
    member_t d = mk(901, STATE_DEAD, 0);
    CHECK(mem_upsert(&n, &d, 5) == MEM_ADDED);
    CHECK(mem_find(&n, 901)->state == STATE_DEAD);
    CHECK(sorted(&n));
}

static void test_full_table(void)
{
    static node_t n;
    fresh(&n);
    fill(&n, CAP);
    CHECK_EQ_U64(n.count, CAP);

    /* No DEAD/LEFT to evict: the newcomer is dropped and counted. */
    member_t fresh_rec = mk(SELF_ID - 1, STATE_ALIVE, 1000);
    CHECK(mem_insert(&n, &fresh_rec) == -1);
    CHECK(mem_upsert(&n, &fresh_rec, 1000) == MEM_REJECTED);
    CHECK_EQ_U64(n.count, CAP);
    CHECK_EQ_U64(n.stats.dropped_table_full, 2);

    /* With DEAD/LEFT records the oldest of them makes room. */
    uint32_t dead_id = n.table[3].node_id, left_id = n.table[7].node_id;
    n.table[3].state = STATE_DEAD;
    n.table[3].last_update_ms = 500;
    n.table[7].state = STATE_LEFT;
    n.table[7].last_update_ms = 300; /* older than the DEAD one */
    CHECK(mem_insert(&n, &fresh_rec) == 0);
    CHECK_EQ_U64(n.count, CAP);
    CHECK(mem_find(&n, left_id) == NULL);
    CHECK(mem_find(&n, dead_id) != NULL);
    CHECK(mem_find(&n, SELF_ID - 1) != NULL);
    CHECK(sorted(&n));

    member_t another = mk(SELF_ID - 2, STATE_ALIVE, 1001);
    CHECK(mem_insert(&n, &another) == 0); /* evicts the remaining DEAD record */
    CHECK(mem_find(&n, dead_id) == NULL);
    CHECK_EQ_U64(n.count, CAP);
    CHECK(sorted(&n));

    member_t third = mk(SELF_ID - 3, STATE_ALIVE, 1002);
    CHECK(mem_insert(&n, &third) == -1);
    CHECK_EQ_U64(n.stats.dropped_table_full, 3);
}

static void test_remove(void)
{
    static node_t n;
    fresh(&n);
    fill(&n, 6);

    mem_remove(&n, 1); /* first */
    CHECK_EQ_U64(n.count, 5);
    CHECK(mem_find(&n, 1) == NULL && sorted(&n));
    mem_remove(&n, 3); /* middle */
    CHECK_EQ_U64(n.count, 4);
    CHECK(mem_find(&n, 3) == NULL && sorted(&n));
    mem_remove(&n, 6); /* last */
    CHECK_EQ_U64(n.count, 3);
    CHECK(mem_find(&n, 6) == NULL && sorted(&n));
    mem_remove(&n, 77); /* unknown id: not an error */
    CHECK_EQ_U64(n.count, 3);
    CHECK(mem_find(&n, 2) != NULL && mem_find(&n, 4) != NULL && mem_find(&n, 5) != NULL);
    mem_remove(&n, 2);
    mem_remove(&n, 4);
    mem_remove(&n, 5);
    CHECK_EQ_U64(n.count, 0);
    mem_remove(&n, 5); /* empty table */
    CHECK_EQ_U64(n.count, 0);
}

static void test_sample(void)
{
    static node_t n;
    member_t out[MAX_ENTRIES_MSG + 5];

    /* Everything fits: self first, then all (table order); LEFT is included. */
    fresh(&n);
    fill(&n, 5);
    member_t dead = mk(6, STATE_DEAD, 1), left = mk(7, STATE_LEFT, 1);
    CHECK(mem_insert(&n, &dead) == 0 && mem_insert(&n, &left) == 0);
    size_t c = mem_sample(&n, out, MAX_ENTRIES_MSG, true);
    CHECK_EQ_U64(c, 8);
    CHECK_EQ_U64(out[0].node_id, SELF_ID);
    CHECK(unique(out, c));
    c = mem_sample(&n, out, MAX_ENTRIES_MSG, false); /* DEAD left out, LEFT stays */
    CHECK_EQ_U64(c, 7);
    bool has_left = false, has_dead = false;
    for (size_t i = 0; i < c; i++) {
        has_left |= out[i].node_id == 7;
        has_dead |= out[i].node_id == 6;
    }
    CHECK(has_left && !has_dead);

    /* max = 1: self only; max = 0: nothing. */
    CHECK_EQ_U64(mem_sample(&n, out, 1, true), 1);
    CHECK_EQ_U64(out[0].node_id, SELF_ID);
    CHECK_EQ_U64(mem_sample(&n, out, 0, true), 0);

    /* Does not fit: exactly max records, self first, no duplicates, random subset. */
    fresh(&n);
    fill(&n, CAP);
    bool seen[MAX_NODES + 1];
    memset(seen, 0, sizeof seen);
    rng_seed(3);
    for (int round = 0; round < 300; round++) {
        c = mem_sample(&n, out, 8, true);
        CHECK_EQ_U64(c, 8);
        CHECK_EQ_U64(out[0].node_id, SELF_ID);
        CHECK(unique(out, c));
        for (size_t i = 1; i < c; i++) {
            CHECK(mem_find(&n, out[i].node_id) != NULL);
            seen[out[i].node_id] = true;
        }
    }
    bool covered = true;
    for (size_t id = 1; id <= CAP; id++)
        covered = covered && seen[id];
    CHECK(covered); /* every member shows up over many samples (D15) */

    /* Priority: non-DEAD first, DEAD only as filler. */
    fresh(&n);
    size_t alive = CAP / 3 > 10 ? 10 : CAP / 3;
    for (size_t i = 1; i <= CAP; i++) {
        member_t m = mk((uint32_t)i, i <= alive ? STATE_ALIVE : STATE_DEAD, 50);
        CHECK(mem_insert(&n, &m) == 0);
    }
    for (int round = 0; round < 50; round++) {
        c = mem_sample(&n, out, alive + 3, true); /* self + all alive + 2 dead */
        CHECK_EQ_U64(c, alive + 3);
        size_t na = 0, nd = 0;
        for (size_t i = 1; i < c; i++) {
            if (out[i].state == STATE_ALIVE)
                na++;
            else if (out[i].state == STATE_DEAD)
                nd++;
        }
        CHECK_EQ_U64(na, alive);
        CHECK_EQ_U64(nd, 2);
        CHECK(unique(out, c));
    }
    c = mem_sample(&n, out, MAX_ENTRIES_MSG, false); /* no DEAD at all */
    CHECK_EQ_U64(c, alive + 1);

    /* max is clamped to MAX_ENTRIES_MSG. */
    if (CAP + 1 > MAX_ENTRIES_MSG) {
        fresh(&n);
        fill(&n, CAP);
        c = mem_sample(&n, out, MAX_ENTRIES_MSG + 5, true);
        CHECK_EQ_U64(c, MAX_ENTRIES_MSG);
        CHECK(unique(out, c));
    }
}

static void test_pick_peers(void)
{
    static node_t n;
    member_t out[8];

    fresh(&n);
    CHECK_EQ_U64(mem_pick_peers(&n, out, 3), 0); /* empty table */

    fill(&n, 6); /* ids 1..6 ALIVE */
    n.table[1].state = STATE_SUSPECT; /* id 2 */
    n.table[2].state = STATE_DEAD;    /* id 3 */
    n.table[3].state = STATE_LEFT;    /* id 4 */
    /* candidates: 1, 2 (SUSPECT), 5, 6 */
    rng_seed(5);
    for (int round = 0; round < 200; round++) {
        size_t c = mem_pick_peers(&n, out, 3);
        CHECK_EQ_U64(c, 3);
        CHECK(unique(out, c));
        for (size_t i = 0; i < c; i++) {
            CHECK(out[i].node_id != SELF_ID);
            CHECK(out[i].node_id == 1 || out[i].node_id == 2 || out[i].node_id == 5 ||
                  out[i].node_id == 6);
        }
    }
    CHECK_EQ_U64(mem_pick_peers(&n, out, 8), 4); /* k > candidates: all of them */
    CHECK_EQ_U64(mem_pick_peers(&n, out, 4), 4);
    CHECK_EQ_U64(mem_pick_peers(&n, out, 0), 0);
    CHECK_EQ_U64(mem_pick_peers(&n, out, 1), 1);

    /* Distribution: 10 candidates, 10 000 picks of one, roughly uniform (+-25%). */
    fresh(&n);
    fill(&n, 10);
    unsigned hits[11] = {0};
    rng_seed(42);
    for (int i = 0; i < 10000; i++) {
        CHECK_EQ_U64(mem_pick_peers(&n, out, 1), 1);
        hits[out[0].node_id]++;
    }
    for (int id = 1; id <= 10; id++) {
        CHECK(hits[id] >= 750 && hits[id] <= 1250);
    }
}

static void test_names_and_log(void)
{
    CHECK_STR_EQ(state_name(STATE_ALIVE), "ALIVE");
    CHECK_STR_EQ(state_name(STATE_SUSPECT), "SUSPECT");
    CHECK_STR_EQ(state_name(STATE_DEAD), "DEAD");
    CHECK_STR_EQ(state_name(STATE_LEFT), "LEFT");
    CHECK_STR_EQ(state_name(4), "?");
    CHECK_STR_EQ(state_name(255), "?");
    CHECK_STR_EQ(role_name(ROLE_MIXER), "MIXER");
    CHECK_STR_EQ(role_name(ROLE_OVEN), "OVEN");
    CHECK_STR_EQ(role_name(ROLE_MONITOR), "MONITOR");
    CHECK_STR_EQ(role_name(7), "?");

    member_t m = mk(4, STATE_ALIVE, 0);
    m.role = ROLE_OVEN;
    mem_log_transition(&m, "ALIVE", "SUSPECT", 5200); /* member 4 OVEN: ALIVE -> SUSPECT (silence 5.2s) */
}

int main(void)
{
    test_init();
    test_insert_dup_sorted();
    test_full_table();
    test_remove();
    test_sample();
    test_pick_peers();
    test_names_and_log();
    TEST_MAIN_END();
}
