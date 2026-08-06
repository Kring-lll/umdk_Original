# bondp 基于 IMM_DATA 的多路径保序序列号设计方案

## 1. 概述

### 1.1 背景

bondp（Bonding Provider）是一个聚合设备层，在 N 个物理（physical, p）设备之上虚拟出 1 个虚拟（virtual, v）设备。应用面向 v 设备编程（v_jetty / v_jfs / v_jfr），bondp 内部将工作请求（WR）转发到多个 p 设备的 p_jetty 上发送，从而实现带宽聚合与冗余。

多路径转发带来两个数据一致性问题：

- **乱序**：同一虚拟连接的报文被分散到多条物理路径，到达对端时顺序可能与发送顺序不一致。
- **重复**：故障切换（failover）时，原路径的重传报文与新路径报文可能产生重复投递。

### 1.2 设计目标

在**不新增协议头、不改动硬件报文格式、对应用透明**的前提下，为多路径聚合流量提供序列号，并在接收侧做去重，保证上抛应用的完成（CR）流不出现重复投递。

### 1.3 核心思路

复用 URMA 协议中已有的 **IMM_DATA（64-bit immediate data）**字段，由聚合设备自身在发送侧编码、接收侧解码，在其中携带：

- **MSN（Message Sequence Number）**：消息序列号，用于去重；
- **vjetty_id**：虚拟 jetty 标识，用于 CR 还原本地 jetty；
- **cr_opcode**：操作码标签，用于还原被改写的 opcode；
- **user_data**：用户原始 imm_data 的部分位，透传。

非聚合对端不感知该机制，实现完全向后兼容。

---

## 2. IMM_DATA 位域编码

### 2.1 位域布局

64-bit imm_data 被划分为 5 个字段：

| Bit 位 | 宽度 | 字段          | 含义                                       |
|--------|------|---------------|--------------------------------------------|
| 0-19   | 20   | user_data     | 用户原始 imm_data 的低 20 位（透传给应用） |
| 20-21  | 2    | reserved      | 保留                                       |
| 22-37  | 16   | vjetty_id     | 虚拟 jetty id（0–65535）                   |
| 38-39  | 2    | cr_opcode     | 操作码标签（0–3）                          |
| 40-63  | 24   | msn           | 消息序列号（0–16M）                        |

> 注：MSN 字段宽 24 位，但实际环形序列号空间只使用低 16 位（见 §4.2）。

### 2.2 双模式编解码

编解码函数根据 `msn_enable` 标志区分两种情形：

- **msn_enable = true（保序模式）**：高 24 位存 MSN，低 20 位存 user_data；
- **msn_enable = false（透传模式）**：不启用保序，高 24 位连同低 20 位原样回填用户 imm_data（即把 user_data 的高位段写入 MSN 位段），实现零损坏透传。

### 2.3 opcode 升格

对于纯 `SEND` / `WRITE` 等本无 imm 的操作，bondp 在发送侧将其升格为 `_WITH_IMM` 形态（如 `SEND → SEND_IMM`），以腾出 64-bit imm_data 承载上述字段。由于 opcode 被改写，原始语义丢失，故用 2 位 `cr_opcode` 显式标记，在接收侧解码时还原。

### 2.4 设计权衡

- **复用既有字段**：不新增协议头、不改硬件格式，纯软件编解码；
- **字段宽度约束**：user_data 在保序模式下仅保留 20 位，超出部分被 MSN 占用；
- **向后兼容**：透传模式下 imm_data 原样无损传递。

### 2.5 编解码伪代码

**位域常量与掩码：**

```c
/* 各字段位宽 */
#define IMM_USER_BITS      20
#define RESERVED_BITS      2
#define VJETTY_ID_BITS     16
#define CR_OPCODE_BITS     2
#define MSN_BITS           24

/* 各字段起始位（累加得到） */
#define USER_SHIFT         0
#define RESERVED_SHIFT     (USER_SHIFT      + IMM_USER_BITS)      // 20
#define VJETTY_ID_SHIFT    (RESERVED_SHIFT  + RESERVED_BITS)     // 22
#define CR_OPCODE_SHIFT    (VJETTY_ID_SHIFT + VJETTY_ID_BITS)    // 38
#define MSN_SHIFT          (CR_OPCODE_SHIFT + CR_OPCODE_BITS)    // 40

/* 各字段掩码 */
#define USER_MASK          ((1ULL << IMM_USER_BITS)  - 1)
#define VJETTY_ID_MASK     ((1ULL << VJETTY_ID_BITS) - 1)
#define CR_OPCODE_MASK     ((1ULL << CR_OPCODE_BITS) - 1)
#define MSN_MASK           ((1ULL << MSN_BITS)       - 1)
```

