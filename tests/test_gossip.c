/*
 * test_gossip.c - gossip round and message handling (issue #8, spec 7.1, F3, F4, D6, D10, D15).
 *
 * Real UDP sockets on 127.0.0.1 with ephemeral ports: node_send() really goes through
 * the kernel and the "peers" are plain sockets the test reads from.  Nothing sleeps;
 * the only waits are poll() timeouts that a loopback datagram never needs.
 * JOIN (#9) and LEAVE (#11) handlers are stubs in #8, so they are not tested here.
 */
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "check.h"
#include "gossip.h"
#include "membership.h"
#include "net.h"
#include "node.h"
#include "proto.h"
#include "util.h"

#define CAP (MAX_NODES - 1) /* table capacity (D1) */
#define WAIT_MS 200         /* a datagram that was sent is already queued; this only guards flakiness */
#define QUIET_MS 30         /* how long "nothing arrives" is observed */

/* ---- helpers ------------------------------------------------------------------ */

typedef struct {
    int fd;
    uint16_t port;
    struct sockaddr_in addr;
} sock_t;

static void sock_open(sock_t *s)
{
    socklen_t len = sizeof s->addr;

    memset(&s->addr, 0, sizeof s->addr);
    s->fd = net_open("127.0.0.1", 0);
    CHECK(s->fd >= 0);
    CHECK(getsockname(s->fd, (struct sockaddr *)&s->addr, &len) == 0);
    s->port = ntohs(s->addr.sin_port);
}

static void sock_close(sock_t *s)
{
    net_close(s->fd);
    s->fd = -1;
}

/* Waits for one datagram on fd and decodes it; false on timeout or a malformed datagram. */
static bool recv_msg(int fd, msg_t *out, struct sockaddr_in *from, int timeout_ms)
{
    uint8_t buf[MAX_DATAGRAM + 1];
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};

    if (poll(&p, 1, timeout_ms) <= 0)
        return false;
    ssize_t len = net_recv(fd, buf, sizeof buf, from);
    if (len < 0)
        return false;
    return proto_decode(buf, (size_t)len, out) == PROTO_OK;
}

static bool silent(const sock_t *s) /* true when nothing is waiting on the socket */
{
    struct pollfd p = {.fd = s->fd, .events = POLLIN, .revents = 0};
    return poll(&p, 1, QUIET_MS) == 0;
}

static void make_node(node_t *n, uint32_t id, uint32_t fanout)
{
    config_t cfg;

    config_defaults(&cfg);
    cfg.fanout = fanout;
    cfg.bind_addr.sin_family = AF_INET;
    cfg.bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cfg.bind_addr.sin_port = 0;
    CHECK(node_init(n, &cfg, id, ROLE_MIXER) == 0);
}

static void add_member(node_t *n, uint32_t id, uint16_t port, uint8_t state)
{
    member_t m;

    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(INADDR_LOOPBACK);
    m.port = port;
    m.role = ROLE_OVEN;
    m.state = state;
    m.incarnation = 1;
    m.heartbeat = 1;
    m.last_update_ms = now_ms();
    CHECK(mem_insert(n, &m) == 0);
}

static member_t entry(uint32_t id, uint16_t port, uint32_t hb)
{
    member_t m;

    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(INADDR_LOOPBACK);
    m.port = port;
    m.role = ROLE_OVEN;
    m.state = STATE_ALIVE;
    m.incarnation = 1;
    m.heartbeat = hb;
    return m;
}

static void set_msg(msg_t *m, uint8_t type, uint32_t sender, const member_t *e, uint16_t cnt)
{
    memset(m, 0, sizeof *m);
    m->type = type;
    m->sender_id = sender;
    m->count = cnt;
    for (uint16_t i = 0; i < cnt; i++)
        m->entries[i] = e[i];
}

