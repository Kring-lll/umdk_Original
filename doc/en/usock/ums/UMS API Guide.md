# UMS
## 1. Supported standard socket API
UMS is a kernel-level network protocol stack that is compatible with the standard socket API and based on the UB network for data transmission, transparently accelerating TCP communication. The current standard socket apis supported by UMS are as follows.

### 1.1 int socket(int domain, int type, int protocal);
Create a socket：
1. The socket type is restricted to SOCK_STREAM. When AF_SMC is used directly, the protocol is 0 (IPv4) or 1 (IPv6); when transparent replacement is performed via libums-preload, AF_INET/AF_INET6 SOCK_STREAM sockets use IPPROTO_IP or IPPROTO_TCP, which the replacement library converts to the corresponding AF_SMC protocol number.
2. In scenarios where UMS is directly used, the domain parameter should be specified as AF_SMC. In scenarios where transparent replacement is used, the domain value should be specified as AF_INET or AF_INET6. For details of the two scenarios, please refer to the UMS User Guide.md.

### 1.2 ssize_t send(int sockfd, const void *buf, size_t len, int flags);
Send data via socket fd.

### 1.3 ssize_t recv(int sockfd, void *buf, size_t len, int flags);
Receive data via socket fd.

### 1.4 ssize_t write(int fd, const void *buf, size_t count);
Send data via socket fd.

### 1.5 ssize_t read(int fd, void *buf, size_t count);
Receive data via socket fd.

### 1.6 int connect(int socket, const struct sockaddr *addr, socklen_t addrlen);
Establish a connection with the peer server.

### 1.7 int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
Bind the local address to the socket fd.

### 1.8 int listen(int sockfd, int backlog);
Convert the socket fd into a server-side socket fd that can receive connections.

### 1.9 int accept(int listenfd, struct sockaddr *addr, socklen_t *addrlen);
Accept the connection from the client side.

### 1.10 int shutdown(int sockfd, int howto);
Disconnect part of the data transmission channel of the socket.

### 1.11 int close(int fd);
Close the socket connection.

### 1.12 int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
Set the parameters related to sockets. Currently, UMS does not support all socket options. The supported options are as follows:
| level       | optname          |      support situation      |
| ----------- | ---------------- | --------------------------- |
| SOL_SOCKET  | SO_SNDBUF        |  Both the management plane and the data plane are supported; actual data buffers are bucketed by 16 KiB × 2^n and rounded up, with a maximum of 2 MiB per data buffer; when buffer creation returns an error other than `-ENOMEM`, smaller buckets are attempted (including data-buffer allocation failures that return `-EAGAIN`)  |
| SOL_SOCKET  | SO_RCVBUF        |  Both the management plane and the data plane are supported; the actual data buffer is rounded up into 16 KiB × 2^n size classes, with a maximum of 2 MiB per data buffer and an additional limit from scatterlist capacity; if buffer creation returns an error other than `-ENOMEM`, a smaller size class will be attempted (including when data buffer allocation fails with `-EAGAIN`)  |
| SOL_SOCKET  | SO_REUSEADDR     |  Management plane is supported |
| SOL_SOCKET  | SO_RCVTIMEO      |  Both the management plane and the data plane are supported |
| IPPROTO_TCP | TCP_USER_TIMEOUT |  Management plane is supported |
| IPPROTO_TCP | TCP_NODELAY      |  Both the management plane and the data plane are supported |
| IPPROTO_TCP | TCP_KEEPINTVL    |  Management plane is supported  |
| IPPROTO_TCP | TCP_KEEPIDLE     |  Management plane is supported  |
| IPPROTO_TCP | TCP_KEEPCNT      |  Management plane is supported  |
| SOL_SOCKET  | SO_KEEPALIVE     |  Management plane is supported, data plane is not supported  |

### 1.13 int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
Get the parameters related to the socket fd.

## DFX API
### 2.1 UMS proc API
UMS supports the use of proc file system to show link status. For IPV4 connections, the usage is:
```bash
cat /proc/net/ums
```
For IPV6 connections, the usage is:
```bash
cat /proc/net/ums6
```

The display items are shown in the following table:
|     item     | introduction |
| ------------ | ------------ |
| SRC_IP:Port  | Local address, denoted as IP:PORT |
| DEST_IP:Port | Peer address, denoted as IP:PORT |
| State        | Indicates the current state of the UMS socket |
| Fallback     | UMS fallback item，True indicates that the connection has fallen back to TCP |
| SRC_EID, JETTY_ID  | The local Jetty's EID and Jetty ID. Displays N/A for non-UMS connections |
| DEST_EID, JETTY_ID | The peer Jetty's EID and Jetty ID. Displays N/A for non-UMS connections |