**编码：将 5 个字段打包进 64-bit imm_data**

```c
uint64_t encode_imm_data(uint32_t cr_opcode, uint32_t msn,
                         uint32_t vjetty_id, uint64_t user_data,
                         bool msn_enable)
{
    uint64_t imm = 0;
    imm |= (uint64_t)(cr_opcode & CR_OPCODE_MASK) << CR_OPCODE_SHIFT;

    if (msn_enable) {
        /* 保序模式：高 24 位存 MSN */
        imm |= (uint64_t)(msn & MSN_MASK) << MSN_SHIFT;
    } else {
        /* 透传模式：user_data 的高 24 位原样填入 MSN 段，保证 imm 无损 */
        imm |= ((user_data >> MSN_SHIFT) & MSN_MASK) << MSN_SHIFT;
    }
    imm |= (uint64_t)(vjetty_id & VJETTY_ID_MASK) << VJETTY_ID_SHIFT;
    imm |= (user_data & USER_MASK) << USER_SHIFT;
    return imm;
}
```

**解码：从 imm_data 拆出各字段**

```c
void decode_imm_data(uint64_t imm, uint32_t *cr_opcode, uint32_t *msn,
                     uint32_t *vjetty_id, uint64_t *user_data,
                     bool msn_enable)
{
    *cr_opcode = (imm >> CR_OPCODE_SHIFT) & CR_OPCODE_MASK;
    *vjetty_id = (imm >> VJETTY_ID_SHIFT) & VJETTY_ID_MASK;
    *user_data = (imm >> USER_SHIFT) & USER_MASK;

    if (msn_enable) {
        *msn = (imm >> MSN_SHIFT) & MSN_MASK;
    } else {
        /* 透传模式：把高 24 位拼回 user_data，恢复原始 imm */
        *user_data |= ((imm >> MSN_SHIFT) & MSN_MASK) << MSN_SHIFT;
    }
}
```

**发送侧 opcode 升格 + 编码**

```c
void encode_jfs_wr_msn(jfs_wr_t *wr, comp_t *comp, uint32_t msn, bool msn_enable)
{
    uint32_t opcode_tag;
    switch (wr->opcode) {
    case OPC_SEND:
        opcode_tag = CR_OPC_SEND;
        wr->opcode = OPC_SEND_IMM;              /* 升格，腾出 imm_data */
        wr->send.imm_data = encode_imm_data(opcode_tag, msn,
                                            comp->v_jetty.id, wr->send.imm_data, msn_enable);
        return;
    case OPC_SEND_IMM:
        opcode_tag = CR_OPC_SEND_WITH_IMM;
        wr->send.imm_data = encode_imm_data(opcode_tag, msn,
                                            comp->v_jetty.id, wr->send.imm_data, msn_enable);
        return;
    case OPC_SEND_INVALIDATE:
        opcode_tag = CR_OPC_SEND_WITH_INV;
        wr->opcode = OPC_SEND_IMM;              /* 升格 */
        wr->send.imm_data = encode_imm_data(opcode_tag, msn,
                                            comp->v_jetty.id, wr->send.imm_data, msn_enable);
        return;
    case OPC_WRITE_IMM:
        opcode_tag = CR_OPC_WRITE_WITH_IMM;
        wr->rw.notify_data = encode_imm_data(opcode_tag, msn,
                                            comp->v_jetty.id, wr->rw.notify_data, msn_enable);
        return;
    case OPC_WRITE:
    case OPC_WRITE_NOTIFY:
    case OPC_READ:
    case OPC_CAS:
    case OPC_FADD:
        /* 无 imm 的操作，不编码 MSN（也无从承载） */
        return;
    default:
        return;
    }
}
```

> 关键点：纯 `SEND` 被「升格」为 `SEND_IMM` 才能携带 imm_data，因此必须用 `cr_opcode` 显式记录原始语义，否则接收端无法区分是 `SEND` 还是 `SEND_IMM`。

---

## 3. 控制平面：保序能力的配置与协商

### 3.1 配置入口

- **用户态开关**：`bondp_toggle_msn(ctx, enable)` 设置 context 级别的 `msn_enable`，要求 context 未被使用（引用计数为 1）时才允许切换。
- **默认值**：provider 创建 context 时默认 `msn_enable = true`。

### 3.2 协商与下发

