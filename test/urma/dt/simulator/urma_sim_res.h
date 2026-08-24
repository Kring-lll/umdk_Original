/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * 方式 1（仿真硬件）内部接口：mmap 拦截 + 假内存 + 轮询。
 *
 * 真 udma provider 代码照常跑：create_context 经 ioctl（sim 回填）→ mmap doorbell/queue/cq
 * （sim 拦 mmap 返回假内存）→ post_jfs_wr 写 WQE 到假 queue + 写 doorbell 到假 db →
 * sim 轮询线程看 db 的 pi 变化 → 解析 WQE → memcpy → 写 CQE 到假 cq → poll_jfc 读到。
 */

#ifndef URMA_SIM_HW_H
#define URMA_SIM_HW_H

#include <stdint.h>
#include <stddef.h>
#include <sys/mman.h>

#include "urma_sim_intercept.h"

/* ⑦A: DBG 日志门控（URMA_SIM_DEBUG=1 才输出，防高吞吐刷屏） */
#define SIM_DBG(fmt, ...) do { \
    if (urma_sim_debug_enabled()) { \
        fprintf(stderr, "DBG " fmt, ##__VA_ARGS__); \
    } \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

int urma_sim_debug_enabled(void);

#define URMA_SIM_MMAP_HUGEPAGE    0
#define URMA_SIM_MMAP_JFC_PAGE    1
#define URMA_SIM_MMAP_JETTY_DSQE  2
#define URMA_SIM_MMAP_RESERVED_SQ 3
#define URMA_SIM_MMAP_KERNEL_BUF  4

/* 假内存映射表：一次 mmap(dev_fd, offset) 对应一块假内存 + 类型。
 * 真 udma 按 offset 访问不同区域（queue/db/cq），sim 用 offset 反解 cmd+idx 找映射。 */
typedef struct urma_sim_mmap_region {
    void *addr;            /* sim 返回的假内存地址（malloc/mmap 匿名） */
    uint64_t size;
    uint32_t cmd;          /* UDMA_MMAP_* */
    uint32_t idx;          /* db id / 0 */
    int in_use;
} urma_sim_mmap_region_t;

#define URMA_SIM_MMAP_MAX 256
extern urma_sim_mmap_region_t g_sim_mmap_regions[URMA_SIM_MMAP_MAX];

int urma_sim_mmap_decode_offset(off_t offset, int page_size, uint32_t *cmd, uint32_t *idx);

void *urma_sim_mmap_alloc(int dev_fd, size_t size, uint32_t cmd, uint32_t idx);

urma_sim_mmap_region_t *urma_sim_mmap_find(void *addr);

int urma_sim_fd_is_cdev(int fd);

void urma_sim_hw_start(void);

/* === queue 关联表（create 时从 udata 拿 jetty_addr/buf_addr/db_addr，轮询用） === */
typedef struct urma_sim_queue_assoc {
    uint32_t id;           /* jfs/jfr id（db.id） */
    uint64_t handle;       /* OUT_HANDLE（DELETE 按它清表） */
    uint64_t jetty_addr;   /* queue 对象指针（CQE user_data） */
    uint64_t buf_addr;     /* WQE 队列内存 */
    uint64_t db_addr;      /* doorbell（pi） */
    uint32_t jfc_id;       /* 关联 CQ（造 CQE 用） */
    int is_jetty;          /* sq 是否 jetty 内嵌（send CQE 的 is_jetty 位） */
    uint8_t eid[16];       /* 所属设备 eid（跨进程发送端身份） */
    int in_use;
    uint32_t pi_seen;      /* 轮询上次看到的 pi（检测变化） */
    /* === JFR（rq）专用 === */
    int q_type;            /* 0=sq, 1=rq(JFR) */
    uint32_t max_sge;      /* recv WQE sge 数（2 幂） */
    uint32_t wqe_shift;    /* recv WQE 位移 */
    uint32_t wqe_cnt;      /* recv WQE 总数（2 幂，掩码用） */
    uint64_t idx_addr;     /* 索引队列（pi→wqe_idx） */
    uint32_t idx_len;
    uint32_t rq_pi_seen;   /* rq doorbell 上次 pi */

    uint32_t pending_head; /* 待用 recv 槽环形队列头（弹出位置） */
    uint32_t pending_tail; /* 待用 recv 槽环形队列尾（压入位置） */
    uint32_t pending_rqe[256];  /* 待用 recv 槽的 wqe_idx（recv CQE entry_idx 填它） */
    uint64_t pending_va[256];    /* 对应 recv WQE 首 sge 的 va（SEND 投递时 memcpy 进它） */
} urma_sim_queue_assoc_t;

#define URMA_SIM_QUEUE_MAX 512
extern urma_sim_queue_assoc_t g_sim_queue_assoc[URMA_SIM_QUEUE_MAX];

extern uint8_t g_inject_cqe_status;
/* 异常注入：URMA_SIM_INJECT_STATUS 控制，produce_cqe 命中设 status≠0 */

/* 从 create_jfs/jetty/jfr 的 ioctl udata 读 cmd，登记 queue 关联（jetty_addr+buf_addr）。
 * 返回关联槽位 index（>=0），-1 失败。id 由 caller 之后回填并调 set_id 存入。 */
int urma_sim_queue_assoc_register(uint64_t udata_data);

void urma_sim_queue_assoc_set_id(int slot, uint32_t id);
void urma_sim_queue_assoc_set_handle(int slot, uint64_t handle);
void urma_sim_queue_assoc_set_is_jetty(int slot, int is_jetty);

void urma_sim_queue_assoc_set_eid(int slot, const uint8_t *eid);
void urma_sim_queue_assoc_release_by_handle(uint64_t handle);

/* doorbell mmap 时调：按 offset 里的 idx（=db.id）找关联，填 db_addr。
 * 返回关联槽位，NULL 没找到。 */
urma_sim_queue_assoc_t *urma_sim_queue_assoc_set_db_by_idx(uint32_t idx, uint64_t db_addr);

urma_sim_queue_assoc_t *urma_sim_queue_assoc_find_by_jetty(uint64_t jetty_addr);

void urma_sim_queue_assoc_set_jfc(int slot, uint32_t jfc_id);

/* === JFR（rq）按 jetty_id 寻址：SEND 带对端 jetty_id → 绑定/映射 → JFR rq === */
urma_sim_queue_assoc_t *urma_sim_jfr_find_by_jetty(uint32_t jetty_id);

/* CREATE_JETTY 回填 jetty_id 后调（带 CREATE_JETTY_IN_JFR_ID 拿到的 jfr_id）：
 * 建 jetty_id→jfr_id 映射。jfr_id=0 表示无共享 JFR（仅 jfs），no-op。 */
void urma_sim_jfr_bind_jetty(uint32_t jetty_id, uint32_t jfr_id);

/* CREATE_JFR 时从 IN attrs 拿 max_sge，补全 JFR rq assoc 的 recv WQE 几何（sim 从 ucmd 拿不到 max_sge）。
 * wqe_shift = ilog2(UDMA_SGE_SIZE(16) * roundup_pow2(max_sge))；wqe_cnt = depth 向上 2 幂。 */
void urma_sim_jfr_set_recv_geom(int slot, uint32_t max_sge, uint32_t depth);

/* SEND 到达本端 JFR：取 recv 槽、写数据、造 recv CQE（is_local 由 process_wqe 调，
 * 对端 SEND 由 IPC 线程调）。0 成功，-1 无槽（RNR）。 */
int urma_sim_jfr_deliver_send(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                              const void *data, uint32_t len,
                              uint8_t cqe_opcode, uint64_t imm_data,
                              uint32_t src_jetty_id, uint32_t src_tpn,
                              const uint8_t *src_eid);
/* 投递 WRITE_WITH_IMM/NOTIFY 的 imm 通知 recv CQE（不搬数据，数据已写 seg） */
int urma_sim_jfr_deliver_imm(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                             uint8_t cqe_opcode, uint64_t imm_data,
                             uint32_t src_jetty_id, uint32_t src_tpn,
                             const uint8_t *src_eid);


/* === CQ 关联表（CREATE_JFC 时从 udata 拿 CQ buf_addr + 生产计数） === */
typedef struct urma_sim_cq_assoc {
    uint32_t jfc_id;      /* create_jfc 回填的 OUT_ID */
    uint64_t cq_addr;     /* CQ 内存地址（CQE 写到这） */
    uint32_t depth;       /* CQ 深度（轮询造 CQE 算 baseblk_shift/cq_shift 用） */
    uint32_t cq_ci;       /* sim 造 CQE 的生产者计数（指向下一个 CQE 槽） */
    uint64_t handle;      /* create 回填的 OUT_HANDLE（DELETE_* 按它清表，节 14） */
    int in_use;
} urma_sim_cq_assoc_t;

#define URMA_SIM_CQ_MAX 512
extern urma_sim_cq_assoc_t g_sim_cq_assoc[URMA_SIM_CQ_MAX];

int urma_sim_cq_assoc_register(uint64_t udata_data);
void urma_sim_cq_assoc_set_id(int slot, uint32_t jfc_id);
void urma_sim_cq_assoc_set_handle(int slot, uint64_t handle);
void urma_sim_cq_assoc_release_by_handle(uint64_t handle);
void urma_sim_cq_assoc_update_addr(uint64_t handle, uint64_t new_addr);

/* 跨文件（res.c ⇄ exec.c）mutual 调用 */
struct urma_sim_jfc_cqe *produce_recv_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t jfr_addr,
                                          uint32_t jetty_id, uint32_t rqe_idx, uint32_t byte_cnt,
                                          uint8_t cqe_opcode, uint64_t imm_data, int is_jetty,
                                          uint8_t err_status, uint8_t err_substatus,
                                          uint32_t rmt_idx, uint32_t tpn,
                                          const uint8_t *rmt_eid);
