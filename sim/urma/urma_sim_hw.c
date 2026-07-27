/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 方式 1 核心实现：拦 mmap → 假 doorbell/queue/cq 内存 → 轮询线程解析 WQE + memcpy + 造 CQE。
 *
 * 真 udma provider 代码原样跑，sim 只在它 mmap(dev_fd, offset) 时给假地址。
 * 真 udma 把 WQE 写进假 queue、把 pi 写进假 doorbell；sim 轮询线程发现 pi 变化，
 * 解析 WQE（udma_jfs_sqe_ctl 位域）执行 READ/WRITE memcpy，再往假 cq 写 CQE
 * （udma_u_jfc_cqe 位域，对齐 parse_cqe_for_send）。
 */

#include "urma_sim_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>

#include "urma_cmd.h"      /* urma_cmd_udrv_priv_t */
#include "udma_abi.h"      /* udma_create_jetty_ucmd（create 时 udata 传的 cmd，含 jetty_addr/buf_addr/db_addr） */

/* WQE 基本块大小 UDMA_JFS_WQEBB 与 CQE 大小 sizeof(udma_u_jfc_cqe)=64。
 * 提前定义：cq_assoc_register 用 URMA_SIM_CQE_SIZE 推 depth，早于位域结构段。 */
#define URMA_SIM_WQEBB 64
#define URMA_SIM_CQE_SIZE 64

/* udma_wqe_sge（见 udma_u_common.h）：length + token_id + va = 16 字节。
 * 提前定义：jfr_refill_pending（早于位域结构段）读 recv WQE 首 sge 用。
 * 与后面 534 行的完整定义一致（同一类型，重复定义会报 redefinition，故这里只一处）。 */
struct urma_sim_wqe_sge {
    uint32_t length;
    uint32_t token_id;
    uint64_t va;
};

/* 提前声明：urma_sim_jfr_deliver_send（早于 produce_cqe/produce_recv_cqe 段）用 produce_recv_cqe。 */
static struct urma_sim_jfc_cqe *produce_recv_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t jfr_addr,
                                                 uint32_t jetty_id, uint32_t rqe_idx, uint32_t byte_cnt,
                                                 uint8_t cqe_opcode, uint64_t imm_data);

/* === 假内存映射表 === */
urma_sim_mmap_region_t g_sim_mmap_regions[URMA_SIM_MMAP_MAX] = {0};

int urma_sim_mmap_decode_offset(off_t offset, int page_size, uint32_t *cmd, uint32_t *idx)
{
    if (page_size <= 0) {
        return -1;
    }
    unsigned long v = (unsigned long)(offset / page_size);
    *cmd = (uint32_t)(v & 0xf);
    *idx = (uint32_t)((v >> 4) & 0xfffffff);
    return 0;
}

/* 找空闲 region 槽 */
static urma_sim_mmap_region_t *find_free_region(void)
{
    for (int i = 0; i < URMA_SIM_MMAP_MAX; i++) {
        if (!g_sim_mmap_regions[i].in_use) {
            return &g_sim_mmap_regions[i];
        }
    }
    return NULL;
}

void *urma_sim_mmap_alloc(int dev_fd, size_t size, uint32_t cmd, uint32_t idx)
{
    (void)dev_fd;
    urma_sim_mmap_region_t *r = find_free_region();
    if (r == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return NULL;
    }
    r->addr = p;
    r->size = size;
    r->cmd = cmd;
    r->idx = idx;
    r->in_use = 1;
    return p;
}

urma_sim_mmap_region_t *urma_sim_mmap_find(void *addr)
{
    if (addr == NULL) {
        return NULL;
    }
    for (int i = 0; i < URMA_SIM_MMAP_MAX; i++) {
        if (g_sim_mmap_regions[i].in_use) {
            char *base = (char *)g_sim_mmap_regions[i].addr;
            char *end = base + g_sim_mmap_regions[i].size;
            if ((char *)addr >= base && (char *)addr < end) {
                return &g_sim_mmap_regions[i];
            }
        }
    }
    return NULL;
}

int urma_sim_fd_is_cdev(int fd)
{
    urma_sim_fd_t *e = urma_sim_fd_get(fd);
    return e != NULL && e->type == URMA_SIM_FD_CDEV;
}

/* === queue 关联表 === */
urma_sim_queue_assoc_t g_sim_queue_assoc[URMA_SIM_QUEUE_MAX] = {0};

/* === 异常注入（错误 CQE 测试用）===
 * 由环境变量 URMA_SIM_INJECT_STATUS 控制（十进制 src_status，对应 udma_abi.h UDMA_CQE_*）：
 *   =1 → LOCAL_OP_ERR（substatus=0）→ cr URMA_CR_UNSUPPORTED_OPCODE_ERR
 *   =4 → TRANSACTION_RETRY_COUNTER_ERR → cr URMA_CR_RNR_RETRY_CNT_EXC_ERR
 *   =5 → TRANSACTION_ACK_TIMEOUT_ERR → cr URMA_CR_ACK_TIMEOUT_ERR
 *   =0（默认）→ 不注入，造 SUCCESS CQE（正常行为）
 * produce_cqe 命中（造 send 侧 CQE）时设 cqe->status = 该值，验真 udma 错误转换链。
 * 注：只注入 send 侧 CQE（READ/WRITE/SEND 的完成），不注入 recv 侧。 */
uint8_t g_inject_cqe_status = 0;

int urma_sim_queue_assoc_register(uint64_t udata_data)
{
    if (udata_data == 0) {
        return -1;
    }
    urma_cmd_udrv_priv_t *udata = (urma_cmd_udrv_priv_t *)(uintptr_t)udata_data;
    if (udata->in_addr == 0 || udata->in_len < sizeof(struct udma_create_jetty_ucmd)) {
        return -1;
    }
    const struct udma_create_jetty_ucmd *cmd = (const struct udma_create_jetty_ucmd *)(uintptr_t)udata->in_addr;
    for (int i = 0; i < URMA_SIM_QUEUE_MAX; i++) {
        if (!g_sim_queue_assoc[i].in_use) {
            g_sim_queue_assoc[i].id = 0;
            g_sim_queue_assoc[i].jetty_addr = cmd->jetty_addr;
            g_sim_queue_assoc[i].buf_addr = cmd->buf_addr;
            g_sim_queue_assoc[i].db_addr = 0;
            g_sim_queue_assoc[i].pi_seen = 0;
            g_sim_queue_assoc[i].in_use = 1;
            /* 区分 sq/rq：CREATE_JFR 的 ucmd 带 idx_addr（索引队列），idx_addr!=0 → rq。
             * CREATE_JETTY/JFS 的 idx_addr=0（sq 不用索引队列）。rq 记 JFR 字段供 SEND 用。 */
            if (cmd->idx_addr != 0) {
                g_sim_queue_assoc[i].q_type = 1;   /* rq（JFR 接收队列） */
                g_sim_queue_assoc[i].max_sge = cmd->buf_len; /* 借 buf_len 暂存？否：buf_len 是 rq.qbuf_size。
                    真 udma max_sge 来自 cfg，ucmd 不直接给。用 idx_len 推：idx_len=idx_que.buf.length，
                    与 max_sge 无直接关系。暂记 0，由 set_id 后另补。 */
                g_sim_queue_assoc[i].max_sge = 0;  /* 待 urma_sim_jfr_set_recv_geom 补 */
                g_sim_queue_assoc[i].wqe_shift = 0;
                g_sim_queue_assoc[i].wqe_cnt = 0;
                g_sim_queue_assoc[i].idx_addr = cmd->idx_addr;
                g_sim_queue_assoc[i].idx_len = cmd->idx_len;
                g_sim_queue_assoc[i].rq_pi_seen = 0;
                g_sim_queue_assoc[i].pending_head = 0;
                g_sim_queue_assoc[i].pending_tail = 0;
                /* JFR 的 db（sw_db）在 ucmd.db_addr；sq 的 db 经 mmap JETTY_DSQE 才填。
                 * JFR 的 db 是 kernel buffer（sw_db 指针），不经 mmap，故在此直接记。 */
                g_sim_queue_assoc[i].db_addr = cmd->db_addr;
                fprintf(stderr, "DBG queue_assoc[%d]: JFR-RQ jetty=%lx buf=%lx sw_db=%lx idx=%lx\n",
                        i, (unsigned long)cmd->jetty_addr, (unsigned long)cmd->buf_addr,
                        (unsigned long)cmd->db_addr, (unsigned long)cmd->idx_addr);
            } else {
                g_sim_queue_assoc[i].q_type = 0;   /* sq */
                /* sq 的 baseblk_cnt（WQE 槽数，环形 mask 用）从 buf_len 推：
                 * buf_len = align(depth*64, page)，baseblk_cnt = depth 向上 2 幂（真 udma roundup_pow_of_two）。
                 * 简化：baseblk_cnt = buf_len/64 向上 2 幂（≥depth）。process_queue 用 (baseblk_cnt-1) 作 pi 索引 mask，
                 * 避免 pi 超 depth 越界写 queue 内存。 */
                uint32_t bb = cmd->buf_len / URMA_SIM_WQEBB;   /* depth 近似 */
                uint32_t bbcnt = 1;
                while (bbcnt < bb) {
                    bbcnt <<= 1;
                }
                g_sim_queue_assoc[i].wqe_cnt = bbcnt;
                fprintf(stderr, "DBG queue_assoc[%d]: jetty=%lx buf=%lx bbcnt=%u\n",
                        i, (unsigned long)cmd->jetty_addr, (unsigned long)cmd->buf_addr, bbcnt);
            }
            return i;
        }
    }
    return -1;
}

