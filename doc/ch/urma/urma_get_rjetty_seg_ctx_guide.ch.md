# `urma_get_rjetty` / `urma_get_seg_ctx` / `urma_get_rjfr` API 介绍及用法

## 概述

URMA 提供了一组"导出"接口，用于将本地创建的 Jetty、JFR 和注册的 Segment 信息序列化为可交换的格式，通过网络传递给对端后，对端可调用对应的 `urma_import_*` 接口在本地重建远程句柄，进而发起 RDMA 通信。

### 接口对照表

| 导出接口 | 释放接口 | 导入接口 | 资源类型 |
|----------|----------|----------|----------|
| `urma_get_rjetty` | `urma_put_rjetty` | `urma_import_jetty` / `urma_import_jetty_ex` | Jetty |
| `urma_get_rjfr` | `urma_put_rjfr` | `urma_import_jfr` / `urma_import_jfr_ex` | JFR |
| `urma_get_seg_ctx` | `urma_put_seg_ctx` | `urma_import_seg` | Segment |

### 典型场景

```
节点 A（资源所有者）                   节点 B（资源使用者）
    │                                        │
    ├─ urma_create_jetty()                   │
    ├─ urma_create_jfr()                     │
    ├─ urma_register_seg()                   │
    │                                        │
    ├─ urma_get_rjetty()                     │
    ├─ urma_get_rjfr()                       │
    ├─ urma_get_seg_ctx()                    │
    │                                        │
    ├─ 序列化并发送 ───────────────────────► ├─ 反序列化
    │                                        ├─ urma_import_jetty()
    │                                        ├─ urma_import_jfr()
    │                                        ├─ urma_import_seg()
    │                                        ├─ urma_bind_jetty() (RC 模式)
    │                                        │
    ├─ urma_put_rjetty()                     │
    ├─ urma_put_rjfr()                       │
    ├─ urma_put_seg_ctx()                    │
    │                                        ├─ urma_post_jetty_send_wr / urma_read / urma_write ...
    │                                        │
    │                                        ├─ urma_unimport_seg()
    │                                        ├─ urma_unimport_jfr()
    │                                        └─ urma_unimport_jetty()
    │
    ├─ urma_delete_jetty()
    ├─ urma_delete_jfr()
    └─ urma_unregister_seg()
```

---

## 接口说明

### `urma_get_rjetty`

```c
urma_status_t urma_get_rjetty(urma_jetty_t *jetty, urma_rjetty_t **rjetty, uint32_t *length);
```

| 参数 | 方向 | 说明 |
|------|------|------|
| `jetty` | in | 本地已创建的 Jetty 句柄（通过 `urma_create_jetty` 获得） |
| `rjetty` | out | 指向 `urma_rjetty_t*` 的指针，函数内部分配内存并填充数据 |
| `length` | out | 返回的 `rjetty` 数据总长度（字节），可能大于 `sizeof(urma_rjetty_t)` |

**返回值：** `URMA_SUCCESS` 成功，`URMA_EINVAL` 参数无效，`URMA_ENOMEM` 内存不足，`URMA_FAIL` 驱动错误。

**说明：**
- 函数内部分配 `urma_rjetty_t` 结构体，填充 Jetty 的远程标识信息（`jetty_id`、`trans_mode`、`policy`、`type`、`flag`、`tp_type` 等）
- 调用者使用完毕后必须通过 `urma_put_rjetty()` 释放
- 返回的 `length` 用于网络传输时确定数据包大小

### `urma_put_rjetty`

```c
void urma_put_rjetty(urma_rjetty_t *rjetty);
```

释放由 `urma_get_rjetty` 分配的内存。

---

### `urma_get_rjfr`

```c
urma_status_t urma_get_rjfr(urma_jfr_t *jfr, urma_rjfr_t **rjfr, uint32_t *length);
```

