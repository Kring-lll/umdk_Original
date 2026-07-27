// SPDX-License-Identifier: GPL-2.0
/*
 * urma stub kmod —— 方案 A：建链 ioctl 命令在内核态处理，数据面留用户态 sim。
 *
 * 注册 /dev/uburma/<dev> cdev + /sys/class/ubcore/<dev> sysfs，实现 URMA_CMD ioctl
 * handler，把 sim 的 urma_sim_handle_ioctl 回填逻辑搬到内核（copy_from/to_user）。
 * 数据面（mmap 给页、轮询 doorbell、解 WQE、造 CQE）仍由用户态 sim 负责，本 kmod 不碰。
 *
 * 设计见 kmod/DESIGN.md。
 *
 * 编译：在容器内（6.6 kernel-devel 头树）：
 *   make -C /lib/modules/6.6.0-159.4.3.154.oe2403sp4.aarch64/build \
 *        M=$(pwd) ARCH=arm64 modules
 * 加载（VM 内）：insmod urma_kmod.ko
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/ioctl.h>
#include <linux/atomic.h>
#include <linux/stddef.h>

#include "udma_abi.h"   /* udma_create_ctx_resp（kernel_headers UAPI，内核安全） */

/* urma 的头（urma_cmd.h→urma_types.h）引用 arpa/inet.h/pthread.h 等用户态头，
 * 不能在内核模块里 include。这里自包含定义 kmod 用到的最小协议结构/宏，
 * 结构布局与 urma_cmd.h / urma_cmd_tlv.h 完全一致（只是不依赖用户态头）。 */

typedef struct urma_cmd_hdr {
    u32 command;
    u32 args_len;
    u64 args_addr;
} urma_cmd_hdr_t;

#define URMA_CMD_MAX_ARGS_SIZE 4096
#define URMA_CMD_MAGIC 'U'
/* _IOWR('U', 1, sizeof(urma_cmd_hdr_t)) —— ioctl 号，与 urma_cmd.h URMA_CMD 一致 */
#define URMA_CMD _IOWR(URMA_CMD_MAGIC, 1, urma_cmd_hdr_t)

typedef struct urma_cmd_attr {
    u8  type;       /* See urma_cmd_xxx_type_t */
    u8  flag;
    u16 field_size;
    union {
        struct {
            u32 el_num   : 12;
            u32 el_size  : 12;
            u32 reserved : 8;
        } bs;
        u32 value;
    } attr_data;
    u64 data;        /* 用户态指针，指向实际值缓冲 */
} urma_cmd_attr_t;

/* urma_cmd_udrv_priv_t（在 urma_cmd.h 里，不能 include，自包含定义；sizeof=32 验过） */
typedef struct urma_kmod_udrv_priv {
    u64 in_addr;
    u32 in_len;
    u64 out_addr;
    u32 out_len;
} urma_kmod_udrv_priv_t;

/* 命令号（urma_cmd_t 枚举值，硬编码避免 include） */
#define UCMD_QUERY_DEV_ATTR   40
#define UCMD_CREATE_CTX        1
#define UCMD_CREATE_JFC       16
#define UCMD_CREATE_JFS        8
#define UCMD_CREATE_JFR        12
#define UCMD_CREATE_JETTY      22
#define UCMD_IMPORT_JETTY      26
#define UCMD_IMPORT_JETTY_EX  49
#define UCMD_BIND_JETTY        30
#define UCMD_ALLOC_TOKEN_ID    2

/* OUT attr type 起始（urma_cmd.h: URMA_CMD_OUT_TYPE_INIT=0x80）。
 * 注意：这些是各命令独立枚举，值会重复（每命令从 0x80 重新数），
 * 所以不能全局映射，必须 handler 内按命令匹配。值用 enum_check 程序打印确认。 */
#define URMA_CMD_OUT_TYPE_INIT 0x80