void urma_sim_queue_assoc_set_id(int slot, uint32_t id)
{
    if (slot >= 0 && slot < URMA_SIM_QUEUE_MAX && g_sim_queue_assoc[slot].in_use) {
        g_sim_queue_assoc[slot].id = id;
        fprintf(stderr, "DBG queue_assoc[%d].id=%u\n", slot, id);
    }
}

urma_sim_queue_assoc_t *urma_sim_queue_assoc_set_db_by_idx(uint32_t idx, uint64_t db_addr)
{
    for (int i = 0; i < URMA_SIM_QUEUE_MAX; i++) {
        if (g_sim_queue_assoc[i].in_use && g_sim_queue_assoc[i].id == idx) {
            g_sim_queue_assoc[i].db_addr = db_addr;
            fprintf(stderr, "DBG queue_assoc[%d].db=%lx (idx=%u)\n", i, (unsigned long)db_addr, idx);
            return &g_sim_queue_assoc[i];
        }
    }
    return NULL;
}

urma_sim_queue_assoc_t *urma_sim_queue_assoc_find_by_jetty(uint64_t jetty_addr)
{
    if (jetty_addr == 0) {
        return NULL;
    }
    for (int i = 0; i < URMA_SIM_QUEUE_MAX; i++) {
        if (g_sim_queue_assoc[i].in_use && g_sim_queue_assoc[i].jetty_addr == jetty_addr) {
            return &g_sim_queue_assoc[i];
        }
    }
    return NULL;
}

void urma_sim_queue_assoc_set_jfc(int slot, uint32_t jfc_id)
{
    if (slot >= 0 && slot < URMA_SIM_QUEUE_MAX && g_sim_queue_assoc[slot].in_use) {
        g_sim_queue_assoc[slot].jfc_id = jfc_id;
    }
}

/* === CQ 关联表 === */
urma_sim_cq_assoc_t g_sim_cq_assoc[URMA_SIM_CQ_MAX] = {0};

int urma_sim_cq_assoc_register(uint64_t udata_data)
{
    if (udata_data == 0) {
        return -1;
    }
    urma_cmd_udrv_priv_t *udata = (urma_cmd_udrv_priv_t *)(uintptr_t)udata_data;
    if (udata->in_addr == 0 || udata->in_len < sizeof(struct udma_create_jfc_ucmd)) {
        return -1;
    }
    /* jfc_ucmd 和 jetty_ucmd 首字段都是 buf_addr（偏移 0），直接读首字段。
     * buf_len 在偏移 8（udma_create_jfc_ucmd.buf_len），用于推算 CQ 深度→cq_shift。 */
    uint64_t cq_addr = *(const uint64_t *)(uintptr_t)udata->in_addr;
    uint32_t buf_len = 0;
    memcpy(&buf_len, (const void *)(uintptr_t)(udata->in_addr + 8), sizeof(buf_len));
    /* depth = buf_len / cqe_size(64) */
    uint32_t depth = buf_len / URMA_SIM_CQE_SIZE;
    for (int i = 0; i < URMA_SIM_CQ_MAX; i++) {
        if (!g_sim_cq_assoc[i].in_use) {
            g_sim_cq_assoc[i].jfc_id = 0;
            g_sim_cq_assoc[i].cq_addr = cq_addr;  /* CQ 内存地址 */
            g_sim_cq_assoc[i].depth = depth;        /* CQ 深度（造 CQE 算 cq_shift） */
            g_sim_cq_assoc[i].cq_ci = 0;
            g_sim_cq_assoc[i].in_use = 1;
            fprintf(stderr, "DBG cq_assoc[%d]: cq_addr=%lx depth=%u\n",
                    i, (unsigned long)cq_addr, depth);
            return i;
        }
    }
    return -1;
}

void urma_sim_cq_assoc_set_id(int slot, uint32_t jfc_id)
{
    if (slot >= 0 && slot < URMA_SIM_CQ_MAX && g_sim_cq_assoc[slot].in_use) {
        g_sim_cq_assoc[slot].jfc_id = jfc_id;
        fprintf(stderr, "DBG cq_assoc[%d].jfc_id=%u\n", slot, jfc_id);
    }
}

urma_sim_cq_assoc_t *urma_sim_cq_assoc_find(uint32_t jfc_id)
{
    if (jfc_id == 0) {
        return NULL;
    }
    for (int i = 0; i < URMA_SIM_CQ_MAX; i++) {
        if (g_sim_cq_assoc[i].in_use && g_sim_cq_assoc[i].jfc_id == jfc_id) {
            return &g_sim_cq_assoc[i];
        }
    }
    return NULL;
}

/* === tid→seg 内存映射表 ===
 * ummu_grant(tid, va, len, perm, seg_attr) 是真 udma register_seg 把 tid→va 写进
 * 硬件的唯一渠道（硬件靠 ummu_grant 知道 tid 对应哪段内存）。sim 拦 ummu_grant
 * 同样记录 tid→va/len，轮询解到 WQE 远端 tid 时查此表拿 src va 做 memcpy。 */
urma_sim_tid_seg_t g_sim_tid_seg[URMA_SIM_TID_MAX] = {0};

void urma_sim_tid_seg_register(uint32_t tid, uint64_t va, uint64_t len)
{
    if (tid == 0) {
        return;
    }
    /* 先查是否已有该 tid 的登记，有则更新 */
    for (int i = 0; i < URMA_SIM_TID_MAX; i++) {
        if (g_sim_tid_seg[i].in_use && g_sim_tid_seg[i].tid == tid) {
            g_sim_tid_seg[i].va = va;
            g_sim_tid_seg[i].len = len;
            return;
        }
    }
    for (int i = 0; i < URMA_SIM_TID_MAX; i++) {
        if (!g_sim_tid_seg[i].in_use) {
            g_sim_tid_seg[i].tid = tid;
            g_sim_tid_seg[i].va = va;
            g_sim_tid_seg[i].len = len;
            g_sim_tid_seg[i].in_use = 1;
            return;
        }
    }
}