static uint64_t total_sent(const node_t *n)
{
    uint64_t sum = 0;

    for (size_t t = 0; t <= MSG_TYPE_MAX; t++)
        sum += n->stats.sent[t];
    return sum;
}

static bool has_id(const msg_t *m, uint32_t id)
{
    for (uint16_t i = 0; i < m->count; i++)
        if (m->entries[i].node_id == id)
            return true;
    return false;
}

static bool unique_ids(const msg_t *m)
{
    for (uint16_t i = 0; i < m->count; i++)
        for (uint16_t j = (uint16_t)(i + 1); j < m->count; j++)
            if (m->entries[i].node_id == m->entries[j].node_id)
                return false;
    return true;
}

static uint64_t id_mask(const msg_t *m) /* bit per node_id < 64, enough for the ids used here */
{
    uint64_t mask = 0;

    for (uint16_t i = 0; i < m->count; i++)
        if (m->entries[i].node_id < 64)
            mask |= 1ull << m->entries[i].node_id;
    return mask;
}

/* ---- stderr capture (LOG_* writes to fd 2, D9) -------------------------------------- */

static int cap_saved = -1;
static int cap_fd = -1;

static void cap_begin(void)
{
    FILE *f = tmpfile();

    CHECK(f != NULL);
    cap_fd = dup(fileno(f));
    fclose(f);
    cap_saved = dup(STDERR_FILENO);
    CHECK(dup2(cap_fd, STDERR_FILENO) >= 0);
    log_init(LOG_INFO, 0);
}

static void cap_end(char *out, size_t cap)
{
    ssize_t got;

    CHECK(dup2(cap_saved, STDERR_FILENO) >= 0); /* restore first: CHECK failures print to stderr */
    close(cap_saved);
    log_init(LOG_WARN, 0);
    lseek(cap_fd, 0, SEEK_SET);
    got = read(cap_fd, out, cap - 1);
    out[got > 0 ? got : 0] = '\0';
    close(cap_fd);
}

static size_t count_str(const char *hay, const char *needle)
{
    size_t cnt = 0;
    size_t len = strlen(needle);

    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + len, needle))
        cnt++;
    return cnt;
}

/* ---- gossip_round ------------------------------------------------------------------- */

static void test_round_no_candidates(void)
{
    static node_t n;
    make_node(&n, 100, 3);

    uint64_t before = now_ms();
    gossip_round(&n); /* alone in the network: not an error, but F4 still holds */
    CHECK_EQ_U64(n.self.heartbeat, 1);
    CHECK(n.self.last_update_ms >= before);
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 2);
    CHECK_EQ_U64(total_sent(&n), 0);

    add_member(&n, 5, 9, STATE_DEAD); /* DEAD / LEFT are not candidates either */
    add_member(&n, 6, 9, STATE_LEFT);
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 3);
    CHECK_EQ_U64(total_sent(&n), 0);
    node_shutdown(&n);
}

