/* SPDX-License-Identifier: MIT */
/* Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * URMA 仿真数据面：CQE 生成 + WQE 解析 + 硬件轮询线程。
 * （从 urma_sim_res.c 拆出，保持各源文件 < 1000 行便于评审） */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

#include "udma_abi.h"  /* UDMA_JFC_DB_VALID_OWNER_M 等 */
#include "urma_sim_res.h"


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
                                             uint8_t cqe_opcode, uint8_t err_status, uint8_t err_substatus,
                                             int is_jetty, uint32_t rmt_idx, uint32_t tpn,
                                             const uint8_t *rmt_eid)
{
    if (cq_a == NULL || cq_a->cq_addr == 0) {
        return NULL;
    }
    (void)pthread_mutex_lock(&g_cq_lock);
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
    cqe->is_jetty = is_jetty;     /* send CQE：jetty 内嵌 sq=1 / 独立 jfs=0（节 3，
                                   * parse_cqe_for_send 按它还原 jetty 或 jfs 对象） */
    cqe->opcode = cqe_opcode & 0x7;   /* hw_cqe_opcode：SEND=0/SEND_WITH_IMM=1/...，send 侧 CQE 用 */
    cqe->entry_idx = entry_idx & 0xffff;
    cqe->byte_cnt = byte_cnt;
    cqe->local_num_l = (uint16_t)(jetty_id & 0xffff);   /* cr->local_id 低 16 位 */
    cqe->local_num_h = (jetty_id >> 16) & 0xf;          /* cr->local_id 高 4 位 */
    /* 节 12: remote 字段——cr.remote_id/tpn/rmt_eid 从 CQE 还原，
     * 此前不填 → 对端信息恒 0，cr.remote_id/cr.tpn 无法校验 */
    cqe->rmt_idx = rmt_idx & 0xfffff;
    cqe->tpn = tpn;
    if (rmt_eid != NULL) {
        memcpy(cqe->rmt_eid, rmt_eid, URMA_SIM_EID_SIZE);
    }
    cqe_set_user_data(cqe, queue_addr);   /* parse_cqe_for_send 用来找回 queue */
    /* 异常注入：命中时造 error CQE（status≠0），验真 udma get_cr_status 转换链。
     * 默认 g_inject_cqe_status=0（SUCCESS，正常行为）。
     * err_status：语义错误（RNR 等）由调用方传入，优先于注入。 */
    uint8_t status = (err_status != 0) ? err_status : g_inject_cqe_status;
    if (status != 0) {
        cqe->status = status;
        /* (status, substatus) 二元组（节 17 ④）：LOCAL_OP_ERR(2)/REMOTE_OP_ERR(3)
         * 必须带 substatus，否则 get_cr_status 判 is_valid=false → JFC_POLL_ERR */
        cqe->substatus = err_substatus;
        SIM_DBG("inject: CQE status=%u substatus=%u (entry_idx=%u)\n",
                cqe->status, cqe->substatus, entry_idx);
    }
    cq_a->cq_ci = ci + 1;
    (void)pthread_mutex_unlock(&g_cq_lock);
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
struct urma_sim_jfc_cqe *produce_recv_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t jfr_addr,
                                                 uint32_t jetty_id, uint32_t rqe_idx, uint32_t byte_cnt,
                                                 uint8_t cqe_opcode, uint64_t imm_data, int is_jetty,
                                                 uint8_t err_status, uint8_t err_substatus)
{
    if (cq_a == NULL || cq_a->cq_addr == 0) {
        return NULL;
    }
    (void)pthread_mutex_lock(&g_cq_lock);
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
    cqe->is_jetty = is_jetty;     /* recv CQE：对端 JFR 属 jetty(共享)=1 / 独立 jfr=0（节 3，
                                   * parse_cqe_for_recv 按它查 jetty_table 或 jfr_table） */
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
    /* 节 17 ④: 错误 recv CQE（如对端长度错误 LOCAL_OP_ERR + LOCAL_LENGTH_ERR） */
    if (err_status != 0) {
        cqe->status = err_status;
        cqe->substatus = err_substatus;
    }
    cqe_set_user_data(cqe, jfr_addr);   /* cr->user_data=&jfr->base */
    cq_a->cq_ci = ci + 1;
    (void)pthread_mutex_unlock(&g_cq_lock);
    return cqe;
}