void urma_sim_tid_seg_unregister(uint32_t tid, uint64_t va)
{
    for (int i = 0; i < URMA_SIM_TID_MAX; i++) {
        if (g_sim_tid_seg[i].in_use && g_sim_tid_seg[i].tid == tid &&
            g_sim_tid_seg[i].va == va) {
            g_sim_tid_seg[i].in_use = 0;
            return;
        }
    }
}

urma_sim_tid_seg_t *urma_sim_tid_seg_find(uint32_t tid)
{
    if (tid == 0) {
        return NULL;
    }
    for (int i = 0; i < URMA_SIM_TID_MAX; i++) {
        if (g_sim_tid_seg[i].in_use && g_sim_tid_seg[i].tid == tid) {
            return &g_sim_tid_seg[i];
        }
    }
    return NULL;
}

/* === JFR 按 jetty_id 寻址（SEND 投递到对端 recv 队列） ===
 * jetty_id→jfr_id 映射表：CREATE_JETTY 时用 IN_JFR_ID 绑（urma_sim_jfr_bind_jetty）。
 * 再按 jfr_id 查 JFR 的 rq assoc（其 .id = jfr_id）。 */
typedef struct {
    uint32_t jetty_id;
    uint32_t jfr_id;
    int in_use;
} urma_sim_jfr_bind_t;

#define URMA_SIM_JFR_BIND_MAX 64
static urma_sim_jfr_bind_t g_sim_jfr_bind[URMA_SIM_JFR_BIND_MAX] = {0};

void urma_sim_jfr_bind_jetty(uint32_t jetty_id, uint32_t jfr_id)
{
    if (jfr_id == 0) {
        return;  /* 无共享 JFR（仅 jfs 场景） */
    }
    for (int i = 0; i < URMA_SIM_JFR_BIND_MAX; i++) {
        if (g_sim_jfr_bind[i].in_use && g_sim_jfr_bind[i].jetty_id == jetty_id) {
            g_sim_jfr_bind[i].jfr_id = jfr_id;  /* 更新 */
            return;
        }
    }
    for (int i = 0; i < URMA_SIM_JFR_BIND_MAX; i++) {
        if (!g_sim_jfr_bind[i].in_use) {
            g_sim_jfr_bind[i].jetty_id = jetty_id;
            g_sim_jfr_bind[i].jfr_id = jfr_id;
            g_sim_jfr_bind[i].in_use = 1;
            fprintf(stderr, "DBG jfr_bind: jetty_id=%u → jfr_id=%u\n", jetty_id, jfr_id);
            return;
        }
    }
}

urma_sim_queue_assoc_t *urma_sim_jfr_find_by_jetty(uint32_t jetty_id)
{
    uint32_t jfr_id = 0;
    for (int i = 0; i < URMA_SIM_JFR_BIND_MAX; i++) {
        if (g_sim_jfr_bind[i].in_use && g_sim_jfr_bind[i].jetty_id == jetty_id) {
            jfr_id = g_sim_jfr_bind[i].jfr_id;
            break;
        }
    }
    if (jfr_id == 0) {
        return NULL;
    }
    /* 按 jfr_id 查 JFR 的 rq assoc（q_type=1, .id=jfr_id） */
    for (int i = 0; i < URMA_SIM_QUEUE_MAX; i++) {
        if (g_sim_queue_assoc[i].in_use && g_sim_queue_assoc[i].q_type == 1 &&
            g_sim_queue_assoc[i].id == jfr_id) {
            return &g_sim_queue_assoc[i];
        }
    }
    return NULL;
}

/* ilog2（向上 2 幂后再取 log2），同真 udma UDMA_U_ILOG32(roundup_pow2(x)) */
static uint32_t sim_ilog2_pow2(uint32_t x)
{
    if (x == 0) {
        return 0;
    }
    uint32_t p = 1;
    uint32_t shift = 0;
    while (p < x) {
        p <<= 1;
        shift++;
    }
    return shift;
}

void urma_sim_jfr_set_recv_geom(int slot, uint32_t max_sge, uint32_t depth)
{
    if (slot < 0 || slot >= URMA_SIM_QUEUE_MAX || !g_sim_queue_assoc[slot].in_use) {
        return;
    }
    urma_sim_queue_assoc_t *qa = &g_sim_queue_assoc[slot];
    /* max_sge 向上 2 幂（真 udma roundup_pow_of_two(cfg->max_sge)）；若 0 给默认 1 */
    uint32_t sge_pow = 1;
    while (sge_pow < max_sge && sge_pow < (1u << 20)) {
        sge_pow <<= 1;
    }
    qa->max_sge = sge_pow;
    /* wqe_shift = ilog2(UDMA_SGE_SIZE(16) * sge_pow)（udma_u_jfr.c:47） */
    qa->wqe_shift = sim_ilog2_pow2(16 * sge_pow);
    /* wqe_cnt = depth 向上 2 幂（udma_u_init_jfr_param: roundup_pow_of_two(depth)） */
    uint32_t cnt = 1;
    while (cnt < depth && cnt < (1u << 24)) {
        cnt <<= 1;
    }
    qa->wqe_cnt = cnt;
    fprintf(stderr, "DBG jfr_geom[%d]: max_sge=%u(%u) wqe_shift=%u wqe_cnt=%u\n",
            slot, max_sge, sge_pow, qa->wqe_shift, qa->wqe_cnt);
}

/* 从 JFR 的 rq 里取一个待用 recv 槽（rqe_idx + va）。
 * 真 udma post recv：写 rq.pi 到 sw_db（record db，db_addr），写 wqe_idx 进索引队列（idx_addr），
 * 写 recv WQE（含 sge va/length）进 rq.qbuf。sim 轮询 sw_db 的 pi 变化发现新 recv，
 * 读索引队列拿 wqe_idx，读 recv WQE 拿首 sge.va，存进 pending 队列供 SEND 取用。
 * 返回取到的槽信息（rqe_idx 写进 *out_rqe，va 写进 *out_va），无槽返回 -1。 */
static int jfr_pop_pending(urma_sim_queue_assoc_t *qa, uint32_t *out_rqe, uint64_t *out_va)
{
    if (qa == NULL || qa->pending_head == qa->pending_tail) {
        return -1;  /* 空 */
    }
    uint32_t pos = qa->pending_head & 0xff;
    if (out_rqe != NULL) {
        *out_rqe = qa->pending_rqe[pos];
    }
    if (out_va != NULL) {
        *out_va = qa->pending_va[pos];
    }
    qa->pending_head++;
    return 0;
}

/* rq 轮询：扫 JFR 的 sw_db（db_addr）pi 变化，把新 post 的 recv WQE 登记进 pending 队列。
 * 由主轮询线程对 q_type=1 的 assoc 周期调用。 */