- **下发到组件**：创建 jfr / jetty 时，将 context 的 `msn_enable` 写入组件信息，传递给数据平面。
- **对端协商**：建立 target_jetty 时，从对端 rjetty 的扩展信息中协商出 `is_msn_enabled`。其本质是「对端是否为多路径聚合 jetty」：仅当两端均为多路径聚合时才真正启用 MSN 编码，单设备 / 非多路径对端不编码，零开销透传。

### 3.3 路径分支

数据平面在 post_send / post_recv 时根据 `msn_enable` 选择实现路径：

- **开启**：走 `_and_store` 路径——存储 WR 上下文以支持 failover 重传，并做 MSN 去重；
- **关闭**：走 `_without_backup` / `_no_store` 路径——无存储、无去重，纯转发。

---

## 4. 数据平面：发送侧 MSN 分配

### 4.1 计数器组织

- MSN 计数器挂在 **v 组件（v_jetty / v_jfs）** 上，是 **per-vjetty 的全局原子计数器**；
- 多条 p 路径共享同一计数器，从而保证整条虚拟连接的 MSN 单调递增、全局唯一。

### 4.2 批量原子取号

一次 post 一串 WR 时：

1. 用 `atomic_fetch_add(&comp->msn, wr_total)` 一次性为整批 WR 取得连续号段 `[base, base + wr_total)`；
2. 逐 WR 编码 `wr_msn = (base + i) % BONDP_MAX_BITMAP_SIZE`；
3. 取模常量 `BONDP_MAX_BITMAP_SIZE = 1 << 16 = 65536`，与接收侧滑窗的环回空间对齐。

批量取号避免逐 WR CAS 的竞争，同时保证同批 WR 内 MSN 连续、跨线程安全。

### 4.3 编码流程

对每个待发送 WR：

1. 调用 `encode_jfs_wr_msn`，按 opcode 决定是否升格为 `_WITH_IMM` 并计算 `cr_opcode` 标签；
2. 调用 `encode_imm_data` 将 `cr_opcode / msn / vjetty_id / user_data` 打包进 64-bit imm_data；
3. 对于无 imm 的操作（WRITE / READ / CAS 等），跳过 MSN 编码。

### 4.4 路径绑定与提交

MSN 编码完成后，WR 仍处于 v 域（引用 v_tseg / v_tjetty）。随后由调度器选定一条 p 路径（send_idx / target_idx），`convert_jfs_vwr_to_pwr` 将 v 域资源映射为 p 域资源，最终提交给 p 设备硬件发送。

### 4.5 发送侧实现伪代码

**v→p 资源映射**：将 WR 中所有 v_tseg / v_tjetty 指针替换为对应路径的 p_tseg / p_tjetty（通过 `CONTAINER_OF` 取回 bondp 私有结构，再按索引取 p 镜像）：

```c
/* 取 v_tjetty 对应路径的 p_tjetty */
target_jetty_t *get_p_tjetty(target_jetty_t *vtjetty, int send_idx, int target_idx)
{
    bdp_target_jetty_t *bdp_tj = container_of(vtjetty, bdp_target_jetty_t, v_tjetty);
    bdp_p_target_jetty_t *p_tj = find_p_tjetty(bdp_tj, send_idx, target_idx);
    return p_tj ? p_tj->p_tjetty : NULL;
}

/* 取 v_tseg 对应路径的 p_tseg（区分本地注册 seg 与 imported seg） */
target_seg_t *get_p_tseg(target_seg_t *vtseg, int local_idx, int remote_idx)
{
    if (vtseg->token_id != NULL) {                          /* 本地注册 seg */
        return container_of(vtseg, bdp_tseg_t, v_tseg)->p_tseg[local_idx];
    } else {                                                 /* imported seg */
        return container_of(vtseg, bdp_import_tseg_t, v_tseg)->p_tseg[local_idx][remote_idx];
    }
}

/* 按操作类型把整条 WR 的 v 域指针改写为 p 域指针 */
void convert_jfs_vwr_to_pwr(jfs_wr_t *wr, int send_idx, int target_idx)
{
    switch (wr->opcode) {
    case OPC_SEND: case OPC_SEND_IMM: case OPC_SEND_INVALIDATE:
        for (i = 0; i < wr->send.src.num_sge; i++)
            wr->send.src.sge[i].tseg = get_p_tseg(wr->send.src.sge[i].tseg, send_idx, target_idx);
        wr->tjetty = get_p_tjetty(wr->tjetty, send_idx, target_idx);
        return;
    case OPC_WRITE: case OPC_WRITE_IMM: case OPC_WRITE_NOTIFY: case OPC_READ:
        for (i = 0; i < wr->rw.src.num_sge; i++)
            wr->rw.src.sge[i].tseg = get_p_tseg(wr->rw.src.sge[i].tseg, send_idx, target_idx);
        for (i = 0; i < wr->rw.dst.num_sge; i++)
            wr->rw.dst.sge[i].tseg = get_p_tseg(wr->rw.dst.sge[i].tseg, send_idx, target_idx);
        wr->tjetty = get_p_tjetty(wr->tjetty, send_idx, target_idx);
        return;
    case OPC_CAS: case OPC_FADD: /* 同理处理原子操作的 src/dst */
        ...
    }
}
```

