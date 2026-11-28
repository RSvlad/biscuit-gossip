/*
 * common.h - shared constants and types (contract for every module).
 *
 * Only typedefs and constants live here; no functions.  The field names of
 * node_t / config_t / stats_t are relied upon by membership, gossip, failure,
 * cli and node, so change them only via a small separate PR (see README).
 * Decisions D1..D18 referenced below are documented in README.md.
 */
#ifndef COMMON_H
#define COMMON_H

#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Capacity (N1: extendable with -DMAX_NODES=128 etc.) ---------------- */

#ifndef MAX_NODES
#define MAX_NODES 64 /* total nodes incl. self (D1: table holds MAX_NODES-1) */
#endif

#ifndef MAX_ENTRIES_MSG
#define MAX_ENTRIES_MSG 32 /* max records per message (wire constant) */
#endif

/* ---- Wire format (spec section 6, D7) ------------------------------------ */

#define PROTO_MAGIC      0x4B58 /* "KX" */
#define PROTO_VERSION    1
#define PROTO_HEADER_LEN 12
#define PROTO_ENTRY_LEN  20
#define MAX_DATAGRAM     1400 /* one message == one UDP datagram */

_Static_assert(PROTO_HEADER_LEN + MAX_ENTRIES_MSG * PROTO_ENTRY_LEN <= MAX_DATAGRAM,
               "largest message must fit in one datagram");

/* ---- Defaults (all in milliseconds) -------------------------------------- */

#define DEF_GOSSIP_MS  1000  /* T_GOSSIP  */
#define DEF_FANOUT     3
#define DEF_SUSPECT_MS 5000  /* T_SUSPECT */
#define DEF_FAIL_MS    10000 /* T_FAIL    */
#define DEF_CLEANUP_MS 30000 /* T_CLEANUP */

#define JOIN_RETRY_MS  2000 /* spec 7.4: JOIN is repeated every 2 s ...   */
#define JOIN_MAX_TRIES 10   /* ... up to 10 times, then exit code 3 (D17) */

/* ---- Enumerations --------------------------------------------------------- */

typedef enum {
    STATE_ALIVE = 0,
    STATE_SUSPECT,
    STATE_DEAD,
    STATE_LEFT
} node_state_t;

typedef enum {
    ROLE_MIXER = 0,
    ROLE_ROLLER,
    ROLE_CUTTER,
    ROLE_OVEN,
    ROLE_COOLER,
    ROLE_PACKER,
    ROLE_MONITOR
} node_role_t;

typedef enum {
    MSG_JOIN = 1,
    MSG_GOSSIP = 2,
    MSG_GOSSIP_REPLY = 3,
    MSG_LEAVE = 4,
    MSG_TYPE_MAX = MSG_LEAVE /* arrays indexed by type have MSG_TYPE_MAX + 1 slots */
} msg_type_t;

/* ---- Membership record (spec section 5) ---------------------------------- */

typedef struct {
    uint32_t node_id;        /* unique ID */
    struct in_addr ip;       /* IPv4 (network order, as in_addr) */
    uint16_t port;           /* UDP port (host order) */
    uint8_t role;            /* node_role_t */
    uint8_t state;           /* node_state_t */
    uint32_t incarnation;    /* grows on every restart (D5: unix time at start) */
    uint32_t heartbeat;      /* grows at the owner once per gossip round */
    uint64_t last_update_ms; /* LOCAL monotonic time of last progress; never sent */
} member_t;

/* ---- Configuration (filled by main, read-only afterwards) ----------------- */

typedef struct {
    uint32_t gossip_ms;
    uint32_t fanout;
    uint32_t suspect_ms;
    uint32_t fail_ms;
    uint32_t cleanup_ms;
    double loss;                  /* 0.0 .. 1.0 send-loss simulation (D12) */
    bool verbose;                 /* --verbose: LOG_D enabled */
    struct sockaddr_in bind_addr; /* --bind (concrete IPv4, D8) */
    struct sockaddr_in seed_addr; /* --seed; valid only if has_seed (D2) */
    bool has_seed;
} config_t;

/* ---- Counters shown by `stats` (D16) -------------------------------------- */

typedef struct {
    uint64_t sent[MSG_TYPE_MAX + 1]; /* indexed by msg_type_t; [0] unused */
    uint64_t recv[MSG_TYPE_MAX + 1];
    uint64_t bytes_sent;
    uint64_t bytes_recv;
    uint64_t dropped_malformed; /* proto_decode failed (counted by node) */
    uint64_t dropped_loss;      /* discarded by --loss in node_send (D12) */
    uint64_t dropped_table_full;
    uint64_t rounds;
} stats_t;

/* ---- Node state ------------------------------------------------------------ */

typedef struct {
    member_t self;             /* D1: NOT part of table[] */
    member_t table[MAX_NODES]; /* other members sorted by node_id; first `count` used */
    size_t count;              /* used entries in table[] (capacity MAX_NODES - 1) */

    config_t cfg;
    stats_t stats;

    int fd;                        /* UDP socket; only net.c and node.c touch it */
    volatile sig_atomic_t running; /* cleared by signal handler, `quit`, `leave` */
    bool frozen;                   /* `fail` (D10): no sending, no heartbeat, no failure_check */
    bool leaving;                  /* `leave` or SIGINT/SIGTERM: send LEAVE, then exit (D13) */
    bool joined;                   /* JOIN answered (a GOSSIP_REPLY from the seed arrived) */
    unsigned join_tries;           /* JOIN datagrams sent so far (max JOIN_MAX_TRIES) */
    int exit_code;                 /* process exit status (D17): 0 normally, 3 when JOIN is
                                      exhausted (#9); #36 adds 1 for loop errors */

    uint64_t next_gossip_at;        /* monotonic ms (D11) */
    uint64_t next_failure_check_at; /* monotonic ms */
    uint64_t next_join_at;          /* monotonic ms of the next JOIN retry */
} node_t;

#endif /* COMMON_H */
