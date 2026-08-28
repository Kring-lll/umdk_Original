# URMA Bond聚合设备端口状态查询设计

## 1. 文档信息

| 项目 | 内容 |
|---|---|
| 代码基线 | `br_openEuler_24.03_LTS_SP3`，提交 `5647a709` |
| 分析对象 | URMA聚合设备用户态provider：`src/urma/lib/urma/bond` |
| 核心接口 | `urma_user_ctl`、`BONDP_USER_CTL_QUERY_PORT_STATUS` |
| 核心场景 | 应用查询本端聚合设备各物理端口的可用状态（好/坏） |

本文给出从接口定义到状态记录、再到代码修改点的完整可执行方案。端口状态由bond provider内部维护：数据面轮询到status 4（`URMA_CR_LOC_ACCESS_ERR`）时置坏，健康检查恢复或后续数据面通信成功时恢复好。

---

## 2. 背景与目标

### 2.1 需求

应用希望不指定具体对象（jetty/jfr），直接通过 `urma_user_ctl` 查询本端聚合设备的端口状态：

- 出参包含：端口数 + 每个端口的状态；
- 状态好（GOOD）：最近一次数据面通信正常，或者健康检查已恢复；
- 状态坏（BAD）：当前端口最近出现过 `poll_jfc` status 4，并且没有被健康检查恢复；

端口状态只有 GOOD/BAD 两态：不是 BAD 即 GOOD（包括从未通信、健康检查尚未确认等场景）。

### 2.2 状态归属：物理端口，而非虚拟对象

端口状态是**物理端口（`local_idx`）的属性**，挂在 `bondp_context_t` 上，与 vjetty 解耦：

- 同一个物理端口会被多个 vjetty/jfr 共享；若以 vjetty 为查询入参，同一端口会被多个对象重复记录且可能互相覆盖（对象A正常、对象B异常时端口好坏无法判定）；
- 端口全集（`enabled_indices[]`、`p_ctxs[]`）是 context 级信息，provider 内部已有，无需应用提供；
- 应用查询语义是“本端聚合设备哪些物理端口可用”，不关心某个虚拟对象。

因此入参为空，出参为端口数 + 每端口状态数组。

### 2.3 非目标

- 不提供“某个 vjetty 的路径状态”查询（那属于按 tjetty/路径的另一个接口）；
- 不承担端口故障诊断的完整语义（如错误码细分），只提供好/坏两态；
- 不改动现有 `BONDP_USER_CTL_QUERY_PORT` 的语义（它查询对象使能/活跃端口集）。

---

## 3. 总体设计

### 3.1 接口形态

```text
应用                        bond provider
  | urma_user_ctl(ctx,        |
  |   in: opcode=QUERY_PORT_STATUS, addr=0, len=0)
  |-------------------------->|
  |                           | 读 port_state[local_idx] 快照
  |                           | 计算 GOOD/BAD
  |<--------------------------| out: port_count + status[port_count]
```

### 3.2 状态机与闩锁语义

```text
        poll到status4
  +----------------------------------+
  |                                  v
+------+                         +-----+
| GOOD | ----------------------> | BAD | (闩锁置位)
+------+                         +-----+
  ^                                 |
  |                                 | 数据面成功CR / 健康检查恢复
  |                                 | (清闩锁)
  +---------------------------------+
```

- **置坏（唯一入口）**：数据面 `bondp_poll_jfc` 在物理端口 `idx` 上轮询到 `cr->status == URMA_CR_LOC_ACCESS_ERR`（值4）；`urma_poll_jfc` 返回错误**不置坏**，只按原有错误处理逻辑返回；
- **恢复（两个入口）**：该端口后续出现成功CR（`cr->status == URMA_CR_SUCCESS`），或健康检查探针成功；
- BAD是闩锁：置位后保持，直到上述任一恢复事件发生；健康检查探针失败**不会**置坏（坏只由数据面status4触发）。

状态判定**不依赖时间戳**：内部只保存一个闩锁标志，查询时 `闩锁置位 ? BAD : GOOD` 即可，无需比较“最近”时间。

### 3.3 并发模型

