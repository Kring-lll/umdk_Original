/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 跨进程 IPC（方式1 扩展）：双进程实时数据传递。
 *
 * 单进程仿真靠 memcpy（同地址空间，test_hw_post 自引用已验证）。但跨进程时
 * A 进程解引用 B 进程的 buf 指针 = SIGSEGV。故用 unix socket 实时传数据：
 *   - 每进程 sim 在 create_context 拿到本进程 eid 后，起后台线程 listen
 *     unix socket，路径含 eid 低 64 位（/tmp/urma_sim_ipc_<hex>）。
 *   - A 解 WQE 拿 rmt_eid=B 的 eid（test 链路天然带的，import_jetty/post 填），
 *     推算 B 的 socket 路径 → 连 B → 发读/写请求。
 *   - B 后台线程收到请求：tid → 查本进程 g_sim_tid_seg 表 → B 的 buf 指针
 *     → 读/写 B 的 buf（同进程，指针有效）→ 回 A。
 *   - tid 只在本进程查本进程指针；eid 定位对端进程。零共享存储，纯 IPC。
 *
 * 协议（极简定长头 + 变长数据）：
 *   请求:  magic(4)=0x53494d55  op(4)=1/2  tid(4)  len(4)  [WRITE 后跟 len 字节]
 *   响应:  magic(4)            status(4)  len(4)        [READ 后跟 len 字节]
 */

#include "urma_sim_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define URMA_SIM_IPC_MAGIC 0x53494d55u   /* "SIMU" */
#define URMA_SIM_IPC_OP_READ  1
#define URMA_SIM_IPC_OP_WRITE 2
#define URMA_SIM_IPC_OP_SEND  3   /* 投递到对端 JFR recv 队列：tid 字段复用为 jetty_id */
#define URMA_SIM_IPC_SOCK_DIR "/tmp"

/* 本进程 eid（16B，与 WQE 里 rmt_eid 同格式=swap 后的 le_eid）。
 * udma_u_import_jetty_ex 用 udma_u_swap_endian128 把 rjetty.eid 反转存进 le_eid，
 * post 时 rmt_eid = le_eid（反转版）。故 sim 全程用反转版，比较/寻址才一致。 */
static uint8_t g_local_eid[16] = {0};
static atomic_int g_ipc_started = 0;
static pthread_t g_ipc_thread;

/* 链路故障注入：按 eid 前 8 字节 hex 设置 down。
 * 环境变量 URMA_SIM_LINK_DOWN=hex 下指定 eid 的链路 down（IPC 请求返错）。
 * URMA_SIM_LINK_UP=hex 恢复。 */
static uint8_t g_link_down_eid[16] = {0};
static int g_link_down_active = 0;

static void link_fault_init(void)
{
    const char *down = getenv("URMA_SIM_LINK_DOWN");
    if (down != NULL && strlen(down) >= 16) {
        for (int i = 0; i < 8; i++) {
            unsigned int v;
            sscanf(down + i * 2, "%02x", &v);
            g_link_down_eid[i] = (uint8_t)v;
        }
        g_link_down_active = 1;
        fprintf(stderr, "DBG ipc: link DOWN for eid %02x%02x%02x%02x%02x%02x%02x%02x\n",
                g_link_down_eid[0],g_link_down_eid[1],g_link_down_eid[2],g_link_down_eid[3],
                g_link_down_eid[4],g_link_down_eid[5],g_link_down_eid[6],g_link_down_eid[7]);
    }
    const char *up = getenv("URMA_SIM_LINK_UP");
    if (up != NULL && g_link_down_active) {
        g_link_down_active = 0;
        fprintf(stderr, "DBG ipc: link UP (fault cleared)\n");
    }
}

/* 检查 rmt_eid 是否处于故障 down 状态 */
static int link_is_down(const uint8_t *rmt_eid)
{
    if (!g_link_down_active) return 0;
    return memcmp(rmt_eid, g_link_down_eid, 8) == 0;
}

/* 16 字节反转（与 udma_u_swap_endian128 一致）。 */
static void swap_eid128(const uint8_t *src, uint8_t *dst)
{
    for (int i = 0; i < 16; i++) {
        dst[i] = src[15 - i];
    }
}

