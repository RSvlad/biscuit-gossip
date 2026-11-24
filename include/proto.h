/*
 * proto.h - wire format encode/decode (spec section 6, decisions D7).
 *
 * Messages are built and parsed field by field in network byte order; structs
 * are never sent directly.  Input from the network is untrusted:
 * proto_decode never reads outside [buf, buf + len) and rejects anything that
 * is not exactly 12 + count * 20 bytes of valid data.
 *
 * Pure functions: no node_t, no counters.  The caller (node, #6) counts
 * rejected datagrams in stats.dropped_malformed.
 */
#ifndef PROTO_H
#define PROTO_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

typedef struct {
    uint8_t type;      /* msg_type_t */
    uint32_t sender_id;
    uint16_t count;    /* valid entries[0 .. count-1] */
    member_t entries[MAX_ENTRIES_MSG]; /* last_update_ms == 0 after decode */
} msg_t;

/* proto_decode() results: 0 = OK, otherwise one of these (negative). */
enum {
    PROTO_OK = 0,
    PROTO_ERR_SHORT = -1,    /* fewer than PROTO_HEADER_LEN bytes */
    PROTO_ERR_MAGIC = -2,
    PROTO_ERR_VERSION = -3,
    PROTO_ERR_TYPE = -4,
    PROTO_ERR_COUNT = -5,    /* count > MAX_ENTRIES_MSG */
    PROTO_ERR_LENGTH = -6,   /* len != PROTO_HEADER_LEN + count * PROTO_ENTRY_LEN */
    PROTO_ERR_ENTRY = -7,    /* role/state out of range or port == 0 */
    PROTO_ERR_SEMANTIC = -8  /* JOIN/LEAVE shape (count, node_id, state) violated */
};

/*
 * Encodes one message into buf.  Returns the number of bytes written, or 0 if
 * cap is too small, count > MAX_ENTRIES_MSG, type is not in [MSG_JOIN, MSG_TYPE_MAX]
 * or (count > 0 && entries == NULL).  The `reserved` field is written as 0.
 * Message-level semantics (JOIN/LEAVE shape) are checked by the decoder only.
 */
size_t proto_encode(uint8_t *buf, size_t cap, uint8_t type, uint32_t sender,
                    const member_t *entries, uint16_t count);

/*
 * Decodes and validates a datagram.  Returns PROTO_OK or a negative PROTO_ERR_*.
 * On failure the contents of *out are unspecified.  `reserved` is ignored.
 */
int proto_decode(const uint8_t *buf, size_t len, msg_t *out);

#endif /* PROTO_H */