- 数据面轮询线程（可多线程，各线程轮询不同/相同JFC）与健康检查worker线程都会写 `port_state[]`；
- 状态只有一个原子闩锁标志，写者直接 `atomic_store` 无需加锁；查询时单次原子读即可；
- 查询除了读 `port_state[]`，还会读 `enabled_indices/enabled_count/port_cfg_enable/port_cfg` 等非原子配置；这些配置可能被 `SET_BONDING_MODE`/`SET_BONDING_PORT` 并发修改，因此查询和配置写入方都必须用 `ctx->mutex` 保护同一组字段，查询在锁内只做配置快照，状态读仍可无锁；
- 不要求查询看到“某一瞬间的完整一致状态”，允许读到轻度过期值，但同一端口的 `port_idx/status` 保证出自同一快照。

---

## 4. 接口说明

### 4.1 opcode

在 `bondp_user_ctl_opcode` 枚举（`src/urma/lib/urma/bond/include/urma_ubagg.h`）末尾追加：

```c
typedef enum bondp_user_ctl_opcode {
    ...
    BONDP_USER_CTL_SET_CTX_CFG,
    BONDP_USER_CTL_QUERY_PORT_STATUS,  /* 新增，取值 = 14 */
} bondp_user_ctl_opcode_t;
```

### 4.2 入参

**空入参**。应用侧：

```c
urma_user_ctl_in_t in = {
    .addr = 0,
    .len = 0,
    .opcode = BONDP_USER_CTL_QUERY_PORT_STATUS,
};
```

context 由 `urma_user_ctl` 第一参数隐式提供，端口集合由 provider 从 `bdp_ctx` 获取，不需要也不允许应用指定端口或虚拟对象。

### 4.3 出参

```c
/* 每个端口一条 */
typedef struct bondp_port_status {
    uint32_t port_idx;   /* 物理端口索引 local_idx */
    uint32_t status;     /* 0=GOOD，1=BAD */
    uint64_t reserved;   /* 预留，当前恒为0 */
} bondp_port_status_t;

typedef struct bondp_query_port_status_out {
    uint32_t port_count;                             /* 本端启用的物理端口数 */
    bondp_port_status_t port_status[URMA_UBAGG_DEV_MAX_NUM];
} bondp_query_port_status_out_t;
```

数组按 `enabled_indices[]`（`port_cfg_enable` 时按 `port_cfg.enabled_indices[]`）顺序紧凑填充，只填前 `port_count` 项。

### 4.4 状态枚举

```c
typedef enum bondp_port_status_val {
    BONDP_PORT_STATUS_GOOD = 0,    /* 最近一次数据面正常 或 健康检查已恢复 */
    BONDP_PORT_STATUS_BAD,         /* 最近出现过status4且未被健康检查恢复 */
} bondp_port_status_val_t;
```

### 4.5 应用调用示例

```c
#include "urma_api.h"
#include "urma_ubagg.h"

bondp_query_port_status_out_t status_out = {0};
urma_user_ctl_in_t in = {
    .addr = 0,
    .len = 0,
    .opcode = BONDP_USER_CTL_QUERY_PORT_STATUS,
};
urma_user_ctl_out_t out = {
    .addr = (uint64_t)(uintptr_t)&status_out,
    .len = sizeof(status_out),
};

urma_status_t ret = urma_user_ctl(ctx, &in, &out);
if (ret != URMA_SUCCESS) {
    /* 处理失败：入参非法或provider内部错误 */
    return ret;
}

for (uint32_t i = 0; i < status_out.port_count; i++) {
    printf("port_idx=%u status=%s\n",
           status_out.port_status[i].port_idx,
           status_out.port_status[i].status == BONDP_PORT_STATUS_GOOD ? "GOOD" :
           "BAD");
}
```

### 4.6 返回值

| 返回值 | 说明 |
|---|---|
| `URMA_SUCCESS` | 查询成功，`port_count` 与 `port_status[]` 有效 |
| `URMA_FAIL` | provider内部错误（`urma_user_ctl` 统一映射） |

handler 内部校验失败返回 `-EINVAL`（`urma_user_ctl` 公共入口统一返回 `URMA_FAIL`）。

