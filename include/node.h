/*
 * node.h - event loop, timers, the single send point (issue #6; D9..D13).
 *
 * One process == one node: a single thread blocked only in poll() on the UDP
 * socket and stdin.  node.c is the only module (besides net.c) that touches
 * the socket; everyone else sends through node_send().  No dynamic allocation.
 */
#ifndef NODE_H
#define NODE_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common.h"

/*
 * Prepares *n (zeroed first): self (id, role, bind address as ip/port,
 * STATE_ALIVE, heartbeat 0, incarnation = (uint32_t)time(NULL), D5), mem_init,
 * net_open on cfg->bind_addr, next_gossip_at = now + gossip_ms,
 * next_failure_check_at = now + gossip_ms, running = true.
 * If the bind port is 0 (tests), the port chosen by the kernel is stored in
 * self.port and cfg.bind_addr.  Returns 0, or -1 (reason logged at ERROR; the
 * socket is not left open).
 */
int node_init(node_t *n, const config_t *cfg, uint32_t id, uint8_t role);

/*
 * Runs the loop until n->running is false (`quit`, `leave`, SIGINT/SIGTERM).
 * Installs the signal handlers (sigaction): SIGINT/SIGTERM request an orderly
 * leave, SIGPIPE is ignored.  If n->leaving is set on exit, gossip_leave() is
 * called once before returning.
 */
void node_run(node_t *n);

/* Closes the socket.  Nothing is freed because nothing was allocated. */
void node_shutdown(node_t *n);

/*
 * The ONLY place that sends (D12).  Encodes with proto_encode, applies --loss,
 * hands the datagram to net_send and updates stats.
 *   - frozen (D10): nothing is sent, returns 0 - unless n->leaving is set.
 *   - dropped by --loss: stats.dropped_loss++, not counted as sent, returns 0.
 *   - otherwise returns the number of bytes sent, or -1 on an encode/send error
 *     (nothing is counted then).
 */
ssize_t node_send(node_t *n, const struct sockaddr_in *to, uint8_t type,
                  const member_t *entries, uint16_t count);

/* Same effect as the `leave` command: leaving = true, running = false (D13). */
void node_request_leave(node_t *n);

#endif /* NODE_H */
