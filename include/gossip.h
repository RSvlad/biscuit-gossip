/*
 * gossip.h - gossip round, message handling, LEAVE (spec 7.1, 7.4, 10).
 *
 * Owner: Dev B (issues #8, #11).  node.c already calls these; until the real
 * implementations land, src/gossip.c contains minimal stubs.  Everything that
 * goes out is sent through node_send() (node.h).
 */
#ifndef GOSSIP_H
#define GOSSIP_H

#include <netinet/in.h>

#include "common.h"
#include "proto.h"

/* One round (push): self.heartbeat++, pick up to cfg.fanout peers, send MSG_GOSSIP (#8). */
void gossip_round(node_t *n);

/* Handles one decoded datagram from another node: merge + reply (pull) (#8). */
void gossip_on_message(node_t *n, const msg_t *m, const struct sockaddr_in *from);

/* Orderly exit: MSG_LEAVE (state LEFT) to up to fanout peers; called once by node_run (#11). */
void gossip_leave(node_t *n);

#endif /* GOSSIP_H */