static void jfr_refill_pending(urma_sim_queue_assoc_t *qa)
{
    if (qa->db_addr == 0 || qa->buf_addr == 0 || qa->idx_addr == 0 || qa->wqe_shift == 0) {
        return;
    }
    volatile uint32_t *sw_db = (volatile uint32_t *)(uintptr_t)qa->db_addr;
    uint32_t pi = *sw_db;
    if (pi == qa->rq_pi_seen) {
        return;
    }
    uint32_t mask = qa->wqe_cnt > 0 ? (qa->wqe_cnt - 1) : 0;
    /* 处理 rq_pi_seen..pi-1 的每个新 recv。索引队列每项 4B（wqe_idx），按 head=pi&(wqe_cnt-1) 排。 */
    while (qa->rq_pi_seen != pi) {
        uint32_t head = qa->rq_pi_seen & mask;
        uint32_t wqe_idx = *((volatile uint32_t *)(uintptr_t)(qa->idx_addr + (uint64_t)head * 4));
        /* recv WQE = buf_addr + (wqe_idx << wqe_shift)，首 sge {length,token_id,va} */
        uint8_t *wqe = (uint8_t *)(uintptr_t)(qa->buf_addr + ((uint64_t)wqe_idx << qa->wqe_shift));
        struct urma_sim_wqe_sge *sge = (struct urma_sim_wqe_sge *)wqe;
        uint32_t cnt = qa->pending_tail - qa->pending_head;  /* 环形容量 256 */
        if (cnt < 256) {
            uint32_t pos = qa->pending_tail & 0xff;
            qa->pending_rqe[pos] = wqe_idx;
            qa->pending_va[pos] = sge->va;
            qa->pending_tail++;
        }
        qa->rq_pi_seen++;
        fprintf(stderr, "DBG jfr_refill: pi=%u wqe_idx=%u va=%lx (pending=%u)\n",
                qa->rq_pi_seen, wqe_idx, (unsigned long)sge->va, cnt + 1);
    }
}

/* SEND 到达本端 JFR：从 pending 队列取一个 recv 槽，memcpy 数据进其 va，造 recv CQE。
 * is_local=1 时由本进程 process_wqe(SEND) 调；is_local=0 时由 IPC 后台线程调（对端 SEND 来）。
 * jfr_qa=对端 JFR 的 rq assoc（含 pending 队列 + CQ via jfc_id）。
 * 内部先 jfr_refill_pending 吸新 post 的 recv（跨进程 IPC 线程也得自己吸，不能只靠主轮询）。
 * 返回 0 成功，-1 无 recv 槽（RNR）/失败。 */
int urma_sim_jfr_deliver_send(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                              const void *data, uint32_t len,
                              uint8_t cqe_opcode, uint64_t imm_data)
{
    if (jfr_qa == NULL) {
        return -1;
    }
    jfr_refill_pending(jfr_qa);   /* 吸新 post 的 recv（主轮询可能还没扫到） */
    uint32_t rqe_idx = 0;
    uint64_t recv_va = 0;
    if (jfr_pop_pending(jfr_qa, &rqe_idx, &recv_va) != 0) {
        fprintf(stderr, "DBG jfr_deliver: no recv slot (RNR) jetty=%u\n", jetty_id);
        return -1;  /* 对端还没 post recv → RNR */
    }
    if (recv_va != 0 && len > 0) {
        memcpy((void *)(uintptr_t)recv_va, data, len);
    }
    /* 造 recv CQE 写进 JFR 关联的 CQ（jfc_id）。真 udma poll 读到 s_r=1 CQE，
     * 用 local_id(=jetty_id) 查 jfr_table 拿 jfr，用 entry_idx(=rqe_idx) 从 wrid[] 还原 user_ctx。 */
    urma_sim_cq_assoc_t *cq_a = urma_sim_cq_assoc_find(jfr_qa->jfc_id);
    if (cq_a != NULL) {
        produce_recv_cqe(cq_a, jfr_qa->jetty_addr, jetty_id, rqe_idx, len, cqe_opcode, imm_data);
        fprintf(stderr, "DBG jfr_deliver: recv CQE jetty=%u rqe=%u len=%u op=%u imm=%lx\n",
                jetty_id, rqe_idx, len, cqe_opcode, (unsigned long)imm_data);
    } else {
        fprintf(stderr, "DBG jfr_deliver: no CQ for jfc_id=%u\n", jfr_qa->jfc_id);
        return -1;
    }
    return 0;
}

/* 投递 WRITE_WITH_IMM/NOTIFY 的 imm 通知给本进程 JFR recv CQE（不搬数据）。
 * 与 deliver_send 区别：数据已由 WRITE 写进远端 seg，这里只产 recv CQE 带 imm
 * （从 recv 槽 wrid 还原 user_ctx，byte_cnt=0，recv buf 不被填）。
 * is_local=1 时由 process_wqe(WRITE_WITH_IMM) 调。返回 0 成功，-1 无 recv 槽（RNR）。 */
int urma_sim_jfr_deliver_imm(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                             uint8_t cqe_opcode, uint64_t imm_data)
{
    if (jfr_qa == NULL) {
        return -1;
    }
    jfr_refill_pending(jfr_qa);
    uint32_t rqe_idx = 0;
    uint64_t recv_va = 0;
    if (jfr_pop_pending(jfr_qa, &rqe_idx, &recv_va) != 0) {
        fprintf(stderr, "DBG jfr_deliver_imm: no recv slot (RNR) jetty=%u\n", jetty_id);
        return -1;
    }
    (void)recv_va;   /* WRITE_WITH_IMM 数据写 seg，不进 recv 槽 */
    urma_sim_cq_assoc_t *cq_a = urma_sim_cq_assoc_find(jfr_qa->jfc_id);
    if (cq_a != NULL) {
        produce_recv_cqe(cq_a, jfr_qa->jetty_addr, jetty_id, rqe_idx, 0, cqe_opcode, imm_data);
        fprintf(stderr, "DBG jfr_deliver_imm: recv CQE jetty=%u rqe=%u op=%u imm=%lx\n",
                jetty_id, rqe_idx, cqe_opcode, (unsigned long)imm_data);
    } else {
        fprintf(stderr, "DBG jfr_deliver_imm: no CQ for jfc_id=%u\n", jfr_qa->jfc_id);
        return -1;
    }
    return 0;
}

/* === 轮询线程：检测 doorbell pi 变化 → 读 WQE → memcpy → 造 CQE ===
 * 真 udma post 时写 doorbell：*(db_addr + UDMA_DOORBELL_OFFSET) = pi（udma_u_jfs.h:162）。
 * sim 轮询扫 queue_assoc，看 db_addr+0x80 的 pi 变化，发现后读 WQE 执行 memcpy，
 * 再造 CQE 写到 CQ（待完整实现，当前先验证 pi 检测 + WQE opcode 读取）。 */

#define URMA_SIM_DOORBELL_OFFSET 0x80

/* === WQE/CQE 位域结构镜像（与 udma_u_jfs.h/udma_u_jfc.h 一致，独立定义避免 include 链断） ===
 * udma_u_common.h → udma_u_ctl.h 缺失，故 udma_u_jfs.h/jfc.h 不能直接 include。
 * 在此镜像最小位域结构 + offsetof 取偏移，保证与真 udma 编译出的布局一致
 * （aarch64 little-endian，位域从最低有效位开始打包）。 */
#define URMA_SIM_EID_SIZE 16   /* URMA_EID_SIZE=16（urma_types.h），rmt_eid[16] */

/* udma_jfs_sqe_ctl（见 udma_u_jfs.h）。sizeof = 4+4+4+4+16+4+4+4+4 = 48。
 * 真 udma 用 udma_inc_ptr_wrap(ctrl, sizeof(udma_jfs_sqe_ctl), ...) 放 sge → sge 在 off 48。 */
