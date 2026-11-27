/*
 * gossip.c - owner: Dev B.  Gossip round, message handling and JOIN
 * (issues #8, #9; spec 7.1, 7.2, 7.4).
 *
 * Push on every round to up to cfg.fanout random peers, pull through the
 * MSG_GOSSIP_REPLY that every MSG_GOSSIP and MSG_JOIN receives (D6).  Single
 * thread, no blocking calls, no malloc: all buffers are on the stack.
 * LEAVE (#11) is still a stub at the bottom of this file.
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

/* "discovered member" line for a MEM_ADDED result (#8); shared by GOSSIP/REPLY and JOIN. */
static void log_discovered(const member_t *e)
{
    char addr[INET_ADDRSTRLEN + 8];

    format_addr(&e->ip, (unsigned)e->port, addr, sizeof addr);
    LOG_I("discovered member %u %s at %s", (unsigned)e->node_id, role_name(e->role), addr);
}

/*
 * Merges every record of m into the table (spec 7.2 lives in mem_upsert).
 * MEM_ADDED is logged here as "discovered member" (required by #8); it adds
 * the address and deliberately sits next to the "NEW -> ALIVE" line that
 * mem_upsert logs through mem_log_transition, so a new member gives two INFO
 * lines.  Other state transitions and MEM_REJECTED are logged only by mem_*
 * (D18) and are not repeated here.
 */
static void merge_entries(node_t *n, const msg_t *m)
{
    uint64_t now = now_ms();

    for (uint16_t i = 0; i < m->count; i++) {
        const member_t *e = &m->entries[i];

        if (mem_upsert(n, e, now) == MEM_ADDED) {
            log_discovered(e);
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

    if (k == 0) { /* config_t has no validation of fanout yet: say the real cause */
        LOG_D("gossip round: fanout is 0, skipping");
        return;
    }
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

/* ---- JOIN (#9, spec 7.4, F2, D2, D17) ------------------------------------------------ */

/*
 * One MSG_JOIN with a single record (self) to the seed.  D2: the seed's id is
 * unknown, so it is NOT written into the table here (spec 7.4 says "self and
 * seed"); it arrives as the first record of the seed's MSG_GOSSIP_REPLY (D15).
 * The attempt counts even when --loss drops it or the node is frozen.
 */
void gossip_join(node_t *n)
{
    char addr[INET_ADDRSTRLEN + 8];

    if (!n->cfg.has_seed) {
        return;
    }
    node_send(n, &n->cfg.seed_addr, MSG_JOIN, &n->self, 1);
    n->join_tries++;
    format_addr(&n->cfg.seed_addr.sin_addr, (unsigned)ntohs(n->cfg.seed_addr.sin_port), addr,
                sizeof addr);
    LOG_I("join attempt %u/%u to %s", n->join_tries, (unsigned)JOIN_MAX_TRIES, addr);
}

/*
 * JOIN retry timer.  Gives up on the tick after the last attempt (so the last
 * JOIN still gets JOIN_RETRY_MS to be answered): exit_code = 3 and the loop
 * stops, without LEAVE because we were never in the network (D17).
 */
void gossip_tick_join(node_t *n, uint64_t now)
{
    if (!n->cfg.has_seed || n->joined || now < n->next_join_at) {
        return;
    }
    if (n->join_tries >= JOIN_MAX_TRIES) {
        LOG_E("seed unreachable after %u JOIN attempts, giving up", n->join_tries);
        n->exit_code = 3;
        n->running = false;
        return;
    }
    gossip_join(n);
    n->next_join_at = now + JOIN_RETRY_MS;
}

/*
 * The seed's side.  Always answers with a sample to `from` (D6), even when the
 * record was ignored or rejected: the joiner at least sees the network, and a
 * repeated JOIN (lost answer, --loss) is harmless.
 */
void gossip_handle_join(node_t *n, const msg_t *m, const struct sockaddr_in *from)
{
    if (m->count != 1) {
        LOG_D("ignoring MSG_JOIN with %u records", (unsigned)m->count);
        return;
    }

    const member_t *e = &m->entries[0];
    const member_t *old = mem_find(n, e->node_id);

    /* Same id, other address, and not a newer incarnation: mem_upsert will ignore it. */
    if (old != NULL && (old->ip.s_addr != e->ip.s_addr || old->port != e->port) &&
        e->incarnation <= old->incarnation) {
        char have[INET_ADDRSTRLEN + 8];
        char got[INET_ADDRSTRLEN + 8];

        format_addr(&old->ip, (unsigned)old->port, have, sizeof have);
        format_addr(&e->ip, (unsigned)e->port, got, sizeof got);
        LOG_W("id conflict: member %u is at %s, JOIN from %s", (unsigned)e->node_id, have, got);
    }

    if (mem_upsert(n, e, now_ms()) == MEM_ADDED) {
        log_discovered(e);
    }
    send_sample(n, from, MSG_GOSSIP_REPLY);
}

/* ---- STUBS: replaced by their own issues -------------------------------------------- */

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
