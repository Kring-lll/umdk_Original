/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
 *
 * ============================================================================
 * 【废弃文件 - 方式 3 遗留，不参与编译，仅保留参考】
 * ============================================================================
 *
 * 本文件是 urma_sim 的"方式 3（sim 当 provider）"遗留实现。方式 3 已废弃，
 * 当前唯一支持的是"方式 1（仿真硬件：URMA_SIM_HW=1，真 udma 跑 + sim 拦截）"。
 *
 * 方式 3 vs 方式 1：
 *   方式 3（本文件）：sim 注册成 provider（name="udma"），urma_match_device 匹配到 sim
 *     而非真 udma。create_context/register_seg/post_jfs_wr 全由本文件的 ops 实现接管，
 *     真 udma provider 代码不跑。post_jfs_wr 对 READ/WRITE 直接 memcpy sim 内存，
 *     不经 doorbell/WQE/CQE。建链 ops（create_jfc/jfs/jetty/import/bind）多是桩返回
 *     NULL/SUCCESS，端到端跑不通。仅"设备枚举 + register_seg + 单边 READ/WRITE memcpy"
 *     可用，但无队列/完成/doorbell 语义，不像真硬件。
 *
 *   方式 1（现行，urma_sim_hw.c + urma_sim_dev.c + urma_sim_ipc.c + urma_sim_ummu.c）：
 *     URMA_SIM_HW=1 时 sim 不注册 provider，真 udma provider 接管跑真代码（post WQE/
 *     写 doorbell/poll CQE），sim 只在 mmap/ioctl/ummu/系统调用层拦截 + 轮询造 CQE。
 *     有完整队列/完成/doorbell 语义，像真硬件。READ/WRITE/SEND 端到端单进程+跨进程
 *     都跑通（test_hw_post / test_ipc_2proc / test_ipc_send），intergration_test 的
 *     send_cr/write_cr 也跑通。
 *
 * 本文件保留供历史参考。不进 CMakeLists 编译。若需重启方式 3，把它复制回上层目录、
 * 改名为 urma_sim_provider.c、并恢复 ctor 的方式 3 分支（注册 g_sim_provider_ops）。
 * ============================================================================
 *
 * 以下为原方式 3 provider 实现原文。
 *
 * URMA 设备仿真 - provider 层。
 *
 * 作为 LD_PRELOAD .so，constructor 在 urma_init 之前调 urma_register_provider_ops
 * 注册 sim provider（name="udma"，与真 udma provider 同名）。urma_init dlopen 真 udma
 * 时它 push_back 到 g_driver_list 后面，但 urma_match_device 取第一个匹配——sim 先
 * 注册在前，所以 sim 赢，设备由 sim provider 接管。
 *
 * 实现：
 *  - create_context：分配 sim context，ctx->ops = &g_sim_ops（含本文件实现）
 *  - register_seg：分配用户态内存当"硬件内存"，返回 sim target_seg
 *  - post_jfs_wr：对 READ/WRITE 直接 memcpy sim 内存（不经 doorbell/硬件）
 *  - 其余 ops：留桩返回 URMA_SUCCESS 或 NULL，注释 TODO，不真做
 *
 * 内存模型：每台 sim 设备一块 mmap 匿名内存（默认 16MB），register_seg 在其中分配。
 */

#include "urma_sim.h"
#include "urma_sim_hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>

#include "urma_api.h"
#include "urma_provider.h"
#include "urma_private.h"

/* === sim context：在 urma_context_t 基础上附加仿真状态 === */
typedef struct urma_sim_ctx {
    urma_context_t base;          /* 必须首字段，可被库当 urma_context_t* 用 */
    int dev_idx;                  /* 对应 g_urma_sim.devices 下标 */
    uint32_t next_handle;         /* 递增句柄生成 */
} urma_sim_ctx_t;

/* === sim 内存：每台设备一块 === */
#define URMA_SIM_MEM_SIZE (16UL * 1024 * 1024)   /* 16MB 每设备 */

typedef struct urma_sim_mem {
    void *base;                   /* mmap 基址 */
    uint64_t size;
    uint64_t used;                /* 下一个分配偏移（简单 bump 分配） */
    pthread_mutex_t lock;
} urma_sim_mem_t;