/* 各命令的 OUT handle/token/depth 等 attr type（精确值，enum_check 跑过） */
#define T_CREATE_JFC_OUT_HANDLE        130
#define T_CREATE_JFS_OUT_HANDLE        133
#define T_CREATE_JFR_OUT_HANDLE        131
#define T_CREATE_JFR_OUT_DEPTH          129
#define T_CREATE_JFR_OUT_MAX_SGE        130
#define T_CREATE_JETTY_OUT_HANDLE       129
#define T_IMPORT_JETTY_OUT_HANDLE       129
#define T_IMPORT_JETTY_EX_OUT_HANDLE   129
#define T_BIND_JETTY_OUT_TPN            128
#define T_ALLOC_TOKEN_ID_OUT_TOKEN_ID   128
#define T_REGISTER_SEG_OUT_TOKEN_ID     128
#define T_REGISTER_SEG_IN_TOKEN_ID        2
#define T_CREATE_CTX_OUT_ASYNC_FD        128
#define T_CREATE_CTX_OUT_UDATA           129
#define T_CREATE_JFCE_OUT_FD             128
#define T_CREATE_JETTY_IN_JFR_ID          20
#define T_CREATE_JETTY_IN_SEND_JFC_ID     11
/* CREATE_JFR 的 IN attr type（从 0 起编号，enum_check 确认） */
#define T_CREATE_JFR_IN_DEPTH              0
#define T_CREATE_JFR_IN_MAX_SGE            3
#define T_CREATE_JFR_IN_JFC_ID             5
/* REGISTER_SEG：IN/OUT TOKEN_ID（type 值） */
#define T_REGISTER_SEG_IN_TOKEN_ID         2
#define T_REGISTER_SEG_OUT_TOKEN_ID      128
/* 命令号：REGISTER_SEG（之前漏了，=4） */
#define UCMD_REGISTER_SEG                  4

/* QUERY_DEV_ATTR 的关键 OUT attr type（精确值，enum_check 跑过） */
#define T_QD_GUID                        128
#define T_QD_MAX_JFC                     130
#define T_QD_MAX_JFS                     131
#define T_QD_MAX_JFR                     132
#define T_QD_MAX_JETTY                   133
#define T_QD_MAX_JFC_DEPTH               136
#define T_QD_MAX_JFS_DEPTH               137
#define T_QD_MAX_JFR_DEPTH               138
#define T_QD_MAX_JFS_SGE                 140
#define T_QD_MAX_JFR_SGE                 142
#define T_QD_MAX_MSG_SIZE                143
#define T_QD_MAX_READ_SIZE               144
#define T_QD_MAX_WRITE_SIZE              145
#define T_QD_ATOMIC_FEAT                 153
#define T_QD_TRANS_MODE                  154
#define T_QD_MAX_EID_CNT                 159
#define T_QD_PAGE_SIZE_CAP               160
#define T_QD_PORT_CNT                    164

MODULE_LICENSE("GPL");
MODULE_AUTHOR("urma_sim");
MODULE_DESCRIPTION("urma stub kernel module (plan A: control path in-kernel)");

#define URMA_KMOD_NAME "urma_kmod"
#define URMA_KMOD_MAX_DEVS 4

/* 设备能力配置（内置，对应 sim 的 urma_sim_config.json udma_sim0 精简版）。
 * 真实环境可从模块参数/配置读，这里先内置一个能跑 post 的最小集。 */
struct urma_kmod_dev {
    char name[32];
    dev_t devt;
    struct cdev cdev;
    struct device *cls_dev;   /* /sys/class/ubcore/<dev> */
};

static struct urma_kmod_dev g_devs[URMA_KMOD_MAX_DEVS];
static int g_dev_cnt;
static struct class *g_urma_class;
static dev_t g_dev_base;

/* ============================================================
 * attr 回填原语（搬自 sim urma_sim_dev.c write_attr_value，改 copy_to_user）
 * attr->data 是用户态指针，按 field_size 往那写值。
 * ============================================================ */
