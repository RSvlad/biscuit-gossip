/*
 * test_cli.c - user commands (issue #13, F8, decision D16).
 * Output goes to open_memstream (POSIX 2008); the clock is injected via cli_exec.
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "cli.h"
#include "membership.h"

#define SELF_ID 5u

static member_t mk(uint32_t id, uint8_t role, uint8_t state, uint32_t hb, uint64_t last_update)
{
    member_t m;
    memset(&m, 0, sizeof m);
    m.node_id = id;
    m.ip.s_addr = htonl(INADDR_LOOPBACK);
    m.port = (uint16_t)(9000u + id);
    m.role = role;
    m.state = state;
    m.incarnation = 1;
    m.heartbeat = hb;
    m.last_update_ms = last_update;
    return m;
}

/* self = 5 MIXER hb 42; table = 2 OVEN SUSPECT (last 3900), 9 PACKER ALIVE (last 9700), 12 ROLLER DEAD, 13 CUTTER LEFT */
static void fresh(node_t *n)
{
    memset(n, 0, sizeof *n);
    member_t self = mk(SELF_ID, ROLE_MIXER, STATE_ALIVE, 42, 0);
    mem_init(n, &self);
    member_t a = mk(2, ROLE_OVEN, STATE_SUSPECT, 37, 3900);
    member_t b = mk(9, ROLE_PACKER, STATE_ALIVE, 3, 9700);
    member_t c = mk(12, ROLE_ROLLER, STATE_DEAD, 8, 100);
    member_t d = mk(13, ROLE_CUTTER, STATE_LEFT, 9, 100);
    CHECK(mem_insert(n, &b) == 0); /* inserted out of order on purpose */
    CHECK(mem_insert(n, &d) == 0);
    CHECK(mem_insert(n, &a) == 0);
    CHECK(mem_insert(n, &c) == 0);
}

static char out_buf[16384];

/* Runs one line, captures everything written to `out` into out_buf. */
static void run(node_t *n, const char *line, uint64_t now)
{
    char *mem = NULL;
    size_t size = 0;
    FILE *f = open_memstream(&mem, &size);
    CHECK(f != NULL);
    if (f == NULL) {
        out_buf[0] = '\0';
        return;
    }
    cli_exec(n, line, f, now);
    fclose(f);
    snprintf(out_buf, sizeof out_buf, "%s", mem ? mem : "");
    free(mem);
}

static int count_lines(const char *s)
{
    int c = 0;
    for (; *s; s++)
        c += (*s == '\n');
    return c;
}

/* Returns the i-th line (0-based) of out_buf into `dst`, or "" when missing. */
static void line_at(int idx, char *dst, size_t cap)
{
    const char *p = out_buf;
    for (int i = 0; i < idx && p != NULL; i++) {
        p = strchr(p, '\n');
        if (p != NULL)
            p++;
    }
    dst[0] = '\0';
    if (p == NULL)
        return;
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, p, len);
    dst[len] = '\0';
}

struct row {
    unsigned id, hb;
    char role[16], addr[32], state[16];
    double silence;
    char suffix;
};

static int parse_row(const char *line, struct row *r)
{
    return sscanf(line, "%u %15s %31s %15s %u %lf%c", &r->id, r->role, r->addr, r->state, &r->hb,
                  &r->silence, &r->suffix) == 7;
}

static void test_list_format_and_order(void)
{
    static node_t n;
    fresh(&n);
    run(&n, "list", 10000);

    CHECK_EQ_U64(count_lines(out_buf), 1 + 5 + 1); /* header + self + 4 members + summary */

    char l[160];
    line_at(0, l, sizeof l);
    CHECK_STR_EQ(l, "ID     ROLE     ADDR                  STATE    HB         SILENCE");

    static const struct {
        unsigned id;
        const char *role, *addr, *state;
        unsigned hb;
        double silence;
    } expect[] = {
        {2, "OVEN", "127.0.0.1:9002", "SUSPECT", 37, 6.1},   {5, "MIXER", "127.0.0.1:9005", "ALIVE", 42, 0.0},
        {9, "PACKER", "127.0.0.1:9009", "ALIVE", 3, 0.3},    {12, "ROLLER", "127.0.0.1:9012", "DEAD", 8, 9.9},
        {13, "CUTTER", "127.0.0.1:9013", "LEFT", 9, 9.9},
    };
    for (int i = 0; i < 5; i++) {
        struct row r;
        line_at(1 + i, l, sizeof l);
        CHECK(parse_row(l, &r)); /* exactly 6 whitespace-separated columns, silence ends with 's' */
        CHECK_EQ_U64(r.id, expect[i].id);
        CHECK_STR_EQ(r.role, expect[i].role);
        CHECK_STR_EQ(r.addr, expect[i].addr);
        CHECK_STR_EQ(r.state, expect[i].state);
        CHECK_EQ_U64(r.hb, expect[i].hb);
        CHECK(r.suffix == 's');
        CHECK(r.silence > expect[i].silence - 0.05 && r.silence < expect[i].silence + 0.05);
    }
    line_at(6, l, sizeof l);
    CHECK_STR_EQ(l, "# total=5 alive=2 suspect=1 dead=1 left=1");
}

