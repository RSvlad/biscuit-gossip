/*
 * cli.c - user commands (issue #13, F8).  Output format: decision D16 (see cli.h).
 * No malloc: all buffers are on the stack and the line length is bounded.
 */
#include "cli.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <string.h>

#include "membership.h"
#include "util.h"

typedef struct {
    unsigned total, alive, suspect, dead, left;
} counts_t;

/* Counts self + table, the same view that `list` shows. */
static counts_t count_states(const node_t *n)
{
    counts_t c = {0, 0, 0, 0, 0};
    for (size_t i = 0; i <= n->count; i++) {
        const member_t *m = (i == 0) ? &n->self : &n->table[i - 1];
        c.total++;
        switch (m->state) {
        case STATE_ALIVE:
            c.alive++;
            break;
        case STATE_SUSPECT:
            c.suspect++;
            break;
        case STATE_DEAD:
            c.dead++;
            break;
        case STATE_LEFT:
            c.left++;
            break;
        default:
            break;
        }
    }
    return c;
}

static void print_row(FILE *out, const member_t *m, double silence_s)
{
    char ip[INET_ADDRSTRLEN];
    char addr[INET_ADDRSTRLEN + 8];
    if (inet_ntop(AF_INET, &m->ip, ip, sizeof ip) == NULL)
        snprintf(ip, sizeof ip, "?");
    snprintf(addr, sizeof addr, "%s:%u", ip, (unsigned)m->port);
    fprintf(out, "%-6u %-8s %-21s %-8s %-10u %.1fs\n", (unsigned)m->node_id, role_name(m->role), addr,
            state_name(m->state), (unsigned)m->heartbeat, silence_s);
}

static double silence_s(const member_t *m, uint64_t now_ms)
{
    return (now_ms >= m->last_update_ms ? now_ms - m->last_update_ms : 0) / 1000.0;
}

static void cmd_list(node_t *n, FILE *out, uint64_t now_ms)
{
    fprintf(out, "%-6s %-8s %-21s %-8s %-10s %s\n", "ID", "ROLE", "ADDR", "STATE", "HB", "SILENCE");
    bool self_done = false;
    for (size_t i = 0; i < n->count; i++) {
        if (!self_done && n->self.node_id < n->table[i].node_id) {
            print_row(out, &n->self, 0.0);
            self_done = true;
        }
        print_row(out, &n->table[i], silence_s(&n->table[i], now_ms));
    }
    if (!self_done)
        print_row(out, &n->self, 0.0);

    counts_t c = count_states(n);
    fprintf(out, "# total=%u alive=%u suspect=%u dead=%u left=%u\n", c.total, c.alive, c.suspect, c.dead,
            c.left);
}

static void cmd_stats(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)now_ms;
    const stats_t *s = &n->stats;
    unsigned long long sent_total = 0, recv_total = 0;
    for (int t = MSG_JOIN; t <= MSG_TYPE_MAX; t++) {
        sent_total += s->sent[t];
        recv_total += s->recv[t];
    }
    counts_t c = count_states(n);

    fprintf(out, "sent_total %llu\n", sent_total);
    fprintf(out, "recv_total %llu\n", recv_total);
    fprintf(out, "sent_join %llu\n", (unsigned long long)s->sent[MSG_JOIN]);
    fprintf(out, "sent_gossip %llu\n", (unsigned long long)s->sent[MSG_GOSSIP]);
    fprintf(out, "sent_reply %llu\n", (unsigned long long)s->sent[MSG_GOSSIP_REPLY]);
    fprintf(out, "sent_leave %llu\n", (unsigned long long)s->sent[MSG_LEAVE]);
    fprintf(out, "recv_join %llu\n", (unsigned long long)s->recv[MSG_JOIN]);
    fprintf(out, "recv_gossip %llu\n", (unsigned long long)s->recv[MSG_GOSSIP]);
    fprintf(out, "recv_reply %llu\n", (unsigned long long)s->recv[MSG_GOSSIP_REPLY]);
    fprintf(out, "recv_leave %llu\n", (unsigned long long)s->recv[MSG_LEAVE]);
    fprintf(out, "bytes_sent %llu\n", (unsigned long long)s->bytes_sent);
    fprintf(out, "bytes_recv %llu\n", (unsigned long long)s->bytes_recv);
    fprintf(out, "dropped_malformed %llu\n", (unsigned long long)s->dropped_malformed);
    fprintf(out, "dropped_loss %llu\n", (unsigned long long)s->dropped_loss);
    fprintf(out, "dropped_table_full %llu\n", (unsigned long long)s->dropped_table_full);
    fprintf(out, "rounds %llu\n", (unsigned long long)s->rounds);
    fprintf(out, "members_alive %u\n", c.alive);
    fprintf(out, "members_suspect %u\n", c.suspect);
    fprintf(out, "members_dead %u\n", c.dead);
    fprintf(out, "members_left %u\n", c.left);
    fprintf(out, "# end\n");
}

