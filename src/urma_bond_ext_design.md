# 聚合设备 Jetty 扩展字段设计文档

---

## 一、设计目标

1. **零核心接口改动**：`urma_create_jetty`、`urma_import_jetty` 等核心接口保持不变
2. **最小扩展**：通过 `urma_rjetty` 携带扩展信息
3. **内存高效**：避免大量内存开销
4. **用户态自主交换**：去除内核 ubagg 模块的信息交换依赖

---

## 二、架构设计

### 2.1 核心流程

```
┌─────────────────────────────────────────────────────────────────────┐
│ 步骤1: 创建 Jetty                                                   │
│  ┌─────────────────┐                                               │
│  │ bondp_create    │──► 内部填充 pjetty 信息                        │
│  │ _jetty_inner()  │                                               │
│  └────────┬────────┘                                               │
│           ▼                                                        │
│  ┌─────────────────┐                                               │
│  │ urma_jetty_t    │◄── 不修改，保持原样                            │
│  └─────────────────┘                                               │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 步骤2: 查询扩展信息（按需分配）                                       │
│  ┌──────────────────────────────────┐                              │
│  │ urma_get_jetty_bond_ext()        │                              │
│  │ ──► 从 bondp_comp_t 读取信息     │                              │
│  │ ──► 填充到用户缓冲区             │                              │
│  └──────────────────────────────────┘                              │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│ 步骤3: 交换 & Import                                                │
│  ┌──────────────┐     网络传输     ┌──────────────┐                │
│  │ 本端 ext_buf │────────────────►│ 对端 ext_buf │                │
│  └──────────────┘                 └──────┬───────┘                │
│                                          ▼                         │
│                              ┌──────────────────────┐               │
│                              │ urma_rjetty.bond_ext │               │
│                              │ ──► import 时使用    │               │
│                              └──────────────────────┘               │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 与内核交换方案对比

| 维度 | 内核交换方案 | 用户态交换方案 |
|------|-------------|---------------|
| **信息交换路径** | 用户态 → 内核 ubagg → 用户态 | 用户态 → 网络 → 用户态 |
| **延迟** | 高（两次系统调用） | 低（直接网络传输） |
| **灵活性** | 固定格式 | 用户可自定义 |
| **依赖** | 内核模块 | 网络通信 |

---

## 三、数据结构定义

### 3.1 Bond 扩展信息结构

```c
#define URMA_JETTY_BOND_EXT_MAX_SLAVES URMA_UBAGG_DEV_MAX_NUM

typedef struct urma_jetty_bond_ext {
    uint16_t version;                                           // 版本号
    uint16_t total_size;                                       // 总大小
    
    // ===== pjetty 核心信息 =====
    urma_jetty_id_t slave_id[URMA_JETTY_BOND_EXT_MAX_SLAVES];   // pjetty ID 数组
    uint8_t enabled_indices[URMA_JETTY_BOND_EXT_MAX_SLAVES];    // 启用索引
    uint32_t enabled_count;                                     // 启用数量
    
    // ===== 传输配置 =====
    urma_transport_mode_t trans_mode;
    
    // ===== 健康检查 =====
    bool is_health_check_enable;
    urma_seg_t health_check_seg[URMA_JETTY_BOND_EXT_MAX_SLAVES];
    int health_check_dev_num;
    
    // ===== 连接矩阵 =====
    bool connected[URMA_JETTY_BOND_EXT_MAX_SLAVES][URMA_JETTY_BOND_EXT_MAX_SLAVES];
    
    // ===== 元数据 =====
    urma_eid_t local_eid;                                      // 本端 EID
    uint32_t reserved;
} urma_jetty_bond_ext_t;

#define URMA_JETTY_BOND_EXT_VERSION_1 0x0100
```

**内存占用计算**：约 512 字节（含对齐）

### 3.2 修改 `urma_rjetty_t`

```c
typedef struct urma_rjetty {
    urma_jetty_id_t jetty_id;
    urma_transport_mode_t trans_mode;
    urma_jetty_flag_t flag;
    urma_policy_t policy;
    urma_jetty_type_t type;
    
    // ★ 新增：bond 扩展信息指针
    const urma_jetty_bond_ext_t *bond_ext;
    size_t bond_ext_size;
} urma_rjetty_t;
```

**内存增加**：8 + 8 = 16 字节

### 3.3 `urma_jetty_t`（不修改）

保持现有定义，不做任何修改。

---

## 四、API 设计

### 4.1 `urma_get_jetty_bond_ext`（新增）

```c
/**
 * 获取 Jetty 的 Bond 扩展信息
 * @param[in] jetty: 指定 jetty
 * @param[out] ext: 扩展信息输出缓冲区
 * @param[in,out] ext_size: 输入缓冲区大小，返回实际大小
 * @return URMA_SUCCESS on success, URMA_EINVAL on invalid param,
 *         URMA_ENOENT if not a bond device, URMA_EOUTBOUND if buffer too small
 * @note 内部实现为 user_ctl 接口，从 bondp_comp_t 读取信息
 */