static void test_list_self_first_and_last(void)
{
    static node_t n;
    memset(&n, 0, sizeof n);
    member_t self = mk(1, ROLE_MIXER, STATE_ALIVE, 1, 0);
    mem_init(&n, &self);
    run(&n, "list", 5000); /* alone: header, self, summary */
    CHECK_EQ_U64(count_lines(out_buf), 3);
    char l[160];
    line_at(2, l, sizeof l);
    CHECK_STR_EQ(l, "# total=1 alive=1 suspect=0 dead=0 left=0");

    member_t low = mk(0, ROLE_OVEN, STATE_ALIVE, 1, 4000);
    CHECK(mem_insert(&n, &low) == 0);
    run(&n, "list", 5000);
    struct row r;
    line_at(1, l, sizeof l);
    CHECK(parse_row(l, &r) && r.id == 0);
    line_at(2, l, sizeof l);
    CHECK(parse_row(l, &r) && r.id == 1); /* self after a smaller id, as the last row */
}

static void test_list_awk_style_count(void)
{
    static node_t n;
    fresh(&n);
    run(&n, "list", 10000);
    /* awk 'NR>1 && NF==6 && $4=="ALIVE"{c++}' over the output */
    int alive = 0;
    char l[160];
    for (int i = 1; i < count_lines(out_buf) - 1; i++) {
        struct row r;
        line_at(i, l, sizeof l);
        if (parse_row(l, &r) && strcmp(r.state, "ALIVE") == 0)
            alive++;
    }
    CHECK_EQ_U64(alive, 2);
    line_at(count_lines(out_buf) - 1, l, sizeof l);
    CHECK(l[0] == '#');
}

static void test_stats(void)
{
    static node_t n;
    fresh(&n);
    n.stats.sent[MSG_JOIN] = 1;
    n.stats.sent[MSG_GOSSIP] = 10;
    n.stats.sent[MSG_GOSSIP_REPLY] = 4;
    n.stats.sent[MSG_LEAVE] = 2;
    n.stats.recv[MSG_JOIN] = 3;
    n.stats.recv[MSG_GOSSIP] = 20;
    n.stats.recv[MSG_GOSSIP_REPLY] = 5;
    n.stats.recv[MSG_LEAVE] = 1;
    n.stats.bytes_sent = 1234;
    n.stats.bytes_recv = 5678;
    n.stats.dropped_malformed = 7;
    n.stats.dropped_loss = 8;
    n.stats.dropped_table_full = 9;
    n.stats.rounds = 11;
    run(&n, "stats", 0);
    static const char *const keys[] = {
        "sent_total 17",   "recv_total 29",       "sent_join 1",      "sent_gossip 10",
        "sent_reply 4",    "sent_leave 2",        "recv_join 3",      "recv_gossip 20",
        "recv_reply 5",    "recv_leave 1",        "bytes_sent 1234",  "bytes_recv 5678",
        "dropped_malformed 7", "dropped_loss 8",  "dropped_table_full 9", "rounds 11",
        "members_alive 2", "members_suspect 1",   "members_dead 1",   "members_left 1",
        "# end",
    };
    CHECK_EQ_U64(count_lines(out_buf), sizeof keys / sizeof keys[0]);
    char l[160];
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        line_at((int)i, l, sizeof l);
        CHECK_STR_EQ(l, keys[i]);
    }
}