static int kmod_write_attr(const urma_cmd_attr_t *attr, uint64_t value)
{
    if (attr->data == 0) {
        return -EINVAL;
    }
    void __user *dst = (void __user *)(uintptr_t)attr->data;
    switch (attr->field_size) {
    case 1: {
        uint8_t v = (uint8_t)value;
        return copy_to_user(dst, &v, 1) ? -EFAULT : 0;
    }
    case 2: {
        uint16_t v = (uint16_t)value;
        return copy_to_user(dst, &v, 2) ? -EFAULT : 0;
    }
    case 4: {
        uint32_t v = (uint32_t)value;
        return copy_to_user(dst, &v, 4) ? -EFAULT : 0;
    }
    case 8: {
        uint64_t v = value;
        return copy_to_user(dst, &v, 8) ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

/* 读 IN attr 值（搬自 sim read_in_attr，改 copy_from_user）。
 * 对 IN attr（type<0x80）按 field_size 从 attr->data（用户态）读。返回 0 成功。 */
static int kmod_read_in_attr(const urma_cmd_attr_t *attrs, size_t n, uint8_t type, uint64_t *out)
{
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type == type && attrs[i].data != 0) {
            const void __user *src = (const void __user *)(uintptr_t)attrs[i].data;
            uint64_t v = 0;
            switch (attrs[i].field_size) {
            case 1: { uint8_t x; if (copy_from_user(&x, src, 1)) return -EFAULT; v = x; break; }
            case 2: { uint16_t x; if (copy_from_user(&x, src, 2)) return -EFAULT; v = x; break; }
            case 4: { uint32_t x; if (copy_from_user(&x, src, 4)) return -EFAULT; v = x; break; }
            case 8: { if (copy_from_user(&v, src, 8)) return -EFAULT; break; }
            default: return -EINVAL;
            }
            *out = v;
            return 0;
        }
    }
    return -ENOENT;
}

/* ============================================================
 * 命令处理（搬自 sim urma_sim_dev.c，逐步实现）
 * 每个 handler 接收已 copy_from_user 的 attrs 数组 + attr_num，
 * 回填 OUT attr（直接对用户态 attr->data copy_to_user）。
 * ============================================================ */

/* 递增 handle 生成器（sim 用 g_sim_next_handle，内核用原子） */
static atomic_t g_handle_ctr = ATOMIC_INIT(0x1000);
static uint32_t kmod_next_handle(void)
{
    return (uint32_t)atomic_inc_return(&g_handle_ctr);
}

/* QUERY_DEV_ATTR(40)：回填设备能力 OUT attr。
 * sim 用 g_query_dev_attr_map（attr type→字段名→field_size）从 config/sysfs 取值。
 * kmod 内置一份精简能力表（对应 udma_sim0），回填关键 OUT attr。 */
static int kmod_handle_query_dev_attr(urma_cmd_attr_t *attrs, size_t n)
{
    /* 内置设备能力（值取自 urma_sim_config.json udma_sim0，post 链路 + device_cap 检查够用）。
     * type = QUERY_DEVICE_OUT_DEV_CAP_* 枚举值（enum_check 打印确认）。
     * 完整 map 见 sim g_query_dev_attr_map，这里搬 post 用到的子集 + 主要 cap。 */
    static const struct {
        uint8_t type;
        uint8_t fsize;
        uint64_t value;
    } caps[] = {
        { T_QD_GUID,            8, 0 },          /* GUID: 0（sim 用 IP 串转 eid，kmod 不转） */
        { T_QD_MAX_JFC,         4, 999 },        /* MAX_JFC（config 表单覆盖成 999） */
        { T_QD_MAX_JFS,         4, 1024 },
        { T_QD_MAX_JFR,         4, 1024 },
        { T_QD_MAX_JETTY,       4, 4096 },
        { T_QD_MAX_JFC_DEPTH,   4, 16384 },
        { T_QD_MAX_JFS_DEPTH,   4, 16384 },
        { T_QD_MAX_JFR_DEPTH,   4, 16384 },
        { T_QD_MAX_JFS_SGE,     4, 8 },
        { T_QD_MAX_JFR_SGE,     4, 8 },
        { T_QD_MAX_MSG_SIZE,    8, 1073741824 }, /* 1GB */
        { T_QD_MAX_READ_SIZE,   4, 1048576 },    /* 1MB */
        { T_QD_MAX_WRITE_SIZE,  4, 1048576 },
        { T_QD_ATOMIC_FEAT,     4, 15 },
        { T_QD_TRANS_MODE,      2, 3 },          /* RC|RM|UM=7，config 是 3 */
        { T_QD_MAX_EID_CNT,     2, 4 },
        { T_QD_PAGE_SIZE_CAP,   8, 65536 },
        { T_QD_PORT_CNT,        1, 1 },
    };
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type < URMA_CMD_OUT_TYPE_INIT) {
            continue;
        }
        for (size_t j = 0; j < ARRAY_SIZE(caps); j++) {
            if (attrs[i].type == caps[j].type) {
                kmod_write_attr(&attrs[i], caps[j].value);
                break;
            }
        }
    }
    return 0;
}