**批量 post send（含 MSN 编码 + 路径重试）**：

```c
/* 一次提交 wr_total 条 WR，内部完成取号、编码、映射、提交 */
status_t bondp_post_send_wr_list_and_store(comp_t *comp, jfs_wr_t *wr,
                                           jfs_wr_t **bad_wr, int wr_total)
{
    bdp_target_jetty_t *bdp_tj = container_of(wr->tjetty, bdp_target_jetty_t, v_tjetty);

    /* ① 批量取号：一次 fetch_add 拿到连续号段，避免逐 WR CAS */
    uint32_t base_msn = 0;
    if (bdp_tj->is_msn_enabled)
        base_msn = atomic_fetch_add(&comp->msn, wr_total) % MAX_BITMAP_SIZE;   /* 65536 */

    for (retry = 0; retry < POST_SEND_MAX_RETRY; retry++) {
        /* ② 调度选路 */
        int send_idx, target_idx;
        if (schedule_send_wr(wr, comp, &send_idx, &target_idx) != 0)
            return FAIL;

        /* ③ 批量分配 WR 存储条目（用于 failover 重传） */
        wr_entry_t *entries[wr_total];
        if (jfs_wr_buf_alloc_batch(&comp->send_wr_buf, entries, wr_total) != wr_total) {
            if (!atomic_load(&comp->valid[send_idx])) continue;   /* 路径失效，重试 */
            return ENOMEM;
        }

        /* ④ 拷贝 + 编码 MSN（此时仍在 v 域） */
        jfs_wr_t *cur = wr;
        for (i = 0; i < wr_total; i++) {
            jfs_wr_t *pwr = &entries[i]->wr;
            copy_jfs_wr(cur, pwr, ...);                           /* 深拷贝 SGE */
            uint32_t wr_msn = (base_msn + i) % MAX_BITMAP_SIZE;   /* 同批连续号 */
            encode_jfs_wr_msn(pwr, comp, wr_msn, bdp_tj->is_msn_enabled);
            if (i > 0) entries[i-1]->wr.next = pwr;               /* 链成链表 */
            cur = cur->next;
        }

        /* ⑤ 临界区：提交路径绑定 + 有效性校验 + 提交（与 failover CR 互斥） */
        spin_lock(&comp->send_lock);
        for (i = 0; i < wr_total; i++) {
            entries[i]->send_idx   = send_idx;
            entries[i]->target_idx = target_idx;
            convert_jfs_vwr_to_pwr(&entries[i]->wr, send_idx, target_idx);  /* v→p */
        }
        if (!atomic_load(&comp->valid[send_idx])) {   /* 提交前路径失效 */
            /* 回滚：把 p 域改回 v 域，释放条目，进入下一轮重试 */
            for (i = 0; i < wr_total; i++)
                convert_jfs_pwr_to_vwr(&entries[i]->wr, &entries[i]->target_vjetty->v_tjetty);
            spin_unlock(&comp->send_lock);
            release_batch(...);
            continue;
        }
        status_t ret = comp_post_send(comp, send_idx, target_idx, &entries[0]->wr, bad_wr, wr_total);
        for (i = 0; i < wr_total; i++)
            convert_jfs_pwr_to_vwr(&entries[i]->wr, &entries[i]->target_vjetty->v_tjetty);  /* 提交后还原，供下次复用 */
        spin_unlock(&comp->send_lock);
        return ret;
    }
    return FAIL;
}
```

> 关键点：`send_lock` 临界区把「路径绑定 + 有效性校验 + 提交」做成原子动作，与 failover CR 处理中的路径失效互斥，避免提交到已失效路径。提交完成后立即把 WR 改回 v 域，便于条目复用。

---

## 5. 数据平面：接收侧滑窗去重

### 5.1 去重上下文组织

去重上下文 **per virtual connection**：