/* eid 转 hex 路径。注意 WQE 里 rmt_eid 是 le_eid（swap 反转版），故此处的 eid
 * 入参也是 swap 后的格式。用前 8 字节（swap 后含 IPv4 段，有区分度；
 * 末 8 字节 swap 后是原前 8 字节=全 0 前缀，会撞，故不用末 8）。 */
static void eid_to_sockpath(const uint8_t *eid, char *path, size_t sz)
{
    const uint8_t *p = eid;   /* 前 8 字节 */
    int n = snprintf(path, sz, "%s/urma_sim_ipc_", URMA_SIM_IPC_SOCK_DIR);
    for (int i = 0; i < 8 && n < (int)sz; i++) {
        n += snprintf(path + n, sz - n, "%02x", p[i]);
    }
}

/* 判断 rmt_eid 是否本进程（与 g_local_eid 完全相等）。
 * 若 g_local_eid 全 0（urma_sim_ipc_start 未调，单进程未启用 IPC），视为本进程
 * （走 memcpy 兼容单进程自引用，如 test_hw_post 不调 ipc_start 的场景）。 */
int urma_sim_ipc_is_local(const uint8_t *rmt_eid)
{
    if (rmt_eid == NULL) {
        return 1;   /* NULL 视为本进程 */
    }
    /* g_local_eid 全 0 → IPC 未启用，一律走本进程 memcpy */
    int all_zero = 1;
    for (int i = 0; i < 16; i++) {
        if (g_local_eid[i] != 0) { all_zero = 0; break; }
    }
    if (all_zero) {
        return 1;
    }
    return memcmp(rmt_eid, g_local_eid, 16) == 0 ? 1 : 0;
}

/* 全读/全写（处理 partial read/write）。返回 0 成功 -1 失败。 */
static int recv_all(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, (char *)buf + got, n - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        got += r;
    }
    return 0;
}
static int send_all(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t r = send(fd, (const char *)buf + sent, n - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        sent += r;
    }
    return 0;
}

/* 后台线程：处理一个连接。读请求 → 查本进程 tid 表 → 读/写 buf → 回响应。 */
static void handle_conn(int cfd)
{
    /* 协议头：基础 4 个 u32(magic/op/tid/len)。SEND 在 magic 校验后还要收 cqe_opcode+imm（3 个 u32）。 */
    uint32_t hdr[4];   /* magic, op, tid, len */
    if (recv_all(cfd, hdr, sizeof(hdr)) != 0) {
        goto done;
    }
    uint32_t magic = hdr[0], op = hdr[1], tid = hdr[2], len = hdr[3];
    if (magic != URMA_SIM_IPC_MAGIC || len > (16u << 20)) {   /* 上限 16MB 防恶意/异常 */
        goto done;
    }
    /* SEND：tid 字段复用为对端 jetty_id。先收数据，再投递到本进程 JFR recv 队列。 */
    if (op == URMA_SIM_IPC_OP_SEND) {
        /* SEND 协议头扩展：再收 cqe_opcode(1) + imm_l(1) + imm_h(1) = 3 个 u32 */
        uint32_t ext[3];
        if (recv_all(cfd, ext, sizeof(ext)) != 0) {
            uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, (uint32_t)-1, 0};
            send_all(cfd, resp, sizeof(resp));
            goto done;
        }
        uint8_t cqe_opcode = (uint8_t)ext[0];
        uint64_t imm_data = ((uint64_t)ext[2] << 32) | (uint64_t)ext[1];
        char *data = (char *)malloc(len > 0 ? len : 1);
        if (data == NULL) {
            uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, (uint32_t)-1, 0};
            send_all(cfd, resp, sizeof(resp));
            goto done;
        }
        if (len > 0 && recv_all(cfd, data, len) != 0) {
            free(data);
            uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, (uint32_t)-1, 0};
            send_all(cfd, resp, sizeof(resp));
            goto done;
        }
        /* tid = 对端 jetty_id → 查本进程 JFR rq → 投递 + 造 recv CQE（带 imm/opcode） */
        urma_sim_queue_assoc_t *jfr_qa = urma_sim_jfr_find_by_jetty(tid);
        int rc = -1;
        if (jfr_qa != NULL) {
            rc = urma_sim_jfr_deliver_send(jfr_qa, tid, data, len, cqe_opcode, imm_data);
        }
        free(data);
        uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, (rc == 0) ? 0u : (uint32_t)-1, 0};
        send_all(cfd, resp, sizeof(resp));
        goto done;
    }
    /* 查本进程 tid → buf */
    urma_sim_tid_seg_t *ts = urma_sim_tid_seg_find(tid);
    if (ts == NULL || ts->va == 0) {
        uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, (uint32_t)-1, 0};
        send_all(cfd, resp, sizeof(resp));
        goto done;
    }
    uint32_t act_len = len;
    if ((uint64_t)act_len > ts->len) {
        act_len = (uint32_t)ts->len;
    }
    if (op == URMA_SIM_IPC_OP_READ) {
        /* B 读自己的 buf → 发给 A */
        char *stackbuf = NULL;
        char *buf = (char *)(uintptr_t)ts->va;
        (void)stackbuf;
        uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, 0, act_len};
        if (send_all(cfd, resp, sizeof(resp)) != 0) goto done;
        if (send_all(cfd, buf, act_len) != 0) goto done;
    } else if (op == URMA_SIM_IPC_OP_WRITE) {
        /* A 发来的数据 → 写进 B 的 buf */
        char *buf = (char *)(uintptr_t)ts->va;
        char *tmp = (char *)malloc(act_len);
        if (tmp == NULL) goto done;
        if (recv_all(cfd, tmp, act_len) != 0) { free(tmp); goto done; }
        memcpy(buf, tmp, act_len);   /* B 同进程,va 有效 */
        free(tmp);
        uint32_t resp[3] = {URMA_SIM_IPC_MAGIC, 0, act_len};
        send_all(cfd, resp, sizeof(resp));
    }