void jfr_refill_pending(urma_sim_queue_assoc_t *qa);
urma_sim_cq_assoc_t *urma_sim_cq_assoc_find(uint32_t jfc_id);

/* === tid→seg 映射表（ummu_grant 建立；轮询按远端 tid 查 src va） ===
 * 注：tid 全局原子自增唯一，多 eid 按全进程表查也不会命中他设备同 tid 段。 */
typedef struct urma_sim_tid_seg {
    uint32_t tid;          /* ummu_allocate_tid 给的 tid（ummu_grant 入参） */
    uint64_t va;           /* tid 对应的内存地址（ummu_grant 入参 data） */
    uint64_t len;          /* 段长（ummu_grant 入参 data_size） */
    int in_use;
} urma_sim_tid_seg_t;

#define URMA_SIM_TID_MAX 256
extern urma_sim_tid_seg_t g_sim_tid_seg[URMA_SIM_TID_MAX];

/* ummu_grant 拦截时调：登记 tid→va。一个 token 可注册多段（urma_api.h:793），
 * 同 (tid,va) 幂等更新、不同 va 追加，不按 tid 覆盖旧段。 */
void urma_sim_tid_seg_register(uint32_t tid, uint64_t va, uint64_t len);

void urma_sim_tid_seg_unregister(uint32_t tid, uint64_t va);