- `bondp_conn_t` 挂在本地 vjetty 的 `v_conn_table` 上；
- 以 **对端 v jetty_id 为 key** 索引；
- 每个 `bondp_conn_t` 内含一个 `recv_wnd` 滑动窗口。

即「本地 vjetty ← 某个对端 vjetty」这条虚拟连接共享一个滑窗，天然覆盖该连接上的所有 p 路径。

### 5.2 滑动窗口算法

| 参数        | 取值             | 含义                                   |
|-------------|------------------|----------------------------------------|
| total_size  | 1 << 16 = 65536  | 序列号环回空间，与发送侧取模对齐       |
| window_size | 1 << 12 = 4096   | 接收窗口大小                           |
| head        | 初始 0           | 指向当前期望的最小 seq                 |

算法基于 bitmap：

- `seq_in_window(seq)`：判断 seq 是否落在 `[head, head + window)` 的循环区间内；
- `has(seq)`：判断 seq 是否已收到（bitmap 对应位为 1）；
- `add(seq)`：窗内且未收 → 置位；若置位的是 head，则循环清零滑出位并推进 head，实现自动滑动。

### 5.3 接收处理流程

对每个收到的 p CR：

1. `convert_pcr_to_vcr` 解码 imm_data，还原 `msn / cr_opcode / vjetty_id / user_data`，并把 `remote_id` 从 p jetty id 还原为 v jetty id；
2. 若 `msn_enable = false`，直接投递；
3. 否则按对端 v jetty_id 查/建 `v_conn`，取 `recv_wnd`：
   - **窗外 或 已收（重复）** → 重新 arm 一条 recv WR，CR 不上抛（CONVERT_SKIP）；
   - **窗内且未收** → bitmap 置位（可能触发滑窗推进）→ CR 上抛给应用。

### 5.4 去重策略定性

- **基于序列号的幂等去重 + 滑窗丢弃**，而非阻塞式重排序缓存；
- 窗内乱序到达的报文**按到达顺序投递**，不强制等序；
- 依赖前提：**乱序跨度 < 窗口大小（4096）**。正常多路径乱序在窗内，首个到达即投递，后续重复被吞；
- 真正的旧报文（已滑出或已收）被丢弃，防止 failover 重传导致的重复投递。

### 5.5 接收侧实现伪代码

**滑动窗口核心算法**（bitmap + 循环区间）：

```c
/* total_size = 65536, window_size = 4096, head 初始 0 */
typedef struct slide_wnd {
    bitmap_t  *bits;          /* total_size 位 */
    uint32_t   total_size;
    uint32_t   window_size;
    uint32_t   head;          /* 期望的最小 seq */
} slide_wnd_t;

/* 判断 seq 是否落在 [head, head+window) 的循环区间内 */
static bool seq_in_loop_range(uint32_t head, uint32_t len, uint32_t max, uint32_t seq)
{
    uint32_t tail = (head + len) % max;
    if (tail < head)           /* 跨越环回点：合法区间为 [head, max) ∪ [0, tail) */
        return (seq >= head) || (seq < tail);
    else                       /* 未跨越：合法区间为 [head, tail) */
        return (seq >= head) && (seq < tail);
}

bool slide_wnd_seq_in_window(slide_wnd_t *w, uint32_t seq)
{
    if (seq >= w->total_size) return false;
    return seq_in_loop_range(w->head, w->window_size, w->total_size, seq);
}

bool slide_wnd_has(slide_wnd_t *w, uint32_t seq)
{
    /* 必须先在窗口内，且 bitmap 已置位 */
    return slide_wnd_seq_in_window(w, seq) && bitmap_is_set(w->bits, seq);
}

int slide_wnd_add(slide_wnd_t *w, uint32_t seq)
{
    if (!slide_wnd_seq_in_window(w, seq))   return OUT_OF_WND;  /* 窗外：旧报文 */
    if (bitmap_is_set(w->bits, seq))        return DUPLICATE;   /* 重复报文 */
    bitmap_set1(w->bits, seq);

    /* 命中 head → 自动滑动：清掉滑出窗口的位，推进 head，
       直到 head 处的位为 0（即下一个期望且未到的 seq） */
    while (bitmap_is_set(w->bits, w->head)) {
        bitmap_set0(w->bits, (w->head + w->window_size) % w->total_size);  /* 清滑出位 */
        w->head = (w->head + 1) % w->total_size;
    }
    return 0;   /* 窗内未收 → 接收，调用方将 CR 上抛 */
}
```

> 滑窗语义：`head` 始终指向「期望到达的最小 seq」。乱序到达时，中间位先置 1、head 暂不推进；等 head 那一位补齐后，连续已收的段一次性滑出。

