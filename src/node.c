/*
 * node.c - event loop, timers, lifecycle, the single send point (issue #6).
 *
 * Single thread, blocked only in poll().  No malloc, no sleep: everything
 * that waits is a poll() timeout computed from the timers (spec 4.1).
 */
#include "node.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "cli.h"
#include "failure.h"
#include "gossip.h"
#include "membership.h"
#include "net.h"
#include "proto.h"
#include "util.h"

#define MAX_POLL_MS         1000 /* upper bound for one poll() timeout */
#define MAX_DGRAMS_PER_ITER 64   /* so that the CLI is not starved by a flood */
#define STDIN_CHUNK         256

/* Set by the signal handler, turned into leaving/running in the loop. */
static volatile sig_atomic_t g_stop_requested;

static void on_stop_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static void install_signals(void)
{
    struct sigaction sa;

    g_stop_requested = 0;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop_signal;
    sa.sa_flags = 0; /* no SA_RESTART: poll() must return EINTR */
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        LOG_W("sigaction failed: %s", strerror(errno));
    }

    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) != 0) {
        LOG_W("sigaction(SIGPIPE) failed: %s", strerror(errno));
    }
}

/* ---- lifecycle ------------------------------------------------------------ */

int node_init(node_t *n, const config_t *cfg, uint32_t id, uint8_t role)
{
    char ip[INET_ADDRSTRLEN];
    struct sockaddr_in bound;
    socklen_t bound_len = sizeof bound;
    member_t self;
    uint16_t port;
    uint64_t now;

    if (n == NULL || cfg == NULL) {
        return -1;
    }
    if (role > ROLE_MONITOR) {
        LOG_E("invalid role %u", (unsigned)role);
        return -1;
    }
    if (cfg->gossip_ms == 0) {
        LOG_E("gossip period must be > 0 ms");
        return -1;
    }

    memset(n, 0, sizeof *n);
    n->cfg = *cfg;
    n->fd = -1;

    if (inet_ntop(AF_INET, &cfg->bind_addr.sin_addr, ip, sizeof ip) == NULL) {
        LOG_E("cannot format bind address: %s", strerror(errno));
        return -1;
    }
    port = ntohs(cfg->bind_addr.sin_port);

    n->fd = net_open(ip, port);
    if (n->fd < 0) {
        return -1; /* net_open logged the reason */
    }

    if (port == 0) { /* ephemeral port (tests): learn what the kernel gave us */
        if (getsockname(n->fd, (struct sockaddr *)&bound, &bound_len) != 0) {
            LOG_E("getsockname failed: %s", strerror(errno));
            net_close(n->fd);
            n->fd = -1;
            return -1;
        }
        n->cfg.bind_addr.sin_port = bound.sin_port;
        port = ntohs(bound.sin_port);
    }

    now = now_ms();

    memset(&self, 0, sizeof self);
    self.node_id = id;
    self.ip = cfg->bind_addr.sin_addr;
    self.port = port;
    self.role = role;
    self.state = STATE_ALIVE;
    self.incarnation = (uint32_t)time(NULL); /* D5 */
    self.heartbeat = 0;                      /* grows in gossip_round (#8) */
    self.last_update_ms = now;
    mem_init(n, &self);

    n->running = true;
    n->frozen = false;
    n->leaving = false;
    n->joined = false;
    n->join_tries = 0;
    n->next_gossip_at = now + n->cfg.gossip_ms;
    n->next_failure_check_at = now + n->cfg.gossip_ms;
    n->next_join_at = now; /* first JOIN right away, when a seed is configured */

    LOG_I("node %u %s listening on %s:%u (incarnation %u)", (unsigned)id, role_name(role), ip,
          (unsigned)port, (unsigned)n->self.incarnation);
    return 0;
}

void node_shutdown(node_t *n)
{
    if (n == NULL) {
        return;
    }
    net_close(n->fd);
    n->fd = -1;
}

void node_request_leave(node_t *n)
{
    n->leaving = true;
    n->running = false;
}

/* ---- sending (D10, D12) ------------------------------------------------------ */

ssize_t node_send(node_t *n, const struct sockaddr_in *to, uint8_t type, const member_t *entries,
                  uint16_t count)
{
    uint8_t buf[MAX_DATAGRAM];
    size_t len;
    ssize_t sent;

    if (n->frozen && !n->leaving) {
        return 0; /* D10: a frozen node is silent (leave has priority) */
    }

    len = proto_encode(buf, sizeof buf, type, n->self.node_id, entries, count);
    if (len == 0) {
        LOG_W("cannot encode message type %u with %u entries", (unsigned)type, (unsigned)count);
        return -1;
    }

    /* --loss hook (D12); the full simulation and logging are issue #14. */
    if (n->cfg.loss > 0.0 && rng_unit() < n->cfg.loss) {
        n->stats.dropped_loss++;
        return 0;
    }

    sent = net_send(n->fd, to, buf, len);
    if (sent < 0) {
        return -1; /* lost datagram; gossip tolerates it */
    }

    n->stats.sent[type]++; /* proto_encode accepted type, so it is in [1, MSG_TYPE_MAX] */
    n->stats.bytes_sent += (uint64_t)sent;
    return sent;
}