struct urma_sim_sqe_ctl {
    uint32_t sqe_bb_idx : 16;       /* off 0 */
    uint32_t flag : 7;
    uint32_t udf_flag : 1;
    uint32_t rsv0 : 3;
    uint32_t nf : 1;
    uint32_t token_en : 1;
    uint32_t rmt_jetty_type : 2;
    uint32_t owner : 1;             /* off 3 */
    uint32_t target_hint : 8;       /* off 4 */
    uint32_t opcode : 8;            /* off 5 */
    uint32_t rsv1 : 6;
    uint32_t inline_msg_len : 10;   /* off 7 */
    uint32_t tp_id : 24;            /* off 8 */
    uint32_t sge_num : 8;           /* off 11 */
    uint32_t rmt_jetty_or_seg_id : 20;  /* off 12 */
    uint32_t write_len : 10;
    uint32_t rsv2 : 2;              /* off 15 */
    uint8_t rmt_eid[URMA_SIM_EID_SIZE];  /* off 16-32 */
    uint32_t rmt_token_value;       /* off 32 */
    uint32_t udf_type : 8;          /* off 36 */
    uint32_t reduce_data_type : 4;
    uint32_t reduce_opcode : 4;
    uint32_t rsv3 : 16;             /* off 39 */
    uint32_t rmt_addr_l_or_token_id;     /* off 40 */
    uint32_t rmt_addr_h_or_token_value;  /* off 44 */
};  /* sizeof = 48 */
_Static_assert(sizeof(struct urma_sim_sqe_ctl) == 48, "sqe_ctl size mismatch");
/* opcode/rmt_jetty 等是位域，C 标准/GCC 不允许对位域取 offsetof（取地址非法），
 * 故无法用 _Static_assert 校位域偏移。布局靠 sizeof + 非位域成员锚点校验：
 * rmt_addr_l_or_token_id 是 sqe_ctl 内最后一个普通 uint32 成员（非位域），@40。 */
_Static_assert(offsetof(struct urma_sim_sqe_ctl, rmt_addr_l_or_token_id) == 40, "rmt_addr_l off");

/* urma_sim_wqe_sge 已在文件头提前定义（jfr_refill 用） */

/* udma_u_jfc_cqe（见 udma_u_jfc.h）。sizeof = 64。 */
struct urma_sim_jfc_cqe {
    uint32_t s_r : 1;          /* off 0 */
    uint32_t is_jetty : 1;
    uint32_t owner : 1;       /* bit2 — get_next_cqe 用 owner^valid_owner==0 判有效 */
    uint32_t inline_en : 1;
    uint32_t opcode : 3;
    uint32_t fd : 1;
    uint32_t rsv : 8;
    uint32_t substatus : 8;
    uint32_t status : 8;       /* off 3 — UDMA_CQE_SUCCESS=0 */
    uint32_t entry_idx : 16;   /* off 4 */
    uint32_t local_num_l : 16; /* off 6 */
    uint32_t local_num_h : 4;  /* off 8 */
    uint32_t rmt_idx : 20;
    uint32_t rsv1 : 8;         /* off 11 */
    uint32_t tpn : 24;         /* off 12 */
    uint32_t rsv2 : 8;         /* off 15 */
    uint32_t byte_cnt;         /* off 16 */
    uint32_t user_data_l;      /* off 20 */
    uint32_t user_data_h;      /* off 24 */
    uint32_t rmt_eid[4];       /* off 28 */
    uint32_t data_l;           /* off 44 */
    uint32_t data_h;           /* off 48 */
    uint32_t inline_data[3];   /* off 52 */
};  /* sizeof = 64 */
_Static_assert(sizeof(struct urma_sim_jfc_cqe) == 64, "cqe size mismatch");
/* owner/entry_idx 是位域，不能取 offsetof；布局靠非位域锚点 user_data_l@20、user_data_h@24 校验 */
_Static_assert(offsetof(struct urma_sim_jfc_cqe, user_data_l) == 20, "user_data_l off");
_Static_assert(offsetof(struct urma_sim_jfc_cqe, user_data_h) == 24, "user_data_h off");
/* CQE_FOR_SEND=0, status UDMA_CQE_SUCCESS=0, is_jetty=0；owner 直接用位域赋值 */

/* 把 64 位 queue ptr 拆 low/high 写进 CQE 的 user_data_l/h。
 * 真 udma 用 (user_data_h<<32)|user_data_l 还原 queue 指针。 */
static void cqe_set_user_data(struct urma_sim_jfc_cqe *cqe, uint64_t queue_addr)
{
    cqe->user_data_l = (uint32_t)(queue_addr & 0xffffffff);
    cqe->user_data_h = (uint32_t)((queue_addr >> 32) & 0xffffffff);
}

/* 造一个 send CQE（s_r=0）写到 CQ。按 cq_ci 算槽位 + owner，填 user_data=queue、entry_idx、
 * byte_cnt、owner=valid_owner、status=SUCCESS、local_num=jetty_id、is_jetty。返回写入的 CQE 槽地址。
 * cr->local_id = local_num_h<<16|local_num_l（udma_u_jfc.c:759，所有 CQE 都经此设 local_id），
 * send CQE 也需填，integration test 检查 cr.local_id == jetty_id.id。 */
static struct urma_sim_jfc_cqe *produce_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t queue_addr,
                                             uint32_t jetty_id, uint32_t entry_idx, uint32_t byte_cnt,
                                             uint8_t cqe_opcode)
{
    if (cq_a == NULL || cq_a->cq_addr == 0) {
        return NULL;
    }
    /* CQ 参数（与真 udma 一致，cqe_size=64 → baseblk_shift=6）：
     *   baseblk_cnt = qbuf_size >> 6；cq_shift = align_power2(baseblk_cnt)
     *   sim 用 depth 近似 baseblk_cnt（qbuf_size=align(depth*64,4096)） */
    uint32_t depth = cq_a->depth > 0 ? cq_a->depth : 64;
    /* baseblk_cnt = depth 向上 2 幂（真 udma alloc_queue_buf 用 roundup_pow_of_two） */
    uint32_t bbcnt = 1;
    while (bbcnt < depth) {
        bbcnt <<= 1;
    }
    uint32_t mask = bbcnt - 1;
    uint32_t cq_shift = 0;
    while ((1U << cq_shift) < bbcnt) {
        cq_shift++;
    }
    uint32_t ci = cq_a->cq_ci;
    uint32_t valid_owner = (ci >> cq_shift) & UDMA_JFC_DB_VALID_OWNER_M;
    struct urma_sim_jfc_cqe *cqe =
        (struct urma_sim_jfc_cqe *)(uintptr_t)(cq_a->cq_addr + (uint64_t)(ci & mask) * URMA_SIM_CQE_SIZE);
    memset(cqe, 0, URMA_SIM_CQE_SIZE);
    /* get_next_cqe 认 CQE 有效条件：(cqe->owner ^ valid_owner) != INVALID(0)，
     * 即 owner 必须与 valid_owner 相反（相等则视为空 CQE）。故 owner = valid_owner ^ 1。 */
    cqe->owner = valid_owner ^ 1;
    cqe->is_jetty = 1;            /* jetty 的 send CQE，parse_cqe_for_send cr.flag.bs.jetty 用 */
    cqe->opcode = cqe_opcode & 0x7;   /* hw_cqe_opcode：SEND=0/SEND_WITH_IMM=1/...，send 侧 CQE 用 */
    cqe->entry_idx = entry_idx & 0xffff;
    cqe->byte_cnt = byte_cnt;
    cqe->local_num_l = (uint16_t)(jetty_id & 0xffff);   /* cr->local_id 低 16 位 */
    cqe->local_num_h = (jetty_id >> 16) & 0xf;          /* cr->local_id 高 4 位 */
    cqe_set_user_data(cqe, queue_addr);   /* parse_cqe_for_send 用来找回 queue */
    /* 异常注入：命中时造 error CQE（status≠0），验真 udma get_cr_status 转换链。
     * 默认 g_inject_cqe_status=0（SUCCESS，正常行为）。 */
    if (g_inject_cqe_status != 0) {
        cqe->status = g_inject_cqe_status;
        fprintf(stderr, "DBG inject: CQE status=%u (entry_idx=%u)\n", cqe->status, entry_idx);
    }
    cq_a->cq_ci = ci + 1;
    return cqe;
}

