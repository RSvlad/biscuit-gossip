#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

int net_open(const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (ip == NULL || inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        LOG_E("net: '%s' is not a valid IPv4 address", ip ? ip : "(null)");
        errno = EINVAL;
        return -1;
    }
    if (addr.sin_addr.s_addr == htonl(INADDR_ANY)) {
        LOG_E("net: bind address must be concrete, 0.0.0.0 is not allowed (D8)");
        errno = EINVAL;
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        int e = errno;
        LOG_E("net: socket: %s", strerror(e));
        errno = e;
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int e = errno;
        LOG_E("net: cannot make socket non-blocking: %s", strerror(e));
        close(fd);
        errno = e;
        return -1;
    }

    /* No SO_REUSEADDR / SO_REUSEPORT (D8): EADDRINUSE must stay visible. */
    if (bind(fd, (const struct sockaddr *)&addr, sizeof addr) < 0) {
        int e = errno;
        LOG_E("net: bind %s:%u: %s", ip, (unsigned)port, strerror(e));
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

ssize_t net_send(int fd, const struct sockaddr_in *to, const uint8_t *buf, size_t len)
{
    if (fd < 0 || to == NULL || buf == NULL) {
        errno = EINVAL;
        return -1;
    }

    ssize_t n = sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to);
    if (n < 0 && errno == EINTR)
        n = sendto(fd, buf, len, 0, (const struct sockaddr *)to, sizeof *to);
    if (n >= 0)
        return n;

    int e = errno;
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &to->sin_addr, ip, sizeof ip);
    if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS || e == ECONNREFUSED ||
        e == EHOSTUNREACH || e == ENETUNREACH || e == EINTR)
        LOG_D("net: send to %s:%u dropped: %s", ip, (unsigned)ntohs(to->sin_port), strerror(e));
    else
        LOG_W("net: send to %s:%u failed: %s", ip, (unsigned)ntohs(to->sin_port), strerror(e));
    errno = e;
    return -1;
}

ssize_t net_recv(int fd, uint8_t *buf, size_t cap, struct sockaddr_in *from)
{
    if (fd < 0 || buf == NULL || from == NULL) {
        errno = EINVAL;
        return -1;
    }

    socklen_t alen = sizeof *from;
    memset(from, 0, sizeof *from);
    ssize_t n = recvfrom(fd, buf, cap, 0, (struct sockaddr *)from, &alen);
    if (n < 0) {
        int e = errno;
        if (e != EAGAIN && e != EWOULDBLOCK && e != EINTR)
            LOG_W("net: recv: %s", strerror(e));
        errno = e;
        return -1;
    }
    return n;
}

int net_parse_addr(const char *s, struct sockaddr_in *out)
{
    if (s == NULL || out == NULL)
        return -1;

    const char *colon = strchr(s, ':');
    if (colon == NULL || strchr(colon + 1, ':') != NULL)
        return -1;

    size_t ip_len = (size_t)(colon - s);
    if (ip_len == 0 || ip_len >= INET_ADDRSTRLEN)
        return -1;
    char ip[INET_ADDRSTRLEN];
    memcpy(ip, s, ip_len);
    ip[ip_len] = '\0';

    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1)
        return -1;

    const char *p = colon + 1;
    size_t digits = strlen(p);
    if (digits == 0 || digits > 5)
        return -1;
    unsigned long port = 0;
    for (size_t i = 0; i < digits; i++) {
        if (p[i] < '0' || p[i] > '9')
            return -1;
        port = port * 10 + (unsigned long)(p[i] - '0');
    }
    if (port < 1 || port > 65535)
        return -1;

    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr = a;
    out->sin_port = htons((uint16_t)port);
    return 0;
}

void net_addr_from_member(const member_t *m, struct sockaddr_in *out)
{
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    out->sin_addr = m->ip;
    out->sin_port = htons(m->port);
}

void net_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