---

## 5. 状态记录方案

### 5.1 内部状态表

在 `bondp_context_t`（`src/urma/lib/urma/bond/bondp_types.h`）追加：

```c
typedef struct bondp_port_state {
#ifndef __cplusplus
    atomic_bool bad_latched; /* status4闩锁：true=BAD，被数据面成功CR或HC恢复清除 */
#else
    std::atomic_bool bad_latched;
#endif
} bondp_port_state_t;

/* bondp_context_t 内追加 */
bondp_port_state_t port_state[URMA_UBAGG_DEV_MAX_NUM];
```

`bondp_types.h` 已包含 `<stdatomic.h>` / `<atomic>`，无需新增头文件。

### 5.2 数据面记录点（`bondp_poll_jfc`）

`bondp_poll_jfc`（`src/urma/lib/urma/bond/bondp_datapath.c:1638`）按物理端口逐个轮询 `p_jfc[idx]`，是天然记录位置。热路径（约`:1657-1680`）与全扫描（约`:1709-1735`）两处轮询循环都需要埋点：

```c
int pcr_cnt = urma_poll_jfc(bdp_jfc->p_jfc[idx], pcr_cnt_max, pcr_buf);
if (pcr_cnt < 0) {
    ... /* 仅按原有逻辑返回错误，不置 BAD */
}
for (int cr_id = 0; cr_id < pcr_cnt; cr_id++) {
    urma_cr_t *pcr = &pcr_buf[cr_id];
    if (pcr->status == URMA_CR_LOC_ACCESS_ERR) { /* status == 4 */
        bondp_update_port_state(bdp_ctx, idx, true);
    } else if (pcr->status == URMA_CR_SUCCESS) {
        bondp_update_port_state(bdp_ctx, idx, false);
    }
    ...原有CR转换逻辑不变...
}
```

注意点：

- 记录要放在CR转换之前、且**已知 `idx`** 的位置（转换可能 `CONVERT_SKIP`，但不影响状态记录）；
- 成功CR（发送完成、接收完成）都会以 `URMA_CR_SUCCESS` 返回，统一作为“数据面通信正常”；
- 伪CR（`SUSPEND_DONE` / `FLUSH_ERROR_DONE`，`bondp_datapath.c` 中 `is_fake_cr()` 已识别）当前不参与status4判定，如需扩展可将它们同样记为坏；
- `bondp_flush_jetty()`（`:1740-1787`）是另一个物理CR转换入口。flush属于错误/清理路径，**不因 flush 返回 `URMA_CR_LOC_ACCESS_ERR` 置 BAD**；但 flush 也可能返回真实 `URMA_CR_SUCCESS`，因此 flush 路径对成功 CR 调用 `bondp_update_port_state(bdp_ctx, i, false)` 清除 BAD，伪 CR 不参与状态记录：

```c
for (int cr_id = 0; cr_id < pcr_cnt; cr_id++) {
    urma_cr_t *pcr = &pcr_buf[cr_id];
    if (pcr->status == URMA_CR_SUCCESS) {
        bondp_update_port_state(bdp_ctx, i, false);
    }
    ...原有CR转换逻辑不变...
}
```

### 5.3 健康检查记录点（`hc_process_probe_cr`）

健康检查探针结果在 `hc_process_probe_cr()`（`src/urma/lib/urma/bond/bondp_dp_health.c:276`）处理，函数已持有 `local_idx`。在成功分支追加：

```c
bool ok = (cr->status == URMA_CR_SUCCESS);
...
if (ok) {
    bondp_update_port_state(hc_ctx->bdp_ctx, (uint32_t)local_idx, false);
}
```

`bondp_update_port_state(..., false)` 在健康检查路径中的语义：

- 清 `bad_latched`（幂等，每次成功探针都执行）；
- 探针失败不做任何置坏操作（坏只由数据面status4触发）。