/* 处理一条 WQE：按 opcode 做 READ/WRITE memcpy + 造 CQE。
 * 单进程仿真：远端 seg 与本端在同地址空间，import_seg 复用了真实 va，
 * 故 WQE 的 rmt_addr（rmt_addr_l/h 拼）就是远端真实地址，可直接 memcpy。
 * tid→seg 映射（ummu_grant 登记）作为远端地址的兜底/校验（单进程冗余）。 */
/* 环形回绕读取：WQE 起始在 qbuf + (idx&mask)*64，但 ctl 头、SGE 列表、inline
 * 数据、原子数据都可能跨队列环形边界——provider 用 udma_inc_ptr_wrap 逐字段
 * 回绕写（udma_u_jfs.c:500,523），sim 不能当连续线性内存读（跨队尾读映射
 * 末尾之外→崩溃/错解析）。先按环形平坦复制到栈缓冲，process_wqe 全部用副本。 */
#define URMA_SIM_WQE_MAX_COPY 512   /* 覆盖：ctl_len(≤80) + 8*SGE(128) + inline/原子数据 */

static void wrap_copy(uint8_t *dst, const uint8_t *qbuf, size_t qsz,
                      uint64_t abs_off, size_t n)
{
    while (n > 0) {
        size_t at = (size_t)(abs_off % qsz);
        size_t chunk = qsz - at;
        if (chunk > n) {
            chunk = n;
        }
        memcpy(dst, qbuf + at, chunk);
        dst += chunk;
        abs_off += chunk;
        n -= chunk;
    }
}