| 参数 | 方向 | 说明 |
|------|------|------|
| `jfr` | in | 本地已创建的 JFR 句柄（通过 `urma_create_jfr` 获得） |
| `rjfr` | out | 指向 `urma_rjfr_t*` 的指针，函数内部分配内存并填充数据 |
| `length` | out | 返回的 `rjfr` 数据总长度（字节） |

**返回值：** `URMA_SUCCESS` 成功，`URMA_EINVAL` 参数无效，`URMA_ENOMEM` 内存不足，`URMA_FAIL` 驱动错误。

**说明：**
- 函数内部分配 `urma_rjfr_t` 结构体，填充 JFR 的远程标识信息（`jfr_id`、`trans_mode`、`flag`、`tp_type` 等）
- 调用者使用完毕后必须通过 `urma_put_rjfr()` 释放
- 返回的 `length` 用于网络传输时确定数据包大小

### `urma_put_rjfr`

```c
void urma_put_rjfr(urma_rjfr_t *rjfr);
```

释放由 `urma_get_rjfr` 分配的内存。

---

### `urma_get_seg_ctx`

```c
urma_status_t urma_get_seg_ctx(urma_target_seg_t *tseg, urma_seg_t **seg, uint32_t *size);
```

| 参数 | 方向 | 说明 |
|------|------|------|
| `tseg` | in | 本地已注册的 Target Segment 句柄（通过 `urma_register_seg` 获得） |
| `seg` | out | 指向 `urma_seg_t*` 的指针，函数内部分配内存并填充数据 |
| `size` | out | 返回的 `seg` 数据总长度（字节） |

**返回值：** `URMA_SUCCESS` 成功，`URMA_EINVAL` 参数无效，`URMA_ENOMEM` 内存不足，`URMA_FAIL` 驱动错误。

**说明：**
- 函数内部分配 `urma_seg_t` 结构体，填充 Segment 的远程内存信息（`ubva`、`len`、`attr`、`token_id` 等）
- 调用者使用完毕后必须通过 `urma_put_seg_ctx()` 释放
- 返回的 `size` 用于网络传输时确定数据包大小

### `urma_put_seg_ctx`

```c
void urma_put_seg_ctx(urma_seg_t *seg);
```

释放由 `urma_get_seg_ctx` 分配的内存。

---

## 数据结构说明

### `urma_rjetty_t` — 远程 Jetty 信息（交换载荷）

```c
typedef struct urma_rjetty {
    urma_jetty_id_t jetty_id;          // 全局标识：{ eid, uasid, id }
    urma_transport_mode_t trans_mode;  // 传输模式（RM/RC/UC/UD）
    urma_jetty_grp_policy_t policy;    // 调度策略（RR/WRR 等）
    urma_target_type_t type;           // 固定为 URMA_JETTY
    urma_import_jetty_flag_t flag;     // 标志位：token_policy、order_type、share_tp 等
    urma_tp_type_t tp_type;            // 传输协议类型（RTP/CTP/UTP）
} urma_rjetty_t;
```

### `urma_rjfr_t` — 远程 JFR 信息（交换载荷）

```c
typedef struct urma_rjfr {
    urma_jfr_id_t jfr_id;             // 全局标识：{ eid, uasid, id }，与 jetty_id 结构相同
    urma_transport_mode_t trans_mode;  // 传输模式（RM/RC/UC/UD）
    urma_import_jetty_flag_t flag;     // 标志位：token_policy、order_type 等
    urma_tp_type_t tp_type;            // 传输协议类型（RTP/CTP/UTP）
} urma_rjfr_t;
```

### `urma_seg_t` — 远程 Segment 信息（交换载荷）

```c
typedef struct urma_seg {
    urma_ubva_t ubva;        // 远程 Buffer 虚拟地址（含 eid、uasid、va）
    uint64_t len;            // Segment 长度
    urma_seg_attr_t attr;    // 访问属性（读写权限、token_policy、cacheable 等）
    uint32_t token_id;       // Token 标识
} urma_seg_t;
```