**实现前置条件（真实代码缺陷修复）**：当前 `bondp_hc_ctx_t`（`bondp_dp_health.c:77-87`）没有指向 `bondp_context_t` 的回指字段；`hc_process_probe_cr()` 内 `bdp_ctx` 只在 `ok && bdp_tjetty != NULL` 时通过 `CONTAINER_OF_FIELD(bdp_tjetty->v_tjetty.urma_ctx, ...)` 取得（`:314-316`），当路径尚未注册 tjetty（`bdp_tjetty == NULL`）时 `bdp_ctx` 为 `NULL`，恢复分支不会执行。因此必须：

1. 在 `bondp_dp_health.c` 的 `struct bondp_hc_ctx` 中新增 `struct bondp_context *bdp_ctx;`；
2. 在 `bondp_hc_init()`（`:807`，入参已含 `bdp_ctx`）中赋值 `hc_ctx->bdp_ctx = bdp_ctx;`；
3. 恢复调用放在 `pthread_rwlock_unlock(&node->lock)` 之后、与 `hc_set_local_idx_jettys_hc_valid()` 同处（约 `:322-325`），与现有恢复逻辑一致。

不改这个前置条件，文档第5.3节方案在 `bdp_tjetty == NULL` 场景下会解引用空指针或漏记恢复。

### 5.4 状态合成（查询时计算）

```c
if (atomic_load(&st->bad_latched)) {
    status = BONDP_PORT_STATUS_BAD;
} else {
    status = BONDP_PORT_STATUS_GOOD;
}
```

与用户定义的对应关系：

| 条件 | 状态 |
|---|---|
| 闩锁置位（最近status4且未被数据面成功CR/健康检查恢复清除） | BAD |
| 闩锁已清（数据面成功CR / 健康检查恢复 / 从未置坏） | GOOD |

---

## 6. 修改方案

### 6.1 `src/urma/lib/urma/bond/include/urma_ubagg.h`

1. `bondp_user_ctl_opcode` 枚举末尾追加 `BONDP_USER_CTL_QUERY_PORT_STATUS`；
2. 在 `bondp_query_port_out_t` 之后追加 `bondp_port_status_t`、`bondp_port_status_val_t`、`bondp_query_port_status_out_t` 公共结构体/枚举。

### 6.2 `src/urma/lib/urma/bond/bondp_types.h`

追加内部结构 `bondp_port_state_t`，并在 `bondp_context_t` 中追加 `bondp_port_state_t port_state[URMA_UBAGG_DEV_MAX_NUM]`。

### 6.3 新增 `bondp_port_state.c` / `bondp_port_state.h`

提供四个接口（`CMakeLists.txt` 使用 `aux_source_directory` 自动收录新源文件，无需改构建脚本；新增 `.c` 后需确保重新运行 CMake configure，使 `aux_source_directory` 重新扫描）：

```c
/* bondp_port_state.h */
void bondp_port_state_init(struct bondp_context *bdp_ctx);
void bondp_update_port_state(struct bondp_context *bdp_ctx, uint32_t local_idx, bool bad);
```

实现要点：

- 所有函数先做 `local_idx >= URMA_UBAGG_DEV_MAX_NUM` 越界保护；
- `bondp_port_state.h` 自包含：加 include guard，包含 `<stdint.h>` 和 `<stdbool.h>`，并前置声明 `struct bondp_context`（或包含 `bondp_types.h`）；
- `bondp_update_port_state` 先判断 `bdp_ctx == NULL` 和越界；状态写入必须使用 `atomic_store` 或等价原子写，不得使用普通赋值；
- `bad == true`：`atomic_store(&entry.bad_latched, true)`，即置 BAD；
- `bad == false`：`atomic_store(&entry.bad_latched, false)`，即恢复 GOOD；
- `init`：对20个条目 `atomic_init(&entry.bad_latched, false)`。

### 6.4 `src/urma/lib/urma/bond/bondp_provider_ops.c`

`bondp_provider_ops.c` 顶部包含 `bondp_port_state.h`；`bondp_create_context()`（`:444`）在 `calloc` 成功、创建vcontext之前调用 `bondp_port_state_init(bdp_ctx)`。

### 6.5 `src/urma/lib/urma/bond/bondp_datapath.c`

