/*
 * net.h - thin wrapper around a POSIX UDP socket (spec section 10, D8).
 *
 * Knows nothing about the protocol, the table or node_t.  Only net.c and
 * node.c (#6) touch the socket.  No global state, no dynamic allocation.
 */
#ifndef NET_H
#define NET_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "common.h"

/*
 * Opens a non-blocking UDP/IPv4 socket bound to ip:port.  Returns the fd, or
 * -1 (with errno preserved and the reason logged at ERROR).
 *   - `ip` must be a concrete dotted-quad address; "0.0.0.0" is rejected (D8)
 *     because the address is published in membership records.
 *   - SO_REUSEADDR/SO_REUSEPORT are deliberately NOT set (D8): a second
 *     process on the same port fails with EADDRINUSE.
 *   - port 0 asks the kernel for an ephemeral port (useful in tests).
 */
int net_open(const char *ip, uint16_t port);

/*
 * Sends one datagram.  Returns the number of bytes sent, or -1.  Transient
 * conditions (EAGAIN, ENOBUFS, ECONNREFUSED, EHOSTUNREACH, ...) are not fatal:
 * the datagram is simply lost, which gossip tolerates; the caller carries on.
 * EINTR is retried once.
 */
ssize_t net_send(int fd, const struct sockaddr_in *to, const uint8_t *buf, size_t len);

/*
 * Receives one datagram without blocking.  Returns its length (>= 0), or -1
 * with errno set; EAGAIN/EWOULDBLOCK/EINTR are normal (nothing to read) and
 * not logged.  *from is filled whenever the result is >= 0.
 *
 * The caller must pass a buffer of MAX_DATAGRAM + 1 bytes (cap == MAX_DATAGRAM + 1):
 * a result > MAX_DATAGRAM then means "oversized datagram" and is treated as
 * malformed (the kernel silently truncates anything beyond cap).
 */
ssize_t net_recv(int fd, uint8_t *buf, size_t cap, struct sockaddr_in *from);

/*
 * Strict "a.b.c.d:port": dotted-quad IPv4, port in [1, 65535], no spaces, no
 * trailing garbage.  Returns 0 and fills *out (sin_family, sin_addr, sin_port
 * in network order), or -1.  Used for --bind and --seed.
 */
int net_parse_addr(const char *s, struct sockaddr_in *out);

/* Builds a sockaddr_in from a membership record (ip as stored, port via htons). */
void net_addr_from_member(const member_t *m, struct sockaddr_in *out);

/* Closes fd; a negative fd is ignored. */
void net_close(int fd);

#endif /* NET_H */