/* ---- timers ------------------------------------------------------------------ */

static bool join_active(const node_t *n)
{
    return n->cfg.has_seed && !n->joined && n->join_tries < JOIN_MAX_TRIES;
}

/* Milliseconds until the nearest timer, never negative, at most MAX_POLL_MS. */
static int ms_until_next_timer(const node_t *n, uint64_t now)
{
    uint64_t next = now + MAX_POLL_MS;

    if (!n->frozen) { /* D10: no gossip/failure timers while frozen, or poll() would spin */
        if (n->next_gossip_at < next) {
            next = n->next_gossip_at;
        }
        if (n->next_failure_check_at < next) {
            next = n->next_failure_check_at;
        }
    }
    if (join_active(n) && n->next_join_at < next) {
        next = n->next_join_at;
    }
    return next > now ? (int)(next - now) : 0;
}

/* Called when now >= deadline.  D11: if more than one period late, restart from now. */
static uint64_t advance_deadline(uint64_t deadline, uint64_t now, uint64_t period)
{
    if (now - deadline > period) {
        return now + period;
    }
    return deadline + period;
}

/* ---- network input ----------------------------------------------------------- */

static const char *decode_error_name(int rc)
{
    switch (rc) {
    case PROTO_ERR_SHORT:    return "short";
    case PROTO_ERR_MAGIC:    return "bad magic";
    case PROTO_ERR_VERSION:  return "bad version";
    case PROTO_ERR_TYPE:     return "bad type";
    case PROTO_ERR_COUNT:    return "bad count";
    case PROTO_ERR_LENGTH:   return "bad length";
    case PROTO_ERR_ENTRY:    return "bad entry";
    case PROTO_ERR_SEMANTIC: return "bad message shape";
    default:                 return "unknown error";
    }
}

static const char *msg_type_name(uint8_t type)
{
    switch (type) {
    case MSG_JOIN:         return "JOIN";
    case MSG_GOSSIP:       return "GOSSIP";
    case MSG_GOSSIP_REPLY: return "GOSSIP_REPLY";
    case MSG_LEAVE:        return "LEAVE";
    default:               return "?";
    }
}

/* Reads up to MAX_DGRAMS_PER_ITER datagrams (until EAGAIN) and dispatches the valid ones. */
static void drain_socket(node_t *n)
{
    uint8_t buf[MAX_DATAGRAM + 1]; /* +1: a longer datagram is detectable (net.h) */
    struct sockaddr_in from;
    msg_t msg;
    char from_ip[INET_ADDRSTRLEN];

    for (int i = 0; i < MAX_DGRAMS_PER_ITER; i++) {
        ssize_t len = net_recv(n->fd, buf, sizeof buf, &from);
        int rc;

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return; /* socket is empty */
            }
            if (errno == ECONNREFUSED) {
                continue; /* leftover ICMP error from an earlier send; not a datagram */
            }
            LOG_W("recv failed: %s", strerror(errno));
            return;
        }

        if (inet_ntop(AF_INET, &from.sin_addr, from_ip, sizeof from_ip) == NULL) {
            from_ip[0] = '?';
            from_ip[1] = '\0';
        }

        if ((size_t)len > MAX_DATAGRAM) {
            n->stats.dropped_malformed++;
            LOG_D("dropped oversized datagram from %s:%u", from_ip, (unsigned)ntohs(from.sin_port));
            continue;
        }

        rc = proto_decode(buf, (size_t)len, &msg);
        if (rc != PROTO_OK) {
            n->stats.dropped_malformed++;
            LOG_D("dropped malformed datagram from %s:%u (%zd bytes): %s", from_ip,
                  (unsigned)ntohs(from.sin_port), len, decode_error_name(rc));
            continue;
        }

        n->stats.recv[msg.type]++;
        n->stats.bytes_recv += (uint64_t)len;

        if (msg.sender_id == n->self.node_id) {
            LOG_D("ignoring %s from own id %u (%s:%u)", msg_type_name(msg.type),
                  (unsigned)msg.sender_id, from_ip, (unsigned)ntohs(from.sin_port));
            continue;
        }

        LOG_D("recv %s from node %u (%s:%u), %u entries", msg_type_name(msg.type),
              (unsigned)msg.sender_id, from_ip, (unsigned)ntohs(from.sin_port),
              (unsigned)msg.count);
        gossip_on_message(n, &msg, &from);
    }
}