`bondp_poll_jfc()` 两处轮询循环（热路径 `:1657-1680`、全扫描 `:1709-1735`）按5.2节埋点；`bondp_flush_jetty()`（`:1740-1787`）仅对 `URMA_CR_SUCCESS` 调用 `bondp_update_port_state(..., false)`，不置 BAD。文件包含头文件 `bondp_port_state.h`。

### 6.6 `src/urma/lib/urma/bond/bondp_dp_health.c`

1. `struct bondp_hc_ctx`（`:77-87`）新增 `struct bondp_context *bdp_ctx;` 回指字段；
2. `bondp_hc_init()`（`:807`）中 `hc_ctx->bdp_ctx = bdp_ctx;`；
3. `hc_process_probe_cr()` 成功分支（`:322-325`，`pthread_rwlock_unlock(&node->lock)` 之后）调用 `bondp_update_port_state(hc_ctx->bdp_ctx, (uint32_t)local_idx, false)`，包含头文件 `bondp_port_state.h`。

### 6.7 `src/urma/lib/urma/bond/bondp_cp_user_ctl.c`

新增 handler 并在 `bondp_user_ctl()` switch 中注册；`bondp_cp_user_ctl.c` 显式包含 `<pthread.h>`，并使用 `ctx->mutex` 做配置快照：

```c
static int bondp_user_ctl_query_port_status(urma_context_t *ctx,
                                            urma_user_ctl_in_t *in,
                                            urma_user_ctl_out_t *out)
{
    if (in->addr != 0 || in->len != 0 || out->addr == 0 ||
        out->len < sizeof(bondp_query_port_status_out_t)) {
        URMA_LOG_ERR("Invalid query port status param.\n");
        return -EINVAL;
    }

    bondp_context_t *bdp_ctx = CONTAINER_OF_FIELD(ctx, bondp_context_t, v_ctx);
    uint32_t indices[URMA_UBAGG_DEV_MAX_NUM] = {0};
    uint32_t count = 0;

    pthread_mutex_lock(&ctx->mutex);
    const uint32_t *src_indices = NULL;
    if (bdp_ctx->port_cfg_enable) {
        src_indices = bdp_ctx->port_cfg.enabled_indices;
        count = bdp_ctx->port_cfg.enabled_count;
    } else {
        src_indices = bdp_ctx->enabled_indices;
        count = bdp_ctx->enabled_count;
    }
    if (count > URMA_UBAGG_DEV_MAX_NUM) {
        pthread_mutex_unlock(&ctx->mutex);
        URMA_LOG_ERR("Invalid enabled port count=%u.\n", count);
        return -EINVAL;
    }
    (void)memcpy(indices, src_indices, count * sizeof(uint32_t));
    pthread_mutex_unlock(&ctx->mutex);

    bondp_query_port_status_out_t *out_buf =
        (bondp_query_port_status_out_t *)(uintptr_t)out->addr;
    memset(out_buf, 0, sizeof(*out_buf));
    out_buf->port_count = count;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t local_idx = indices[i];
        if (local_idx >= URMA_UBAGG_DEV_MAX_NUM) {
            URMA_LOG_ERR("Invalid local_idx=%u.\n", local_idx);
            return -EINVAL;
        }
        bondp_port_state_t *st = &bdp_ctx->port_state[local_idx];
        bondp_port_status_t *ps = &out_buf->port_status[i];
        ps->port_idx = local_idx;
        ps->reserved = 0;
        if (atomic_load(&st->bad_latched)) {
            ps->status = BONDP_PORT_STATUS_BAD;
        } else {
            ps->status = BONDP_PORT_STATUS_GOOD;
        }
    }
    return 0;
}
```

现有 `bondp_user_ctl_set_bonding_port()` 的配置写入部分同步修改为：

```c
    /* 写 port_cfg 前必须与查询线程持有同一把 ctx->mutex */
    pthread_mutex_lock(&ctx->mutex);
    bdp_ctx->port_cfg.enabled_count = enabled_count;
    (void)memcpy(bdp_ctx->port_cfg.enabled_indices, enabled_indices,
                 enabled_count * sizeof(uint32_t));
    bdp_ctx->port_cfg_enable = true;
    pthread_mutex_unlock(&ctx->mutex);
```