**虚拟连接表**（per 本地 vjetty，按对端 v jetty_id 索引）：

```c
typedef struct bondp_conn {
    slide_wnd_t recv_wnd;     /* 唯一字段：去重滑窗 */
} bondp_conn_t;

/* 初始化：total=65536, window=4096, head=0 */
int v_conn_init(bondp_conn_t *conn) {
    return slide_wnd_init(&conn->recv_wnd, MAX_BITMAP_SIZE, RECV_WND_SIZE, 0);
}

/* 按 target_id(eid+uasid+id) 查/建连接节点（hash 表 + 比较函数） */
int conn_table_get_or_create(hash_table_t *tbl, jetty_id_t *target_id,
                             bondp_conn_t **conn);
```

**CR 处理主流程**（含解码 + 去重 + 重新 arm）：

```c
cr_ret_t handle_recv_cr_with_store(context_t *bdp_ctx, int idx, cr_t *cr)
{
    comp_t *recv_comp = get_comp_by_cr(bdp_ctx, idx, cr);          /* 按 p CR 的 local_id 找回 v jetty */
    wr_buf_t *recv_buf = get_recv_wr_buf(recv_comp);
    jfr_wr_entry_t *entry = jfr_wr_buf_get(recv_buf, cr->user_ctx);  /* 找回接收 WR */
    comp_t *post_comp = entry->bdp_comp;
    uint32_t recv_idx = entry->recv_idx;

    /* ① 解码 imm_data：还原 msn/opcode/vjetty_id/user_data，remote_id 由 p→v */
    uint32_t msn = 0;
    convert_pcr_to_vcr(cr, bdp_ctx, &msn);                         /* 内部调 decode_imm_data */
    cr->local_id  = recv_comp->v_jetty.id;                        /* p jetty id → v jetty id */
    cr->user_ctx  = entry->user_ctx;                              /* 还原应用上下文 */

    /* 接收 WR 计数 */
    post_comp->rqe_cnt[recv_idx]--;

    /* ② 关闭保序：直接投递 */
    if (!bdp_ctx->msn_enable)
        goto SUCCESS;

    /* ③ 开启保序：按对端 v jetty_id 找去重上下文 */
    bondp_conn_t *v_conn;
    jetty_id_t target = cr->remote_id;                            /* 已是 v jetty_id */
    conn_table_get_or_create(&recv_comp->v_conn_table, &target, &v_conn);

    /* ④ 滑窗判定：窗外或重复 → 丢弃，重 arm recv WR，CR 不上抛 */
    if (!slide_wnd_seq_in_window(&v_conn->recv_wnd, msn) ||
        slide_wnd_has(&v_conn->recv_wnd, msn)) {
        jfr_wr_t *bad = NULL;
        comp_post_recv(post_comp, recv_idx, &entry->wr, &bad, 1);  /* 重 arm 同一 recv WR */
        return CONVERT_SKIP;                                       /* CR 不上抛应用 */
    }

    /* ⑤ 窗内未收：置位 + 滑窗推进，CR 上抛 */
    slide_wnd_add(&v_conn->recv_wnd, msn);

SUCCESS:
    jfr_wr_buf_release(recv_buf, entry);
    return CONVERT_SUCCESS;                                       /* 调用方将 CR 拷出上抛 */
}
```

**CR 分发**（顶层按 CR 类型选处理函数）：

```c
cr_ret_t bondp_handle_cr_with_store(context_t *bdp_ctx, int idx, cr_t *cr)
{
    if (is_fake_cr(cr))                       /* 硬件构造的 SUSPEND_DONE/FLUSH_ERR_DONE */
        return handle_fake_cr_with_store(...);
    if (is_recv_cr(cr))                       /* 接收完成 */
        return bdp_ctx->msn_enable ? handle_recv_cr_with_store(...)
                                   : handle_recv_cr_without_backup(...);  /* 无存储/去重 */
    /* 发送完成 */
    return handle_send_cr_with_store(...);     /* 含 failover 重传逻辑 */
}
```

> 关键点：
> 1. **窗外或重复即丢弃 + 重 arm**：被丢弃的 CR 不会上抛应用，对应的 recv WR 被 re-post，避免接收队列耗尽；
> 2. **CR 不重排**：窗内乱序报文按到达顺序上抛，首个到达即投递，靠窗口覆盖乱序跨度；
> 3. **failover 协同**：发送侧重传保留原 MSN，对端 `slide_wnd_has` 判定为重复而丢弃，从而切换不产生重复投递。

---