static urma_sim_mem_t g_sim_mem[16] = {0};   /* 每台设备一块，按下标 */

/* === sim target_seg：register_seg 返回，记录分配的内存段 === */
typedef struct urma_sim_tseg {
    urma_target_seg_t base;       /* 必须首字段 */
    void *mem;                    /* 实际内存指针（在 g_sim_mem 里） */
    uint64_t len;
} urma_sim_tseg_t;

/* 在 urma_context_t* 上取回 sim ctx（base 是首字段，直接强转） */
static urma_sim_ctx_t *to_sim_ctx(urma_context_t *ctx)
{
    return (urma_sim_ctx_t *)ctx;
}
static urma_sim_tseg_t *to_sim_tseg(urma_target_seg_t *tseg)
{
    return (urma_sim_tseg_t *)tseg;
}

/* === provider 生命周期 === */
extern urma_ops_t g_sim_ops;  /* 前向声明，定义在文件末尾 */
static urma_status_t sim_provider_init(urma_init_attr_t *conf)
{
    (void)conf;
    urma_sim_init();   /* 确保 config 加载（可能 constructor 已调） */
    for (int i = 0; i < g_urma_sim.dev_cnt; i++) {
        if (g_sim_mem[i].base == NULL) {
            g_sim_mem[i].base = mmap(NULL, URMA_SIM_MEM_SIZE, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            g_sim_mem[i].size = URMA_SIM_MEM_SIZE;
            g_sim_mem[i].used = 0;
            pthread_mutex_init(&g_sim_mem[i].lock, NULL);
        }
    }
    return URMA_SUCCESS;
}

static urma_status_t sim_provider_uninit(void)
{
    for (int i = 0; i < 16; i++) {
        if (g_sim_mem[i].base != NULL) {
            munmap(g_sim_mem[i].base, g_sim_mem[i].size);
            g_sim_mem[i].base = NULL;
            pthread_mutex_destroy(&g_sim_mem[i].lock);
        }
    }
    return URMA_SUCCESS;
}

static urma_status_t sim_query_device(urma_device_t *dev, urma_device_attr_t *dev_attr)
{
    /* 设备能力已在 sysfs/config 里，库会读 sysfs；这里填库未覆盖的兜底 */
    if (dev == NULL || dev_attr == NULL) {
        return URMA_EINVAL;
    }
    /* 从 config 取设备名对应的能力（简化：用第一台，TODO 按 dev->name 选） */
    memset(dev_attr, 0, sizeof(*dev_attr));
    dev_attr->dev_cap.max_jfc = 64;
    dev_attr->dev_cap.max_jfs = 1024;
    dev_attr->dev_cap.max_jfr = 1024;
    dev_attr->dev_cap.max_jetty = 4096;
    dev_attr->dev_cap.max_jfc_depth = 16384;
    dev_attr->dev_cap.max_jfs_depth = 16384;
    dev_attr->dev_cap.max_jfr_depth = 16384;
    dev_attr->dev_cap.trans_mode = 7;
    dev_attr->port_cnt = 1;
    return URMA_SUCCESS;
}

static urma_context_t *sim_create_context(urma_device_t *dev, uint32_t eid_index, int dev_fd)
{
    (void)dev_fd;
    urma_sim_ctx_t *sctx = calloc(1, sizeof(*sctx));
    if (sctx == NULL) {
        return NULL;
    }
    sctx->base.dev = dev;
    sctx->base.dev_fd = dev_fd;
    sctx->base.eid_index = eid_index;
    sctx->base.ops = &g_sim_ops;
    atomic_init(&sctx->base.ref.atomic_cnt, 1);
    sctx->dev_idx = 0;   /* TODO: 按 dev->name 选设备下标 */
    sctx->next_handle = 0x1000;
    return &sctx->base;
}

static urma_status_t sim_delete_context(urma_context_t *ctx)
{
    urma_sim_ctx_t *sctx = to_sim_ctx(ctx);
    if (sctx != NULL) {
        free(sctx);
    }
    return URMA_SUCCESS;
}

/* === Segment：分配仿真内存 === */
static urma_target_seg_t *sim_register_seg(urma_context_t *ctx, urma_seg_cfg_t *seg_cfg)
{
    urma_sim_ctx_t *sctx = to_sim_ctx(ctx);
    if (sctx == NULL || seg_cfg == NULL || seg_cfg->len == 0) {
        return NULL;
    }
    urma_sim_mem_t *mem = &g_sim_mem[sctx->dev_idx];
    if (mem->base == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&mem->lock);
    if (mem->used + seg_cfg->len > mem->size) {
        pthread_mutex_unlock(&mem->lock);
        return NULL;
    }
    void *p = (char *)mem->base + mem->used;
    mem->used += seg_cfg->len;
    pthread_mutex_unlock(&mem->lock);

    urma_sim_tseg_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->mem = p;
    st->len = seg_cfg->len;
    st->base.urma_ctx = ctx;
    st->base.seg.ubva.va = (uint64_t)(uintptr_t)p;   /* sim：ubva 直接用用户态地址 */
    st->base.seg.len = seg_cfg->len;
    st->base.mva = (uint64_t)(uintptr_t)p;
    st->base.handle = ++sctx->next_handle;
    return &st->base;
}

static urma_status_t sim_unregister_seg(urma_target_seg_t *target_seg)
{
    urma_sim_tseg_t *st = to_sim_tseg(target_seg);
    if (st != NULL) {
        /* 内存不真正释放（bump 分配），仅释放 tseg 结构 */
        free(st);
    }
    return URMA_SUCCESS;
}

static urma_target_seg_t *sim_import_seg(urma_context_t *ctx, urma_seg_t *seg, urma_token_t *token,
    uint64_t addr, urma_import_seg_flag_t flag)
{
    /* 单进程仿真：import 直接复用对端 seg 的地址（仿真里就是同一进程内存） */
    (void)token;
    (void)flag;
    urma_sim_tseg_t *st = calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->mem = (void *)(uintptr_t)(seg ? seg->ubva.va : addr);
    st->len = seg ? seg->len : 0;
    st->base.urma_ctx = ctx;
    st->base.mva = seg ? seg->ubva.va : addr;
    st->base.seg.ubva.va = seg ? seg->ubva.va : addr;
    st->base.seg.len = st->len;
    return &st->base;
}

static urma_status_t sim_unimport_seg(urma_target_seg_t *target_seg)
{
    urma_sim_tseg_t *st = to_sim_tseg(target_seg);
    if (st != NULL) {
        free(st);
    }
    return URMA_SUCCESS;
}

/* === token：桩 === */
static urma_token_id_t *sim_alloc_token_id(urma_context_t *ctx)
{
    (void)ctx;
    urma_token_id_t *t = calloc(1, sizeof(*t));
    if (t != NULL) {
        t->urma_ctx = ctx;
        atomic_init(&t->ref.atomic_cnt, 1);
        t->token_id = 0xabc;
    }
    return t;
}

static urma_status_t sim_free_token_id(urma_token_id_t *token_id)
{
    free(token_id);
    return URMA_SUCCESS;
}

/* === 数据面：READ/WRITE 直接 memcpy sim 内存 === */
/* 从 wr 的 rw 段取首个 sge 的地址。urma_read/write 的 src/dst 是 urma_sg_t（sge[]+num_sge）。
 * sim 里 sge.addr 是绝对地址（register_seg 时设成用户态指针）。 */
static void *sim_rw_addr(urma_jfs_wr_t *wr, bool is_src)
{
    urma_sg_t *sg = is_src ? &wr->rw.src : &wr->rw.dst;
    if (sg->sge == NULL || sg->num_sge == 0) {
        return NULL;
    }
    return (void *)(uintptr_t)(sg->sge[0].addr);
}

static uint32_t sim_rw_len(urma_jfs_wr_t *wr, bool is_src)
{
    urma_sg_t *sg = is_src ? &wr->rw.src : &wr->rw.dst;
    if (sg->sge == NULL || sg->num_sge == 0) {
        return 0;
    }
    return sg->sge[0].len;
}

static urma_status_t sim_post_jfs_wr(urma_jfs_t *jfs, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr)
{
    (void)jfs;
    if (wr == NULL) {
        return URMA_EINVAL;
    }
    switch (wr->opcode) {
        case URMA_OPC_READ: {
            /* READ：从对端 src 段搬到本端 dst。仿真直接 memcpy。 */
            void *src = sim_rw_addr(wr, true);
            void *dst = sim_rw_addr(wr, false);
            uint32_t len = sim_rw_len(wr, true);
            if (src != NULL && dst != NULL && len > 0) {
                memcpy(dst, src, len);
            }
            return URMA_SUCCESS;
        }
        case URMA_OPC_WRITE: {
            /* WRITE：从本地 src 搬到对端 dst。 */
            void *src = sim_rw_addr(wr, true);
            void *dst = sim_rw_addr(wr, false);
            uint32_t len = sim_rw_len(wr, true);
            if (src != NULL && dst != NULL && len > 0) {
                memcpy(dst, src, len);
            }
            return URMA_SUCCESS;
        }
        case URMA_OPC_SEND:
            /* TODO: 双边 send/recv 暂不仿真，返回成功但不搬数据 */
            return URMA_SUCCESS;
        default:
            if (bad_wr != NULL) {
                *bad_wr = wr;
            }
            return URMA_FAIL;
    }
}

/* === 其余 ops：留 TODO 桩 === */
/* 返回 SUCCESS 的桩（非破坏性，调用方可继续） */
#define SIM_TODO_OK(name, ...) \
    static urma_status_t sim_##name(__VA_ARGS__) { return URMA_SUCCESS; }
/* 返回 NULL 的桩 */
#define SIM_TODO_NULL(ret, name, ...) \
    static ret sim_##name(__VA_ARGS__) { return NULL; }

SIM_TODO_OK(modify_jfc, urma_jfc_t *jfc, urma_jfc_attr_t *attr)   /* TODO */
SIM_TODO_OK(delete_jfc, urma_jfc_t *jfc)                          /* TODO */
SIM_TODO_OK(delete_jfc_batch, urma_jfc_t **jfc, int n, urma_jfc_t **bad)  /* TODO */
SIM_TODO_OK(alloc_jfc, urma_context_t *ctx, urma_jfc_cfg_t *cfg, urma_jfc_t **jfc)  /* TODO: 应分配 jfc */
SIM_TODO_OK(set_jfc_opt, urma_jfc_t *jfc, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(active_jfc, urma_jfc_t *jfc)                          /* TODO */
SIM_TODO_OK(get_jfc_opt, urma_jfc_t *jfc, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(deactive_jfc, urma_jfc_t *jfc)                       /* TODO */
SIM_TODO_OK(free_jfc, urma_jfc_t *jfc)                           /* TODO */
SIM_TODO_NULL(urma_jfc_t *, create_jfc, urma_context_t *ctx, urma_jfc_cfg_t *cfg)  /* TODO */
SIM_TODO_NULL(urma_jfs_t *, create_jfs, urma_context_t *ctx, urma_jfs_cfg_t *cfg)  /* TODO */
SIM_TODO_OK(modify_jfs, urma_jfs_t *jfs, urma_jfs_attr_t *a)     /* TODO */
SIM_TODO_OK(query_jfs, urma_jfs_t *jfs, urma_jfs_cfg_t *c, urma_jfs_attr_t *a)  /* TODO */
SIM_TODO_OK(flush_jfs, urma_jfs_t *jfs, int n, urma_cr_t *cr)    /* TODO: 返回0完成数 */
SIM_TODO_OK(delete_jfs, urma_jfs_t *jfs)                          /* TODO */
SIM_TODO_OK(delete_jfs_batch, urma_jfs_t **jfs, int n, urma_jfs_t **bad)  /* TODO */
SIM_TODO_OK(alloc_jfs, urma_context_t *ctx, urma_jfs_cfg_t *cfg, urma_jfs_t **jfs)  /* TODO */
SIM_TODO_OK(set_jfs_opt, urma_jfs_t *jfs, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(active_jfs, urma_jfs_t *jfs)                          /* TODO */
SIM_TODO_OK(get_jfs_opt, urma_jfs_t *jfs, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(deactive_jfs, urma_jfs_t *jfs)                        /* TODO */
SIM_TODO_OK(free_jfs, urma_jfs_t *jfs)                            /* TODO */
SIM_TODO_NULL(urma_jfr_t *, create_jfr, urma_context_t *ctx, urma_jfr_cfg_t *cfg)  /* TODO */
SIM_TODO_OK(modify_jfr, urma_jfr_t *jfr, urma_jfr_attr_t *a)     /* TODO */
SIM_TODO_OK(query_jfr, urma_jfr_t *jfr, urma_jfr_cfg_t *c, urma_jfr_attr_t *a)  /* TODO */
SIM_TODO_OK(delete_jfr, urma_jfr_t *jfr)                          /* TODO */
SIM_TODO_OK(delete_jfr_batch, urma_jfr_t **jfr, int n, urma_jfr_t **bad)  /* TODO */
SIM_TODO_NULL(urma_target_jetty_t *, import_jfr, urma_context_t *ctx, urma_rjfr_t *r, urma_token_t *t)  /* TODO */
SIM_TODO_OK(unimport_jfr, urma_target_jetty_t *t)                /* TODO */
SIM_TODO_OK(advise_jfr, urma_jfs_t *jfs, urma_target_jetty_t *t) /* TODO */
SIM_TODO_OK(unadvise_jfr, urma_jfs_t *jfs, urma_target_jetty_t *t)  /* TODO */
SIM_TODO_OK(advise_jfr_async, urma_jfs_t *jfs, urma_target_jetty_t *t, urma_advise_async_cb_func cb, void *arg)  /* TODO */
SIM_TODO_OK(alloc_jfr, urma_context_t *ctx, urma_jfr_cfg_t *cfg, urma_jfr_t **jfr)  /* TODO */
SIM_TODO_OK(set_jfr_opt, urma_jfr_t *jfr, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(active_jfr, urma_jfr_t *jfr)                          /* TODO */
SIM_TODO_OK(get_jfr_opt, urma_jfr_t *jfr, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(deactive_jfr, urma_jfr_t *jfr)                        /* TODO */
SIM_TODO_OK(free_jfr, urma_jfr_t *jfr)                            /* TODO */
SIM_TODO_NULL(urma_jetty_t *, create_jetty, urma_context_t *ctx, urma_jetty_cfg_t *cfg)  /* TODO */
SIM_TODO_OK(modify_jetty, urma_jetty_t *j, urma_jetty_attr_t *a) /* TODO */
SIM_TODO_OK(query_jetty, urma_jetty_t *j, urma_jetty_cfg_t *c, urma_jetty_attr_t *a)  /* TODO */
SIM_TODO_OK(flush_jetty, urma_jetty_t *j, int n, urma_cr_t *cr)  /* TODO */
SIM_TODO_OK(delete_jetty, urma_jetty_t *j)                       /* TODO */
SIM_TODO_OK(delete_jetty_batch, urma_jetty_t **j, int n, urma_jetty_t **bad)  /* TODO */
SIM_TODO_NULL(urma_target_jetty_t *, import_jetty, urma_context_t *ctx, urma_rjetty_t *r, urma_token_t *t)  /* TODO */
SIM_TODO_OK(unimport_jetty, urma_target_jetty_t *t)             /* TODO */
SIM_TODO_OK(advise_jetty, urma_jetty_t *j, urma_target_jetty_t *t)  /* TODO */
SIM_TODO_OK(unadvise_jetty, urma_jetty_t *j, urma_target_jetty_t *t)  /* TODO */
SIM_TODO_OK(advise_jetty_async, urma_jetty_t *j, urma_target_jetty_t *t, urma_advise_async_cb_func cb, void *arg)  /* TODO */
SIM_TODO_OK(bind_jetty, urma_jetty_t *j, urma_target_jetty_t *t)  /* TODO */
SIM_TODO_OK(unbind_jetty, urma_jetty_t *j)                       /* TODO */
SIM_TODO_OK(alloc_jetty, urma_context_t *ctx, urma_jetty_cfg_t *cfg, urma_jetty_t **j)  /* TODO */
SIM_TODO_OK(set_jetty_opt, urma_jetty_t *j, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(active_jetty, urma_jetty_t *j)                       /* TODO */
SIM_TODO_OK(get_jetty_opt, urma_jetty_t *j, uint64_t o, void *b, uint32_t l)  /* TODO */
SIM_TODO_OK(deactive_jetty, urma_jetty_t *j)                     /* TODO */
SIM_TODO_OK(free_jetty, urma_jetty_t *j)                         /* TODO */
SIM_TODO_NULL(urma_jetty_grp_t *, create_jetty_grp, urma_context_t *ctx, urma_jetty_grp_cfg_t *cfg)  /* TODO */
SIM_TODO_OK(delete_jetty_grp, urma_jetty_grp_t *g)              /* TODO */
SIM_TODO_NULL(urma_jfce_t *, create_jfce, urma_context_t *ctx)  /* TODO */
SIM_TODO_OK(delete_jfce, urma_jfce_t *j)                         /* TODO */
SIM_TODO_OK(get_tpn, urma_jetty_t *j)                            /* TODO: 返回tpn */
SIM_TODO_OK(modify_tp, urma_context_t *ctx, uint32_t tpn, urma_tp_cfg_t *c, urma_tp_attr_t *a, urma_tp_attr_mask_t m)  /* TODO */
SIM_TODO_OK(get_tp_list, urma_context_t *ctx, urma_get_tp_cfg_t *c, uint32_t *n, urma_tp_info_t *l)  /* TODO */
SIM_TODO_OK(set_tp_attr, const urma_context_t *ctx, const uint64_t h, const uint8_t c, const uint32_t b, const urma_tp_attr_value_t *v)  /* TODO */
SIM_TODO_OK(get_tp_attr, const urma_context_t *ctx, const uint64_t h, uint8_t *c, uint32_t *b, urma_tp_attr_value_t *v)  /* TODO */
SIM_TODO_NULL(urma_target_jetty_t *, import_jetty_ex, urma_context_t *ctx, urma_rjetty_t *r, urma_token_t *t, urma_active_tp_cfg_t *a)  /* TODO */
SIM_TODO_NULL(urma_target_jetty_t *, import_jfr_ex, urma_context_t *ctx, urma_rjfr_t *r, urma_token_t *t, urma_active_tp_cfg_t *a)  /* TODO */
SIM_TODO_OK(bind_jetty_ex, urma_jetty_t *j, urma_target_jetty_t *t, urma_active_tp_cfg_t *a)  /* TODO */
SIM_TODO_NULL(urma_token_id_t *, alloc_token_id_ex, urma_context_t *ctx, urma_token_id_flag_t f)  /* TODO */
SIM_TODO_OK(get_async_event, urma_context_t *ctx, urma_async_event_t *e)  /* TODO */
SIM_TODO_OK(user_ctl, urma_context_t *ctx, urma_user_ctl_in_t *i, urma_user_ctl_out_t *o)  /* TODO */
SIM_TODO_OK(post_jfr_wr, urma_jfr_t *jfr, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad)  /* TODO */
SIM_TODO_OK(post_jetty_send_wr, urma_jetty_t *j, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad)  /* TODO */
SIM_TODO_OK(post_jetty_recv_wr, urma_jetty_t *j, urma_jfr_wr_t *wr, urma_jfr_wr_t **bad)  /* TODO */
SIM_TODO_OK(poll_jfc, urma_jfc_t *jfc, int n, urma_cr_t *cr)    /* TODO: 返回0 */
SIM_TODO_OK(rearm_jfc, urma_jfc_t *jfc, bool s)                /* TODO */
SIM_TODO_OK(wait_jfc, urma_jfce_t *j, uint32_t n, int t, urma_jfc_t *js[])  /* TODO: 返回0 */
SIM_TODO_NULL(urma_target_jetty_t *, import_jetty_async, urma_notifier_t *n, const urma_rjetty_t *r, const urma_token_t *t, uint64_t u, int to)  /* TODO */
SIM_TODO_OK(unimport_jetty_async, urma_target_jetty_t *t)       /* TODO */
SIM_TODO_OK(bind_jetty_async, urma_notifier_t *n, urma_jetty_t *j, urma_target_jetty_t *t, uint64_t u, int to)  /* TODO */
SIM_TODO_OK(unbind_jetty_async, urma_jetty_t *j)               /* TODO */
SIM_TODO_NULL(urma_notifier_t *, create_notifier, urma_context_t *ctx)  /* TODO */
SIM_TODO_OK(delete_notifier, urma_notifier_t *n)               /* TODO */
SIM_TODO_OK(wait_notify, urma_notifier_t *n, uint32_t c, urma_notify_t *ns, int t)  /* TODO */
SIM_TODO_OK(get_eid_by_ip, const urma_context_t *ctx, const urma_net_addr_t *a, urma_eid_t *e)  /* TODO */
SIM_TODO_OK(get_ip_by_eid, const urma_context_t *ctx, const urma_eid_t *e, urma_net_addr_t *a)  /* TODO */
SIM_TODO_OK(get_smac, const urma_context_t *ctx, uint8_t *m)   /* TODO */
SIM_TODO_OK(get_dmac, const urma_context_t *ctx, const urma_net_addr_t *a, uint8_t *m)  /* TODO */
static void sim_ack_async_event(urma_async_event_t *e) { (void)e; }
static void sim_ack_jfc(urma_jfc_t *js[], uint32_t ns[], uint32_t n) { (void)js; (void)ns; (void)n; }
static void sim_ack_notify(uint32_t c, urma_notify_t *ns) { (void)c; (void)ns; }

/* === sim ops 表（name="UDMA_OPS" 与真 udma 一致） === */
urma_ops_t g_sim_ops = {
    .name = "UDMA_OPS",
    .create_jfc = sim_create_jfc,
    .modify_jfc = sim_modify_jfc,
    .delete_jfc = sim_delete_jfc,
    .delete_jfc_batch = sim_delete_jfc_batch,
    .alloc_jfc = sim_alloc_jfc,
    .set_jfc_opt = sim_set_jfc_opt,
    .active_jfc = sim_active_jfc,
    .get_jfc_opt = sim_get_jfc_opt,
    .deactive_jfc = sim_deactive_jfc,
    .free_jfc = sim_free_jfc,
    .create_jfs = sim_create_jfs,
    .modify_jfs = sim_modify_jfs,
    .query_jfs = sim_query_jfs,
    .flush_jfs = sim_flush_jfs,
    .delete_jfs = sim_delete_jfs,
    .delete_jfs_batch = sim_delete_jfs_batch,
    .alloc_jfs = sim_alloc_jfs,
    .set_jfs_opt = sim_set_jfs_opt,
    .active_jfs = sim_active_jfs,
    .get_jfs_opt = sim_get_jfs_opt,
    .deactive_jfs = sim_deactive_jfs,
    .free_jfs = sim_free_jfs,
    .create_jfr = sim_create_jfr,
    .modify_jfr = sim_modify_jfr,
    .query_jfr = sim_query_jfr,
    .delete_jfr = sim_delete_jfr,
    .delete_jfr_batch = sim_delete_jfr_batch,
    .import_jfr = sim_import_jfr,
    .unimport_jfr = sim_unimport_jfr,
    .advise_jfr = sim_advise_jfr,
    .unadvise_jfr = sim_unadvise_jfr,
    .advise_jfr_async = sim_advise_jfr_async,
    .alloc_jfr = sim_alloc_jfr,
    .set_jfr_opt = sim_set_jfr_opt,
    .active_jfr = sim_active_jfr,
    .get_jfr_opt = sim_get_jfr_opt,
    .deactive_jfr = sim_deactive_jfr,
    .free_jfr = sim_free_jfr,
    .create_jetty = sim_create_jetty,
    .modify_jetty = sim_modify_jetty,
    .query_jetty = sim_query_jetty,
    .flush_jetty = sim_flush_jetty,
    .delete_jetty = sim_delete_jetty,
    .delete_jetty_batch = sim_delete_jetty_batch,
    .import_jetty = sim_import_jetty,
    .unimport_jetty = sim_unimport_jetty,
    .advise_jetty = sim_advise_jetty,
    .unadvise_jetty = sim_unadvise_jetty,
    .advise_jetty_async = sim_advise_jetty_async,
    .bind_jetty = sim_bind_jetty,
    .unbind_jetty = sim_unbind_jetty,
    .alloc_jetty = sim_alloc_jetty,
    .set_jetty_opt = sim_set_jetty_opt,
    .active_jetty = sim_active_jetty,
    .get_jetty_opt = sim_get_jetty_opt,
    .deactive_jetty = sim_deactive_jetty,
    .free_jetty = sim_free_jetty,
    .create_jetty_grp = sim_create_jetty_grp,
    .delete_jetty_grp = sim_delete_jetty_grp,
    .create_jfce = sim_create_jfce,
    .delete_jfce = sim_delete_jfce,
    .get_tpn = sim_get_tpn,
    .modify_tp = sim_modify_tp,
    .get_tp_list = sim_get_tp_list,
    .set_tp_attr = sim_set_tp_attr,
    .get_tp_attr = sim_get_tp_attr,
    .import_jetty_ex = sim_import_jetty_ex,
    .import_jfr_ex = sim_import_jfr_ex,
    .bind_jetty_ex = sim_bind_jetty_ex,
    .alloc_token_id = sim_alloc_token_id,
    .alloc_token_id_ex = sim_alloc_token_id_ex,
    .free_token_id = sim_free_token_id,
    .register_seg = sim_register_seg,
    .unregister_seg = sim_unregister_seg,
    .import_seg = sim_import_seg,
    .unimport_seg = sim_unimport_seg,
    .get_async_event = sim_get_async_event,
    .ack_async_event = sim_ack_async_event,
    .user_ctl = sim_user_ctl,
    .post_jfs_wr = sim_post_jfs_wr,
    .post_jfr_wr = sim_post_jfr_wr,
    .post_jetty_send_wr = sim_post_jetty_send_wr,
    .post_jetty_recv_wr = sim_post_jetty_recv_wr,
    .poll_jfc = sim_poll_jfc,
    .rearm_jfc = sim_rearm_jfc,
    .wait_jfc = sim_wait_jfc,
    .ack_jfc = sim_ack_jfc,
    .import_jetty_async = sim_import_jetty_async,
    .unimport_jetty_async = sim_unimport_jetty_async,
    .bind_jetty_async = sim_bind_jetty_async,
    .unbind_jetty_async = sim_unbind_jetty_async,
    .create_notifier = sim_create_notifier,
    .delete_notifier = sim_delete_notifier,
    .wait_notify = sim_wait_notify,
    .ack_notify = sim_ack_notify,
    .get_eid_by_ip = sim_get_eid_by_ip,
    .get_ip_by_eid = sim_get_ip_by_eid,
    .get_smac = sim_get_smac,
    .get_dmac = sim_get_dmac,
};

/* === provider 注册表（urma_register_provider_ops 在库内，sim 调它注册） === */
urma_provider_ops_t g_sim_provider_ops = {
    .name = "udma",
    .attr = { .version = 1, .transport_type = URMA_TRANSPORT_UB },
    .match_table = NULL,
    .init = sim_provider_init,
    .uninit = sim_provider_uninit,
    .query_device = sim_query_device,
    .create_context = sim_create_context,
    .delete_context = sim_delete_context,
};

/* 声明库导出的注册函数（sim .so 链接 liburma，constructor 调它把 sim 注册进库） */
extern int urma_register_provider_ops(urma_provider_ops_t *provider_ops);

/* constructor：在 urma_init 之前注册 sim provider，使其排在真 udma 前面（先注册先匹配）。
 * 方式 1（仿真硬件）：设 URMA_SIM_HW=1 时停用 sim provider，让真 udma provider 接管，
 * 真 udma 代码（post WQE/写 doorbell/poll CQE）才会跑，sim 只在 mmap/轮询层介入。 */
__attribute__((constructor))
static void urma_sim_provider_ctor(void)
{
    urma_sim_init();
    const char *hw = getenv("URMA_SIM_HW");
    if (hw != NULL && hw[0] == '1') {
        /* 方式 1：不注册 sim provider，真 udma 接管 */
        urma_sim_hw_start();
        return;
    }
    (void)urma_register_provider_ops(&g_sim_provider_ops);
}