urma_sim_tid_seg_t *urma_sim_tid_seg_find(uint32_t tid);
/* 按 (tid, addr∈[va,va+len)) 定位段：多 segment token 时各段 va 不同，
 * IPC 服务端用请求的远端地址定位对应段（不能只按 tid 取首条）。 */
urma_sim_tid_seg_t *urma_sim_tid_seg_find_addr(uint32_t tid, uint64_t addr);

/* === 跨进程 IPC：unix socket（路径含 eid），对端线程代执行内存操作 === */

/* 在 create_context 拿到本进程 eid 后调：起后台 socket 线程，listen 路径含 eid。
 * 幂等（已起则 no-op）。eid 为 16 字节。 */
void urma_sim_ipc_start(const uint8_t *eid /* 16 bytes */);

/* 判断 rmt_eid 是不是本进程。是→走单进程 memcpy 路径；否→走跨进程 IPC。
 * eid 为 16 字节。返回 1=本进程，0=对端。 */
int urma_sim_ipc_is_local(const uint8_t *rmt_eid /* 16 bytes */);

const uint8_t *urma_sim_ipc_get_local_eid(void);

/* IPC 客户端返回：0 成功；-1 链路/协议失败（ACK_TIMEOUT 类）；
 * -2 = 远端长度错误（须造 REM_RSP_LENGTH_ERR CQE，勿折叠成链路失败） */