```c
/* bondp_user_ctl() switch 中追加 */
case BONDP_USER_CTL_QUERY_PORT_STATUS:
    return bondp_user_ctl_query_port_status(ctx, in, out);
```

---

## 7. 测试建议

### 7.1 单元测试

1. `bondp_port_state_init` / `bondp_update_port_state`：越界保护、闩锁置位/清除；
2. 状态合成：闩锁置位 → BAD，闩锁清除 → GOOD；
3. handler：`in->addr!=0`、`in->len!=0`、`out->addr==0`、`out->len` 不足均返回 `-EINVAL`；空入参返回0；
4. `port_cfg_enable` 开/关两种端口集合来源。

### 7.2 集成测试

1. 从未通信、健康检查未开始 → 查询全部为 GOOD；
2. mock 数据面CR `status=4`（`URMA_CR_LOC_ACCESS_ERR`）→ 该端口 BAD；
3. 该端口后续成功CR → GOOD；
4. status4后健康检查探针成功 → BAD清除、GOOD；
5. 健康检查探针失败（`URMA_CR_ACK_TIMEOUT_ERR`等）→ 不改变端口状态（不置BAD）；
6. 多线程并发poll + 查询，验证无崩溃、快照字段自洽；
7. 多vjetty共享同一物理端口时，任一对象成功CR都恢复该端口状态；
8. 并发 `SET_BONDING_PORT` / `SET_BONDING_MODE` 与端口状态查询，验证 `ctx->mutex` 配置快照无 torn read、无越界；
9. 并发 `SET_BONDING_PORT` 多次覆盖 `port_cfg` 时，查询看到的是某一次完整配置快照，而不是新旧数组混合。

---

## 8. 兼容性与注意事项

- 新opcode为纯增量，不改变既有opcode语义和入参约定；既有应用不受影响；
- `bondp_port_status_t` 预留u64字段保持结构可演进，应用不得依赖其内容；
- 状态查询返回的是快照，两个连续查询之间状态可能已变化，应用应以“最终一致”使用；
- 查询与 `SET_BONDING_PORT`/`SET_BONDING_MODE` 共享 `ctx->mutex` 保护配置快照；端口状态本身仍按原子闩锁最终一致读取，不要求跨端口统一快照；
- status4 的判定以 `urma_cr_status_t` 枚举值为准：`URMA_CR_LOC_ACCESS_ERR == 4`（`src/urma/lib/urma/core/include/urma_opcode.h`）；
- 状态判定不依赖时间戳：只要未被数据面成功CR或健康检查恢复清除，闩锁就一直保持BAD；如需“最近N毫秒”类时效语义，需另行引入时间字段（当前不在本接口范围内）。

---

## 9. 关键代码索引

| 功能 | 文件/位置 |
|---|---|
| `urma_user_ctl` 公共入口 | `src/urma/lib/urma/core/urma_cp_api.c:3201` |
| `urma_user_ctl_in/out` 定义 | `src/urma/lib/urma/core/include/urma_types.h:1004-1018` |
| opcode枚举 | `src/urma/lib/urma/bond/include/urma_ubagg.h:38-49` |
| 现有QUERY_PORT handler（风格参照） | `src/urma/lib/urma/bond/bondp_cp_user_ctl.c:57-83` |
| user_ctl dispatch | `src/urma/lib/urma/bond/bondp_cp_user_ctl.c:357-392` |
| `bondp_context_t` | `src/urma/lib/urma/bond/bondp_types.h` |
| context创建/初始化 | `src/urma/lib/urma/bond/bondp_provider_ops.c:444-452` |
| 数据面轮询（埋点） | `src/urma/lib/urma/bond/bondp_datapath.c:1638-1787` |
| CR转换入口 | `src/urma/lib/urma/bond/bondp_datapath.c:1596-1636` |
| 健康检查探针处理（恢复埋点） | `src/urma/lib/urma/bond/bondp_dp_health.c:276-327` |
| `bondp_hc_ctx_t`定义（需加回指字段） | `src/urma/lib/urma/bond/bondp_dp_health.c:77-87` |
| `bondp_hc_init`（回指字段赋值点） | `src/urma/lib/urma/bond/bondp_dp_health.c:807-847` |
| status枚举（`URMA_CR_LOC_ACCESS_ERR=4`） | `src/urma/lib/urma/core/include/urma_opcode.h` |
| 构建（自动收录新源文件） | `src/urma/lib/urma/bond/CMakeLists.txt` |