/* CREATE_CTX(1)：回填 async_fd + cqe_size=64 等。
 * sim 的 handle_create_ctx 还起 IPC（读 eids/eid0→urma_sim_ipc_start），
 * IPC 是用户态的，kmod 不做——sim 侧会在拦到 CREATE_CTX 的别的方式或保留处理。
 * 方案 A 下 CREATE_CTX 的回填由 kmod 做，IPC 起动由 sim 在 ioctl 返回后
 * 用别的钩子做（或暂不起 IPC，先验单进程）。 */
/* CREATE_CTX(1)：回填 async_fd + udata.out_addr 指向的 resp（设 cqe_size=64）。
 * 搬自 sim handle_create_ctx。注意：sim 还在此起 IPC（读 eids/eid0→urma_sim_ipc_start），
 * IPC 是用户态的，kmod 不做——由 sim 侧在 ioctl 返回后用别的钩子起，或暂不起（先验单进程）。
 * udata（urma_kmod_udrv_priv_t）在用户态，需 copy_from_user 读、copy_to_user 写 resp。 */
static int kmod_handle_create_ctx(urma_cmd_attr_t *attrs, size_t n)
{
    /* OUT_ASYNC_FD(type=128)：给假 async_fd=0（真 udma 用它取异步事件，kmod 无事件） */
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type == T_CREATE_CTX_OUT_ASYNC_FD) {
            /* field_size 通常 sizeof(int)=4，但按 attr 的 field_size 写 */
            kmod_write_attr(&attrs[i], 0);
            break;
        }
    }
    /* OUT_UDATA(type=129)：data 指向用户态 urma_kmod_udrv_priv_t。
     * 其 out_addr/out_len 指向 resp。往 resp 写：第一 u32 低 8 位 cqe_size=64，其余 0
     * （关所有可选特性，只留 cqe_size=64 让 CQ baseblk_shift=6）。 */
    urma_kmod_udrv_priv_t udata = {0};
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type == T_CREATE_CTX_OUT_UDATA && attrs[i].data != 0) {
            if (copy_from_user(&udata, (void __user *)(uintptr_t)attrs[i].data,
                               sizeof(udata))) {
                return -EFAULT;
            }
            break;
        }
    }
    if (udata.out_addr != 0 && udata.out_len >= sizeof(struct udma_create_ctx_resp)) {
        /* resp 第一字段是位域 __u32（cqe_size:8 + dwqe_enable:1 + ...）。
         * 设 cqe_size=64(0x40)，其余位 0 → 整个第一 u32 = 0x40。 */
        u32 first_word = 0x40;  /* cqe_size=64, 其他特性全关 */
        if (copy_to_user((void __user *)(uintptr_t)udata.out_addr, &first_word,
                         sizeof(first_word))) {
            return -EFAULT;
        }
    }
    return 0;
}

