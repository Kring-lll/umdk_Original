# UMDK 核心数据结构索引（框架特异，由 umdk-skill-updater 维护）

> 采集自 `umdk/` 下 [urma_types.h](umdk/src/urma/lib/urma/core/include/urma_types.h) 等头文件的行内注释。
> 字段含义以头文件注释为准；本文件摘录高频结构体。完整定义读头文件。

## URMA 核心结构体（urma_types.h）

### 上下文与设备

| 结构体 | 行号 | 用途 | 关键字段 |
|--------|------|------|----------|
| `urma_context_t` | [L390](umdk/src/urma/lib/urma/core/include/urma_types.h#L390) | 设备上下文，后续操作都基于它 | `dev`(设备)、`ops`(操作集)、`dev_fd`(sysfs fd)、`async_fd`(异步事件 fd)、`eid`、`uasid`(进程标识)、`ref`(引用计数) |
| `urma_device_t` | [L365](umdk/src/urma/lib/urma/core/include/urma_types.h#L365) | URMA 设备 | — |
| `urma_device_attr_t` | [L303](umdk/src/urma/lib/urma/core/include/urma_types.h#L303) | 设备属性（只读规格/可写配置/端口状态） | — |
| `urma_device_cap_t` | [L254](umdk/src/urma/lib/urma/core/include/urma_types.h#L254) | 设备能力 | — |
| `urma_eid_t` | [L147](umdk/src/urma/lib/urma/core/include/urma_types.h#L147) | 16 字节端点 ID | — |
| `urma_eid_info_t` | [L403](umdk/src/urma/lib/urma/core/include/urma_types.h#L403) | eid 信息 | — |
| `urma_init_attr_t` | [L85](umdk/src/urma/lib/urma/core/include/urma_types.h#L85) | `urma_init` 配置 | — |

### Jetty 家族

| 结构体 | 行号 | 用途 |
|--------|------|------|
| `urma_jfc_t` | [L488](umdk/src/urma/lib/urma/core/include/urma_types.h#L488) | 完成队列对象 |
| `urma_jfc_cfg_t` / `urma_jfc_attr_t` | [L430](umdk/src/urma/lib/urma/core/include/urma_types.h#L430) / [L444](umdk/src/urma/lib/urma/core/include/urma_types.h#L444) | jfc 配置/属性 |
| `urma_jfce_t` / `urma_jfce_cfg_t` | [L413](umdk/src/urma/lib/urma/core/include/urma_types.h#L413) / [L408](umdk/src/urma/lib/urma/core/include/urma_types.h#L408) | 完成事件通道（中断模式） |
| `urma_jetty_id_t` | [L450](umdk/src/urma/lib/urma/core/include/urma_types.h#L450) | jetty 标识（jfs_id_t/jfr_id_t/jfc_id_t 同此） |
| `urma_jfs_flag_t` | [L525](umdk/src/urma/lib/urma/core/include/urma_types.h#L525) | jfs 标志（含 fence 保序等） |
| `urma_order_type_t` | [L506](umdk/src/urma/lib/urma/core/include/urma_types.h#L506) | 保序类型（OT/OI/OL/NO） |

> `urma_jetty_t` / `urma_jfs_t` / `urma_jfr_t` 为 opaque 句柄，定义在 types.h 后段，字段多为 `[Private]`。

### Segment 与内存

| 结构体 | 用途 | 关键字段 |
|--------|------|----------|
| `urma_seg_t` | segment 描述（注册/导入基础对象） | `ubva`(UB 虚拟地址)、`len`、`attr`(权限/token 策略/缓存性)、`token_id` |
| `urma_target_seg_t` | register/import 后的本地代理对象 | `seg`、`mva`(导入映射地址)、`urma_ctx`、`token_id`(注册时有效，导入时 NULL) |
| `urma_ubva_t` | UB 虚拟地址（EID + VA） | — |
| `urma_token_t` / `urma_token_id_t` | 访问令牌 / 令牌 id | — |

> `urma_seg_t` / `urma_target_seg_t` 字段说明详见 `doc/ch/urma/URMA User Guide.ch.md` §5.2.3 代码示例。

### 传输层（tp）

| 结构体/枚举 | 行号 | 用途 |
|-------------|------|------|
| `urma_transport_type_t` | [L330](umdk/src/urma/lib/urma/core/include/urma_types.h#L330) | 传输类型（UB/IB/IP） |
| `urma_transport_mode_t` | [L336](umdk/src/urma/lib/urma/core/include/urma_types.h#L336) | 传输模式：RM/RC/UM |
| `urma_tp_type_cap_t` | [L243](umdk/src/urma/lib/urma/core/include/urma_types.h#L243) | tp 类型能力 |
| `urma_tp_feature_t` | [L252](umdk/src/urma/lib/urma/core/include/urma_types.h#L252) | tp 特性 |
| `urma_cc_entry_t` | [L365](umdk/src/urma/lib/urma/core/include/urma_types.h#L365) | 拥塞控制表项 |

## 关键枚举/宏

| 名称 | 含义 | 来源 |
|------|------|------|
| `urma_jfs_flag_t` | 含 `fence`(保序)、inline、imm 等标志 | types.h §525 |
| `urma_order_type_t` | OT(target order)/OI(init order)/OL(layer order)/NO(no order) | types.h §506 |
| 异常事件枚举 | `URMA_EVENT_JFC_ERR`/`JFS_ERR`/`JFR_ERR`/`JETTY_ERR`/`PORT_ACTIVE`/`PORT_DOWN`/`DEV_FATAL`/`EID_CHANGE`/`ELR_ERR` 等 | 见 `doc/ch/urma/URMA User Guide.ch.md` §5.2.4 |
| 传输模式 | `URMA_TM_RC`(可靠连接,保序) / `URMA_TM_RM`(可靠消息,多对多) / `URMA_TM_UM`(不可靠) | types.h §336 |

## 待补充

以下结构体索引在用户反复询问后再由 `umdk-skill-updater` 采集补全，避免一次灌入低频内容：

- URPC：`urpc_*` 类型（见 [urpc_framework_types.h](umdk/src/urpc/include/framework/urpc_framework_types.h)）
- UMQ：`umq_*` 类型（见 [umq_types.h](umdk/src/urpc/include/umq/umq_types.h)）
- DLock：锁对象结构（见 [dlock_types.h](umdk/src/ulock/dlock/lib/include/dlock_types.h)，原子锁 64bit / 公平锁 128bit / 读写锁 64bit）
