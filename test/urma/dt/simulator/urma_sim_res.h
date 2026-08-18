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

/* ⑦A: DBG 日志门控——URMA_SIM_DEBUG=1 才输出（默认静默）。
 * 此前 34 处无条件 fprintf(stderr, "DBG ...") 在 perftest 高吞吐时
 * 每秒数万行刷屏，拖慢测试并淹没真错误。 */
#define SIM_DBG(fmt, ...) do { \
    if (urma_sim_debug_enabled()) { \
        fprintf(stderr, "DBG " fmt, ##__VA_ARGS__); \
    } \
} while (0)

#ifdef __cplusplus
extern "C" {
#endif

int urma_sim_debug_enabled(void);

/* db_mmap_type（与 udma_abi.h 一致，避免 include kernel_headers） */
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

/* 反解 offset → (cmd, idx)。返回 0 成功。 */
int urma_sim_mmap_decode_offset(off_t offset, int page_size, uint32_t *cmd, uint32_t *idx);

/* 分配一块假内存并登记。返回地址，失败 NULL。 */
void *urma_sim_mmap_alloc(int dev_fd, size_t size, uint32_t cmd, uint32_t idx);

/* 按假地址查映射区域。 */
urma_sim_mmap_region_t *urma_sim_mmap_find(void *addr);

/* 判断 fd 是否是 sim cdev 假 fd（供 mmap 拦截判断要不要接管）。 */
int urma_sim_fd_is_cdev(int fd);

/* 轮询线程：盯着所有 doorbell region 的 pi 变化，解析 WQE 执行 memcpy + 造 CQE。 */
void urma_sim_hw_start(void);

/* === queue 关联表（create 时从 udata 读 jetty_addr/buf_addr/db_addr） ===
 * sim 拦 CREATE_JFS/JETTY/JFR ioctl，从 udata.in_addr 读 udma_create_jetty_ucmd，
 * 拿到 queue 对象指针(jetty_addr) + 队列内存地址(buf_addr,sim mmap 给的) +
 * doorbell 内存地址(db_addr,sim mmap 给的)。三者关联存此表，轮询/造 CQE 时用。 */
typedef struct urma_sim_queue_assoc {
    uint32_t id;           /* jfs_id / db.id（create 回填的 OUT_ID，doorbell mmap offset 含它） */
    uint64_t handle;       /* create 回填的 OUT_HANDLE（DELETE_* 按它清表，节 14） */
    uint64_t jetty_addr;   /* queue 对象指针（CQE 的 user_data_h/l 填它） */
    uint64_t buf_addr;     /* 队列内存地址（WQE 在这，sim mmap 给的假地址） */
    uint64_t db_addr;      /* doorbell 内存地址（pi 在这，sim mmap 给的假地址，mmap JETTY_DSQE 时填） */
    uint32_t jfc_id;       /* 关联的 jfc id（CREATE_JFS 从 IN_JFC_ID 拿，造 CQE 找 CQ 用） */
    int is_jetty;          /* sq 是否 jetty 内嵌（CREATE_JETTY=1 / CREATE_JFS=0）——
                            * send CQE 的 is_jetty 位按它填（节 3：独立 jfs 还原用 jfs 结构） */
    int in_use;
    uint32_t pi_seen;      /* 轮询上次看到的 pi（检测变化） */
    /* === JFR（recv 队列）专用：CREATE_JFR ucmd 携带的 rq 信息 ===
     * q_type=1 表此 assoc 是 JFR 的 rq（非 sq）。SEND 投递到对端 JFR 时按这些读 recv WQE。
     * sw_db=db_addr（JFR record db，post recv 写 rq.pi 到此）；idx_addr/idx_len 是索引队列
     * （pi→wqe_idx 间接寻址）；max_sge/wqe_shift 决定 recv WQE 在 rq.qbuf 的布局。 */
    int q_type;            /* 0=sq(jfs/jetty 发送队列), 1=rq(jfr 接收队列) */
    uint32_t max_sge;      /* recv WQE 容纳的 sge 数（向上 2 幂，udma_u_init_jfr_param） */
    uint32_t wqe_shift;    /* recv WQE 在 rq.qbuf 的位移（ilog2(SGE_SIZE*max_sge_pow2)） */
    uint32_t wqe_cnt;      /* recv WQE 总数（=depth 向上 2 幂），索引队列 head 掩码用 */
    uint64_t idx_addr;     /* 索引队列内存（pi→wqe_idx），post recv 写入 */
    uint32_t idx_len;      /* 索引队列长度 */
    uint32_t rq_pi_seen;   /* 轮询 rq sw_db 上次看到的 pi（检测新 post 的 recv） */
    /* 待用 recv 槽队列：rq 轮询发现新 post 的 recv WQE 入队；SEND 到达时取一个用。
     * rqe_idx 是该 recv WQE 在 rq.qbuf 的索引（recv CQE 的 entry_idx 填它）。 */
    uint32_t pending_head; /* 待用 recv 槽环形队列头（弹出位置） */
    uint32_t pending_tail; /* 待用 recv 槽环形队列尾（压入位置） */
    uint32_t pending_rqe[256];  /* 待用 recv 槽的 wqe_idx（recv CQE entry_idx 填它） */
    uint64_t pending_va[256];    /* 对应 recv WQE 首 sge 的 va（SEND 投递时 memcpy 进它） */
} urma_sim_queue_assoc_t;

#define URMA_SIM_QUEUE_MAX 512
extern urma_sim_queue_assoc_t g_sim_queue_assoc[URMA_SIM_QUEUE_MAX];

/* 异常注入：URMA_SIM_INJECT_STATUS 环境变量控制（constructor 读），produce_cqe 命中设 status≠0 */
extern uint8_t g_inject_cqe_status;

/* 从 create_jfs/jetty/jfr 的 ioctl udata 读 cmd，登记 queue 关联（jetty_addr+buf_addr）。
 * 返回关联槽位 index（>=0），-1 失败。id 由 caller 之后回填并调 set_id 存入。 */
int urma_sim_queue_assoc_register(uint64_t udata_data);

/* 给某槽位设 id（create 回填 OUT_ID 后，用 id 把 doorbell mmap 关联回该 queue）。 */
void urma_sim_queue_assoc_set_id(int slot, uint32_t id);
void urma_sim_queue_assoc_set_handle(int slot, uint64_t handle);
void urma_sim_queue_assoc_set_is_jetty(int slot, int is_jetty);
void urma_sim_queue_assoc_release_by_handle(uint64_t handle);

/* doorbell mmap 时调：按 offset 里的 idx（=db.id）找关联，填 db_addr。
 * 返回关联槽位，NULL 没找到。 */
urma_sim_queue_assoc_t *urma_sim_queue_assoc_set_db_by_idx(uint32_t idx, uint64_t db_addr);

/* 按 queue 对象指针查关联（造 CQE 填 user_data 时用）。 */
urma_sim_queue_assoc_t *urma_sim_queue_assoc_find_by_jetty(uint64_t jetty_addr);

/* 给某槽位设 jfc_id（CREATE_JFS 从 IN_JFC_ID 拿，关联 queue 到 CQ）。 */
void urma_sim_queue_assoc_set_jfc(int slot, uint32_t jfc_id);

/* === JFR（recv 队列）按 jetty_id 寻址（SEND 投递用） ===
 * CREATE_JFR udata 携带 rq buf/db/idx，register 时记进 q_type=1 的 assoc（id=jfr_id）。
 * CREATE_JETTY（shared jfr）的 IN attrs 带 CREATE_JETTY_IN_JFR_ID（共享的 jfr_id），
 * sim 用它建 jetty_id→jfr_id 映射（urma_sim_jfr_bind_jetty）。
 * SEND 的 WQE 带对端 jetty_id → 查本进程 jetty_id→jfr_id → 按 jfr_id(=assoc.id) 查 JFR rq assoc。
 * 跨进程 IPC 请求带 jetty_id，对端进程同样查自己的映射。 */
urma_sim_queue_assoc_t *urma_sim_jfr_find_by_jetty(uint32_t jetty_id);

/* CREATE_JETTY 回填 jetty_id 后调（带 CREATE_JETTY_IN_JFR_ID 拿到的 jfr_id）：
 * 建 jetty_id→jfr_id 映射。jfr_id=0 表示无共享 JFR（仅 jfs），no-op。 */
void urma_sim_jfr_bind_jetty(uint32_t jetty_id, uint32_t jfr_id);

/* CREATE_JFR 时从 IN attrs 拿 max_sge，补全 JFR rq assoc 的 recv WQE 几何（sim 从 ucmd 拿不到 max_sge）。
 * wqe_shift = ilog2(UDMA_SGE_SIZE(16) * roundup_pow2(max_sge))；wqe_cnt = depth 向上 2 幂。 */
void urma_sim_jfr_set_recv_geom(int slot, uint32_t max_sge, uint32_t depth);

/* SEND 到达本端 JFR：从 pending 队列取一个 recv 槽，memcpy 数据进其 va，造 recv CQE。
 * 由本进程 process_wqe(SEND) 调（is_local），或由 IPC 后台线程调（对端 SEND 来）。
 * 返回 0 成功，-1 无 recv 槽（RNR）/失败。 */
int urma_sim_jfr_deliver_send(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                              const void *data, uint32_t len,
                              uint8_t cqe_opcode, uint64_t imm_data);
/* 投递 WRITE_WITH_IMM/NOTIFY 的 imm 通知 recv CQE（不搬数据，数据已写 seg） */
int urma_sim_jfr_deliver_imm(urma_sim_queue_assoc_t *jfr_qa, uint32_t jetty_id,
                             uint8_t cqe_opcode, uint64_t imm_data);


/* === CQ 关联表（CREATE_JFC 时从 udata 拿 CQ buf_addr） ===
 * create_jfc 的 udata cmd.buf_addr = CQ 内存地址（sim mmap 给的假地址），
 * sim 回填 jfc_id，存 jfc_id → cq_addr。轮询造 CQE 时用 queue.jfc_id 查 CQ 地址。 */
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

/* 从 create_jfc 的 udata 读 CQ buf_addr，回填 jfc_id 时调 set_cq_id 关联。
 * 返回槽位，-1 失败。 */
int urma_sim_cq_assoc_register(uint64_t udata_data);
void urma_sim_cq_assoc_set_id(int slot, uint32_t jfc_id);
void urma_sim_cq_assoc_set_handle(int slot, uint64_t handle);
void urma_sim_cq_assoc_release_by_handle(uint64_t handle);
void urma_sim_cq_assoc_update_addr(uint64_t handle, uint64_t new_addr);
/* 跨文件（wqe.c ⇄ hw.c）：CQE 生成与 recv 槽投递的 mutual 调用 */
struct urma_sim_jfc_cqe *produce_recv_cqe(urma_sim_cq_assoc_t *cq_a, uint64_t jfr_addr,
                                          uint32_t jetty_id, uint32_t rqe_idx, uint32_t byte_cnt,
                                          uint8_t cqe_opcode, uint64_t imm_data, int is_jetty,
                                          uint8_t err_status, uint8_t err_substatus);
void jfr_refill_pending(urma_sim_queue_assoc_t *qa);
urma_sim_cq_assoc_t *urma_sim_cq_assoc_find(uint32_t jfc_id);

/* === tid→seg 内存映射表（ummu_grant 时建立） ===
 * 真 udma register_seg 调 ummu_grant(tid, va, len, perm, seg_attr) 把 tid→va 映射
 * 写进硬件（这是硬件知道 tid→va 的唯一渠道）。sim 拦 ummu_grant 同样记录 tid→va，
 * 轮询解到 WQE 里远端 tid 时查此表拿 src va 做 memcpy。 */
typedef struct urma_sim_tid_seg {
    uint32_t tid;          /* ummu_allocate_tid 给的 tid（ummu_grant 入参） */
    uint64_t va;           /* tid 对应的内存地址（ummu_grant 入参 data） */
    uint64_t len;          /* 段长（ummu_grant 入参 data_size） */
    int in_use;
} urma_sim_tid_seg_t;

#define URMA_SIM_TID_MAX 256
extern urma_sim_tid_seg_t g_sim_tid_seg[URMA_SIM_TID_MAX];

/* ummu_grant 拦截时调：登记 tid→va。 */
void urma_sim_tid_seg_register(uint32_t tid, uint64_t va, uint64_t len);
/* ummu_ungrant 拦截时调：清登记。 */
void urma_sim_tid_seg_unregister(uint32_t tid, uint64_t va);
/* 按 tid 查映射（轮询解到 WQE 远端 tid 时用）。返回 NULL 没找到。 */
urma_sim_tid_seg_t *urma_sim_tid_seg_find(uint32_t tid);

/* === 跨进程 IPC（方式1 扩展：双进程实时数据传递） ===
 * 单进程仿真靠 memcpy（同地址空间）。跨进程时 A 解引用 B 的指针会 SIGSEGV，
 * 故用 unix socket 实时传数据：本进程 sim 起后台线程 listen，路径含本进程 eid。
 * A 解 WQE 拿 rmt_eid=B 的 eid → 推算 B 的 socket 路径 → 连 B → B 查本进程 tid 表
 * → 读/写 B 的 buf → 回 A。tid 只在本进程查本进程指针，eid 定位对端进程。
 * WQE 的 rmt_eid/rmt_tid 由 test 链路天然携带（import_jetty/post 填），sim 白拿。 */

/* 在 create_context 拿到本进程 eid 后调：起后台 socket 线程，listen 路径含 eid。
 * 幂等（已起则 no-op）。eid 为 16 字节。 */
void urma_sim_ipc_start(const uint8_t *eid /* 16 bytes */);

/* 判断 rmt_eid 是不是本进程。是→走单进程 memcpy 路径；否→走跨进程 IPC。
 * eid 为 16 字节。返回 1=本进程，0=对端。 */
int urma_sim_ipc_is_local(const uint8_t *rmt_eid /* 16 bytes */);

/* 跨进程读：连 rmt_eid 对应进程的 socket，请求读其 tid 对应的 seg 数据，写入 local_buf。
 * 返回 0 成功，<0 失败。len 为要读的字节数。 */
int urma_sim_ipc_read(const uint8_t *rmt_eid, uint32_t tid, void *local_buf, uint32_t len);

/* 跨进程写：连 rmt_eid 对应进程的 socket，把 local_buf 的数据写进其 tid 对应的 seg。
 * 返回 0 成功，<0 失败。 */
int urma_sim_ipc_write(const uint8_t *rmt_eid, uint32_t tid, const void *local_buf, uint32_t len);

/* === 跨进程 SEND（投递到对端 JFR recv 队列） ===
 * SEND 与 READ/WRITE 不同：WQE 不带 rmt_addr，数据要投进对端预先 post 的 recv WQE buf。
 * 跨进程时发 SEND 请求给对端进程：{jetty_id, data, len}，对端查自己 JFR recv 槽 →
 * memcpy 数据进 recv buf → 造 recv CQE。本接口是"发起方"侧：连对端 socket 发 SEND 请求。
 * 返回 0 成功（对端已收下并造了 recv CQE），<0 失败。 */
int urma_sim_ipc_send(const uint8_t *rmt_eid, uint32_t jetty_id,
                      const void *data, uint32_t len, uint32_t src_jfc_id,
                      uint8_t cqe_opcode, uint64_t imm_data);

/* FG-05: 跨进程 WRITE_WITH_IMM 的 imm 通知投递（对端产 imm recv CQE，不写 recv 槽）。
 * 返回 0 成功，<0 失败。 */
int urma_sim_ipc_write_imm(const uint8_t *rmt_eid, uint32_t jetty_id,
                           uint8_t cqe_opcode, uint64_t imm_data);

/* FG-01: 跨进程 CAS/FAA——在对端进程按 tid 定位 seg，执行读-改-写（原子），
 * 原值回 local_buf。data 指向 WQE 的原子数据区（CAS: swap+cmp 共 2*len 字节；
 * FAA: operand len 字节）。返回 0 成功，<0 失败。 */
int urma_sim_ipc_atomic(const uint8_t *rmt_eid, uint32_t tid, uint8_t opcode,
                        uint32_t len, const uint8_t *data, uint8_t *local_buf);

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
#endif /* URMA_SIM_HW_H */