### 序列化方式

三种交换载荷均可直接通过 `memcpy` 全部字节发送到对端。接收方使用对应 `length` 大小的缓冲区接收，然后直接转换为对应结构体指针使用。

---

## 用户建链示例代码

以下示例展示两个节点（服务端作为资源所有者，客户端作为资源使用者）通过 TCP 交换 Jetty、JFR 和 Segment 信息的完整流程。

### 服务端（节点 A）— 导出资源

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "urma_api.h"

/* 简化的 TCP 发送封装 */
static int send_all(int sock, const void *buf, uint32_t len)
{
    uint32_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, (const char *)buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* 服务端：导出 Jetty、JFR 和 Segment 信息发送给客户端 */
urma_status_t server_export_resources(int tcp_sock, urma_context_t *ctx,
                                       urma_jetty_t *jetty, urma_jfr_t *jfr,
                                       urma_target_seg_t *tseg)
{
    urma_rjetty_t *rjetty = NULL;
    urma_rjfr_t *rjfr = NULL;
    urma_seg_t *seg = NULL;
    uint32_t rjetty_len = 0, rjfr_len = 0, seg_size = 0;
    urma_status_t ret;

    /* 1. 获取 Jetty 远程信息 */
    ret = urma_get_rjetty(jetty, &rjetty, &rjetty_len);
    if (ret != URMA_SUCCESS) {
        printf("urma_get_rjetty failed: %d\n", ret);
        return ret;
    }

    /* 2. 获取 JFR 远程信息 */
    ret = urma_get_rjfr(jfr, &rjfr, &rjfr_len);
    if (ret != URMA_SUCCESS) {
        urma_put_rjetty(rjetty);
        printf("urma_get_rjfr failed: %d\n", ret);
        return ret;
    }

    /* 3. 获取 Segment 远程信息 */
    ret = urma_get_seg_ctx(tseg, &seg, &seg_size);
    if (ret != URMA_SUCCESS) {
        urma_put_rjetty(rjetty);
        urma_put_rjfr(rjfr);
        printf("urma_get_seg_ctx failed: %d\n", ret);
        return ret;
    }

    /* 4. 依次发送 rjetty / rjfr / seg 长度和数据 */
    uint32_t net_len = htonl(rjetty_len);
    send_all(tcp_sock, &net_len, sizeof(net_len));
    send_all(tcp_sock, rjetty, rjetty_len);

    net_len = htonl(rjfr_len);
    send_all(tcp_sock, &net_len, sizeof(net_len));
    send_all(tcp_sock, rjfr, rjfr_len);

    net_len = htonl(seg_size);
    send_all(tcp_sock, &net_len, sizeof(net_len));
    send_all(tcp_sock, seg, seg_size);

    /* 5. 释放本地缓冲区 */
    urma_put_rjetty(rjetty);
    urma_put_rjfr(rjfr);
    urma_put_seg_ctx(seg);

    printf("Exported resources: rjetty_len=%u, rjfr_len=%u, seg_size=%u\n",
           rjetty_len, rjfr_len, seg_size);
    return URMA_SUCCESS;
}

int main(void)
{
    urma_context_t *ctx;
    urma_jetty_t *jetty;
    urma_jfr_t *jfr;
    urma_target_seg_t *tseg;

    /* 省略：urma_init_context, urma_create_jetty, urma_create_jfr, urma_register_seg */

    int tcp_sock = ...; /* 已连接的 TCP socket */
    server_export_resources(tcp_sock, ctx, jetty, jfr, tseg);

    /* 后续可接收对端的 read/write 请求 */

    return 0;
}
```

### 客户端（节点 B）— 导入资源

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "urma_api.h"

/* 简化的 TCP 接收封装 */
static int recv_all(int sock, void *buf, uint32_t len)
{
    uint32_t rcvd = 0;
    while (rcvd < len) {
        ssize_t n = recv(sock, (char *)buf + rcvd, len - rcvd, 0);
        if (n <= 0) return -1;
        rcvd += n;
    }
    return 0;
}

/* 客户端：接收并导入远程 Jetty、JFR 和 Segment */
urma_status_t client_import_resources(int tcp_sock, urma_context_t *ctx,
                                       urma_target_jetty_t **out_tjetty,
                                       urma_target_jetty_t **out_tjfr,
                                       urma_target_seg_t **out_tseg)
{
    urma_rjetty_t *rjetty = NULL;
    urma_rjfr_t *rjfr = NULL;
    urma_seg_t *seg = NULL;
    uint32_t rjetty_len = 0, rjfr_len = 0, seg_size = 0;
    urma_target_jetty_t *tjetty = NULL;
    urma_target_jetty_t *tjfr = NULL;
    urma_target_seg_t *tseg = NULL;
    urma_token_t token = {0}; /* 根据实际 token_policy 设置 */

    /* 1. 接收 rjetty */
    recv_all(tcp_sock, &rjetty_len, sizeof(rjetty_len));
    rjetty_len = ntohl(rjetty_len);
    rjetty = (urma_rjetty_t *)malloc(rjetty_len);
    if (rjetty == NULL) return URMA_ENOMEM;
    recv_all(tcp_sock, rjetty, rjetty_len);

    /* 2. 接收 rjfr */
    recv_all(tcp_sock, &rjfr_len, sizeof(rjfr_len));
    rjfr_len = ntohl(rjfr_len);
    rjfr = (urma_rjfr_t *)malloc(rjfr_len);
    if (rjfr == NULL) { free(rjetty); return URMA_ENOMEM; }
    recv_all(tcp_sock, rjfr, rjfr_len);

    /* 3. 接收 seg */
    recv_all(tcp_sock, &seg_size, sizeof(seg_size));
    seg_size = ntohl(seg_size);
    seg = (urma_seg_t *)malloc(seg_size);
    if (seg == NULL) { free(rjetty); free(rjfr); return URMA_ENOMEM; }
    recv_all(tcp_sock, seg, seg_size);

    /* 4. 导入远程 Jetty */
    tjetty = urma_import_jetty(ctx, rjetty, &token);
    if (tjetty == NULL) {
        printf("urma_import_jetty failed\n");
        free(rjetty); free(rjfr); free(seg);
        return URMA_FAIL;
    }

    /* 5. 导入远程 JFR */
    tjfr = urma_import_jfr(ctx, rjfr, &token);
    if (tjfr == NULL) {
        printf("urma_import_jfr failed\n");
        urma_unimport_jetty(tjetty);
        free(rjetty); free(rjfr); free(seg);
        return URMA_FAIL;
    }

    /* 6. 导入远程 Segment */
    urma_import_seg_flag_t flag = {0};
    flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    tseg = urma_import_seg(ctx, seg, &token, 0, flag);
    if (tseg == NULL) {
        printf("urma_import_seg failed\n");
        urma_unimport_jetty(tjetty);
        urma_unimport_jfr(tjfr);
        free(rjetty); free(rjfr); free(seg);
        return URMA_FAIL;
    }

    /* 7. （可选）如果是 RC 模式，需要 bind Jetty */
    if (rjetty->trans_mode == URMA_TRANS_MODE_RC) {
        /* urma_bind_jetty(jetty_local, tjetty); */
    }

    free(rjetty);
    free(rjfr);
    free(seg);

    *out_tjetty = tjetty;
    *out_tjfr = tjfr;
    *out_tseg = tseg;
    printf("Imported resources successfully\n");
    return URMA_SUCCESS;
}

int main(void)
{
    urma_context_t *ctx;
    urma_target_jetty_t *tjetty;
    urma_target_jetty_t *tjfr;
    urma_target_seg_t *tseg;

    /* 省略：urma_init_context */

    int tcp_sock = ...; /* 已连接的 TCP socket */
    client_import_resources(tcp_sock, ctx, &tjetty, &tjfr, &tseg);

    /* 现在可以通过 tjetty / tjfr / tseg 发起 RDMA 操作 */
    /* urma_post_jetty_send_wr, urma_post_jfr_wr, urma_read, urma_write ... */

    /* 清理 */
    /* urma_unimport_seg(tseg); */
    /* urma_unimport_jfr(tjfr); */
    /* urma_unimport_jetty(tjetty); */

    return 0;
}
```

---

## `urma_get_rjfr` 设计方案

### 当前缺失的接口

参考 `urma_get_rjetty` / `urma_put_rjetty` 的模式，`urma_rjfr_t` 类型已存在（[urma_types.h:708-713](src/urma/lib/urma/core/include/urma_types.h#L708-L713)），但缺少配套的导出/释放接口：

| 接口 | 状态 | 说明 |
|------|------|------|
| `urma_get_rjfr` | ❌ 缺失 | 导出 JFR 远程信息 |
| `urma_put_rjfr` | ❌ 缺失 | 释放 get_rjfr 分配的内存 |

### `urma_get_rjfr` 接口设计

```c
urma_status_t urma_get_rjfr(urma_jfr_t *jfr, urma_rjfr_t **rjfr, uint32_t *length);
```

#### 参数说明

| 参数 | 方向 | 说明 |
|------|------|------|
| `jfr` | in | 本地已创建的 JFR 句柄（通过 `urma_create_jfr` 获得） |
| `rjfr` | out | 指向 `urma_rjfr_t*` 的指针，函数内部分配内存并填充数据 |
| `length` | out | 返回的 `rjfr` 数据总长度（字节） |

#### 实现逻辑

参考 `urma_get_rjetty` 的实现（[urma_cp_api.c:2135-2185](src/urma/lib/urma/core/urma_cp_api.c#L2135-L2185)），`urma_get_rjfr` 的实现如下：

```c
urma_status_t urma_get_rjfr(urma_jfr_t *jfr, urma_rjfr_t **rjfr, uint32_t *length)
{
    if (jfr == NULL || rjfr == NULL || length == NULL) {
        URMA_LOG_ERR("Invalid parameter.\n");
        return URMA_EINVAL;
    }

    urma_context_t *urma_ctx = jfr->urma_ctx;
    urma_status_t status = urma_validate_ctx_for_remote_query(urma_ctx);
    if (status != URMA_SUCCESS) {
        return status;
    }

    urma_rjfr_t *new_rjfr = NULL;
    if (urma_is_bonding_dev(urma_ctx->dev->name)) {
        status = urma_fetch_bond_user_info(urma_ctx, BONDP_USER_CTL_GET_RJFR,
                                           (uint64_t)(uintptr_t)jfr, sizeof(urma_jfr_t),
                                           (uint64_t)(uintptr_t)&new_rjfr);
        if (status != URMA_SUCCESS) {
            return URMA_FAIL;
        }
    } else {
        new_rjfr = (urma_rjfr_t *)calloc(1, sizeof(urma_rjfr_t));
        if (new_rjfr == NULL) {
            URMA_LOG_ERR("Failed to alloc rjfr.\n");
            return URMA_ENOMEM;
        }
    }

    if (new_rjfr == NULL) {
        URMA_LOG_ERR("Failed to get rjfr.\n");
        return URMA_FAIL;
    }

    new_rjfr->jfr_id = jfr->jfr_id;
    new_rjfr->trans_mode = jfr->jfr_cfg.trans_mode;
    new_rjfr->flag.bs.order_type = jfr->jfr_cfg.flag.bs.order_type;
    new_rjfr->flag.bs.token_policy = jfr->jfr_cfg.flag.bs.token_policy;
    new_rjfr->tp_type = jfr->jfr_cfg.tp_type;

    *rjfr = new_rjfr;
    *length = urma_calc_user_info_total_len(new_rjfr, sizeof(urma_rjfr_t),
                                            new_rjfr->flag.bs.has_user_info != 0);
    return URMA_SUCCESS;
}
```

#### 关键字段映射

| `urma_rjetty_t` 字段 | 数据来源 | `urma_rjfr_t` 字段 | 数据来源 |
|----------------------|----------|---------------------|----------|
| `jetty_id` | `jetty->jetty_id` | `jfr_id` | `jfr->jfr_id` |
| `trans_mode` | `jetty->jetty_cfg.jfs_cfg.trans_mode` | `trans_mode` | `jfr->jfr_cfg.trans_mode` |
| `flag.bs.order_type` | `jetty->jetty_cfg.jfs_cfg.flag.bs.order_type` | `flag.bs.order_type` | `jfr->jfr_cfg.flag.bs.order_type` |
| `flag.bs.token_policy` | `jetty->jetty_cfg.shared.jfr->jfr_cfg.flag.bs.token_policy` | `flag.bs.token_policy` | `jfr->jfr_cfg.flag.bs.token_policy` |
| `tp_type` | jetty 配置 | `tp_type` | `jfr->jfr_cfg.tp_type` |

> **注意：** `urma_rjfr_t` 没有 `policy` 和 `type` 字段，因为 JFR 不需要 jetty_group 调度策略，类型由 `urma_target_type_t::URMA_JFR` 隐含标识。

### `urma_put_rjfr` 接口设计

```c
void urma_put_rjfr(urma_rjfr_t *rjfr);
```

与 `urma_put_rjetty` 一致，内部直接 `free(rjfr)`。

### 需要修改的文件清单

| 文件 | 修改内容 |
|------|----------|
| [urma_api.h](src/urma/lib/urma/core/include/urma_api.h) | 新增 `urma_get_rjfr`、`urma_put_rjfr` 声明 |
| [urma_cp_api.c](src/urma/lib/urma/core/urma_cp_api.c) | 新增 `urma_get_rjfr`、`urma_put_rjfr` 实现 |
| [bondp_api.h](src/urma/lib/urma/bond/bondp_api.h) | 新增 `bondp_get_rjfr` / `bondp_put_rjfr` 声明 |
| [bondp_api.c](src/urma/lib/urma/bond/bondp_api.c) | 新增 bond 层实现 |
| [udma_u_jfr.h](src/urma/hw/udma/udma_u_jfr.h) | 新增 udma 层 `get_rjfr` 声明 |
| [udma_u_jfr.c](src/urma/hw/udma/udma_u_jfr.c) | 新增 udma 层实现 |

---

## 注意事项

1. **对称性**：`urma_get_*` 配套 `urma_put_*` 使用，导出方必须配对调用 put 释放内存。

2. **Token 管理**：导入方需要提供与远程 Segment `token_policy` 一致的 `urma_token_t`。如果 `token_policy` 为 `URMA_TOKEN_POLICY_NONE`，可传入全零的 token。

3. **传输模式**：导入的远程句柄的 `trans_mode` 必须与本地资源匹配才能通信。RC 模式下需要额外调用 `urma_bind_jetty` / `urma_bind_jfr` 绑定本地-远程对。

4. **length / size 字段**：`urma_get_*` 返回的长度可能大于结构体的 `sizeof`，因为尾部可能附加扩展数据（如 `has_user_info` 标志位为 1 时）。网络传输时应使用返回的完整长度，接收方分配相应大小的缓冲区。

5. **生命周期**：导入方使用完毕后应调用 `urma_unimport_jetty` / `urma_unimport_jfr` / `urma_unimport_seg` 释放远程句柄。导出方可在资源导出后正常删除本地资源。