static void test_round_frozen(void)
{
    static node_t n;
    sock_t peer;
    msg_t m;
    struct sockaddr_in from;

    make_node(&n, 100, 3);
    sock_open(&peer);
    add_member(&n, 1, peer.port, STATE_ALIVE);

    n.frozen = true; /* D10: no heartbeat, no sending */
    uint64_t last = n.self.last_update_ms;
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 0);
    CHECK_EQ_U64(n.self.last_update_ms, last);
    CHECK_EQ_U64(total_sent(&n), 0);
    CHECK(silent(&peer));

    n.frozen = false; /* `recover`: the round works again */
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 1);
    CHECK(recv_msg(peer.fd, &m, &from, WAIT_MS));
    CHECK(m.type == MSG_GOSSIP);

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_round_fanout_limit(void)
{
    static node_t n;
    sock_t peers[5];
    size_t got = 0;

    make_node(&n, 100, 3);
    for (uint32_t i = 0; i < 5; i++) {
        sock_open(&peers[i]);
        add_member(&n, i + 1, peers[i].port, STATE_ALIVE);
    }

    gossip_round(&n);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 3); /* exactly FANOUT, not N */
    CHECK_EQ_U64(total_sent(&n), 3);
    CHECK(n.stats.bytes_sent > 0);

    for (size_t i = 0; i < 5; i++) {
        msg_t m;
        struct sockaddr_in from;

        if (!recv_msg(peers[i].fd, &m, &from, i == 0 ? WAIT_MS : QUIET_MS))
            continue;
        got++;
        CHECK(m.type == MSG_GOSSIP);
        CHECK_EQ_U64(m.sender_id, 100);
        CHECK_EQ_U64(m.count, 6); /* self + 5 members all fit */
        CHECK_EQ_U64(m.entries[0].node_id, 100); /* out[0] is always self */
        CHECK_EQ_U64(m.entries[0].heartbeat, 1);
        CHECK_EQ_U64(m.entries[0].port, n.self.port);
        CHECK(unique_ids(&m));
        CHECK(!recv_msg(peers[i].fd, &m, &from, QUIET_MS)); /* one datagram per neighbour */
    }
    CHECK_EQ_U64(got, 3); /* three different neighbours, matching the counter */

    for (size_t i = 0; i < 5; i++)
        sock_close(&peers[i]);
    node_shutdown(&n);
}

static void test_round_fewer_candidates(void)
{
    static node_t n;
    sock_t peers[2];

    make_node(&n, 100, 3); /* FANOUT > candidates: everyone gets one, not an error */
    for (uint32_t i = 0; i < 2; i++) {
        sock_open(&peers[i]);
        add_member(&n, i + 1, peers[i].port, STATE_ALIVE);
    }
    gossip_round(&n);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 2);
    for (size_t i = 0; i < 2; i++) {
        msg_t m;
        struct sockaddr_in from;

        CHECK(recv_msg(peers[i].fd, &m, &from, WAIT_MS));
        CHECK(m.type == MSG_GOSSIP);
    }
    for (size_t i = 0; i < 2; i++)
        sock_close(&peers[i]);
    node_shutdown(&n);
}

static void test_round_candidate_states(void)
{
    static node_t n;
    sock_t s[4]; /* ALIVE, SUSPECT, DEAD, LEFT */
    const uint8_t st[4] = {STATE_ALIVE, STATE_SUSPECT, STATE_DEAD, STATE_LEFT};

    make_node(&n, 100, 10);
    for (uint32_t i = 0; i < 4; i++) {
        sock_open(&s[i]);
        add_member(&n, i + 1, s[i].port, st[i]);
    }
    gossip_round(&n);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 2); /* SUSPECT stays a candidate, DEAD/LEFT do not */

    for (size_t i = 0; i < 4; i++) {
        msg_t m;
        struct sockaddr_in from;

        if (i < 2) {
            CHECK(recv_msg(s[i].fd, &m, &from, WAIT_MS));
            CHECK_EQ_U64(m.count, 5);   /* the sample itself carries DEAD (include_dead) ... */
            CHECK(has_id(&m, 3));
            CHECK(has_id(&m, 4));       /* ... and LEFT, so that LEAVE keeps spreading */
        } else {
            CHECK(silent(&s[i]));
        }
    }
    for (size_t i = 0; i < 4; i++)
        sock_close(&s[i]);
    node_shutdown(&n);
}

static void test_round_counters_and_loss(void)
{
    static node_t n;
    sock_t peers[3];

    make_node(&n, 100, 3);
    for (uint32_t i = 0; i < 3; i++) {
        sock_open(&peers[i]);
        add_member(&n, i + 1, peers[i].port, STATE_ALIVE);
    }
    gossip_round(&n);
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 2);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 6);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP_REPLY], 0);

    n.cfg.loss = 1.0; /* --loss hook: dropped datagrams are not counted as sent (D12) */
    gossip_round(&n);
    CHECK_EQ_U64(n.self.heartbeat, 3); /* the heartbeat does not depend on delivery */
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 6);
    CHECK_EQ_U64(n.stats.dropped_loss, 3);

    for (size_t i = 0; i < 3; i++)
        sock_close(&peers[i]);
    node_shutdown(&n);
}

