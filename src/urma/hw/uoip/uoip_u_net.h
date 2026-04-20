/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2025. All rights reserved.
 * Description: uoip — TCP transport helpers
 */

#ifndef UOIP_U_NET_H
#define UOIP_U_NET_H

#include <stdint.h>
#include <stddef.h>

/*
 * Wire frame layout (all fields big-endian / network byte order):
 *
 *   0        3   4        7   8 ... 8+len-1
 *  +----------+----------+-----------+
 *  |  magic   |   len    |  payload  |
 *  +----------+----------+-----------+
 */

/* Create a non-blocking TCP listen socket bound to *port_inout.
 * If *port_inout == 0, the OS chooses a free port and writes it back.
 * Returns fd on success, -1 on error. */
int uoip_tcp_listen(uint16_t *port_inout);

/* TCP connect to ip:port (both network byte order).
 * Returns connected fd on success, -1 on error. */
int uoip_tcp_connect(uint32_t remote_ip, uint16_t port_nbo);

/* Send a complete frame (header + data) on fd.
 * Returns 0 on success, -1 on error. */
int uoip_frame_send(int fd, const void *buf, uint32_t len);

/* Receive exactly one frame from fd into *buf / *len.
 * Caller must free(*buf) after use.
 * Returns 0 on success, -1 on error / connection closed. */
int uoip_frame_recv(int fd, void **buf, uint32_t *len);

/* Return the first non-loopback IPv4 address of this host
 * (network byte order), or 127.0.0.1 as fallback.
 * If env UOIP_LOCAL_IP is set it takes priority. */
uint32_t uoip_get_local_ipv4(void);

#endif /* UOIP_U_NET_H */