done:
    close(cfd);
}

static void *urma_sim_ipc_thread(void *arg)
{
    (void)arg;
    int lfd = (int)(intptr_t)arg;
    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_conn(cfd);
    }
    return NULL;
}

void urma_sim_ipc_start(const uint8_t *eid)
{
    if (atomic_exchange(&g_ipc_started, 1) != 0) {
        return;   /* 已起 */
    }
    if (eid != NULL) {
        /* 存 swap 版，与 WQE 里 rmt_eid（le_eid，反转版）同格式，比较/寻址一致 */
        swap_eid128(eid, g_local_eid);
    }
    link_fault_init();  /* 初始化链路故障注入（读环境变量）*/
    char path[96];   /* 实际约 40 字节，留余量但 < sun_path(108) 防 -Wformat-truncation */
    eid_to_sockpath(g_local_eid, path, sizeof(path));
    unlink(path);   /* 清理上次残留 */

    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) {
        fprintf(stderr, "DBG ipc: socket fail %s\n", strerror(errno));
        atomic_store(&g_ipc_started, 0);
        return;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* sun_path 108 字节，path 最大约 40 字节，不会截断 */
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "DBG ipc: bind %s fail %s\n", path, strerror(errno));
        close(lfd);
        atomic_store(&g_ipc_started, 0);
        return;
    }
    if (listen(lfd, 16) != 0) {
        fprintf(stderr, "DBG ipc: listen fail %s\n", strerror(errno));
        close(lfd);
        atomic_store(&g_ipc_started, 0);
        return;
    }
    fprintf(stderr, "DBG ipc: listen %s (lfd=%d)\n", path, lfd);
    pthread_create(&g_ipc_thread, NULL, urma_sim_ipc_thread, (void *)(intptr_t)lfd);
    pthread_detach(g_ipc_thread);
}

/* 客户端：连 rmt_eid 对应进程的 socket。返回 fd，<0 失败。 */
static int ipc_connect(const uint8_t *rmt_eid)
{
    char path[96];
    eid_to_sockpath(rmt_eid, path, sizeof(path));
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    /* 重试连接（对端可能还没起） */
    for (int i = 0; i < 50; i++) {
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            return fd;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) {
            break;
        }
        usleep(10000);   /* 10ms × 50 = 0.5s */
    }
    fprintf(stderr, "DBG ipc: connect %s fail %s\n", path, strerror(errno));
    close(fd);
    return -1;
}