static void test_round_big_table(void)
{
    static node_t n;
    sock_t s[3];
    msg_t m[3];
    uint64_t mask[3];
    size_t total = CAP < 40 ? CAP : 40;

    if (total <= MAX_ENTRIES_MSG) /* the whole table fits in one message: nothing to sample */
        return;

    rng_seed(7);
    make_node(&n, 100, MAX_NODES); /* everybody is a target */
    for (uint32_t id = 1; id <= total; id++) {
        uint16_t port = 9;

        if (id <= 3) {
            sock_open(&s[id - 1]);
            port = s[id - 1].port;
        }
        add_member(&n, id, port, STATE_ALIVE);
    }
    gossip_round(&n);

    for (size_t i = 0; i < 3; i++) {
        struct sockaddr_in from;

        CHECK(recv_msg(s[i].fd, &m[i], &from, WAIT_MS));
        CHECK(m[i].type == MSG_GOSSIP);
        CHECK_EQ_U64(m[i].count, MAX_ENTRIES_MSG); /* capped, not overflowed */
        CHECK_EQ_U64(m[i].entries[0].node_id, 100);
        CHECK(unique_ids(&m[i]));
        for (uint16_t k = 1; k < m[i].count; k++)
            CHECK(m[i].entries[k].node_id >= 1 && m[i].entries[k].node_id <= total);
        mask[i] = id_mask(&m[i]);
    }
    /* D15: a new sample per neighbour, so the neighbours do not all see the same subset. */
    CHECK(mask[0] != mask[1] || mask[1] != mask[2]);

    for (size_t i = 0; i < 3; i++)
        sock_close(&s[i]);
    node_shutdown(&n);
}

/* ---- gossip_on_message ---------------------------------------------------------------- */

static void test_on_gossip_replies_to_source(void)
{
    static node_t n;
    sock_t peer, decoy;
    msg_t in, out;
    struct sockaddr_in from;

    make_node(&n, 100, 3);
    sock_open(&peer);
    sock_open(&decoy);

    /* The record of sender 7 claims decoy's port; the datagram really came from `peer` (D6). */
    member_t e[3] = {entry(7, decoy.port, 5), entry(8, 9, 2), entry(9, 9, 3)};
    set_msg(&in, MSG_GOSSIP, 7, e, 3);
    gossip_on_message(&n, &in, &peer.addr);

    CHECK_EQ_U64(n.count, 3);
    CHECK(mem_find(&n, 7) != NULL && mem_find(&n, 8) != NULL && mem_find(&n, 9) != NULL);
    CHECK_EQ_U64(mem_find(&n, 7)->heartbeat, 5);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP_REPLY], 1);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP], 0);
    CHECK_EQ_U64(total_sent(&n), 1);

    CHECK(recv_msg(peer.fd, &out, &from, WAIT_MS)); /* the pull answer goes to the source address */
    CHECK(out.type == MSG_GOSSIP_REPLY);
    CHECK_EQ_U64(out.sender_id, 100);
    CHECK_EQ_U64(out.entries[0].node_id, 100);
    CHECK_EQ_U64(out.count, 4); /* fresh sample taken after the merge: self + 7, 8, 9 */
    CHECK(has_id(&out, 7) && has_id(&out, 8) && has_id(&out, 9));
    CHECK(silent(&peer));
    CHECK(silent(&decoy)); /* never to the ip:port written inside a record */

    sock_close(&peer);
    sock_close(&decoy);
    node_shutdown(&n);
}

