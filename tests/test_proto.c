#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "proto.h"
#include "util.h"

/* ---- helpers ---- */

static uint64_t xs_state = 88172645463325252ull;
static uint32_t xs(void) /* local xorshift64: the test must not depend on util's RNG */
{
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 7;
    xs_state ^= xs_state << 17;
    return (uint32_t)(xs_state >> 16);
}

static member_t mk(uint32_t id, uint32_t ip_host, uint16_t port, uint8_t role, uint8_t state,
                   uint32_t inc, uint32_t hb)
{
    member_t m;
    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(ip_host);
    m.port = port;
    m.role = role;
    m.state = state;
    m.incarnation = inc;
    m.heartbeat = hb;
    return m;
}

static int same_member(const member_t *a, const member_t *b)
{
    return a->node_id == b->node_id && a->ip.s_addr == b->ip.s_addr && a->port == b->port &&
           a->role == b->role && a->state == b->state && a->incarnation == b->incarnation &&
           a->heartbeat == b->heartbeat;
}

static member_t sample_entry(uint32_t i)
{
    return mk(1000 + i, 0x7F000001u + i, (uint16_t)(9000 + i), (uint8_t)(i % 7),
              (uint8_t)(i % 4), 1700000000u + i, i * 3);
}

/* Encodes, decodes and compares. Returns 1 if the round trip is exact. */
static int round_trip(uint8_t type, uint32_t sender, const member_t *e, uint16_t count)
{
    uint8_t buf[MAX_DATAGRAM];
    msg_t out;
    size_t n = proto_encode(buf, sizeof buf, type, sender, e, count);
    if (n != PROTO_HEADER_LEN + (size_t)count * PROTO_ENTRY_LEN)
        return 0;
    if (proto_decode(buf, n, &out) != PROTO_OK)
        return 0;
    if (out.type != type || out.sender_id != sender || out.count != count)
        return 0;
    for (uint16_t i = 0; i < count; i++)
        if (!same_member(&out.entries[i], &e[i]) || out.entries[i].last_update_ms != 0)
            return 0;
    return 1;
}

/* A valid GOSSIP datagram with one entry; returns its length. */
static size_t valid_gossip(uint8_t *buf, size_t cap)
{
    member_t e = sample_entry(1);
    return proto_encode(buf, cap, MSG_GOSSIP, 7, &e, 1);
}

static int dec(const uint8_t *buf, size_t len)
{
    msg_t out;
    return proto_decode(buf, len, &out);
}

/* ---- tests ---- */

static void test_round_trip(void)
{
    member_t e[MAX_ENTRIES_MSG];
    for (uint32_t i = 0; i < MAX_ENTRIES_MSG; i++)
        e[i] = sample_entry(i);

    const uint16_t counts[] = {0, 1, 31, 32};
    for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        CHECK(round_trip(MSG_GOSSIP, 5, e, counts[c]));
        CHECK(round_trip(MSG_GOSSIP_REPLY, 5, e, counts[c]));
    }

    /* JOIN / LEAVE: exactly one entry, node_id == sender, LEAVE carries LEFT. */
    member_t j = mk(9, 0x0A000009u, 9009, ROLE_OVEN, STATE_ALIVE, 5, 0);
    member_t l = mk(9, 0x0A000009u, 9009, ROLE_OVEN, STATE_LEFT, 5, 77);
    CHECK(round_trip(MSG_JOIN, 9, &j, 1));
    CHECK(round_trip(MSG_LEAVE, 9, &l, 1));

    /* Extreme values. */
    member_t x = mk(0xFFFFFFFFu, 0xFFFFFFFFu, 65535, ROLE_MONITOR, STATE_LEFT, 0xFFFFFFFFu,
                    0xFFFFFFFFu);
    CHECK(round_trip(MSG_GOSSIP, 0xFFFFFFFFu, &x, 1));
    member_t hi = mk(0, 0xC0A80001u, 1, ROLE_MIXER, STATE_ALIVE, 0, 0); /* high bit set in IP */
    CHECK(round_trip(MSG_GOSSIP, 0, &hi, 1));
}

