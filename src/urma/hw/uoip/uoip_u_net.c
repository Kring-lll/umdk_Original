/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — TCP transport helpers
 */

#include "uoip_u_net.h"
#include "uoip_u_log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Reliable write: keep writing until all bytes are sent or an error */
static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Reliable read: keep reading until all bytes are received */
static int read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p   += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Set TCP_NODELAY to reduce latency */
static void set_nodelay(int fd)
{
    int yes = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int uoip_tcp_listen(uint16_t *port_inout)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        UOIP_LOG_ERR("socket: %s\n", strerror(errno));
        return -1;
    }

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(*port_inout),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        UOIP_LOG_ERR("bind: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 128) < 0) {
        UOIP_LOG_ERR("listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* Query the actual port (important when port was 0) */
    socklen_t slen = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &slen) == 0)
        *port_inout = ntohs(addr.sin_port);

    UOIP_LOG_INFO("TCP listen fd=%d port=%u\n", fd, *port_inout);
    return fd;
}

int uoip_tcp_connect(uint32_t remote_ip, uint16_t port_nbo)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        UOIP_LOG_ERR("socket: %s\n", strerror(errno));
        return -1;
    }

    set_nodelay(fd);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = port_nbo,
        .sin_addr.s_addr = remote_ip,
    };

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &remote_ip, ip_str, sizeof(ip_str));
        UOIP_LOG_ERR("connect %s:%u: %s\n", ip_str, ntohs(port_nbo), strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int uoip_frame_send(int fd, const void *buf, uint32_t len)
{
    uint32_t hdr[2] = { htonl(0x554F4950u), htonl(len) };

    if (write_all(fd, hdr, sizeof(hdr)) < 0) {
        UOIP_LOG_ERR("send hdr fd=%d: %s\n", fd, strerror(errno));
        return -1;
    }
    if (len > 0 && write_all(fd, buf, len) < 0) {
        UOIP_LOG_ERR("send payload fd=%d: %s\n", fd, strerror(errno));
        return -1;
    }
    return 0;
}

int uoip_frame_recv(int fd, void **buf_out, uint32_t *len_out)
{
    uint32_t hdr[2];

    if (read_all(fd, hdr, sizeof(hdr)) < 0)
        return -1;

    if (ntohl(hdr[0]) != 0x554F4950u) {
        UOIP_LOG_ERR("bad frame magic 0x%08x fd=%d\n", ntohl(hdr[0]), fd);
        return -1;
    }

    uint32_t len = ntohl(hdr[1]);
    if (len > UOIP_MAX_PAYLOAD) {
        UOIP_LOG_ERR("frame too large (%u) fd=%d\n", len, fd);
        return -1;
    }

    void *buf = NULL;
    if (len > 0) {
        buf = malloc(len);
        if (!buf)
            return -1;
        if (read_all(fd, buf, len) < 0) {
            free(buf);
            return -1;
        }
    }

    *buf_out = buf;
    *len_out = len;
    return 0;
}

uint32_t uoip_get_local_ipv4(void)
{
    /* Env override: UOIP_LOCAL_IP=a.b.c.d */
    const char *env = getenv("UOIP_LOCAL_IP");
    if (env) {
        struct in_addr a;
        if (inet_pton(AF_INET, env, &a) == 1) {
            UOIP_LOG_INFO("local IP from env: %s\n", env);
            return a.s_addr;
        }
        UOIP_LOG_WARN("invalid UOIP_LOCAL_IP=%s, auto-detecting\n", env);
    }

    /* Walk network interfaces; skip loopback */
    struct ifaddrs *ifa_list = NULL, *ifa;
    uint32_t found = htonl(INADDR_LOOPBACK);  /* fallback */

    if (getifaddrs(&ifa_list) == 0) {
        for (ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_flags & IFF_LOOPBACK)
                continue;
            if (!(ifa->ifa_flags & IFF_UP))
                continue;
            found = ((struct sockaddr_in *)ifa->ifa_addr)->sin_addr.s_addr;
            break;
        }
        freeifaddrs(ifa_list);
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &found, ip_str, sizeof(ip_str));
    UOIP_LOG_INFO("local IPv4: %s\n", ip_str);
    return found;
}
