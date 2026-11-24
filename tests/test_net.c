#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "check.h"
#include "net.h"

static uint16_t bound_port(int fd)
{
    struct sockaddr_in a;
    socklen_t len = sizeof a;
    if (getsockname(fd, (struct sockaddr *)&a, &len) != 0)
        return 0;
    return ntohs(a.sin_port);
}

/* Waits up to 1 s for a datagram (loopback delivery is immediate; this only guards flakiness). */
static ssize_t recv_wait(int fd, uint8_t *buf, size_t cap, struct sockaddr_in *from)
{
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 1000) <= 0)
        return -1;
    return net_recv(fd, buf, cap, from);
}

static void test_parse_addr(void)
{
    static const struct {
        const char *s;
        int ok;
    } cases[] = {
        {"1.2.3.4:5", 1},        {"127.0.0.1:9001", 1}, {"255.255.255.255:65535", 1},
        {"1.2.3.4", 0},          {"1.2.3.4:", 0},       {"1.2.3.4:0", 0},
        {"1.2.3.4:65536", 0},    {"1.2.3.4:123456", 0}, {"a:b", 0},
        {" 1.2.3.4:5", 0},       {"1.2.3.4:5 ", 0},     {"1.2.3.4:5x", 0},
        {"1.2.3.4:-5", 0},       {"1.2.3.4:+5", 0},     {":5", 0},
        {"1.2.3:5", 0},          {"1.2.3.4.5:5", 0},    {"256.1.1.1:5", 0},
        {"1.2.3.4:5:6", 0},      {"localhost:5", 0},    {"", 0},
        {"1.2.3.4::5", 0},       {"::1:5", 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct sockaddr_in a;
        int r = net_parse_addr(cases[i].s, &a);
        if (cases[i].ok != (r == 0))
            fprintf(stderr, "parse case '%s'\n", cases[i].s);
        CHECK((r == 0) == (cases[i].ok != 0));
    }

    struct sockaddr_in a;
    CHECK(net_parse_addr("10.20.30.40:9001", &a) == 0);
    CHECK(a.sin_family == AF_INET);
    CHECK(a.sin_port == htons(9001));
    CHECK(a.sin_addr.s_addr == htonl(0x0A141E28u));
    CHECK(net_parse_addr(NULL, &a) == -1);
    CHECK(net_parse_addr("1.2.3.4:5", NULL) == -1);
}

static void test_open_errors(void)
{
    CHECK(net_open("0.0.0.0", 0) == -1); /* D8 */
    CHECK(errno == EINVAL);
    CHECK(net_open("not-an-ip", 0) == -1);
    CHECK(net_open(NULL, 0) == -1);

    int a = net_open("127.0.0.1", 0);
    CHECK(a >= 0);
    uint16_t port = bound_port(a);
    CHECK(port != 0);

    /* D8: no SO_REUSEADDR/SO_REUSEPORT, a second bind must fail visibly. */
    int b = net_open("127.0.0.1", port);
    CHECK(b == -1);
    CHECK(errno == EADDRINUSE);

    net_close(a);
    net_close(-1); /* ignored */

    /* After closing, the port is free again (no TIME_WAIT for UDP). */
    int c = net_open("127.0.0.1", port);
    CHECK(c >= 0);
    net_close(c);
}

static void test_round_trip(void)
{
    int a = net_open("127.0.0.1", 0);
    int b = net_open("127.0.0.1", 0);
    CHECK(a >= 0 && b >= 0);
    if (a < 0 || b < 0)
        return;

    uint8_t buf[MAX_DATAGRAM + 1];
    struct sockaddr_in from;

    /* Empty non-blocking socket: -1 / EAGAIN. */
    errno = 0;
    CHECK(net_recv(a, buf, sizeof buf, &from) == -1);
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);

    member_t b_rec;
    memset(&b_rec, 0, sizeof b_rec);
    inet_pton(AF_INET, "127.0.0.1", &b_rec.ip);
    b_rec.port = bound_port(b);
    struct sockaddr_in to_b;
    net_addr_from_member(&b_rec, &to_b);
    CHECK(to_b.sin_family == AF_INET);
    CHECK(to_b.sin_port == htons(b_rec.port));
    CHECK(to_b.sin_addr.s_addr == b_rec.ip.s_addr);

    static const uint8_t msg[] = {0x4B, 0x58, 0x01, 0x02, 0x00, 0xFF};
    CHECK(net_send(a, &to_b, msg, sizeof msg) == (ssize_t)sizeof msg);

    ssize_t n = recv_wait(b, buf, sizeof buf, &from);
    CHECK(n == (ssize_t)sizeof msg);
    CHECK(n > 0 && memcmp(buf, msg, sizeof msg) == 0);
    CHECK(from.sin_family == AF_INET);
    CHECK(from.sin_port == htons(bound_port(a))); /* reply address */
    CHECK(from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));

    /* An oversized datagram is visible as len > MAX_DATAGRAM (cap = MAX + 1). */
    uint8_t big[MAX_DATAGRAM + 100];
    memset(big, 0xAB, sizeof big);
    CHECK(net_send(a, &to_b, big, MAX_DATAGRAM + 1) == MAX_DATAGRAM + 1);
    n = recv_wait(b, buf, sizeof buf, &from);
    CHECK(n == MAX_DATAGRAM + 1);

    /* Zero-length datagrams are legal and arrive as length 0. */
    CHECK(net_send(a, &to_b, msg, 0) == 0);
    n = recv_wait(b, buf, sizeof buf, &from);
    CHECK(n == 0);

    /* Bad arguments never crash. */
    CHECK(net_send(-1, &to_b, msg, sizeof msg) == -1);
    CHECK(net_send(a, NULL, msg, sizeof msg) == -1);
    CHECK(net_recv(-1, buf, sizeof buf, &from) == -1);
    CHECK(net_recv(a, buf, sizeof buf, NULL) == -1);

    net_close(a);
    net_close(b);
}

static void test_send_to_nowhere_is_not_fatal(void)
{
    int a = net_open("127.0.0.1", 0);
    CHECK(a >= 0);
    if (a < 0)
        return;

    /* A closed port on loopback: UDP just loses the datagram. */
    int tmp = net_open("127.0.0.1", 0);
    uint16_t dead_port = bound_port(tmp);
    net_close(tmp);

    struct sockaddr_in to;
    CHECK(net_parse_addr("127.0.0.1:1", &to) == 0);
    to.sin_port = htons(dead_port);
    static const uint8_t msg[] = {1, 2, 3};
    for (int i = 0; i < 3; i++)
        CHECK(net_send(a, &to, msg, sizeof msg) == (ssize_t)sizeof msg);

    net_close(a);
}

int main(void)
{
    test_parse_addr();
    test_open_errors();
    test_round_trip();
    test_send_to_nowhere_is_not_fatal();
    TEST_MAIN_END();
}