static void cmd_fail(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)now_ms;
    n->frozen = true; /* D10 */
    fprintf(out, "OK frozen\n");
}

static void cmd_recover(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)now_ms;
    n->frozen = false;
    fprintf(out, "OK recovered\n");
}

/* Same effect as node_request_leave() from #6/#11: LEAVE is sent by the loop, then it exits. */
static void cmd_leave(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)now_ms;
    n->leaving = true;
    n->running = 0;
    fprintf(out, "OK leaving\n");
}

static void cmd_quit(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)now_ms;
    n->leaving = false; /* D13: no LEAVE, simulates a crash but releases resources */
    n->running = 0;
    fprintf(out, "OK bye\n");
}

static void cmd_help(node_t *n, FILE *out, uint64_t now_ms);

static const struct {
    const char *name;
    void (*run)(node_t *, FILE *, uint64_t);
    const char *help;
} commands[] = {
    {"list", cmd_list, "list      show the membership table"},
    {"stats", cmd_stats, "stats     show message and member counters"},
    {"fail", cmd_fail, "fail      freeze: stop sending and heartbeats (simulated crash)"},
    {"recover", cmd_recover, "recover   resume after fail"},
    {"leave", cmd_leave, "leave     announce LEAVE and exit"},
    {"quit", cmd_quit, "quit      exit without LEAVE"},
    {"help", cmd_help, "help      show this list"},
};

#define N_COMMANDS (sizeof commands / sizeof commands[0])

static void cmd_help(node_t *n, FILE *out, uint64_t now_ms)
{
    (void)n;
    (void)now_ms;
    for (size_t i = 0; i < N_COMMANDS; i++)
        fprintf(out, "%s\n", commands[i].help);
    fprintf(out, "# end\n");
}

void cli_exec(node_t *n, const char *line, FILE *out, uint64_t now_ms)
{
    if (strnlen(line, CLI_MAX_LINE) >= CLI_MAX_LINE) {
        fprintf(out, "ERR line too long\n");
        fflush(out);
        return;
    }

    char buf[CLI_MAX_LINE];
    snprintf(buf, sizeof buf, "%s", line);

    char *cmd = buf;
    while (*cmd != '\0' && isspace((unsigned char)*cmd))
        cmd++;
    if (*cmd == '\0') /* empty line is ignored */
        return;

    char *end = cmd;
    while (*end != '\0' && !isspace((unsigned char)*end))
        end++;
    char *rest = end;
    while (*rest != '\0' && isspace((unsigned char)*rest))
        rest++;
    bool has_args = *rest != '\0';
    *end = '\0';

    for (size_t i = 0; i < N_COMMANDS; i++) {
        if (strcmp(cmd, commands[i].name) != 0)
            continue;
        if (has_args)
            fprintf(out, "ERR '%s' takes no arguments\n", commands[i].name);
        else
            commands[i].run(n, out, now_ms);
        fflush(out);
        return;
    }
    fprintf(out, "ERR unknown command '%.32s%s' (try: help)\n", cmd, strlen(cmd) > 32 ? "..." : "");
    fflush(out);
}

void cli_handle_line(node_t *n, const char *line, FILE *out)
{
    cli_exec(n, line, out, now_ms());
}