/* 造一个 recv CQE（s_r=1）写到 CQ。与 send CQE 不同：
 * - s_r=1（路由到 parse_cqe_for_recv，udma_u_jfc.c:763）
 * - is_jetty=1（shared jetty 的 recv，udma_u_update_jfr_idx:646 走 jetty_table 查 jfr）
 * - local_num_l/h = jetty_id（cr->local_id，查 jfr_table/jetty_table，udma_u_jfc.c:642/759）
 * - entry_idx = rqe_idx（cr->user_ctx = queue->wrid[entry_idx & mask]，udma_u_jfc.c:675，关键）
 * - opcode = HW_CQE_OPC_SEND(0)（cr->opcode=URMA_CR_OPC_SEND，udma_u_jfc.c:598）
 * - user_data_l/h = jfr/jetty 对象指针（parse_cqe_for_recv 设 cr->user_data=&jfr->base，665）
 *   注：parse_cqe_for_recv 不读 CQE 的 user_data_l/h 还原 queue（它用 local_id 查表），
 *   故 user_data 此处填 jfr 对象指针与真 udma 一致（jfr_addr）。
 * 返回写入的 CQE 槽地址。 */
static struct urma_sim_jfc_cqe *produce_recv_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t jfr_addr,
                                                 uint32_t jetty_id, uint32_t rqe_idx, uint32_t byte_cnt,
                                                 uint8_t cqe_opcode, uint64_t imm_data)
{
    if (cq_a == NULL || cq_a->cq_addr == 0) {
        return NULL;
    }
    uint32_t depth = cq_a->depth > 0 ? cq_a->depth : 64;
    uint32_t bbcnt = 1;
    while (bbcnt < depth) {
        bbcnt <<= 1;
    }
    uint32_t mask = bbcnt - 1;
    uint32_t cq_shift = 0;
    while ((1U << cq_shift) < bbcnt) {
        cq_shift++;
    }
    uint32_t ci = cq_a->cq_ci;
    uint32_t valid_owner = (ci >> cq_shift) & UDMA_JFC_DB_VALID_OWNER_M;
    struct urma_sim_jfc_cqe *cqe =
        (struct urma_sim_jfc_cqe *)(uintptr_t)(cq_a->cq_addr + (uint64_t)(ci & mask) * URMA_SIM_CQE_SIZE);
    memset(cqe, 0, URMA_SIM_CQE_SIZE);
    cqe->owner = valid_owner ^ 1;
    cqe->s_r = 1;                 /* CQE_FOR_RECEIVE=1 */
    cqe->is_jetty = 1;            /* recv 属 shared jetty */
    cqe->opcode = cqe_opcode & 0x7;   /* HW_CQE_OPC_SEND=0/SEND_WITH_IMM=1/SEND_WITH_INV=2/WRITE_WITH_IMM=3 */
    cqe->entry_idx = rqe_idx & 0xffff;
    cqe->byte_cnt = byte_cnt;
    /* IMM 变体：imm_data 填 data_l/h（urma parse_cqe_for_recv: imm = (data_h<<32)|data_l）。
     * SEND_WITH_INVALID 的 token 也走 data_l（cqe->data_l & inv_mask）。 */
    cqe->data_l = (uint32_t)(imm_data & 0xffffffff);
    cqe->data_h = (uint32_t)((imm_data >> 32) & 0xffffffff);
    /* local_num = jetty_id：低 16 位 local_num_l，高 4 位 local_num_h（jetty_id 20 位够） */
    cqe->local_num_l = (uint16_t)(jetty_id & 0xffff);
    cqe->local_num_h = (jetty_id >> 16) & 0xf;
    cqe_set_user_data(cqe, jfr_addr);   /* cr->user_data=&jfr->base */
    cq_a->cq_ci = ci + 1;
    return cqe;
}

/* 处理一条 WQE：按 opcode 做 READ/WRITE memcpy + 造 CQE。
 * 单进程仿真：远端 seg 与本端在同地址空间，import_seg 复用了真实 va，
 * 故 WQE 的 rmt_addr（rmt_addr_l/h 拼）就是远端真实地址，可直接 memcpy。
 * tid→seg 映射（ummu_grant 登记）作为远端地址的兜底/校验（单进程冗余）。 */