### 2.2 UMS sysctl API
UMS supports configuring and querying some attributes through the sysctl API. The usage is as follows: \
**query**
```bash
cat /proc/sys/net/ums/[attribute]
# or
sysctl net.ums.[attribute]
```
**config**
```bash
echo [value] > /proc/sys/net/ums/[attribute]
# or
sysctl -w net.ums.[attribute]=[vale]
```

The currently supported attributes and their configurable value are listed in the table below.
| attribute | configurable value | introduction |
| -----------  | ----------- |   -----------        |
| autocorking_size  | [1, 1073741823]       |  Corking size in UMS (bytes)  |
| rcv_buf           | [16384, 2147483648)   |  Receive Buffer Size (bytes)  |
| snd_buf           | [16384, 2147483648)   |  Send Buffer Size (bytes)     |
| dim_enable        | [0, 1]                 |  Dynamic interrupt moderation switch, default 0; 4.x kernel builds do not include the DIM implementation, so setting this parameter does not enable dynamic interrupt moderation; other builds also require the device to support `modify_jfc`, and the value is read during JFC DIM initialization, so changes do not automatically update existing DIM instances  |

**Note**
1. The actual corking size will not exceed half of the send buffer size. If a larger value is configured, it will not take effect. Instead, half of the send buffer size will be used as the effective corking size.
2. These sysctl parameters can be modified repeatedly after the module is loaded. snd_buf, rcv_buf, and autocorking_size are read when a UMS socket is created through socket(); subsequent modifications do not automatically update existing sockets. A socket created by accepting a connection inherits the listening socket's buffer settings; the current accept path neither reads autocorking_size nor copies it from the listening socket. Builds for 4.x kernels do not include the DIM implementation, so dim_enable does not enable dynamic interrupt moderation. Other builds also require the device to support modify_jfc, and this parameter is read only when JFC DIM is initialized; modifying it does not automatically update existing DIM instances. Configure the corresponding parameters before creating service sockets and initializing JFC DIM.

### 2.3 UMS Runtime Module Parameters
For the semantics, valid values, and load-time configuration of UMS kernel module parameters, see [UMS User Guide - 3.4 Kernel Module Parameter Configuration](./UMS%20User%20Guide.md#34-kernel-module-parameter-configuration). This section only describes the runtime access methods.

**Load-time only parameters (permission 0)**

The following parameters cannot be read or written after the UMS kernel module is loaded and running. They can only be specified at module load time (via `insmod`/`modprobe` command parameters, or by writing to `/etc/modprobe.d/ums.conf`):

| Parameter | Runtime Behavior |
| --------- | ---------------- |
| `ub_token_mode` | Not readable or writable; parsed once at load time, the effective value must be confirmed from kernel logs |
| `ub_token_disable` | **Deprecated**, not readable or writable; used to select the effective mode only when `ub_token_mode` remains at the unset sentinel value `UINT_MAX` and this parameter is not `UINT_MAX` (`0`→LEGACY, any other non-sentinel value→DISABLE) |

To check the effective token mode after loading, confirm via kernel logs: the module prints `ub_token_mode=SECURE/LEGACY/DISABLE(<value>)` at load time.
```bash
dmesg | grep "UMS_" | grep "ub_token_mode="
```

**Readable and writable parameters (permission 0644)**

The following parameters are used to validate the UID/GID in ums_agent registration requests in all token modes; only SECURE mode marks the agent as available after successful registration. They can be queried and modified via `/sys/module/ums/parameters/<parameter>`:
```bash
# Query
cat /sys/module/ums/parameters/ums_agent_uid
cat /sys/module/ums/parameters/ums_agent_gid

# Modify (writing returns -EBUSY when the kernel marks ums_agent as available; otherwise, valid values can be written)
echo <UID> > /sys/module/ums/parameters/ums_agent_uid
echo <GID> > /sys/module/ums/parameters/ums_agent_gid
```

| Parameter | sysfs Permission | Runtime Behavior |
| --------- | ---------------- | ---------------- |
| `ums_agent_uid` | 0644 | Readable; writing returns `-EBUSY` when the kernel marks the agent as available; otherwise, valid values can be written |
| `ums_agent_gid` | 0644 | Readable; writing returns `-EBUSY` when the kernel has marked the agent as available; otherwise, valid values can be written |