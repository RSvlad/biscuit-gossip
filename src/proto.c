#include "proto.h"

#include <string.h>

#include "util.h"

/* ---- byte helpers: manual big-endian, no casts of byte buffers to structs ---- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int fail(int code, const char *why)
{
    LOG_D("proto: datagram rejected (%s)", why);
    return code;
}

size_t proto_encode(uint8_t *buf, size_t cap, uint8_t type, uint32_t sender,
                    const member_t *entries, uint16_t count)
{
    if (buf == NULL || type < MSG_JOIN || type > MSG_TYPE_MAX)
        return 0;
    if (count > MAX_ENTRIES_MSG || (count > 0 && entries == NULL))
        return 0;

    size_t need = PROTO_HEADER_LEN + (size_t)count * PROTO_ENTRY_LEN;
    if (cap < need)
        return 0;

    put_u16(buf, PROTO_MAGIC);
    buf[2] = PROTO_VERSION;
    buf[3] = type;
    put_u32(buf + 4, sender);
    put_u16(buf + 8, count);
    put_u16(buf + 10, 0); /* reserved */

    for (uint16_t i = 0; i < count; i++) {
        uint8_t *p = buf + PROTO_HEADER_LEN + (size_t)i * PROTO_ENTRY_LEN;
        const member_t *m = &entries[i];

        put_u32(p, m->node_id);
        memcpy(p + 4, &m->ip.s_addr, 4); /* already network order; 4 bytes into a 20-byte slot */
        put_u16(p + 8, m->port);
        p[10] = m->role;
        p[11] = m->state;
        put_u32(p + 12, m->incarnation);
        put_u32(p + 16, m->heartbeat);
    }
    return need;
}

int proto_decode(const uint8_t *buf, size_t len, msg_t *out)
{
    if (buf == NULL || out == NULL)
        return fail(PROTO_ERR_SHORT, "null argument");
    if (len < PROTO_HEADER_LEN)
        return fail(PROTO_ERR_SHORT, "shorter than header");
    if (get_u16(buf) != PROTO_MAGIC)
        return fail(PROTO_ERR_MAGIC, "bad magic");
    if (buf[2] != PROTO_VERSION)
        return fail(PROTO_ERR_VERSION, "bad version");

    uint8_t type = buf[3];
    if (type < MSG_JOIN || type > MSG_TYPE_MAX)
        return fail(PROTO_ERR_TYPE, "unknown type");

    uint16_t count = get_u16(buf + 8);
    if (count > MAX_ENTRIES_MSG)
        return fail(PROTO_ERR_COUNT, "count too large");
    if (len != PROTO_HEADER_LEN + (size_t)count * PROTO_ENTRY_LEN)
        return fail(PROTO_ERR_LENGTH, "length does not match count");

    out->type = type;
    out->sender_id = get_u32(buf + 4);
    out->count = count;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *p = buf + PROTO_HEADER_LEN + (size_t)i * PROTO_ENTRY_LEN;
        member_t *m = &out->entries[i];

        memset(m, 0, sizeof *m); /* last_update_ms = 0 and no stale padding */
        m->node_id = get_u32(p);
        memcpy(&m->ip.s_addr, p + 4, 4);
        m->port = get_u16(p + 8);
        m->role = p[10];
        m->state = p[11];
        m->incarnation = get_u32(p + 12);
        m->heartbeat = get_u32(p + 16);

        if (m->role > ROLE_MONITOR || m->state > STATE_LEFT || m->port == 0)
            return fail(PROTO_ERR_ENTRY, "invalid entry");
    }

    if (type == MSG_JOIN || type == MSG_LEAVE) {
        if (count != 1 || out->entries[0].node_id != out->sender_id)
            return fail(PROTO_ERR_SEMANTIC, "JOIN/LEAVE must carry exactly the sender");
        if (type == MSG_LEAVE && out->entries[0].state != STATE_LEFT)
            return fail(PROTO_ERR_SEMANTIC, "LEAVE must carry STATE_LEFT");
    }
    return PROTO_OK;
}