## 6. 整体逻辑顺序图

### 6.1 端到端数据流

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            发送侧 (v_jfs / vjetty)                       │
│                                                                         │
│  ┌──────────────┐   ① 批量取号                                          │
│  │ 应用 post WR │───────────────┐                                       │
│  └──────────────┘               ▼                                       │
│                      ┌──────────────────────┐                            │
│                      │ atomic_fetch_add     │  base_msn                  │
│                      │ (&comp->msn, N) %65536│                           │
│                      └──────────┬───────────┘                            │
│                                 ▼                                        │
│                      ┌──────────────────────┐                            │
│                      | ② 逐 WR 编码 MSN     │                            │
│                      | encode_imm_data(...)  │  → 64-bit imm_data        │
│                      | (opcode 升格 _IMM)    │     {cr_opcode|msn|        │
│                      └──────────┬───────────┘        vjetty_id|user_data} │
│                                 ▼                                        │
│                      ┌──────────────────────┐                            │
│                      | ③ 调度选路 + 映射    │  send_idx/target_idx       │
│                      | convert vwr→pwr      │  (v_tseg→p_tseg,          │
│                      └──────────┬───────────┘   v_tjetty→p_tjetty)      │
│                                 ▼                                        │
│                      ┌──────────────────────┐                            │
│                      │ ④ comp_post_send     │                            │
│                      └──────────┬───────────┘                            │
└─────────────────────────────────┼─────────────────────────────────────────┘
                                  │
              ┌───────────────────┼───────────────────┐
              │    p 路径 0       │      p 路径 1     │   ... 可能乱序/重复
              ▼                   ▼                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                            接收侧 (v_jfr / vjetty)                       │
│                                                                         │
│                      ┌──────────────────────┐                            │
│                      │ ⑤ 硬件 p_jfr 收到    │  → p CR (含 imm_data)     │
│                      └──────────┬───────────┘                            │
│                                 ▼                                        │
│                      ┌──────────────────────┐                            │
│                      | ⑥ convert_pcr_to_vcr |  decode imm_data          │
│                      |   还原 msn/opcode/   |  → msn                     │
│                      |   vjetty_id/imm      │  remote_id: p→v           │
│                      └──────────┬───────────┘                            │
│                                 ▼                                        │
│                      ┌──────────────────────┐                            │
│                      | ⑦ 查/建 v_conn       │  key = 对端 v jetty_id     │
│                      |   取 recv_wnd        │                            │
│                      └──────────┬───────────┘                            │
│                                 ▼                                        │
│                ┌──────────────────────────────────┐                      │
│                │ ⑧ 滑窗判定                       │                      │
│                │  窗外 / 已收(重复)? ──yes──► 重新 arm recv WR          │
│                │        │ no                       │ CR 不上抛 (SKIP)     │
│                │        ▼                          │                      │
│                │  置位 + 滑窗推进                 │                      │
│                └────────┬─────────────────────────┘                      │
│                         ▼                                                │
│                      ┌──────────────────────┐                            │
│                      │ ⑨ CR 上抛应用        │                            │
│                      └──────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────────┘
```

### 6.2 MSN 编码时序（发送侧）

```
应用           bondp(v_jfs)         comp.msn(atomic)      p_jetty(某路径)
 │   post N WR │                         │                      │
 │────────────>│                         │                      │
 │             │  fetch_add(msn, N)      │                      │
 │             │────────────────────────>|                      │
 │             │       base_msn          │                      │
 │             │<────────────────────────│                      │
 │             │  for i in [0,N):        │                      │
 │             │    wr_msn = (base+i)%65536                      │
 │             │    encode_imm_data(cr_op, wr_msn, vjetty_id,    │
 │             │                  user_imm, msn_enable)          │
 │             │    convert_jfs_vwr_to_pwr(send_idx, target_idx)│
 │             │    comp_post_send ─────────────────────────────>│
 │             │                         │                      │ (硬件发送)
```

### 6.3 MSN 去重时序（接收侧）

```
p_jfr          bondp(v_jfr)         v_conn.recv_wnd       应用
 │  p CR       │                         │                   │
 │────────────>│                         │                   │
 │             │  decode imm_data        │                   │
 │             │  → msn, 还原 opcode/id  │                   │
 │             │  查 v_conn(remote_vjid) │                   │
 │             │────────────────────────>|                   │
 │             │       recv_wnd          │                   │
 │             │<────────────────────────│                   │
 │             │  seq_in_window? has?    │                   │
 │             │  ┌─ yes(窗外/重复) ──► comp_post_recv(重arm)│
 │             │  │                      │   CR 不上抛      │
 │             │  └─ no(窗内未收)        │                   │
 │             │     bitmap set1 + 滑窗  │                   │
 │             │     CR 上抛 ─────────────────────────────────>│
