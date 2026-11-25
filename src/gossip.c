/* gossip.c - owner: Dev B.  Everything below is a placeholder so that node.c links. */
#include "gossip.h"

/* STUB until #8: the real round increments the heartbeat and pushes to peers. */
void gossip_round(node_t *n)
{
    (void)n;
}

/* STUB until #8: the real handler merges the entries and answers GOSSIP/JOIN. */
void gossip_on_message(node_t *n, const msg_t *m, const struct sockaddr_in *from)
{
    (void)n;
    (void)m;
    (void)from;
}

/* STUB until #11: the real function sends MSG_LEAVE to up to fanout peers. */
void gossip_leave(node_t *n)
{
    (void)n;
}