---

## 10. 与真实实现的一致性核查记录

本文所有结构体字段与实现引用均已对照当前分支（`5647a709`）真实代码逐项核实，结论如下。

### 10.1 已核实无误

| 文档引用 | 真实实现 | 结论 |
|---|---|---|
| `urma_user_ctl_in_t`/`out_t` | `urma_types.h:1004-1018`（in含`addr/len/opcode`，out含`addr/len/reserved`） | 一致 |
| `urma_user_ctl`公共入口 | `urma_cp_api.c:3201` | 一致 |
| opcode枚举自增取值 | `urma_ubagg.h:38-49`，`SET_BONDING_MODE_LEGACY=4`起自增，`QUERY_PORT_STATUS`追加后=14 | 一致 |
| `bondp_context_t`字段 | `bondp_types.h:116`起，含`enabled_indices/enabled_count`（`:148-149`）、`port_cfg_enable/port_cfg`（`:150-151`）、`hc_ctx`（`:141`） | 一致 |
| `bondp_create_context`初始化点 | `bondp_provider_ops.c:444` | 一致 |
| `bondp_poll_jfc`热路径/全扫描 | `bondp_datapath.c:1638`起，热路径轮询 `:1657`，全扫描轮询 `:1709` | 一致 |
| `URMA_CR_LOC_ACCESS_ERR == 4` | `urma_opcode.h`枚举，`URMA_CR_SUCCESS=0`后第4个成员 | 一致 |
| `bondp_cp_user_ctl.c`头文件依赖 | 已包含`bondp_types.h`、`urma_ubagg.h` | 一致 |
| 新符号无冲突 | 当前分支无`bondp_port_state`/`bondp_port_status`/`bondp_port_status_val`符号 | 一致 |

### 10.2 发现的问题与修复

1. **健康检查恢复点取不到 `bdp_ctx`（必须修改）**：`bondp_hc_ctx_t` 无回指字段，`hc_process_probe_cr()` 中 `bdp_ctx` 仅在 `ok && bdp_tjetty != NULL` 时有值（`:314-316`）。文档原方案在 `bdp_tjetty == NULL` 时不可执行（空指针/漏记）。修复见第5.3节与第6.6节：给 `bondp_hc_ctx_t` 增加 `bdp_ctx` 回指字段并在 `bondp_hc_init()` 赋值。
2. **flush路径边界需明确**：`bondp_flush_jetty()`（`:1740-1787`）也是CR转换入口，方案明确**不因 flush 状态置 BAD**，只对真实 `URMA_CR_SUCCESS` 清除 BAD，伪 CR 不参与状态记录（见第5.2节）。
3. **行号勘误**：`bondp_user_ctl` 实际位于 `bondp_cp_user_ctl.c:357`（原文档误写358），已修正。
4. **公共类型命名冲突已修复**：`bondp_port_status_t` 保留为结构体类型，状态枚举改为 `bondp_port_status_val_t`。
5. **轮询错误不置 BAD**：`urma_poll_jfc()` 返回负值仅按原逻辑返回错误，不调用 `bondp_update_port_state(..., true)`。
6. **查询边界与头文件补齐**：handler 增加 `count`/`local_idx` 越界保护，`bondp_provider_ops.c` 显式包含 `bondp_port_state.h`。
7. **配置快照并发保护**：查询和 `SET_BONDING_PORT` 对 `port_cfg` 的读写统一使用 `ctx->mutex`，查询在锁内复制配置快照后再无锁读取原子状态。

除上述外，本文定义的所有新结构体/枚举（`bondp_port_status_t`、`bondp_port_status_val_t`、`bondp_query_port_status_out_t`、`bondp_port_state_t`）与新增函数（`bondp_port_state_init`、`bondp_update_port_state`）均为本方案新增，当前分支不存在同名字段或符号，无未定义字段冲突。