static void test_golden(void)
{
    /* Hand-written GOSSIP from sender 0x01020304 with one record. */
    static const uint8_t golden[] = {
        0x4B, 0x58, 0x01, 0x02, 0x01, 0x02, 0x03, 0x04, 0x00, 0x01, 0x00, 0x00, /* header */
        0x00, 0x00, 0x00, 0x07,                                                 /* node_id 7 */
        0x7F, 0x00, 0x00, 0x01,                                                 /* 127.0.0.1 */
        0x23, 0x29,                                                             /* port 9001 */
        0x03, 0x00,                                                             /* OVEN, ALIVE */
        0x11, 0x22, 0x33, 0x44,                                                 /* incarnation */
        0x00, 0x00, 0x00, 0x2A                                                  /* heartbeat 42 */
    };
    msg_t m;
    CHECK_EQ_U64(sizeof golden, PROTO_HEADER_LEN + PROTO_ENTRY_LEN);
    CHECK_EQ_U64(proto_decode(golden, sizeof golden, &m), PROTO_OK);
    CHECK_EQ_U64(m.type, MSG_GOSSIP);
    CHECK_EQ_U64(m.sender_id, 0x01020304u);
    CHECK_EQ_U64(m.count, 1);
    CHECK_EQ_U64(m.entries[0].node_id, 7);
    CHECK_EQ_U64(ntohl(m.entries[0].ip.s_addr), 0x7F000001u);
    CHECK_EQ_U64(m.entries[0].port, 9001);
    CHECK_EQ_U64(m.entries[0].role, ROLE_OVEN);
    CHECK_EQ_U64(m.entries[0].state, STATE_ALIVE);
    CHECK_EQ_U64(m.entries[0].incarnation, 0x11223344u);
    CHECK_EQ_U64(m.entries[0].heartbeat, 42);

    uint8_t buf[64];
    size_t n = proto_encode(buf, sizeof buf, MSG_GOSSIP, 0x01020304u, m.entries, 1);
    CHECK_EQ_U64(n, sizeof golden);
    CHECK(memcmp(buf, golden, sizeof golden) == 0);
}

static void test_reject_header(void)
{
    uint8_t buf[MAX_DATAGRAM];
    size_t n = valid_gossip(buf, sizeof buf);
    CHECK_EQ_U64(n, 32);
    CHECK_EQ_U64(dec(buf, n), PROTO_OK);

    for (size_t len = 0; len < PROTO_HEADER_LEN; len++)
        CHECK_EQ_U64(dec(buf, len), PROTO_ERR_SHORT);

    uint8_t t[MAX_DATAGRAM];
    memcpy(t, buf, n);
    t[0] ^= 0xFF;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_MAGIC);

    memcpy(t, buf, n);
    t[2] = 2;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_VERSION);
    t[2] = 0;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_VERSION);

    memcpy(t, buf, n);
    t[3] = 0;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_TYPE);
    t[3] = 5;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_TYPE);

    /* count = 33 (and 0xFFFF) */
    memcpy(t, buf, n);
    t[8] = 0;
    t[9] = 33;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_COUNT);
    t[8] = 0xFF;
    t[9] = 0xFF;
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_COUNT);

    /* length +-1, truncated last record, trailing garbage */
    CHECK_EQ_U64(dec(buf, n - 1), PROTO_ERR_LENGTH);
    memcpy(t, buf, n);
    t[n] = 0;
    CHECK_EQ_U64(dec(t, n + 1), PROTO_ERR_LENGTH);
    CHECK_EQ_U64(dec(buf, PROTO_HEADER_LEN), PROTO_ERR_LENGTH); /* count 1, no record */
    CHECK_EQ_U64(dec(buf, PROTO_HEADER_LEN + PROTO_ENTRY_LEN - 5), PROTO_ERR_LENGTH);

    /* `reserved` is ignored on receive. */
    memcpy(t, buf, n);
    t[10] = 0xAB;
    t[11] = 0xCD;
    CHECK_EQ_U64(dec(t, n), PROTO_OK);
}

static void test_reject_entry_and_semantics(void)
{
    uint8_t buf[MAX_DATAGRAM], t[MAX_DATAGRAM];
    size_t n = valid_gossip(buf, sizeof buf);
    const size_t e0 = PROTO_HEADER_LEN;

    memcpy(t, buf, n);
    t[e0 + 10] = 7; /* role */
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_ENTRY);

    memcpy(t, buf, n);
    t[e0 + 11] = 4; /* state */
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_ENTRY);

    memcpy(t, buf, n);
    t[e0 + 8] = 0;
    t[e0 + 9] = 0; /* port 0 */
    CHECK_EQ_U64(dec(t, n), PROTO_ERR_ENTRY);

    /* JOIN with two records */
    member_t two[2] = {mk(7, 0x7F000001u, 9001, 0, 0, 1, 1), mk(8, 0x7F000001u, 9002, 0, 0, 1, 1)};
    n = proto_encode(buf, sizeof buf, MSG_JOIN, 7, two, 2);
    CHECK(n != 0);
    CHECK_EQ_U64(dec(buf, n), PROTO_ERR_SEMANTIC);

    /* JOIN whose record is not the sender */
    n = proto_encode(buf, sizeof buf, MSG_JOIN, 8, two, 1);
    CHECK_EQ_U64(dec(buf, n), PROTO_ERR_SEMANTIC);

    /* JOIN with count 0 */
    n = proto_encode(buf, sizeof buf, MSG_JOIN, 7, NULL, 0);
    CHECK_EQ_U64(dec(buf, n), PROTO_ERR_SEMANTIC);

    /* LEAVE that does not carry STATE_LEFT */
    n = proto_encode(buf, sizeof buf, MSG_LEAVE, 7, two, 1);
    CHECK_EQ_U64(dec(buf, n), PROTO_ERR_SEMANTIC);
}