urma_status_t urma_get_jetty_bond_ext(urma_jetty_t *jetty,
                                       urma_jetty_bond_ext_t *ext,
                                       size_t *ext_size);
```

### 4.2 `urma_set_rjetty_bond_ext`（新增）

```c
/**
 * 设置远端 Jetty 的 Bond 扩展信息
 * @param[in,out] rjetty: 远端 jetty 信息
 * @param[in] ext: bond 扩展信息
 * @param[in] ext_size: 扩展信息大小
 * @return URMA_SUCCESS on success, URMA_EINVAL on invalid param
 * @note 在调用 urma_import_jetty 之前调用
 */
urma_status_t urma_set_rjetty_bond_ext(urma_rjetty_t *rjetty,
                                         const urma_jetty_bond_ext_t *ext,
                                         size_t ext_size);
```

---

## 五、内存消耗分析

### 5.1 内存增加对比

| 结构 | 原有大小 | 新增后大小 | 增加量 |
|------|---------|-----------|--------|
| `urma_rjetty_t` | ~64 字节 | ~80 字节 | **+16 字节** |
| `urma_jetty_t` | ~200 字节 | ~200 字节 | **0 字节** |
| 用户缓冲区 | 0 | ~512 字节 | **按需分配** |

### 5.2 场景分析

| 场景 | 内存增加 | 说明 |
|------|---------|------|
| 普通设备 | 0 | 不涉及 bond 扩展 |
| Bond 设备未查询 | 16 字节 | 仅 `urma_rjetty` 指针开销 |
| Bond 设备已查询 | ~528 字节 | `urma_rjetty` 指针 + 用户缓冲区 |

### 5.3 内存优化策略

1. **按需分配**：用户缓冲区仅在调用 `urma_get_jetty_bond_ext` 时分配
2. **共享内存**：`urma_rjetty.bond_ext` 指向用户缓冲区，不额外拷贝
3. **延迟释放**：用户负责管理缓冲区生命周期

---

## 六、使用示例

### 6.1 端到端交换流程

```c
// ============ Side A ============
void side_a_setup(urma_context_t *ctx)
{
    urma_jetty_cfg_t cfg = {
        .depth = 1024,
        .trans_mode = URMA_TM_RM,
    };

    // 1. 创建聚合 Jetty（接口不变）
    urma_jetty_t *local_jetty = urma_create_bond_jetty(ctx, &cfg);

    // 2. 查询扩展信息
    urma_jetty_bond_ext_t local_ext;
    size_t ext_size = sizeof(local_ext);
    urma_get_jetty_bond_ext(local_jetty, &local_ext, &ext_size);

    // 3. 发送给对端（用户负责）
    send_to_peer(&local_ext, ext_size);
}