/* 建链命令统一 handler（搬自 sim handle_link_cmd）。
 * CREATE_JFC/JFS/JFR/JETTY/IMPORT_JETTY/IMPORT_JETTY_EX/BIND_JETTY/ALLOC_TOKEN_ID。
 * 回填 OUT handle/token_id，从 IN udata 读关联信息（数据面关联仍由 sim mmap 拦截做）。 */
/* 建链命令统一 handler（搬自 sim handle_link_cmd 的 OUT 回填部分）。
 * CREATE_JFC/JFS/JFR/JETTY/IMPORT_JETTY/IMPORT_JETTY_EX/BIND_JETTY/ALLOC_TOKEN_ID/REGISTER_SEG。
 * kmod 只回填 OUT（id/handle/token_id/depth/max_sge）。
 * 数据面关联登记（cq_assoc/queue_assoc/jfr_bind/jfr_set_recv_geom）由 sim 在拦 ioctl
 * 后、透传给 kmod 前做（sim 侧改造），kmod 不碰。 */
static int kmod_handle_link_cmd(uint32_t command, urma_cmd_attr_t *attrs, size_t n)
{
    /* 所有建链命令的 OUT_ID 都是 128（urma_cmd_tlv.h 各 *_OUT_ID=URMA_CMD_OUT_TYPE_INIT）。
     * 给 OUT_ID 回填递增假 id（sim 用 g_sim_next_handle，kmod 用 atomic）。 */
    uint32_t id = kmod_next_handle();

    uint8_t out_handle_type = 0;
    switch (command) {
    case UCMD_CREATE_JFC:        out_handle_type = T_CREATE_JFC_OUT_HANDLE; break;
    case UCMD_CREATE_JFS:       out_handle_type = T_CREATE_JFS_OUT_HANDLE; break;
    case UCMD_CREATE_JFR:       out_handle_type = T_CREATE_JFR_OUT_HANDLE; break;
    case UCMD_CREATE_JETTY:     out_handle_type = T_CREATE_JETTY_OUT_HANDLE; break;
    case UCMD_IMPORT_JETTY:     out_handle_type = T_IMPORT_JETTY_OUT_HANDLE; break;
    case UCMD_IMPORT_JETTY_EX:  out_handle_type = T_IMPORT_JETTY_EX_OUT_HANDLE; break;
    case UCMD_BIND_JETTY:       out_handle_type = T_BIND_JETTY_OUT_TPN; break;
    case UCMD_ALLOC_TOKEN_ID:  out_handle_type = T_ALLOC_TOKEN_ID_OUT_TOKEN_ID; break;
    default:
        return -ENOSYS;
    }

    /* 回填 OUT_ID=id */
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type == URMA_CMD_OUT_TYPE_INIT) {
            kmod_write_attr(&attrs[i], id);
            break;
        }
    }

    /* ALLOC_TOKEN_ID：OUT_TOKEN_ID 须 = ummu_tid<<8（与 ummu_grant 登记的 tid 一致）。
     * sim ummu_allocate_tid 固定返回 0x1234，故回填 0x1234<<8。 */
    if (command == UCMD_ALLOC_TOKEN_ID) {
        for (size_t i = 0; i < n; i++) {
            if (attrs[i].type == T_ALLOC_TOKEN_ID_OUT_TOKEN_ID) {
                kmod_write_attr(&attrs[i], 0x1234U << 8);
            }
        }
        return 0;
    }

    /* 回填 OUT_HANDLE（除 BIND_JETTY 的 OUT_TPN 也用同 id，简化用 id）。 */
    for (size_t i = 0; i < n; i++) {
        if (attrs[i].type == out_handle_type) {
            kmod_write_attr(&attrs[i], id);
        }
    }

    /* REGISTER_SEG：回填 OUT_TOKEN_ID = IN_TOKEN_ID（sim 的 fill_registered_tseg 用
     * arg.out.token_id 设 tseg->seg.token_id；不回填则=0 → 对端 import 后 WQE rmt_tid=0
     * 查不到）。原样回填 IN 的 token_id。 */
    if (command == UCMD_REGISTER_SEG) {
        uint64_t in_tid = 0;
        if (kmod_read_in_attr(attrs, n, T_REGISTER_SEG_IN_TOKEN_ID, &in_tid) == 0) {
            for (size_t i = 0; i < n; i++) {
                if (attrs[i].type == T_REGISTER_SEG_OUT_TOKEN_ID) {
                    kmod_write_attr(&attrs[i], in_tid);
                }
            }
        }
        return 0;
    }

    /* CREATE_JFR：回填 OUT_DEPTH/OUT_MAX_SGE = IN 值（urma_fill_jfr 用 out 覆盖 jfr_cfg，
     * 不回填则 depth=0 → dev_cap 检查 "jfr_depth==0 out of range" 失败）。 */
    if (command == UCMD_CREATE_JFR) {
        uint64_t in_depth = 0, in_sge = 0;
        if (kmod_read_in_attr(attrs, n, T_CREATE_JFR_IN_DEPTH, &in_depth) == 0) {
            for (size_t i = 0; i < n; i++) {
                if (attrs[i].type == T_CREATE_JFR_OUT_DEPTH) {
                    kmod_write_attr(&attrs[i], in_depth);
                }
            }
        }
        if (kmod_read_in_attr(attrs, n, T_CREATE_JFR_IN_MAX_SGE, &in_sge) == 0) {
            for (size_t i = 0; i < n; i++) {
                if (attrs[i].type == T_CREATE_JFR_OUT_MAX_SGE) {
                    kmod_write_attr(&attrs[i], in_sge);
                }
            }
        }
    }
    return 0;
}

