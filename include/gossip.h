/*
 * gossip.h - gossip round, message handling, JOIN, LEAVE (spec 7.1, 7.4, 10).
 *
 * Owner: Dev B (issues #8, #9, #11).  node.c already calls gossip_round,
 * gossip_on_message and gossip_leave.  Header-first: the JOIN (#9) and LEAVE
 * (#11) functions are declared here by #8 and implemented by their own issues;
 * until then src/gossip.c contains minimal stubs for the ones that
 * gossip_on_message() calls.  Everything that goes out is sent through
 * node_send() (node.h).
 */
#ifndef GOSSIP_H
#define GOSSIP_H

#include <netinet/in.h>
#include <stdint.h>

#include "common.h"
#include "proto.h"

/*
 * One round (push) (#8, F4, D10, D15).  While n->frozen nothing happens.
 * Otherwise self.heartbeat++ and self.last_update_ms = now, then up to
 * cfg.fanout peers are picked with mem_pick_peers() (ALIVE/SUSPECT, never
 * self) and each one gets its own fresh mem_sample() in a MSG_GOSSIP.  No
 * candidates (alone in the network, or not joined yet) is not an error: the
 * round is skipped quietly (LOG_D).  No blocking calls, no malloc.
 */
void gossip_round(node_t *n);

/*
 * Handles one decoded datagram from another node (#8, D6).  `from` is the
 * source address of the datagram; replies go there, never to the ip:port
 * carried inside a record.
 *   MSG_GOSSIP       merge every record, answer MSG_GOSSIP_REPLY with a fresh sample
 *   MSG_GOSSIP_REPLY merge every record, NO answer (no ping-pong); the first reply
 *                    that comes from cfg.seed_addr sets n->joined
 *   MSG_JOIN         gossip_handle_join()  (#9)
 *   MSG_LEAVE        gossip_handle_leave() (#11)
 * New members are logged at INFO ("discovered member"); state transitions are
 * logged by mem_* (D18), not here.
 */
void gossip_on_message(node_t *n, const msg_t *m, const struct sockaddr_in *from);

/* ---- JOIN (#9, spec 7.4): declared by #8, implemented in #9 -------------------- */

/*
 * Sends one MSG_JOIN (a single record: self) to cfg.seed_addr and counts it in
 * n->join_tries.  Does nothing without a seed.
 */
void gossip_join(node_t *n);

/*
 * Join retry timer, called from node_run when now >= n->next_join_at: while
 * cfg.has_seed && !n->joined, sends gossip_join() every JOIN_RETRY_MS up to
 * JOIN_MAX_TRIES times, then gives up (exit code 3, D17).
 */
void gossip_tick_join(node_t *n, uint64_t now_ms);

/*
 * The seed's side: a MSG_JOIN arrived.  Adds the joiner (mem_upsert) and answers
 * MSG_GOSSIP_REPLY with a fresh sample to `from` (D6).
 */
void gossip_handle_join(node_t *n, const msg_t *m, const struct sockaddr_in *from);

/* ---- LEAVE (#11): declared by #8, implemented in #11 --------------------------- */

/* Orderly exit: MSG_LEAVE (state LEFT) to up to fanout peers; called once by node_run (#11). */
void gossip_leave(node_t *n);

/* A MSG_LEAVE arrived: merge the single STATE_LEFT record.  No answer (D6). */
void gossip_handle_leave(node_t *n, const msg_t *m);

#endif /* GOSSIP_H */