// ============ Side B ============
void side_b_setup(urma_context_t *ctx)
{
    // 1. 创建本地聚合 Jetty
    urma_jetty_t *local_jetty = urma_create_bond_jetty(ctx, &cfg);
    
    // 2. 获取并发送本端信息
    urma_jetty_bond_ext_t local_ext;
    size_t ext_size = sizeof(local_ext);
    urma_get_jetty_bond_ext(local_jetty, &local_ext, &ext_size);
    send_to_peer(&local_ext, ext_size);

    // 3. 接收对端信息
    urma_jetty_bond_ext_t peer_ext;
    recv_from_peer(&peer_ext, sizeof(peer_ext));

    // 4. 设置到 rjetty
    urma_rjetty_t rjetty = {
        .jetty_id = peer_ext.slave_id[0],
        .trans_mode = URMA_TM_RM,
    };
    urma_set_rjetty_bond_ext(&rjetty, &peer_ext, sizeof(peer_ext));

    // 5. Import（接口不变，内部自动使用 bond_ext）
    urma_target_jetty_t *remote_tjetty = urma_import_jetty(ctx, &rjetty, NULL);
}
```

---

## 七、内部实现

### 7.1 `urma_get_jetty_bond_ext` 实现框架

```c
urma_status_t urma_get_jetty_bond_ext(urma_jetty_t *jetty,
                                       urma_jetty_bond_ext_t *ext,
                                       size_t *ext_size)
{
    if (jetty == NULL || ext == NULL || ext_size == NULL) {
        return URMA_EINVAL;
    }

    // 通过 user_ctl 获取 bond 信息
    bondp_comp_t *bdp_comp = (bondp_comp_t *)jetty;
    
    if (*ext_size < sizeof(urma_jetty_bond_ext_t)) {
        *ext_size = sizeof(urma_jetty_bond_ext_t);
        return URMA_EOUTBOUND;
    }

    // 填充扩展信息
    ext->version = URMA_JETTY_BOND_EXT_VERSION_1;
    ext->total_size = sizeof(urma_jetty_bond_ext_t);
    ext->enabled_count = bdp_comp->enabled_count;
    
    for (uint32_t i = 0; i < bdp_comp->enabled_count; i++) {
        ext->enabled_indices[i] = bdp_comp->enabled_indices[i];
        if (bdp_comp->p_jetty[i] != NULL) {
            ext->slave_id[i] = bdp_comp->p_jetty[i]->jetty_id;
        }
    }

    *ext_size = sizeof(urma_jetty_bond_ext_t);
    return URMA_SUCCESS;
}
```

### 7.2 `bondp_import_vjetty` 修改

```c
static int bondp_import_vjetty(
    urma_context_t *ctx,
    urma_rjetty_t *rjetty,
    urma_token_t *rjetty_token,
    bondp_target_jetty_t *bdp_tjetty,
    urma_bond_id_info_out_t *udata_out)
{
    // 如果 rjetty 携带了 bond_ext，直接使用
    if (rjetty->bond_ext != NULL) {
        const urma_jetty_bond_ext_t *bond_ext = rjetty->bond_ext;

        // 填充 udata_out
        udata_out->enabled_count = bond_ext->enabled_count;
        for (uint32_t i = 0; i < bond_ext->enabled_count; i++) {
            udata_out->slave_id[i] = bond_ext->slave_id[i];
            udata_out->enabled_indices[i] = bond_ext->enabled_indices[i];
        }

        // 跳过内核 ubagg 交换
        return 0;
    }

    // 原有逻辑：走内核 ubagg 交换
    return bondp_import_vjetty_via_kernel(ctx, rjetty, rjetty_token, bdp_tjetty, udata_out);
}
```

---

## 八、文件修改清单

| 文件路径 | 修改内容 |
|----------|---------|
| `src/urma/lib/urma/core/include/urma_types.h` | 新增 `urma_jetty_bond_ext_t` 定义 |
| `src/urma/lib/urma/core/include/urma_types.h` | `urma_rjetty_t` 新增 `bond_ext`、`bond_ext_size` 字段 |
| `src/urma/lib/urma/core/include/urma_api.h` | 新增 `urma_get_jetty_bond_ext`、`urma_set_rjetty_bond_ext` 声明 |
| `src/urma/lib/urma/core/urma_cp_api.c` | 新增 `urma_get_jetty_bond_ext` 实现 |
| `src/urma/lib/urma/core/urma_cp_api.c` | 新增 `urma_set_rjetty_bond_ext` 实现 |
| `src/urma/lib/urma/bond/bondp_api.c` | 修改 `bondp_import_vjetty`，支持从 `rjetty->bond_ext` 读取 |

---

## 九、兼容性考虑

### 9.1 二进制兼容性

| 修改项 | 兼容性 | 说明 |
|--------|--------|------|
| `urma_rjetty_t` 新增字段 | 向后兼容 | 新增字段在结构末尾 |
| `urma_jetty_t` | 完全兼容 | 不做任何修改 |
| API 修改 | 向后兼容 | 仅新增 API，不修改现有接口 |

### 9.2 协议版本控制

```c
#define URMA_JETTY_BOND_EXT_VERSION_1 0x0100  // Major.Minor
```

版本字段用于跨版本协商，支持前向兼容。

---

## 十、总结

| 特性 | 说明 |
|------|------|
| **内存开销** | 最小化，仅 `urma_rjetty` 增加 16 字节指针 |
| **API 改动** | 仅新增 2 个查询/设置接口，核心接口不变 |
| **`urma_jetty_t`** | **不修改**，保持原样 |
| **向后兼容** | 完全兼容现有代码 |
| **用户态交换** | 去除内核 ubagg 模块依赖 |