static void test_encode_limits(void)
{
    member_t e[MAX_ENTRIES_MSG + 1];
    for (uint32_t i = 0; i < MAX_ENTRIES_MSG + 1; i++)
        e[i] = sample_entry(i);
    uint8_t buf[MAX_DATAGRAM];

    CHECK_EQ_U64(proto_encode(buf, sizeof buf, MSG_GOSSIP, 1, e, MAX_ENTRIES_MSG + 1), 0);
    CHECK_EQ_U64(proto_encode(buf, PROTO_HEADER_LEN + PROTO_ENTRY_LEN - 1, MSG_GOSSIP, 1, e, 1), 0);
    CHECK_EQ_U64(proto_encode(buf, PROTO_HEADER_LEN + PROTO_ENTRY_LEN, MSG_GOSSIP, 1, e, 1),
                 PROTO_HEADER_LEN + PROTO_ENTRY_LEN);
    CHECK_EQ_U64(proto_encode(buf, PROTO_HEADER_LEN - 1, MSG_GOSSIP, 1, NULL, 0), 0);
    CHECK_EQ_U64(proto_encode(buf, sizeof buf, 0, 1, e, 1), 0);
    CHECK_EQ_U64(proto_encode(buf, sizeof buf, MSG_TYPE_MAX + 1, 1, e, 1), 0);
    CHECK_EQ_U64(proto_encode(buf, sizeof buf, MSG_GOSSIP, 1, NULL, 1), 0);
    CHECK_EQ_U64(proto_encode(buf, sizeof buf, MSG_GOSSIP, 1, e, MAX_ENTRIES_MSG),
                 PROTO_HEADER_LEN + MAX_ENTRIES_MSG * PROTO_ENTRY_LEN);
}

/* If decode accepts a buffer, re-encoding must reproduce it (except `reserved`). */
static void fuzz_check(const uint8_t *buf, size_t len, unsigned long *accepted)
{
    msg_t m;
    if (proto_decode(buf, len, &m) != PROTO_OK)
        return;
    (*accepted)++;
    uint8_t again[MAX_DATAGRAM];
    size_t n = proto_encode(again, sizeof again, m.type, m.sender_id, m.entries, m.count);
    CHECK_EQ_U64(n, len);
    if (n == len) {
        CHECK(memcmp(again, buf, 10) == 0);
        CHECK(memcmp(again + 12, buf + 12, len - 12) == 0);
    }
}

static void test_fuzz(void)
{
    uint8_t buf[1500];
    unsigned long accepted = 0;

    /* 1. pure random buffers of random length 0..1500 */
    for (int i = 0; i < 200000; i++) {
        size_t len = xs() % 1501;
        for (size_t k = 0; k < len; k++)
            buf[k] = (uint8_t)xs();
        fuzz_check(buf, len, &accepted);
    }

    /* 2. mutations of valid messages: flip a byte, truncate, extend */
    member_t e[MAX_ENTRIES_MSG];
    for (uint32_t i = 0; i < MAX_ENTRIES_MSG; i++)
        e[i] = sample_entry(i);
    for (int i = 0; i < 200000; i++) {
        uint8_t type = (uint8_t)(1 + xs() % 4);
        uint16_t count = (type == MSG_JOIN || type == MSG_LEAVE) ? 1 : (uint16_t)(xs() % 33);
        member_t me[MAX_ENTRIES_MSG];
        memcpy(me, e, sizeof me);
        if (type == MSG_JOIN || type == MSG_LEAVE) {
            me[0].node_id = 7;
            me[0].state = (type == MSG_LEAVE) ? STATE_LEFT : (uint8_t)(xs() % 4);
        }
        size_t len = proto_encode(buf, sizeof buf, type, 7, me, count);
        if (len == 0)
            continue;
        switch (xs() % 4) {
        case 0:
            buf[xs() % len] = (uint8_t)xs();
            break;
        case 1:
            len = xs() % (len + 1);
            break;
        case 2:
            if (len + 1 < sizeof buf)
                buf[len++] = (uint8_t)xs();
            break;
        default:
            break; /* untouched: must be accepted */
        }
        fuzz_check(buf, len, &accepted);
    }
    CHECK(accepted > 1000); /* the mutation corpus does reach the accept path */
}

int main(void)
{
    log_init(LOG_ERROR, 0); /* keep LOG_D quiet during fuzzing */
    test_round_trip();
    test_golden();
    test_reject_header();
    test_reject_entry_and_semantics();
    test_encode_limits();
    test_fuzz();
    TEST_MAIN_END();
}