/* ============================================================
 * ioctl 入口：copy_from_user hdr + attrs，分发，copy_to_user 回写
 * ============================================================ */
static long kmod_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (cmd != URMA_CMD) {
        return -ENOTTY;
    }

    urma_cmd_hdr_t hdr;
    if (copy_from_user(&hdr, (void __user *)arg, sizeof(hdr))) {
        return -EFAULT;
    }
    if (hdr.args_len == 0 || hdr.args_len > URMA_CMD_MAX_ARGS_SIZE || hdr.args_addr == 0) {
        return -EINVAL;
    }

    urma_cmd_attr_t *attrs = kmalloc(hdr.args_len, GFP_KERNEL);
    if (!attrs) {
        return -ENOMEM;
    }
    if (copy_from_user(attrs, (void __user *)(uintptr_t)hdr.args_addr, hdr.args_len)) {
        kfree(attrs);
        return -EFAULT;
    }
    size_t attr_num = hdr.args_len / sizeof(urma_cmd_attr_t);

    int ret = -ENOSYS;
    switch (hdr.command) {
    case UCMD_QUERY_DEV_ATTR:
        ret = kmod_handle_query_dev_attr(attrs, attr_num);
        break;
    case UCMD_CREATE_CTX:
        ret = kmod_handle_create_ctx(attrs, attr_num);
        break;
    case UCMD_CREATE_JFC:
    case UCMD_CREATE_JFS:
    case UCMD_CREATE_JFR:
    case UCMD_CREATE_JETTY:
    case UCMD_IMPORT_JETTY:
    case UCMD_IMPORT_JETTY_EX:
    case UCMD_BIND_JETTY:
    case UCMD_ALLOC_TOKEN_ID:
    case UCMD_REGISTER_SEG:
        ret = kmod_handle_link_cmd(hdr.command, attrs, attr_num);
        break;
    /* GET_EID_LIST / REGISTER_SEG / CREATE_JFCE 等：TODO 按 sim 补 */
    default:
        ret = -ENOSYS;
        break;
    }

    /* 回填后的 attrs 写回用户态 */
    if (ret == 0) {
        if (copy_to_user((void __user *)(uintptr_t)hdr.args_addr, attrs, hdr.args_len)) {
            ret = -EFAULT;
        }
    }
    kfree(attrs);
    return ret;
}