```

### 6.4 failover 与保序协同

```
原路径(路径A)        bondp发送侧            对端接收侧(recv_wnd)
     │                    │                        │
     │  WR(m=5) 已发      │                        │
     │  WR(m=6) 已发      │                        │
     │  ✗ 故障             │                        │
     │<───────────────────│                        │
     │              failover 重传                   │
     │              schedule_send 选路径B           │
     │              resend WR(m=5,6) 原MSN不变      │
     │              ──────────────────────────────>│
     │                                          收到 m=5 (可能重复/窗外)
     │                                          滑窗判定 → 丢弃，重arm
     │                                          CR 不重复上抛
```

> 关键：重传报文携带原始 MSN，对端滑窗会将其判定为重复而丢弃，从而避免故障切换导致的重复投递。

---

## 7. 数据结构组织

### 7.1 关键结构

```
bondp_context_t  (per v context)
├─ msn_enable           // context 级开关
├─ p_ctxs[N]            // N 个 p context
└─ ...

bondp_comp_t  (per v jetty / jfs / jfr)
├─ v_jetty / v_jfs / v_jfr    // 暴露给应用
├─ p_jfs[N] / p_jfr[N] / p_jetty[N]  // N 个 p 镜像
├─ msn (atomic_uint)          // ★ per-vjetty 全局 MSN 计数器
├─ send_wr_buf                // 发送 WR 存储（支持 failover 重传）
├─ recv_wr_buf                // 接收 WR 存储
├─ v_conn_table               // ★ 虚拟连接表（接收去重）
└─ valid[N] / sqe_cnt[N][N]   // 路径有效性 / 在途计数

bondp_target_jetty_t  (per 本地→对端 vjetty 绑定)
├─ v_tjetty
├─ is_msn_enabled             // ★ 该对端是否启用 MSN（协商结果）
└─ p_tjettys[]                // 各 p 路径的 p_tjetty

bondp_conn_t  (per virtual connection，去重上下文)
└─ recv_wnd  (bdp_slide_wnd_t)
   ├─ bits (bitmap)           // total_size = 65536
   ├─ window_size = 4096
   └─ head                     // 期望最小 seq

urma_cr_t.imm_data  (64-bit，承载编码后的位域)
├─ [40-63] msn
├─ [38-39] cr_opcode
├─ [22-37] vjetty_id
├─ [20-21] reserved
└─ [0-19]  user_data
```

### 7.2 作用域对照

| 关注点             | 组织粒度                        | 说明                                   |
|--------------------|---------------------------------|----------------------------------------|
| MSN 唯一性         | per v_jetty（comp.msn）         | 多 p 路径共享，全局单调递增            |
| MSN 去重           | per virtual connection（v_conn）| 按对端 v jetty_id 索引，覆盖所有 p 路径|
| MSN 编码开关       | per target_jetty（is_msn_enabled）| 两端多路径协商结果                   |
| context 级开关     | per v_context（msn_enable）     | 用户可配，下发到 comp / target_jetty   |

---

## 8. 设计权衡与适用边界

### 8.1 优势

1. **零协议开销**：复用既有 IMM 字段，不新增头、不改硬件格式；
2. **对应用透明**：透传模式下 imm_data 无损，非聚合对端无感知；
3. **批量取号低竞争**：`fetch_add(N)` 一次取连续号段，避免逐 WR CAS；
4. **failover 与保序协同**：重传保留原 MSN，滑窗自动丢弃重复，无缝切换；
5. **粒度恰当**：发送 per-vjetty 计数保唯一，接收 per-connection 滑窗保去重。

### 8.2 局限与前提

1. **非严格按序投递**：窗内乱序按到达顺序上抛，不缓存重排。需上层容忍乱序或依赖窗口覆盖乱序跨度；
2. **乱序跨度上限**：乱序跨度需 < 4096（窗口大小），否则误判窗外丢弃；
3. **ABA 规避**：环回空间 65536 远大于窗口 4096，保证旧 MSN 在环回前已滑出窗口；
4. **user_data 截断**：保序模式下 user_data 仅保留 20 位。

### 8.3 适用场景

- 多路径聚合的高性能通信场景，乱序跨度受控（远小于 4096）；
- 对重复投递零容忍、对按序投递要求可放宽的语义；
- 需要与 failover 重传机制协同的高可用场景。