int urma_sim_ipc_read(const uint8_t *rmt_eid, uint32_t tid, void *local_buf, uint32_t len)
{
    if (link_is_down(rmt_eid)) return -1;
    int fd = ipc_connect(rmt_eid);
    if (fd < 0) return -1;
    uint32_t hdr[4] = {URMA_SIM_IPC_MAGIC, URMA_SIM_IPC_OP_READ, tid, len};
    if (send_all(fd, hdr, sizeof(hdr)) != 0) { close(fd); return -1; }
    uint32_t resp[3];
    if (recv_all(fd, resp, sizeof(resp)) != 0) { close(fd); return -1; }
    if (resp[0] != URMA_SIM_IPC_MAGIC || resp[1] != 0) { close(fd); return -1; }
    uint32_t act = resp[2];
    if (act > len) act = len;
    if (recv_all(fd, local_buf, act) != 0) { close(fd); return -1; }
    close(fd);
    return 0;
}

int urma_sim_ipc_write(const uint8_t *rmt_eid, uint32_t tid, const void *local_buf, uint32_t len)
{
    if (link_is_down(rmt_eid)) return -1;
    int fd = ipc_connect(rmt_eid);
    if (fd < 0) return -1;
    uint32_t hdr[4] = {URMA_SIM_IPC_MAGIC, URMA_SIM_IPC_OP_WRITE, tid, len};
    if (send_all(fd, hdr, sizeof(hdr)) != 0) { close(fd); return -1; }
    if (send_all(fd, local_buf, len) != 0) { close(fd); return -1; }
    uint32_t resp[3];
    if (recv_all(fd, resp, sizeof(resp)) != 0) { close(fd); return -1; }
    if (resp[0] != URMA_SIM_IPC_MAGIC || resp[1] != 0) { close(fd); return -1; }
    close(fd);
    return 0;
}

/* 跨进程 SEND：连 rmt_eid 对端进程 socket，发 {op=SEND, jetty_id, len, data}，
 * 对端查自己 JFR recv 槽 → memcpy 进 recv buf → 造 recv CQE → 回 status。
 * tid 参数这里语义是 jetty_id（对端 JFR 的 jetty id，WQE 里 rmt_jetty_or_seg_id）。
 * cqe_opcode/imm_data：IMM 变体（SEND_WITH_IMM/INV）的对端 recv CQE opcode + imm/token 数据。 */
int urma_sim_ipc_send(const uint8_t *rmt_eid, uint32_t jetty_id,
                      const void *data, uint32_t len, uint32_t src_jfc_id,
                      uint8_t cqe_opcode, uint64_t imm_data)
{
    (void)src_jfc_id;   /* 保留接口，当前不用（send CQE 由发起方自己的 process_wqe 造） */
    if (link_is_down(rmt_eid)) return -1;
    int fd = ipc_connect(rmt_eid);
    if (fd < 0) return -1;
    /* 协议头扩展：magic/op/tid(jetty_id)/len + cqe_opcode(4) + imm_l(4) + imm_h(4) = 7 个 u32 */
    uint32_t hdr[7] = {URMA_SIM_IPC_MAGIC, URMA_SIM_IPC_OP_SEND, jetty_id, len,
                        (uint32_t)cqe_opcode,
                        (uint32_t)(imm_data & 0xffffffff),
                        (uint32_t)((imm_data >> 32) & 0xffffffff)};
    if (send_all(fd, hdr, sizeof(hdr)) != 0) { close(fd); return -1; }
    if (len > 0 && send_all(fd, data, len) != 0) { close(fd); return -1; }
    uint32_t resp[3];
    if (recv_all(fd, resp, sizeof(resp)) != 0) { close(fd); return -1; }
    if (resp[0] != URMA_SIM_IPC_MAGIC || resp[1] != 0) { close(fd); return -1; }
    close(fd);
    return 0;
}
