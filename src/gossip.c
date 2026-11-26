/*
 * gossip.c - owner: Dev B.  Gossip round and message handling (issue #8, spec 7.1, 7.2).
 *
 * Push on every round to up to cfg.fanout random peers, pull through the
 * MSG_GOSSIP_REPLY that every MSG_GOSSIP receives (D6).  Single thread, no
 * blocking calls, no malloc: all buffers are on the stack.
 * JOIN (#9) and LEAVE (#11) are still stubs at the bottom of this file.
 */
#include "gossip.h"

#include <arpa/inet.h>
#include <stdio.h>

#include "membership.h"
#include "net.h"
#include "node.h"
#include "util.h"

/* ---- helpers ------------------------------------------------------------------ */

/* Formats the address of a record or datagram for logs; never fails. */
static void format_addr(const struct in_addr *ip, unsigned port, char *out, size_t cap)
{
    char host[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, ip, host, sizeof host) == NULL) {
        host[0] = '?';
        host[1] = '\0';
    }
    snprintf(out, cap, "%s:%u", host, port);
}

/* True when the datagram came from the configured seed (same address and port). */
static bool from_seed(const node_t *n, const struct sockaddr_in *from)
{
    return n->cfg.has_seed && from->sin_addr.s_addr == n->cfg.seed_addr.sin_addr.s_addr &&
           from->sin_port == n->cfg.seed_addr.sin_port;
}

/*
 * Merges every record of m into the table (spec 7.2 lives in mem_upsert).
 * MEM_ADDED is logged here; the state transitions and MEM_REJECTED are
 * already logged by mem_* (D18), so nothing is duplicated.
 */
static void merge_entries(node_t *n, const msg_t *m)
{
    uint64_t now = now_ms();

    for (uint16_t i = 0; i < m->count; i++) {
        const member_t *e = &m->entries[i];
        int rc = mem_upsert(n, e, now);

        if (rc == MEM_ADDED) {
            char addr[INET_ADDRSTRLEN + 8];

            format_addr(&e->ip, (unsigned)e->port, addr, sizeof addr);
            LOG_I("discovered member %u %s at %s", (unsigned)e->node_id, role_name(e->role), addr);
        }
    }
}

/* Answers to the source address of the datagram with a fresh sample (D6, D15). */
static void send_sample(node_t *n, const struct sockaddr_in *to, uint8_t type)
{
    member_t buf[MAX_ENTRIES_MSG];
    size_t cnt = mem_sample(n, buf, MAX_ENTRIES_MSG, true);

    node_send(n, to, type, buf, (uint16_t)cnt);
}

/* ---- round (spec 7.1, F3, F4) ---------------------------------------------------- */

void gossip_round(node_t *n)
{
    member_t peers[MAX_NODES]; /* mem_pick_peers writes at most min(k, candidates) records */
    member_t buf[MAX_ENTRIES_MSG];
    size_t k = n->cfg.fanout;

    if (n->frozen) { /* D10: a frozen node does not even count heartbeats */
        return;
    }

    n->self.heartbeat++; /* F4 */
    n->self.last_update_ms = now_ms();

    if (k > MAX_NODES) { /* cannot have more candidates than that anyway */
        k = MAX_NODES;
    }
    size_t npeers = mem_pick_peers(n, peers, k);
    if (npeers == 0) {
        LOG_D("gossip round: no candidates, skipping");
        return;
    }

    for (size_t i = 0; i < npeers; i++) {
        struct sockaddr_in to;

        net_addr_from_member(&peers[i], &to);
        /* A new sample per neighbour: better coverage when the table exceeds one message (D15). */
        size_t cnt = mem_sample(n, buf, MAX_ENTRIES_MSG, true);
        node_send(n, &to, MSG_GOSSIP, buf, (uint16_t)cnt);
    }
}

/* ---- incoming messages (spec 7.1, D6) ---------------------------------------------- */

void gossip_on_message(node_t *n, const msg_t *m, const struct sockaddr_in *from)
{
    switch (m->type) {
    case MSG_GOSSIP:
        merge_entries(n, m);
        send_sample(n, from, MSG_GOSSIP_REPLY); /* pull; the reply is never answered (D6) */
        break;

    case MSG_GOSSIP_REPLY:
        merge_entries(n, m); /* no answer here, otherwise ping-pong (D6) */
        if (!n->joined && from_seed(n, from)) {
            char addr[INET_ADDRSTRLEN + 8];

            n->joined = true; /* first valid answer from the seed (#9) */
            format_addr(&from->sin_addr, (unsigned)ntohs(from->sin_port), addr, sizeof addr);
            LOG_I("joined via seed %s", addr);
        }
        break;

    case MSG_JOIN:
        gossip_handle_join(n, m, from); /* #9 */
        break;

    case MSG_LEAVE:
        gossip_handle_leave(n, m); /* #11 */
        break;

    default:
        LOG_D("ignoring message of unexpected type %u", (unsigned)m->type);
        break;
    }
}

/* ---- STUBS: replaced by their own issues -------------------------------------------- */

/* STUB until #9: the real handler adds the joiner and answers MSG_GOSSIP_REPLY. */
void gossip_handle_join(node_t *n, const msg_t *m, const struct sockaddr_in *from)
{
    (void)n;
    (void)m;
    (void)from;
}

/* STUB until #11: the real handler merges the single STATE_LEFT record. */
void gossip_handle_leave(node_t *n, const msg_t *m)
{
    (void)n;
    (void)m;
}

/* STUB until #11: the real function sends MSG_LEAVE to up to fanout peers. */
void gossip_leave(node_t *n)
{
    (void)n;
}