/* mmap：方案 A 下不实现——让数据面页由用户态 sim 给（sim 拦 mmap 对 cdev fd 给匿名页）。
 * 返回 -ENOSYS 时，真 udma 的 mmap 会怎样？需验证 sim 能否拦到对真 cdev fd 的 mmap。
 * 若 sim 拦不到，此处需实现 mmap fault 给页（备选）。先 -ENODEV 观察。 */
static int kmod_mmap(struct file *f, struct vm_area_struct *vma)
{
    return -ENODEV;
}

static int kmod_open(struct inode *inode, struct file *f)
{
    return 0;
}
static int kmod_release(struct inode *inode, struct file *f)
{
    return 0;
}

static const struct file_operations g_fops = {
    .owner = THIS_MODULE,
    .open = kmod_open,
    .release = kmod_release,
    .unlocked_ioctl = kmod_ioctl,
    .mmap = kmod_mmap,
};

/* ============================================================
 * sysfs：/sys/class/ubcore/<dev>/driver_name 等
 * urma 库 urma_read_sysfs_file 读这些属性。读时返回内置值。
 * TODO: 用 device_attribute 注册完整属性表（max_jfc/eids/eid0/transport_type/...
 *   搬自 sim config sysfs 段）。先不做，验 cdev+device_create 跑通。
 * ============================================================ */

/* ============================================================
 * init/exit：注册 cdev + class
 * ============================================================ */
static int __init urma_kmod_init(void)
{
    int ret;
    /* 设备名表（内置 udma_sim0，可扩展） */
    static const char *dev_names[] = {"udma_sim0"};
    g_dev_cnt = ARRAY_SIZE(dev_names);

    ret = alloc_chrdev_region(&g_dev_base, 0, g_dev_cnt, URMA_KMOD_NAME);
    if (ret) {
        pr_err("urma_kmod: alloc_chrdev_region failed %d\n", ret);
        return ret;
    }

    g_urma_class = class_create("ubcore");
    if (IS_ERR(g_urma_class)) {
        ret = PTR_ERR(g_urma_class);
        unregister_chrdev_region(g_dev_base, g_dev_cnt);
        return ret;
    }

    for (int i = 0; i < g_dev_cnt; i++) {
        struct urma_kmod_dev *d = &g_devs[i];
        strncpy(d->name, dev_names[i], sizeof(d->name) - 1);
        d->devt = g_dev_base + i;
        cdev_init(&d->cdev, &g_fops);
        d->cdev.owner = THIS_MODULE;
        ret = cdev_add(&d->cdev, d->devt, 1);
        if (ret) {
            pr_err("urma_kmod: cdev_add %s failed %d\n", d->name, ret);
            continue;
        }
        /* /dev/uburma/<dev> */
        d->cls_dev = device_create(g_urma_class, NULL, d->devt, d, "uburma/%s", d->name);
        if (IS_ERR(d->cls_dev)) {
            pr_err("urma_kmod: device_create %s failed %ld\n", d->name, PTR_ERR(d->cls_dev));
            cdev_del(&d->cdev);
            continue;
        }
        pr_info("urma_kmod: /dev/uburma/%s ready (devt %u)\n", d->name, d->devt);
    }

    pr_info("urma_kmod: loaded, %d device(s)\n", g_dev_cnt);
    return 0;
}

static void __exit urma_kmod_exit(void)
{
    for (int i = 0; i < g_dev_cnt; i++) {
        struct urma_kmod_dev *d = &g_devs[i];
        if (!IS_ERR_OR_NULL(d->cls_dev)) {
            device_destroy(g_urma_class, d->devt);
        }
        cdev_del(&d->cdev);
    }
    if (!IS_ERR_OR_NULL(g_urma_class)) {
        class_destroy(g_urma_class);
    }
    unregister_chrdev_region(g_dev_base, g_dev_cnt);
    pr_info("urma_kmod: unloaded\n");
}

module_init(urma_kmod_init);
module_exit(urma_kmod_exit);