static void process_wqe(urma_sim_queue_assoc_t *qa, uint8_t *wqe_buf, uint32_t wqe_idx)
{
    struct urma_sim_sqe_ctl *wqe = (struct urma_sim_sqe_ctl *)wqe_buf;
    uint8_t opcode = (uint8_t)wqe->opcode;
    /* 远端地址 = (rmt_addr_h << 32) | rmt_addr_l（udma_fill_read/write_sqe 写入）。
     * 单进程下就是对端 va；跨进程下无意义（对端进程指针），改用 rmt_tid + rmt_eid 经 IPC。 */
    uint64_t rmt_addr = ((uint64_t)wqe->rmt_addr_h_or_token_value << 32) |
                        (uint64_t)wqe->rmt_addr_l_or_token_id;
    uint32_t tid = wqe->rmt_jetty_or_seg_id & 0xfffff;
    uint32_t rmt_jetty_id = wqe->rmt_jetty_or_seg_id & 0xfffff;  /* SEND 用：对端 jetty id */
    const uint8_t *rmt_eid = wqe->rmt_eid;   /* 对端 EID（16B），test 链路天然带的 */
    int is_local = urma_sim_ipc_is_local(rmt_eid);   /* 本进程→memcpy；对端→IPC */
    /* ctl_len（WQE 头长度）随 opcode 变（udma_u_jfs.h: get_ctl_len）：
     *   SEND/SEND_IMM/SEND_INV/READ/WRITE/CAS/FAA: SQE_NORMAL_CTL_LEN=48
     *   WRITE_WITH_IMM(4): 64（imm@48）；WRITE_WITH_NOTIFY(5): 80（notify_addr@56/notify_data@64）
     * 首 SGE 在 WQE 头之后（offset=ctl_len）。sim 的 urma_sim_sqe_ctl 只是头 48B 的镜像，
     * IMM/NOTIFY 变体头更长，按 opcode 算偏移。 */
    uint32_t ctl_len = 48;  /* SQE_NORMAL_CTL_LEN */
    if (opcode == 4) {       /* WRITE_WITH_IMM */
        ctl_len = 64;        /* SQE_WRITE_IMM_CTL_LEN */
    } else if (opcode == 5) { /* WRITE_WITH_NOTIFY */
        ctl_len = 80;        /* SQE_WRITE_NOTIFY_CTL_LEN */
    }
    struct urma_sim_wqe_sge *sge = (struct urma_sim_wqe_sge *)((uint8_t *)wqe + ctl_len);
    uint64_t local_va = sge->va;
    uint32_t local_len = sge->length;

    uint32_t done_len = 0;
    /* CQE opcode（hw_cqe_opcode 枚举，udma_abi.h）：默认按 WQE opcode 映射。
     * SEND/SEND_IMM/SEND_INV/WRITE_IMM 有专门 CQE opcode（parse_cqe 据此设 cr.opcode + 取 imm/inv）。
     * READ/WRITE/原子等走 SEND CQE(opcode=0)。 */
    uint8_t cqe_opcode = 0;   /* HW_CQE_OPC_SEND=0 */
    uint64_t imm_data = 0;    /* IMM 变体的 immediate 数据（填 CQE data_l/h） */
    int has_imm = 0;
    switch (opcode) {
        case 0:  /* UDMA_OPCODE_SEND=0（投递到对端 JFR recv 队列）。
                   * 与 READ/WRITE 不同：WQE 不带 rmt_addr，数据投进对端预先 post 的 recv WQE buf。
                   * rmt_jetty_or_seg_id = 对端 jetty id（非 seg tid）。 */
        case 1:  /* UDMA_OPCODE_SEND_WITH_IMM=1：SEND + 带 imm_data，recv CQE opcode=SEND_WITH_IMM */
        case 2: { /* UDMA_OPCODE_SEND_WITH_INVALID=2：SEND + 带 invalidate token，recv CQE opcode=SEND_WITH_INV */
            /* A 类变体：数据投递同 SEND。udma_fill_send_sqe 不填 rmt_addr（SEND 不用），
             * IMM/INV 复用偏移 40/44 这 8 字节放 imm/token（udma_u_jfs.c:835 SQE_SEND_IMM_FIELD=40）。
             * 故 rmt_addr（process_wqe 开头算的 rmt_addr_l/h 拼）= imm_data（IMM）或 token（INV）。 */
            if (opcode == 1) { cqe_opcode = 1; has_imm = 1; imm_data = rmt_addr; }
            if (opcode == 2) { cqe_opcode = 2; imm_data = rmt_addr; }  /* token 走 data_l */
            void *src = (void *)(uintptr_t)local_va;
            if (src != NULL && local_len > 0) {
                if (is_local) {
                    urma_sim_queue_assoc_t *jfr_qa = urma_sim_jfr_find_by_jetty(rmt_jetty_id);
                    if (jfr_qa != NULL) {
                        if (urma_sim_jfr_deliver_send(jfr_qa, rmt_jetty_id, src, local_len,
                                                      cqe_opcode, imm_data) == 0) {
                            done_len = local_len;
                        }
                    } else {
                        fprintf(stderr, "DBG poll: SEND no JFR for jetty=%u\n", rmt_jetty_id);
                    }
                } else {
                    if (urma_sim_ipc_send(rmt_eid, rmt_jetty_id, src, local_len, qa->jfc_id,
                                          cqe_opcode, imm_data) == 0) {
                        done_len = local_len;
                    } else {
                        fprintf(stderr, "DBG poll: ipc_send fail jetty=%u\n", rmt_jetty_id);
                    }
                }
            }
            break;
        }
        case 6: { /* UDMA_OPCODE_READ=6（从远端 src 读到本端 dst） */
            void *dst = (void *)(uintptr_t)local_va;
            if (dst != NULL && local_len > 0) {
                if (is_local) {
                    void *src = (void *)(uintptr_t)rmt_addr;
                    if (src != NULL) {
                        memcpy(dst, src, local_len);
                        done_len = local_len;
                    }
                } else {
                    if (urma_sim_ipc_read(rmt_eid, tid, dst, local_len) == 0) {
                        done_len = local_len;
                    } else {
                        /* 链路故障或 IPC 失败：造 error CQE 而非 success */
                        fprintf(stderr, "DBG poll: ipc_read fail tid=%u (link down?)\n", tid);
                        g_inject_cqe_status = 5;  /* ACK_TIMEOUT 表示链路故障 */
                    }
                }
            }
            break;
        }
        case 3:  /* UDMA_OPCODE_WRITE=3（从本端 src 写到远端 dst） */
        case 4:  /* UDMA_OPCODE_WRITE_WITH_IMM=4：WRITE + imm，对端 recv CQE opcode=WRITE_WITH_IMM(3) */
        case 5: { /* UDMA_OPCODE_WRITE_WITH_NOTIFY=5：WRITE + notify（notify_data 同 imm 机制） */
            /* A 类变体：数据搬运同 WRITE（写到远端 seg）。imm/notify 在 WQE 头扩展区：
             * WRITE_WITH_IMM imm@48（SQE_WRITE_IMM_FIELD，头内 ctl_len=64 之前）；
             * WRITE_WITH_NOTIFY notify_data@64（SQE_NOTIFY_DATA_FIELD）。投递给对端 JFR recv CQE。
             * 注：imm 投递要求对端预先 post recv WQE（与 SEND_WITH_IMM 同路径，parse_cqe_for_recv）。
             *   单进程：写 seg + 给本进程 JFR 产 recv CQE（imm 从 recv 槽 wrid 还原 user_ctx）。
             *   跨进程：TODO——需新 IPC 把 imm 通知投给对端 JFR（数据走 ipc_write 写 seg）。*/
            if (opcode == 4) {
                has_imm = 1;
                imm_data = *(const uint64_t *)((const uint8_t *)wqe + 48);  /* SQE_WRITE_IMM_FIELD */
                cqe_opcode = 3;  /* HW_CQE_OPC_WRITE_WITH_IMM */
            } else if (opcode == 5) {
                has_imm = 1;
                imm_data = *(const uint64_t *)((const uint8_t *)wqe + 64);  /* SQE_NOTIFY_DATA_FIELD */
                cqe_opcode = 3;  /* notify 走 WRITE_WITH_IMM CQE 路径（无专门 notify opcode） */
            }
            void *src = (void *)(uintptr_t)local_va;
            if (src != NULL && local_len > 0) {
                if (is_local) {
                    void *dst = (void *)(uintptr_t)rmt_addr;
                    if (dst != NULL) {
                        memcpy(dst, src, local_len);
                        done_len = local_len;
                    }
                    /* WRITE_WITH_IMM/NOTIFY：单进程额外给本进程 JFR 产 recv CQE 带 imm。
                     * 按 rmt_jetty_id 查 JFR（jetty 的共享 jfr），取 recv 槽 wrid 还原 user_ctx。
                     * 数据不进 recv 槽（已写 seg），只产 imm 通知 CQE。 */
                    if (has_imm) {
                        urma_sim_queue_assoc_t *jfr_qa = urma_sim_jfr_find_by_jetty(rmt_jetty_id);
                        if (jfr_qa != NULL) {
                            urma_sim_jfr_deliver_imm(jfr_qa, rmt_jetty_id, cqe_opcode, imm_data);
                        }
                    }
                } else {
                    if (urma_sim_ipc_write(rmt_eid, tid, src, local_len) == 0) {
                        done_len = local_len;
                    } else {
                        fprintf(stderr, "DBG poll: ipc_write fail tid=%u\n", tid);
                    }
                    /* 跨进程 imm 通知投递 TODO：需 IPC 把 (jetty_id, imm) 投给对端 JFR。
                     * 现暂不投，imm 丢失（数据已正确写 seg）。*/
                    if (has_imm) {
                        fprintf(stderr, "DBG poll: WRITE_IMM cross-proc imm notify TODO jetty=%u\n",
                                rmt_jetty_id);
                    }
                }
            }
            break;
        }
        case 7:   /* UDMA_OPCODE_CAS=7：compare-and-swap（远端值==cmp 则换 swap，原值回本端） */
        case 0xb: { /* UDMA_OPCODE_FAA=0xb：fetch-and-add（远端值+=operand，原值回本端） */
            /* B 类原子：读-改-写远端值。原子长度 len=首SGE.length（4 或 8，CAS 可 16）。
             * WQE 布局：头(48) + 首 SGE(48-64, va=本端 buf 存原值, length=len) + 原子数据(64+)。
             *   CAS: wqe+64 放 swap_data(len), wqe+64+len 放 cmp_data(len)
             *   FAA: wqe+64 放 operand(len)
             * rmt_addr = 远端目标地址（CAS/FAA 操作的原子变量地址）。
             * 单进程：远端地址在本进程，直接读-改-写。跨进程：需 IPC OP_ATOMIC（TODO）。 */
            uint32_t atom_len = local_len;   /* 首 SGE length = 原子长度 */
            if (atom_len == 0 || atom_len > 16) {
                done_len = 0;
                break;
            }
            /* 本端 buf（存读到的原值）：首 SGE 的 va = local_va */
            void *local_buf = (void *)(uintptr_t)local_va;
            /* 原子数据在 wqe+64（SQE_ATOMIC_DATA_FIELD） */
            const uint8_t *atom_data = (const uint8_t *)wqe + 64;
            if (is_local) {
                /* 单进程：远端 rmt_addr 直接操作 */
                void *rmt = (void *)(uintptr_t)rmt_addr;
                if (rmt == NULL) { done_len = 0; break; }
                /* 先读远端原值进本端 buf（CAS/FAA 都要返回原值） */
                memcpy(local_buf, rmt, atom_len);
                if (opcode == 7) {  /* CAS：若远端值==cmp_data 则换 swap_data */
                    const uint8_t *swap = atom_data;           /* wqe+64: swap_data */
                    const uint8_t *cmp = atom_data + atom_len; /* wqe+64+len: cmp_data */
                    if (memcmp(rmt, cmp, atom_len) == 0) {
                        memcpy(rmt, swap, atom_len);
                    }
                } else {  /* FAA：远端值 += operand */
                    /* operand 在 wqe+64，len 字节。按无符号整数加（4/8 字节） */
                    const uint8_t *operand = atom_data;
                    if (atom_len == 4) {
                        uint32_t old = *(const uint32_t *)rmt;
                        uint32_t op;
                        memcpy(&op, operand, 4);
                        *(uint32_t *)rmt = old + op;
                    } else if (atom_len == 8) {
                        uint64_t old = *(const uint64_t *)rmt;
                        uint64_t op;
                        memcpy(&op, operand, 8);
                        *(uint64_t *)rmt = old + op;
                    }
                    /* 16 字节 FAA 不支持（check_atomic_len 只 CAS 允许 16） */
                }
                done_len = atom_len;
            } else {
                /* 跨进程：需 IPC OP_ATOMIC（读远端值+条件写+回原值）。TODO。
                 * 现造成功 CQE 但不真做原子（数据未动）。 */
                fprintf(stderr, "DBG poll: atomic cross-proc TODO opcode=%u tid=%u\n", opcode, tid);
                done_len = atom_len;
            }
            break;
        }
        case 0x19: { /* UDMA_OPCODE_WRITE_WITH_ATOMICSTORE_ADD=0x19：write + atomic add（复合，TODO）*/
            /* 字段布局复杂（eid@16/rmt_addr@24/atomic_addr@40/data@48/...），TODO。 */
            done_len = local_len;
            break;
        }
        case 0x11: { /* UDMA_OPCODE_NOP=0x11：no-op，不搬数据只占位产 CQE */
            done_len = 0;
            break;
        }
        default:
            /* 未知 opcode：造成功 CQE 不搬数据，让 poll 推进 */
            done_len = local_len;
            break;
    }
    fprintf(stderr, "DBG poll: WQE idx=%u opcode=%u tid=%u local_va=%lx len=%u rmt=%lx %s cqe_op=%u imm=%u/%lx\n",
            wqe_idx, opcode, tid, (unsigned long)local_va, local_len, (unsigned long)rmt_addr,
            is_local ? "local" : "ipc", cqe_opcode, has_imm, (unsigned long)imm_data);

    /* 造 CQE 写到关联的 CQ。entry_idx=wqe_idx（pi），让真 udma 更新 queue->ci */
    urma_sim_cq_assoc_t *cq_a = urma_sim_cq_assoc_find(qa->jfc_id);
    if (cq_a != NULL) {
        produce_cqe(cq_a, qa->jetty_addr, qa->id, wqe_idx, done_len, cqe_opcode);
    } else {
        fprintf(stderr, "DBG poll: no CQ for jfc_id=%u, CQE dropped\n", qa->jfc_id);
    }
}