static void test_on_gossip_one_reply_each(void)
{
    static node_t n;
    sock_t peer;
    msg_t in, out;
    struct sockaddr_in from;

    make_node(&n, 100, 3);
    sock_open(&peer);

    /* A record about self is ignored ("I decide about myself"); a repeat adds nothing, yet
     * every received MSG_GOSSIP still gets exactly one MSG_GOSSIP_REPLY. */
    member_t e[2] = {entry(7, peer.port, 5), entry(100, 1, 9999)};
    set_msg(&in, MSG_GOSSIP, 7, e, 2);
    for (int i = 0; i < 3; i++)
        gossip_on_message(&n, &in, &peer.addr);

    CHECK_EQ_U64(n.count, 1);
    CHECK_EQ_U64(n.self.heartbeat, 0);
    CHECK_EQ_U64(n.self.port, ntohs(n.cfg.bind_addr.sin_port));
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP_REPLY], 3);
    for (int i = 0; i < 3; i++) {
        CHECK(recv_msg(peer.fd, &out, &from, WAIT_MS));
        CHECK(out.type == MSG_GOSSIP_REPLY);
    }
    CHECK(silent(&peer));

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_on_reply_no_answer(void)
{
    static node_t n;
    sock_t peer;
    msg_t in;

    make_node(&n, 100, 3);
    sock_open(&peer);

    member_t e[3] = {entry(7, peer.port, 5), entry(8, 9, 2), entry(9, 9, 3)};
    set_msg(&in, MSG_GOSSIP_REPLY, 7, e, 3);
    gossip_on_message(&n, &in, &peer.addr);

    CHECK_EQ_U64(n.count, 3); /* merged ... */
    CHECK_EQ_U64(total_sent(&n), 0); /* ... but never answered (D6: no ping-pong) */
    CHECK(silent(&peer));

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_joined_from_seed_only(void)
{
    static node_t n;
    sock_t seed, other;
    msg_t reply, gossip;
    struct sockaddr_in off_port;

    make_node(&n, 100, 3);
    sock_open(&seed);
    sock_open(&other);
    n.cfg.has_seed = true;
    n.cfg.seed_addr = seed.addr;

    member_t e[1] = {entry(7, seed.port, 1)};
    set_msg(&reply, MSG_GOSSIP_REPLY, 7, e, 1);
    set_msg(&gossip, MSG_GOSSIP, 7, e, 1);

    gossip_on_message(&n, &reply, &other.addr); /* a reply, but not from the seed */
    CHECK(!n.joined);
    off_port = seed.addr; /* same ip, different port: not the seed */
    off_port.sin_port = htons((uint16_t)(seed.port == 65535 ? 1024 : seed.port + 1));
    gossip_on_message(&n, &reply, &off_port);
    CHECK(!n.joined);
    gossip_on_message(&n, &gossip, &seed.addr); /* from the seed, but a push, not an answer */
    CHECK(!n.joined);
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP_REPLY], 1); /* it is answered as usual */

    gossip_on_message(&n, &reply, &seed.addr); /* the first reply from the seed */
    CHECK(n.joined);
    gossip_on_message(&n, &reply, &other.addr);
    CHECK(n.joined); /* stays joined */

    static node_t m; /* no seed configured: nobody can set joined */
    make_node(&m, 101, 3);
    gossip_on_message(&m, &reply, &seed.addr);
    CHECK(!m.joined);

    sock_close(&seed);
    sock_close(&other);
    node_shutdown(&n);
    node_shutdown(&m);
}