static void test_flags(void)
{
    static node_t n;
    fresh(&n);
    n.running = 1;

    run(&n, "fail", 0);
    CHECK_STR_EQ(out_buf, "OK frozen\n");
    CHECK(n.frozen);
    run(&n, "fail", 0); /* twice is not an error */
    CHECK_STR_EQ(out_buf, "OK frozen\n");
    run(&n, "list", 10000); /* list works while frozen */
    CHECK_EQ_U64(count_lines(out_buf), 7);
    run(&n, "stats", 0);
    CHECK(strstr(out_buf, "# end\n") != NULL);
    run(&n, "recover", 0);
    CHECK_STR_EQ(out_buf, "OK recovered\n");
    CHECK(!n.frozen);
    run(&n, "recover", 0);
    CHECK_STR_EQ(out_buf, "OK recovered\n");

    run(&n, "leave", 0);
    CHECK_STR_EQ(out_buf, "OK leaving\n");
    CHECK(n.leaving && !n.running);

    n.running = 1;
    run(&n, "quit", 0);
    CHECK_STR_EQ(out_buf, "OK bye\n");
    CHECK(!n.leaving && !n.running);

    n.running = 1; /* quit overrides an earlier leave request (D13: exit without LEAVE) */
    n.leaving = true;
    run(&n, "quit", 0);
    CHECK(!n.leaving && !n.running);

    n.frozen = true; /* leave while frozen is accepted (D10: leave takes precedence) */
    n.running = 1;
    run(&n, "leave", 0);
    CHECK(n.leaving && !n.running);
}

static void test_help(void)
{
    static node_t n;
    fresh(&n);
    run(&n, "help", 0);
    CHECK_EQ_U64(count_lines(out_buf), 8); /* 7 commands + "# end" */
    CHECK(strstr(out_buf, "list") != NULL && strstr(out_buf, "quit") != NULL);
    char l[160];
    line_at(7, l, sizeof l);
    CHECK_STR_EQ(l, "# end");
}

static void test_input_handling(void)
{
    static node_t n;
    fresh(&n);

    run(&n, "", 0);
    CHECK_STR_EQ(out_buf, "");
    run(&n, "   \t ", 0);
    CHECK_STR_EQ(out_buf, "");

    run(&n, "frobnicate", 0);
    CHECK_STR_EQ(out_buf, "ERR unknown command 'frobnicate' (try: help)\n");
    run(&n, "LIST", 0); /* commands are case-sensitive */
    CHECK(strncmp(out_buf, "ERR unknown command", 19) == 0);

    run(&n, "  list \r", 10000); /* surrounding whitespace and CR are trimmed */
    CHECK_EQ_U64(count_lines(out_buf), 7);
    run(&n, "fail now", 0);
    CHECK_STR_EQ(out_buf, "ERR 'fail' takes no arguments\n");
    CHECK(!n.frozen);

    static char longline[10001];
    memset(longline, 'a', sizeof longline - 1);
    longline[sizeof longline - 1] = '\0';
    run(&n, longline, 0);
    CHECK_STR_EQ(out_buf, "ERR line too long\n");
    memset(longline, ' ', 600); /* long run of blanks followed by a command is also too long */
    memcpy(longline + 600, "list", 4);
    longline[604] = '\0';
    run(&n, longline, 0);
    CHECK_STR_EQ(out_buf, "ERR line too long\n");

    char just_fits[CLI_MAX_LINE];
    memset(just_fits, 'b', sizeof just_fits - 1);
    just_fits[sizeof just_fits - 1] = '\0'; /* longest accepted line, unknown command, name truncated */
    run(&n, just_fits, 0);
    CHECK(strncmp(out_buf, "ERR unknown command 'bbbb", 25) == 0);
    CHECK(strstr(out_buf, "...' (try: help)\n") != NULL);
    CHECK(strlen(out_buf) < 100);
}

static void test_cli_handle_line_uses_real_clock(void)
{
    static node_t n;
    fresh(&n);
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (f == NULL)
        return;
    cli_handle_line(&n, "fail", f);
    CHECK(n.frozen);
    rewind(f);
    char l[64] = "";
    CHECK(fgets(l, sizeof l, f) != NULL);
    CHECK_STR_EQ(l, "OK frozen\n");
    fclose(f);
}

int main(void)
{
    test_list_format_and_order();
    test_list_self_first_and_last();
    test_list_awk_style_count();
    test_stats();
    test_flags();
    test_help();
    test_input_handling();
    test_cli_handle_line_uses_real_clock();
    TEST_MAIN_END();
}