#define URMA_SIM_IPC_RC_LEN_ERR (-2)

/* 跨进程读：向对端请求读 tid 对应 seg，rmt_addr 为远端地址（对端按段内偏移
 * off=rmt_addr-seg.va 定位），数据写 local_buf。0 成功，<0 失败。 */
int urma_sim_ipc_read(const uint8_t *rmt_eid, uint32_t tid, uint64_t rmt_addr,
                      void *local_buf, uint32_t len);

/* 跨进程写：local_buf → 对端 tid seg（rmt_addr 按段内偏移定位）。0/-<0。 */
int urma_sim_ipc_write(const uint8_t *rmt_eid, uint32_t tid, uint64_t rmt_addr,
                       const void *local_buf, uint32_t len);

/* === 跨进程 SEND：发 {jetty_id,data,len}，对端投进其 JFR recv 槽+造 recv CQE === */
int urma_sim_ipc_send(const uint8_t *rmt_eid, uint32_t jetty_id,
                      const void *data, uint32_t len, uint32_t src_jfc_id,
                      uint8_t cqe_opcode, uint64_t imm_data,
                      uint32_t src_jetty_id, uint32_t src_tpn,
                      const uint8_t *src_eid);

/* FG-05: 跨进程 WRITE_WITH_IMM 的 imm 通知投递（对端产 imm recv CQE，不写 recv 槽）。
 * 返回 0 成功，<0 失败。 */
int urma_sim_ipc_write_imm(const uint8_t *rmt_eid, uint32_t jetty_id,
                           uint8_t cqe_opcode, uint64_t imm_data,
                           uint32_t src_jetty_id, uint32_t src_tpn,
                           const uint8_t *src_eid);

/* FG-01: 跨进程 CAS/FAA（对端执行原子读改写，原值回 local_buf） */
int urma_sim_ipc_atomic(const uint8_t *rmt_eid, uint32_t tid, uint64_t rmt_addr,
                        uint8_t opcode, uint32_t len, const uint8_t *data,
                        uint8_t *local_buf);

#ifdef __cplusplus
}
#endif


#define UDMA_JFC_DB_VALID_OWNER_M 1  /* udma_u_jfc.h: cq_shift 每轮翻转 */
#include <pthread.h>
extern pthread_mutex_t g_pending_lock;
extern pthread_mutex_t g_cq_lock;
typedef struct urma_sim_wqe_sge {
    uint32_t length;
    uint32_t token_id;
    uint64_t va;
} urma_sim_wqe_sge_t;

#define URMA_SIM_DOORBELL_OFFSET 0x80
#define URMA_SIM_WQEBB 64
#define URMA_SIM_CQE_SIZE 64
#define URMA_SIM_EID_SIZE 16   /* URMA_EID_SIZE=16（urma_types.h），rmt_eid[16] */

/* WQE/CQE 位域结构镜像（独立于 udma 头，避免 include 链断） */
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
#endif /* URMA_SIM_HW_H */