static void test_unknown_type_ignored(void)
{
    static node_t n;
    sock_t peer;
    msg_t in;
    const uint8_t types[] = {0, 5, 99, 255};

    make_node(&n, 100, 3);
    sock_open(&peer);
    member_t e[1] = {entry(7, peer.port, 1)};
    for (size_t i = 0; i < sizeof types; i++) {
        set_msg(&in, types[i], 7, e, 1);
        gossip_on_message(&n, &in, &peer.addr);
    }
    CHECK_EQ_U64(n.count, 0);
    CHECK_EQ_U64(total_sent(&n), 0);
    CHECK(silent(&peer));

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_frozen_sends_no_reply(void)
{
    static node_t n;
    sock_t peer;
    msg_t in;

    make_node(&n, 100, 3);
    sock_open(&peer);
    n.frozen = true; /* D10: node_send is silent, gossip_on_message must not crash or leak a datagram */
    member_t e[1] = {entry(7, peer.port, 1)};
    set_msg(&in, MSG_GOSSIP, 7, e, 1);
    gossip_on_message(&n, &in, &peer.addr);
    CHECK_EQ_U64(total_sent(&n), 0);
    CHECK(silent(&peer));

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_table_full_rejected(void)
{
    static node_t n;
    sock_t peer;
    msg_t in, out;
    struct sockaddr_in from;
    static char log[8192];

    make_node(&n, 100, 3);
    sock_open(&peer);
    for (uint32_t id = 1; id <= CAP; id++)
        add_member(&n, id, 9, STATE_ALIVE);

    member_t e[1] = {entry(5000, 9, 1)};
    set_msg(&in, MSG_GOSSIP, 5000, e, 1);
    cap_begin();
    gossip_on_message(&n, &in, &peer.addr);
    cap_end(log, sizeof log);

    CHECK_EQ_U64(n.count, CAP);
    CHECK(mem_find(&n, 5000) == NULL);
    CHECK(n.stats.dropped_table_full >= 1);
    CHECK_EQ_U64(count_str(log, "discovered member"), 0); /* MEM_REJECTED is not "discovered" */
    CHECK_EQ_U64(n.stats.sent[MSG_GOSSIP_REPLY], 1); /* the pull answer still goes out */
    CHECK(recv_msg(peer.fd, &out, &from, WAIT_MS));
    CHECK(out.type == MSG_GOSSIP_REPLY);
    CHECK_EQ_U64(out.count, CAP + 1 < MAX_ENTRIES_MSG ? CAP + 1 : MAX_ENTRIES_MSG); /* capped */
    CHECK(unique_ids(&out));

    sock_close(&peer);
    node_shutdown(&n);
}

static void test_discovered_member_log(void)
{
    static node_t n;
    sock_t peer;
    msg_t gossip, reply;
    static char log[8192];

    make_node(&n, 100, 3);
    sock_open(&peer);

    member_t e[3] = {entry(7, peer.port, 5), entry(8, 9, 2), entry(100, 1, 9)};
    member_t f[2] = {entry(8, 9, 3), entry(9, 9, 1)};
    set_msg(&gossip, MSG_GOSSIP, 7, e, 3);
    set_msg(&reply, MSG_GOSSIP_REPLY, 7, f, 2);

    cap_begin();
    gossip_on_message(&n, &gossip, &peer.addr); /* 7 and 8 are new, self is not */
    gossip_on_message(&n, &gossip, &peer.addr); /* nothing new */
    gossip_on_message(&n, &reply, &peer.addr);  /* 9 is new, 8 only progresses */
    cap_end(log, sizeof log);

    CHECK_EQ_U64(count_str(log, "discovered member"), 3);
    CHECK(strstr(log, "discovered member 7 OVEN at 127.0.0.1:") != NULL);
    CHECK(strstr(log, "discovered member 9 OVEN at 127.0.0.1:") != NULL);
    CHECK(strstr(log, "discovered member 100") == NULL);
    /* the state change is logged once, by mem_*, and not duplicated by gossip.c (D18) */
    CHECK_EQ_U64(count_str(log, "NEW -> ALIVE"), 3);

    sock_close(&peer);
    node_shutdown(&n);
}

/* ---- three nodes, real sockets (acceptance: all see all, counters match) ------------------ */

/* Delivers everything that is queued on the nodes' sockets until the network is quiet.
 * Stands in for node_run's receive path.  Returns how many MSG_GOSSIP / MSG_GOSSIP_REPLY
 * were handled. */
static void pump(node_t *ns, size_t cnt, uint64_t *gossips, uint64_t *replies)
{
    bool activity;

    do {
        activity = false;
        for (size_t i = 0; i < cnt; i++) {
            msg_t m;
            struct sockaddr_in from;

            while (recv_msg(ns[i].fd, &m, &from, 10)) {
                activity = true;
                if (m.type == MSG_GOSSIP)
                    (*gossips)++;
                else if (m.type == MSG_GOSSIP_REPLY)
                    (*replies)++;
                gossip_on_message(&ns[i], &m, &from);
            }
        }
    } while (activity);
}

static void test_three_nodes_converge(void)
{
    static node_t ns[3]; /* ids 1, 2, 3; 1 plays the seed that 2 and 3 already reached */
    uint64_t gossips = 0, replies = 0;
    unsigned rounds = 0;
    bool all = false;

    for (uint32_t i = 0; i < 3; i++)
        make_node(&ns[i], i + 1, 3);
    add_member(&ns[1], 1, ns[0].self.port, STATE_ALIVE);
    add_member(&ns[2], 1, ns[0].self.port, STATE_ALIVE);

    while (rounds < 5 && !all) { /* 5 rounds == the 5 s of the acceptance criterion */
        rounds++;
        for (size_t i = 0; i < 3; i++)
            gossip_round(&ns[i]);
        pump(ns, 3, &gossips, &replies);
        all = ns[0].count == 2 && ns[1].count == 2 && ns[2].count == 2;
    }
    CHECK(all);

    uint64_t sent_gossip = 0, sent_reply = 0;
    for (size_t i = 0; i < 3; i++) {
        CHECK_EQ_U64(ns[i].self.heartbeat, rounds); /* F4: one increment per round */
        CHECK(ns[i].stats.sent[MSG_GOSSIP] <= (uint64_t)rounds * 3); /* <= FANOUT per round */
        sent_gossip += ns[i].stats.sent[MSG_GOSSIP];
        sent_reply += ns[i].stats.sent[MSG_GOSSIP_REPLY];
        for (uint32_t id = 1; id <= 3; id++) {
            if (id == ns[i].self.node_id)
                continue;
            member_t *peer = mem_find(&ns[i], id);
            CHECK(peer != NULL);
            if (peer != NULL)
                CHECK(peer->state == STATE_ALIVE);
        }
    }
    CHECK(mem_find(&ns[1], 1) != NULL && mem_find(&ns[1], 1)->heartbeat >= 1);
    CHECK(gossips > 0);
    CHECK_EQ_U64(sent_gossip, gossips);  /* loopback loses nothing */
    CHECK_EQ_U64(sent_reply, gossips);   /* exactly one reply per received MSG_GOSSIP ... */
    CHECK_EQ_U64(replies, gossips);      /* ... and a reply is never answered (no ping-pong) */

    for (size_t i = 0; i < 3; i++)
        node_shutdown(&ns[i]);
}

int main(void)
{
    log_init(LOG_WARN, 0); /* node_init and mem_* log at INFO; cap_begin raises it where needed */
    rng_seed(42);

    test_round_no_candidates();
    test_round_frozen();
    test_round_fanout_limit();
    test_round_fewer_candidates();
    test_round_candidate_states();
    test_round_counters_and_loss();
    test_round_big_table();
    test_on_gossip_replies_to_source();
    test_on_gossip_one_reply_each();
    test_on_reply_no_answer();
    test_joined_from_seed_only();
    test_unknown_type_ignored();
    test_frozen_sends_no_reply();
    test_table_full_rejected();
    test_discovered_member_log();
    test_three_nodes_converge();

    TEST_MAIN_END();
}