/* ---- stdin ---------------------------------------------------------------------- */

typedef struct {
    char buf[CLI_MAX_LINE + 1];
    size_t len;
    bool discarding; /* inside a line that was too long: skip up to the next '\n' */
} line_buf_t;

/* Assembles lines from arbitrary chunks (partial lines are kept) and runs each complete one. */
static void feed_stdin(node_t *n, line_buf_t *lb, const char *data, size_t len)
{
    for (size_t i = 0; i < len && n->running; i++) {
        char c = data[i];

        if (c != '\n') {
            if (lb->discarding) {
                continue;
            }
            if (lb->len >= CLI_MAX_LINE) {
                lb->discarding = true;
                lb->len = 0;
                continue;
            }
            lb->buf[lb->len++] = c;
            continue;
        }

        if (lb->discarding) {
            lb->discarding = false;
            lb->len = 0;
            LOG_W("stdin line longer than %d bytes discarded", CLI_MAX_LINE);
            fputs("ERR line too long\n", stdout); /* answer promised by cli.h */
            fflush(stdout);
            continue;
        }

        if (lb->len > 0 && lb->buf[lb->len - 1] == '\r') {
            lb->len--;
        }
        lb->buf[lb->len] = '\0';
        lb->len = 0;
        cli_handle_line(n, lb->buf, stdout);
        fflush(stdout);
    }
}

/* One read() on stdin.  Returns false when stdin must not be polled any more (D9). */
static bool handle_stdin(node_t *n, line_buf_t *lb, short revents)
{
    char chunk[STDIN_CHUNK];
    ssize_t r;

    if (revents & POLLNVAL) {
        LOG_D("stdin is not open; CLI disabled");
        return false;
    }

    r = read(STDIN_FILENO, chunk, sizeof chunk);
    if (r > 0) {
        feed_stdin(n, lb, chunk, (size_t)r);
        return true;
    }
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return true;
    }
    if (r < 0) {
        LOG_W("stdin read failed: %s; CLI disabled", strerror(errno));
    } else {
        LOG_D("stdin closed (EOF); CLI disabled, node keeps running");
    }
    return false; /* EOF/error: do not shut the node down (D9) */
}

/* ---- main loop (spec 4.1) --------------------------------------------------------- */

void node_run(node_t *n)
{
    struct pollfd fds[2];
    line_buf_t lb;

    memset(&lb, 0, sizeof lb);
    install_signals();

    fds[0].fd = n->fd;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = STDIN_FILENO;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    while (n->running) {
        uint64_t now = now_ms();
        int timeout = ms_until_next_timer(n, now);
        int ready = poll(fds, 2, timeout);

        if (ready < 0) {
            if (errno != EINTR) { /* EINTR is normal: just fall through to the flag check */
                LOG_E("poll failed: %s", strerror(errno));
                n->running = false;
                break;
            }
        } else if (ready > 0) {
            if (fds[0].revents & POLLNVAL) {
                LOG_E("UDP socket is invalid");
                n->running = false;
                break;
            }
            if (fds[0].revents & (POLLIN | POLLERR)) {
                drain_socket(n);
            }
            if (fds[1].fd >= 0 &&
                (fds[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
                if (!handle_stdin(n, &lb, fds[1].revents)) {
                    fds[1].fd = -1; /* poll() ignores negative descriptors */
                }
            }
        }

        if (g_stop_requested) { /* SIGINT/SIGTERM: orderly leave, then exit (D13) */
            n->leaving = true;
            n->running = false;
        }
        if (!n->running) {
            break;
        }

        now = now_ms(); /* one fresh reading for all timers of this iteration */

        if (!n->frozen) { /* D10: frozen = no gossip rounds, no failure checks */
            if (now >= n->next_gossip_at) {
                gossip_round(n);
                n->stats.rounds++;
                n->next_gossip_at = advance_deadline(n->next_gossip_at, now, n->cfg.gossip_ms);
            }
            if (now >= n->next_failure_check_at) {
                failure_check(n, now);
                n->next_failure_check_at =
                    advance_deadline(n->next_failure_check_at, now, n->cfg.gossip_ms);
            }
        }

        if (join_active(n) && now >= n->next_join_at) {
            /* STUB until #8: JOIN retries (send MSG_JOIN, join_tries++) are added here.
             * The deadline still moves so that poll() does not spin. */
            n->next_join_at = now + JOIN_RETRY_MS;
        }
    }

    if (n->leaving) {
        gossip_leave(n);
    }
}