static void process_queue(urma_sim_queue_assoc_t *qa)
{
    if (qa->db_addr == 0 || qa->buf_addr == 0) {
        return;
    }
    volatile uint32_t *pi_ptr = (volatile uint32_t *)(uintptr_t)(qa->db_addr + URMA_SIM_DOORBELL_OFFSET);
    uint32_t pi = *pi_ptr;
    if (pi == qa->pi_seen) {
        return;  /* 无变化 */
    }
    /* 处理 pi_seen..pi-1 的每个 WQE。WQE 按 pi 索引，在 buf_addr + (pi&mask)*64。
     * baseblk_shift=UDMA_JFS_WQEBB_SHIFT=6（WQEBB=64），mask=baseblk_cnt-1（=wqe_cnt-1）。
     * 用 pi&mask 索引，环形回绕——pi 超 depth 不会越界写 queue 内存（之前裸 pi_seen++ 会越界）。
     * 注：sim 简化每 WQE 占 1 个 WQEBB（真 udma sqe_bb_cnt 可能 >1，但测试用例都是单 WQEBB）。 */
    uint32_t mask = (qa->wqe_cnt > 0) ? (qa->wqe_cnt - 1) : 0;
    while (qa->pi_seen != pi) {
        uint32_t idx = qa->pi_seen & mask;
        uint8_t *wqe = (uint8_t *)(uintptr_t)(qa->buf_addr + (uint64_t)idx * URMA_SIM_WQEBB);
        process_wqe(qa, wqe, idx);
        /* pi_seen 步进按该 WQE 的 wqebb_cnt（WQE 占几个 64B 块）：
         * 普通单 WQEBB WQE =1；CAS/FAA 原子 WQE 占 2（UDMA_ATOMIC_WQE_BB_NUM=2，放 cmp/swap/operand）。
         * 真 udma pi 按 wqebb_cnt 步进，sim 若按 1 会把多块 WQE 当多个独立空 WQE 处理。 */
        uint8_t op = ((struct urma_sim_sqe_ctl *)wqe)->opcode;
        uint32_t step = (op == 7 || op == 0xb) ? 2 : 1;   /* CAS(7)/FAA(0xb) 占 2 WQEBB */
        qa->pi_seen += step;
    }
}

static atomic_int g_hw_thread_run = 0;
static pthread_t g_hw_thread;

static void *urma_sim_hw_thread(void *arg)
{
    (void)arg;
    while (atomic_load(&g_hw_thread_run)) {
        for (int i = 0; i < URMA_SIM_QUEUE_MAX; i++) {
            if (!g_sim_queue_assoc[i].in_use || g_sim_queue_assoc[i].db_addr == 0) {
                continue;
            }
            if (g_sim_queue_assoc[i].q_type == 1) {
                /* JFR recv 队列：扫 sw_db(pi) 变化，吸新 post 的 recv 进 pending 队列（供 SEND 取用） */
                jfr_refill_pending(&g_sim_queue_assoc[i]);
            } else {
                /* sq 发送队列：扫 doorbell pi 变化，解 WQE 执行 READ/WRITE/SEND */
                process_queue(&g_sim_queue_assoc[i]);
            }
        }
        usleep(100);
    }
    return NULL;
}

void urma_sim_hw_start(void)
{
    if (atomic_exchange(&g_hw_thread_run, 1) != 0) {
        return; /* 已启动 */
    }
    pthread_create(&g_hw_thread, NULL, urma_sim_hw_thread, NULL);
}