static void process_wqe(urma_sim_queue_assoc_t *qa, uint8_t *wqe_buf, uint32_t wqe_idx)
{
    uint8_t wqe_copy[URMA_SIM_WQE_MAX_COPY];
    uint32_t wqe_cnt = (qa->wqe_cnt > 0) ? qa->wqe_cnt : 1;
    size_t qsz = (size_t)wqe_cnt * URMA_SIM_WQEBB;
    uint64_t wqe_abs = (uint64_t)((uint8_t *)wqe_buf - (uint8_t *)(uintptr_t)qa->buf_addr);
    if (wqe_abs >= qsz) {
        wqe_abs %= qsz;   /* 防御：偏移越界时折回 */
    }
    wrap_copy(wqe_copy, (const uint8_t *)(uintptr_t)qa->buf_addr, qsz, wqe_abs,
              sizeof(wqe_copy));
    struct urma_sim_sqe_ctl *wqe = (struct urma_sim_sqe_ctl *)wqe_copy;
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
    /* 数据源解析（节 1/2）：
     * - inline（flag&0x40，仅 SEND/IMM/INV/WRITE/IMM/NOTIFY）：数据本体在 WQE 头之后
     *   （偏移=ctl_len 起，长度=inline_msg_len），不写 SGE 列表；
     * - 非 inline：SGE 列表在 ctl_len 起，sge_num 段，逐段 {length, token_id, va}。 */
    uint32_t sge_num = wqe->sge_num;
    const struct urma_sim_wqe_sge *sges =
        (const struct urma_sim_wqe_sge *)((uint8_t *)wqe + ctl_len);
    uint64_t local_va = 0;
    uint32_t local_len = 0;
    if ((wqe->flag & 0x40 /* UDMAWQE_INLINE_EN */) && sge_num == 0) {
        local_va = (uint64_t)(uintptr_t)((uint8_t *)wqe + ctl_len);
        local_len = (uint32_t)wqe->inline_msg_len;
    } else if (sge_num > 0) {
        local_va = sges[0].va;
        local_len = sges[0].length;
    }

    uint32_t done_len = 0;
    uint8_t err_status = 0;   /* 语义错误（RNR 等）→ 错误 CQE，见节 10 */
    uint8_t err_substatus = 0; /* 与 status 组成 (status, substatus) 二元组（节 17 ④） */
    uint32_t imm_jetty_id = rmt_jetty_id; /* FG-04: WRITE_WITH_IMM 的对端 jetty id（默认回退） */
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
            /* 多 SGE（节 2）：数据分散在 sges[0..sge_num-1]，拼装成连续消息再投递。
             * inline（sge_num=0）时数据在 local_va（队列内副本），直接投递。
             * 零 SGE/零长 SEND 是合法请求（udma_u_jfs.c:518 sge_num==0 的 WQE，
             * bondp_datapath.c:133 不拒绝）：同样投递并产 recv CQE（byte_cnt=0），
             * 否则接收方永远收不到零长完成、recv 槽不消费 → 握手类用例超时。 */
            {
                char *msg = NULL;
                const void *src = NULL;
                uint32_t msg_len = local_len;
                int payload_ok = 1;
                if (sge_num > 1) {
                    msg = (char *)malloc(local_len > 0 ? local_len : 1);
                    if (msg == NULL) {
                        payload_ok = 0;
                    } else {
                        char *p = msg;
                        for (uint32_t i = 0; i < sge_num; i++) {
                            if (sges[i].va != 0 && sges[i].length > 0) {
                                memcpy(p, (void *)(uintptr_t)sges[i].va, sges[i].length);
                            }
                            p += sges[i].length;
                        }
                        src = msg;
                    }
                } else {
                    src = (void *)(uintptr_t)local_va;   /* 零 SGE 时可能为 NULL */
                }
                if (payload_ok && (src != NULL || msg_len == 0)) {
                    if (is_local) {
                        urma_sim_queue_assoc_t *jfr_qa = urma_sim_jfr_find_by_jetty(rmt_jetty_id);
                        if (jfr_qa != NULL) {
                            if (urma_sim_jfr_deliver_send(jfr_qa, rmt_jetty_id, src, msg_len,
                                                          cqe_opcode, imm_data) == 0) {
                                done_len = msg_len;
                            } else {
                                /* 无 recv 槽（RNR）：错误 CQE（节 10），不再假绿 */
                                err_status = 4;  /* UDMA_CQE_TRANSACTION_RETRY_COUNTER_ERR */
                            }
                        } else {
                            SIM_DBG("poll: SEND no JFR for jetty=%u\n", rmt_jetty_id);
                            err_status = 4;  /* 对端 JFR 不存在同样视为 RNR */
                        }
                    } else {
                        if (urma_sim_ipc_send(rmt_eid, rmt_jetty_id, src, msg_len, qa->jfc_id,
                                              cqe_opcode, imm_data) == 0) {
                            done_len = msg_len;
                        } else {
                            SIM_DBG("poll: ipc_send fail jetty=%u\n", rmt_jetty_id);
                            err_status = 4;
                        }
                    }
                }
                free(msg);
            }
            break;
        }
        case 6: { /* UDMA_OPCODE_READ=6（从远端 src 读到本端 dst） */
            /* 多 SGE（节 2）：dst 各段依次承接 rmt+off 的数据 */
            uint32_t off = 0;
            if (is_local) {
                for (uint32_t i = 0; i < sge_num; i++) {
                    void *dst = (void *)(uintptr_t)sges[i].va;
                    if (dst != NULL && sges[i].length > 0) {
                        void *src = (void *)(uintptr_t)(rmt_addr + off);
                        if (src != NULL) {
                            memcpy(dst, src, sges[i].length);
                            done_len += sges[i].length;
                        }
                    }
                    off += sges[i].length;
                }
            } else {
                /* 跨进程：整段经 IPC 读回（ipc_read 按 tid 段做边界 clamp） */
                if (local_len > 0) {
                    if (urma_sim_ipc_read(rmt_eid, tid, (void *)(uintptr_t)local_va,
                                          local_len) == 0) {
                        done_len = local_len;
                    } else {
                        /* 链路故障或 IPC 失败：造 error CQE 而非 success */
                        SIM_DBG("poll: ipc_read fail tid=%u (link down?)\n", tid);
                        err_status = 5;  /* UDMA_CQE_TRANSACTION_ACK_TIMEOUT_ERR */
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
                /* FG-04: WRITE_WITH_IMM 的对端 jetty id 在 WRITE_IMM_TOKEN_FIELD(56)
                 * 的 token_info.normal.token_id（udma_u_jfs.c:854）；
                 * rmt_jetty_or_seg_id 是 seg tid，不能用于查 JFR。 */
                imm_jetty_id = 0;
                memcpy(&imm_jetty_id, (const uint8_t *)wqe + 56, sizeof(uint32_t));
            } else if (opcode == 5) {
                /* WRITE_WITH_NOTIFY（节 11）：notify_data@64 写 notify_addr@56（门铃），
                 * 对端无 recv CQE（门铃值即通知）。 */
                uint64_t notify_addr = *(const uint64_t *)((const uint8_t *)wqe + 56); /* SQE_NOTIFY_ADDR_FIELD */
                uint64_t notify_data = *(const uint64_t *)((const uint8_t *)wqe + 64); /* SQE_NOTIFY_DATA_FIELD */
                if (notify_addr != 0) {
                    *(volatile uint64_t *)(uintptr_t)notify_addr = notify_data;
                }
            }
            /* 多 SGE（节 2）：src 各段依次搬出，done_len = 各段之和。
             * inline WRITE（udma_u_jfs.c:557：WRITE/WRITE_WITH_IMM/WRITE_WITH_NOTIFY
             * 都适用）：WQE 无 SGE 列表（sge_num=0、非 inline 才设 ctrl->sge_num），
             * 数据在 ctl 头之后的连续区（inline_msg_len）；原循环零次执行导致
             * payload 不搬、跨进程 OK 但同进程假成功——显式搬 inline 数据。 */
            if (is_local) {
                uint32_t off = 0;
                if (local_len > 0 && sge_num == 0) {
                    void *dst = (void *)(uintptr_t)rmt_addr;
                    if (dst != NULL) {
                        memcpy(dst, (void *)(uintptr_t)local_va, local_len);
                        done_len = local_len;
                    }
                } else {
                    for (uint32_t i = 0; i < sge_num; i++) {
                        void *src = (void *)(uintptr_t)sges[i].va;
                        if (src != NULL && sges[i].length > 0) {
                            void *dst = (void *)(uintptr_t)(rmt_addr + off);
                            if (dst != NULL) {
                                memcpy(dst, src, sges[i].length);
                                done_len += sges[i].length;
                            }
                        }
                        off += sges[i].length;
                    }
                }
                /* WRITE_WITH_IMM：单进程额外给本进程 JFR 产 recv CQE 带 imm。
                 * 数据不进 recv 槽（已写 seg），只产 imm 通知 CQE。
                 * 注：WRITE_WITH_NOTIFY 不产 recv CQE（节 11——门铃写内存即通知）。
                 * FG-04: rmt_jetty_or_seg_id 对 WRITE 族是 seg tid；对端 jetty id
                 * 在 WRITE_IMM_TOKEN_FIELD(56) 的 token_info.normal.token_id
                 * （udma_u_jfs.c:854）。 */
                if (has_imm && opcode == 4) {
                    urma_sim_queue_assoc_t *jfr_qa = urma_sim_jfr_find_by_jetty(imm_jetty_id);
                    if (jfr_qa != NULL) {
                        if (urma_sim_jfr_deliver_imm(jfr_qa, imm_jetty_id, cqe_opcode,
                                                     imm_data) != 0) {
                            /* 无 recv 槽：imm 通知投递失败 → RNR 错误 CQE（节 10） */
                            err_status = 4;
                        }
                    } else {
                        SIM_DBG("poll: WRITE_IMM no JFR for jetty=%u\n", imm_jetty_id);
                        err_status = 4;
                    }
                }
            } else {
                /* 跨进程：整段经 IPC 写（ipc_write 按 tid 段做边界 clamp） */
                if (local_len > 0) {
                    if (urma_sim_ipc_write(rmt_eid, tid, (void *)(uintptr_t)local_va,
                                           local_len) == 0) {
                        done_len = local_len;
                    } else {
                        /* FG-06: 跨进程 WRITE 链路失败 → 错误 CQE（与 READ 的
                         * err_status=5 对齐），不再 SUCCESS 假绿 */
                        SIM_DBG("poll: ipc_write fail tid=%u\n", tid);
                        err_status = 5;
                    }
                }
                /* 跨进程 WRITE_WITH_IMM（FG-05）：imm 通知经 IPC 投给对端 JFR
                 * （对端产 imm recv CQE，不写 recv 槽——数据已写 seg）。 */
                if (has_imm && opcode == 4) {
                    if (urma_sim_ipc_write_imm(rmt_eid, imm_jetty_id, cqe_opcode,
                                               imm_data) != 0) {
                        err_status = 4;   /* 通知投递失败 → RNR 类错误 */
                    }
                }
                /* 跨进程 WRITE_WITH_NOTIFY（FG-05b）：notify_addr 是对端进程 va，
                 * 未实现前显式错误（不再假成功）。 */
                if (opcode == 5) {
                    SIM_DBG("poll: WRITE_NOTIFY cross-proc notify TODO jetty=%u\n",
                            rmt_jetty_id);
                    err_status = 5;
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
                /* FG-01: 跨进程原子经 IPC 在对端进程执行读-改-写，原值回本端 buf。
                 * 失败 → 错误 CQE（不再 TODO 假成功）。 */
                if (urma_sim_ipc_atomic(rmt_eid, tid, opcode, atom_len, atom_data,
                                        (uint8_t *)(uintptr_t)local_va) == 0) {
                    done_len = atom_len;
                } else {
                    SIM_DBG("poll: atomic cross-proc fail opcode=%u tid=%u\n", opcode, tid);
                    err_status = 5;
                }
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
    SIM_DBG("poll: WQE idx=%u opcode=%u tid=%u local_va=%lx len=%u rmt=%lx %s cqe_op=%u imm=%u/%lx\n",
            wqe_idx, opcode, tid, (unsigned long)local_va, local_len, (unsigned long)rmt_addr,
            is_local ? "local" : "ipc", cqe_opcode, has_imm, (unsigned long)imm_data);

    /* 造 CQE 写到关联的 CQ。entry_idx=wqe_idx（pi），让真 udma 更新 queue->ci */
    /* complete_enable（⑥）：bit5=0x20 时抑制 send CQE（cq moderation，perftest
     * cq_mod=100 默认置位规律）。不影响数据搬运与队列推进（ci/wrid 照常）。 */
    if (wqe->flag & 0x20 /* complete_enable */) {
        urma_sim_cq_assoc_t *cq_a = urma_sim_cq_assoc_find(qa->jfc_id);
        if (cq_a != NULL) {
            produce_cqe(cq_a, qa->jetty_addr, qa->id, wqe_idx, done_len, cqe_opcode, err_status,
                        err_substatus, qa->is_jetty,
                        wqe->rmt_jetty_or_seg_id & 0xfffff,   /* 对端 jetty/jfr id */
                        (uint32_t)wqe->tp_id,                 /* 对端 tpn */
                        wqe->rmt_eid);                        /* 对端 eid */
        } else {
            SIM_DBG("poll: no CQ for jfc_id=%u, CQE dropped\n", qa->jfc_id);
        }
    }
}

/* 复刻真 udma get_wqebb_cnt（udma_u_jfs.c:45-75）：返回 WQE 占几个 64B WQEBB。
 * - SEND/SEND_IMM/SEND_INV/WRITE/WRITE_IMM/WRITE_NOTIFY：inline 时
 *   (ctl_len + inline_msg_len - 1)/64 + 1；
 * - NOP=1、WRITE_WITH_ATOMICSTORE_ADD=1；
 * - 其余：sq_cal_wqebb_num(ctl_len, sge_num, 64) = (ctl_len + (sge_num-1)*16)/64 + 1，
 *   CAS/FAA 的 sge_num = UDMA_ATOMIC_SGE_NUM(1)+1 = 2；
 * ctl_len：WRITE_WITH_IMM(4)=64、WRITE_WITH_NOTIFY(5)=80、其余=48。 */
static uint32_t sim_get_wqebb_cnt(const struct urma_sim_sqe_ctl *wqe)
{
    uint8_t opcode = (uint8_t)wqe->opcode;
    uint32_t ctl_len = 48;  /* SQE_NORMAL_CTL_LEN */
    if (opcode == 4) {       /* UDMA_OPCODE_WRITE_WITH_IMM */
        ctl_len = 64;        /* SQE_WRITE_IMM_CTL_LEN */
    } else if (opcode == 5) { /* UDMA_OPCODE_WRITE_WITH_NOTIFY */
        ctl_len = 80;        /* SQE_WRITE_NOTIFY_CTL_LEN */
    }
    if ((opcode == 0 || opcode == 1 || opcode == 2 || opcode == 3 ||
         opcode == 4 || opcode == 5) && (wqe->flag & 0x40 /* UDMAWQE_INLINE_EN */)) {
        /* inline：数据在 ctl_len 起，长度 = inline_msg_len */
        return (ctl_len + (uint32_t)wqe->inline_msg_len - 1) / 64 + 1;
    }
    if (opcode == 0x11) {    /* UDMA_OPCODE_NOP */
        return 1;            /* NOP_WQEBB_CNT */
    }
    if (opcode == 0x19) {    /* UDMA_OPCODE_WRITE_WITH_ATOMICSTORE_ADD */
        return 1;            /* WRITE_WITH_ATOMICSTORE_ADD_WQEBB_CNT */
    }
    uint32_t sge_num = wqe->sge_num;
    if (opcode == 7 || opcode == 0xb) {  /* UDMA_OPCODE_CAS / UDMA_OPCODE_FAA */
        sge_num = 2;         /* UDMA_ATOMIC_SGE_NUM(1) + 1 */
    }
    if (sge_num == 0) {
        sge_num = 1;
    }
    return (ctl_len + (sge_num - 1) * 16) / 64 + 1;
}

/* 处理 pi_seen..pi-1 的每个 WQE。WQE 按 pi 索引，在 buf_addr + (pi&mask)*64。
 * baseblk_shift=UDMA_JFS_WQEBB_SHIFT=6（WQEBB=64），mask=baseblk_cnt-1（=wqe_cnt-1）。
 * 用 pi&mask 索引，环形回绕——pi 超 depth 不会越界写 queue 内存（之前裸 pi_seen++ 会越界）。
 * 步进按真 udma get_wqebb_cnt 复刻（sim_get_wqebb_cnt）：inline/IMM/NOTIFY/≥2 SGE/
 * CAS/FAA 的 WQE 占多个 WQEBB，按 1 步进会导致后续 WQE 全部错位解析（节 9）。 */
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
     * 用 pi&mask 索引，环形回绕——pi 超 depth 不会越界写 queue 内存。
     * 步进按真 udma get_wqebb_cnt 复刻（sim_get_wqebb_cnt），见上。 */
    uint32_t mask = (qa->wqe_cnt > 0) ? (qa->wqe_cnt - 1) : 0;
    size_t qsz = ((size_t)qa->wqe_cnt > 0 ? (size_t)qa->wqe_cnt : 1) * URMA_SIM_WQEBB;
    while (qa->pi_seen != pi) {
        uint32_t idx = qa->pi_seen & mask;
        uint8_t *wqe = (uint8_t *)(uintptr_t)(qa->buf_addr + (uint64_t)idx * URMA_SIM_WQEBB);
        process_wqe(qa, wqe, idx);
        /* pi_seen 步进按该 WQE 的 wqebb_cnt（inline/IMM/NOTIFY/≥2 SGE/CAS/FAA 占多块）。
         * ctl 头也可能跨队尾，先环形复制 80B 再算步进（与 process_wqe 的 wrap 一致）。 */
        uint8_t hdr_copy[80];
        wrap_copy(hdr_copy, (const uint8_t *)(uintptr_t)qa->buf_addr, qsz,
                  (uint64_t)idx * URMA_SIM_WQEBB, sizeof(hdr_copy));
        uint32_t step = sim_get_wqebb_cnt((struct urma_sim_sqe_ctl *)hdr_copy);
        qa->pi_seen += step;
    }
}

static atomic_int g_hw_thread_run = 0;
static pthread_t g_hw_thread;
/* ⑦B: 轮询间隔（us）可配置——URMA_SIM_POLL_US 环境变量（默认 100）。
 * 时延测试可调小（真实硬件中断驱动是微秒级），性能测试可调大。 */
static uint32_t g_sim_poll_us = 100;

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
        usleep(g_sim_poll_us);
    }
    return NULL;
}

void urma_sim_hw_start(void)
{
    if (atomic_exchange(&g_hw_thread_run, 1) != 0) {
        return; /* 已启动 */
    }
    const char *poll_us = getenv("URMA_SIM_POLL_US");
    if (poll_us != NULL && poll_us[0] != '\0') {
        long v = strtol(poll_us, NULL, 0);
        if (v > 0 && v <= 1000000) {
            g_sim_poll_us = (uint32_t)v;
        }
    }
    pthread_create(&g_hw_thread, NULL, urma_sim_hw_thread, NULL);
}
