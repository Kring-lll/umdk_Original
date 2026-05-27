# FetchAndPreprocessInt8ToInt4 过程分析

> 对应源码：[grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h](../src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/gemm/kernel/grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h)

---

## 一、功能概述

将 INT8 量化权重数据转换为 INT4 格式。每个 INT8 字节拆分为**高 4 位（有符号）**和**低 4 位（无符号 → 重中心化为有符号）**，分别写出为 INT4。

---

## 二、UbBuf 分配布局（从 ubOffset 开始）

设 `hiddenSize = H`。

| 偏移量 | Buffer | 声明类型 | 物理字节 | 逻辑形状 | 双/单缓冲 |
|---|---|---|---|---|---|
| 0 | `xTensor0` / `tmpBuffer0` | `int8_t` | H | [H] int8 | 双缓冲 Ping |
| H | `xTensor1` / `tmpBuffer1` | `int8_t` | H | [H] int8 | 双缓冲 Pong |
| 2H | `xHighI4Tensor` | `int4b_t` | H/2 | [H] int4 | 单缓冲 |
| 2.5H | `xLowI4Tensor` | `int4b_t` | H/2 | [H] int4 | 单缓冲 |
| 3H | `xHighHalfTensor` | `half` | H×2 | [H] fp16 | 单缓冲 |
| 5H | `xLowHalfTensor` | `half` | H×2 | [H] fp16（被 ReinterpretCast 为 int16 使用） | 单缓冲 |
| 7H | `xLowHalfTensor2` | `half` | H×2 | [H] fp16 | 单缓冲 |
| 9H | `xLowI16Tensor` | `int16_t` | 256 | [128] int16 = 0x0F0F | 单缓冲（常量） |

> **注**：`int4b_t` 每个元素占 4 bits，H 个元素 = H/2 bytes。`xLowI16Tensor` 用 `Duplicate` 填充 128 个 `0x0F0F`（每字节低 4 位的掩码）。

---

## 三、高 4 位处理

### 数学原理

有符号 INT8 的高 4 位本身就是有符号的，乘以 `1/16 = 0.0625` 等效于**算术右移 4 位**。`CAST_FLOOR` 确保向负无穷取整。

### Step 1a：`Cast(xHighHalfTensor, xTensor, CAST_NONE, H)` — INT8 → fp16

| 项目 | 描述 |
|---|---|
| **输入** | `xTensor`: [H] int8, H bytes |
| **输出** | `xHighHalfTensor`: [H] fp16, H×2 bytes |

每个 INT8 值无损转为 fp16：

```
xTensor (int8):        [52]   [-8]   [-85]  [127]
                          ↓       ↓       ↓      ↓
xHighHalfTensor (fp16): [52.0] [-8.0] [-85.0] [127.0]
```

### Step 1b：`Muls(xHighHalfTensor, xHighHalfTensor, 1/16, H)` — ×0.0625

| 项目 | 描述 |
|---|---|
| **输入/输出** | `xHighHalfTensor`: [H] fp16, H×2 bytes（原地修改） |

```
xHighHalfTensor (fp16): [52.0]   [-8.0]  [-85.0]   [127.0]
                        ×1/16     ×1/16    ×1/16      ×1/16
                          ↓         ↓        ↓          ↓
xHighHalfTensor (fp16): [3.25]  [-0.5]  [-5.3125]  [7.9375]
```

`52/16 = 3.25`（高 nibble `0x3` = 3），`-8/16 = -0.5`（高 nibble `0xF` = -1）。

### Step 1c：`Cast(xHighI4Tensor, xHighHalfTensor, CAST_FLOOR, H)` — fp16 → INT4

| 项目 | 描述 |
|---|---|
| **输入** | `xHighHalfTensor`: [H] fp16, H×2 bytes |
| **输出** | `xHighI4Tensor`: [H] int4, H/2 bytes |

向负无穷取整后转为有符号 INT4：

```
xHighHalfTensor (fp16): [3.25]  [-0.5]  [-5.3125] [7.9375]
                        floor    floor     floor     floor
                          ↓        ↓         ↓         ↓
xHighI4Tensor (int4):   [3]     [-1]      [-6]      [7]
                        0x3      0xF       0xA       0x7
```

物理存储（H=4 时 2 bytes）：`[0xF3][0x7A]`（高 4bit 在前，低 4bit 在后）。

### Step 1d：`DataCopy(src[output], xHighI4Tensor.ReinterpretCast<int8_t>(), H/2)` — 写回 GM

| 项目 | 描述 |
|---|---|
| **拷贝量** | H/2 bytes（H 个 INT4 元素的物理字节数） |
| **写入位置** | GM 的 `src[outputOffset + 0]` 到 `src[outputOffset + H/2 - 1]` |

---

## 四、低 4 位处理

### 数学原理

INT8 的低 4 位是无符号的 [0, 15]，但 INT4 是有符号范围 [-8, 7]。处理分为三步：
1. `& 0x0F` 掩码提取无符号低 nibble
2. 转为 fp16
3. 减去 8（重新中心化：0→-8, 8→0, 15→7）

### Step 2a：`And(xLowHalfTensor as int16, xTensor as int16, xLowI16Tensor, ...)` — 掩码提取

| 项目 | 描述 |
|---|---|
| **输入A** | `xTensor` 重解释为 [H/2] int16, H bytes |
| **掩码** | `xLowI16Tensor`: [128] int16 = 0x0F0F, 256 bytes |
| **输出** | `xLowHalfTensor` 重解释为 [H/2] int16, 使用 H bytes |
| **LEN_VK** | `(H/2) / 128`（完整 128-int16 块数） |
| **LAST_LEN_VK** | `(H % 256) / 2`（尾部剩余 int16 元素数） |

按 int16 与 `0x0F0F` 做按位 AND，保留每个字节的低 4 位：

```
xTensor INT8:         [0x34] [0xF8] [0xAB] [0x7F]
重解释为 int16 (LE):  [0xF834]        [0x7FAB]
                      1111_1000_0011_0100  0111_1111_1010_1011
AND 0x0F0F:           0000_1000_0000_0100  0000_1111_0000_1011
                      [0x0804]            [0x0F0B]
每字节低4位:          0x08  0x04        0x0F  0x0B
```

### Step 2b：`Cast(xLowHalfTensor2 as half, xLowHalfTensor as int8, CAST_NONE, H)` — INT8 → fp16

| 项目 | 描述 |
|---|---|
| **输入** | `xLowHalfTensor` 重解释为 [H] int8, H bytes（掩码后的低 4 位值，范围 0~15） |
| **输出** | `xLowHalfTensor2`: [H] fp16, H×2 bytes |

```
xLowHalfTensor (int8):  [0x04] [0x08] [0x0B] [0x0F]
                            ↓       ↓       ↓       ↓
xLowHalfTensor2 (fp16):  [4.0]  [8.0]  [11.0] [15.0]
```

### Step 2c：`Adds(xHighHalfTensor, xLowHalfTensor2, -8, H)` — 加 -8（重中心化）

| 项目 | 描述 |
|---|---|
| **输入A** | `xLowHalfTensor2`: [H] fp16, H×2 bytes |
| **输入B** | 标量 `-8.0` |
| **输出** | `xHighHalfTensor`: [H] fp16, H×2 bytes（**复用**，高 4 位结果已写走） |

将无符号 [0, 15] → 有符号 [-8, 7]：

```
xLowHalfTensor2 (fp16):  [4.0]  [8.0]  [11.0] [15.0]
                         +(-8)   +(-8)   +(-8)   +(-8)
                            ↓       ↓       ↓       ↓
xHighHalfTensor (fp16):  [-4.0]  [0.0]  [3.0]   [7.0]
```

### Step 2d：`Cast(xLowI4Tensor, xHighHalfTensor, CAST_NONE, H)` — fp16 → INT4

| 项目 | 描述 |
|---|---|
| **输入** | `xHighHalfTensor` as fp16: [H] fp16, H×2 bytes |
| **输出** | `xLowI4Tensor`: [H] int4, H/2 bytes |

此步用 `CAST_NONE`（非 FLOOR），因为加 -8 后值已是整数，无需舍入。

```
xHighHalfTensor (fp16): [-4.0] [0.0] [3.0] [7.0]
                            ↓     ↓     ↓     ↓
xLowI4Tensor (int4):     [-4]   [0]   [3]   [7]
                          0xC   0x0   0x3   0x7
```

### Step 2e：`DataCopy(src[output + H/2], xLowI4Tensor as int8, H/2)` — 写回 GM

| 项目 | 描述 |
|---|---|
| **拷贝量** | H/2 bytes（H 个 INT4 元素） |
| **写入位置** | GM 的 `src[outputOffset + H/2]` 到 `src[outputOffset + H - 1]` |

---

## 五、GM 最终输出布局（每行，共 H bytes）

```
┌─────────────────────────────────┬──────────────────────────────────┐
│ 高4位 INT4 结果                 │ 低4位 INT4 结果                  │
│ H/2 bytes = H 个 int4 元素      │ H/2 bytes = H 个 int4 元素       │
│ src[output + 0 .. output+H/2-1] │ src[output+H/2 .. output+H-1]   │
└─────────────────────────────────┴──────────────────────────────────┘
```

输入 H bytes INT8 → 输出 H bytes INT4（H 个高 nibble + H 个低 nibble 各自打包）。

---

## 六、完整示例（H=4，单行）

```
输入 INT8:  [0x34] [0xF8] [0xAB] [0x7F]     ← 4 bytes GM 源数据
              52     -8     -85     127

═══════════════ 高4位处理 ═══════════════
Cast INT8→fp16:     [52.0]  [-8.0]  [-85.0]   [127.0]     xHighHalfTensor [4×fp16, 8B]
Muls ×1/16:         [3.25]  [-0.5]  [-5.3125] [7.9375]    xHighHalfTensor [4×fp16, 8B]
Cast fp16→INT4(fl): [3]     [-1]    [-6]      [7]          xHighI4Tensor  [4×int4, 2B]
写回 GM:            0x34→高=3   0xF8→高=-1   0xAB→高=-6   0x7F→高=7

═══════════════ 低4位处理 ═══════════════
And & 0x0F0F:       [0x04]  [0x08]  [0x0B]    [0x0F]       xLowHalfTensor  [4×int8有效, 8B buffer]
Cast INT8→fp16:     [4.0]   [8.0]   [11.0]    [15.0]       xLowHalfTensor2 [4×fp16, 8B]
Adds -8:            [-4.0]  [0.0]   [3.0]     [7.0]        xHighHalfTensor [4×fp16, 8B] (复用)
Cast fp16→INT4:     [-4]    [0]     [3]       [7]           xLowI4Tensor   [4×int4, 2B]
写回 GM:            0x34→低=4   0xF8→低=8   0xAB→低=11  0x7F→低=15
                        → -4       → 0        → 3         → 7  (重中心化后)

═══════════════ GM输出 (共4 bytes) ═══════════════
Byte 0-1: [0xF3][0xA7]  高4位: -1(3) -6(7)  → 打包为 INT4
Byte 2-3: [0x0C][0x73]  低4位: -4(0) 3(7)    → 打包为 INT4
```

---

## 七、Ping-Pong 事件同步机制

### 7.1 事件类型总览

每个 EVENT_ID（ID6/ID7）内部包含 6 种**独立**的事件类型：

| 事件类型 | 谁设信号 | 谁等信号 | 含义 |
|---|---|---|---|
| `MTE3_MTE2` | MTE3 | MTE2 | "MTE3 已释放输入 buffer，MTE2 可覆盖" |
| `V_MTE2` | V | MTE2 | "V 已读完输入 buffer，MTE2 可覆盖" |
| `MTE2_V` | MTE2 | V | "输入 buffer 数据就绪，V 可计算" |
| `MTE2_MTE3` | MTE2 | MTE3 | "输入 buffer 数据就绪，MTE3 可感知" |
| `V_MTE3` | V | MTE3 | "输出结果就绪，MTE3 可搬出" |
| `MTE3_V` | MTE3 | V | "MTE3 已搬完输出，V 可覆写输出 buffer" |

`SetFlag<X_Y>` 将设信号指令入队到 X 流水线；`WaitFlag<X_Y>` 将等待指令入队到 Y 流水线。

### 7.2 资源分类

| 类型 | 资源 | 保护方式 |
|---|---|---|
| 双缓冲输入 | `xTensor0`/`tmpBuffer0`, `xTensor1`/`tmpBuffer1` | EVENT_ID6/ID7 各管一个，隔轮复用 |
| 单缓冲输出 | `xHighI4Tensor`, `xLowI4Tensor` | EVENT_ID6/ID7 交叉耦合保护 |
| 单缓冲临时 | `xHighHalfTensor`, `xLowHalfTensor`, `xLowHalfTensor2` | PipeBarrier + 同流水线 V-only 保证 |

### 7.3 单缓冲交叉耦合

```
                       V→MTE3 (V写完通知MTE3搬)    MTE3→V (MTE3搬完通知V可写)
xHighI4Tensor 保护:     EVENT_ID6 的 V_MTE3        EVENT_ID7 的 MTE3_V
xLowI4Tensor  保护:     EVENT_ID7 的 V_MTE3        EVENT_ID6 的 MTE3_V
```

### 7.4 双缓冲信号分配

```
buffer0 保护:  MTE3释放→MTE2 = EVENT_ID6.MTE3_MTE2
               V释放→MTE2    = EVENT_ID6.V_MTE2

buffer1 保护:  MTE3释放→MTE2 = EVENT_ID7.MTE3_MTE2
               V释放→MTE2    = EVENT_ID7.V_MTE2
```

### 7.5 跨迭代保护 vs 迭代内保护

**跨迭代保护**：针对会被"下一轮覆写、上一轮还在用"的物理 buffer：

| 保护目标 | 事件 | 设信号 | 等信号 |
|---|---|---|---|
| `xHighI4Tensor` | `MTE3_V(EVENT_ID7)` | Iter N 低4位 MTE3 (行 1377) | Iter N+1 高4位 V (行 1369) |
| `xLowI4Tensor` | `MTE3_V(EVENT_ID6)` | Iter N 低4位 MTE3 (行 1398) | Iter N+1 低4位 V (行 1393) |
| `buffer0` | `MTE3_MTE2(EVENT_ID6)` | Iter N MTE3 (行 1361) | Iter N+2 MTE2 (行 1353) |
| `buffer0` | `V_MTE2(EVENT_ID6)` | Iter N V (行 1386) | Iter N+2 MTE2 (行 1354) |
| `buffer1` | `MTE3_MTE2(EVENT_ID7)` | Iter N+1 MTE3 (行 1361) | Iter N+3 MTE2 (行 1353) |
| `buffer1` | `V_MTE2(EVENT_ID7)` | Iter N+1 V (行 1386) | Iter N+3 MTE2 (行 1354) |

**迭代内前后依赖**：同一轮内 MTE2 → V → MTE3 流水级握手：

| 依赖 | 事件 | 跨度 |
|---|---|---|
| MTE2 搬完 → V 开始算 | `MTE2_V` | 迭代内 |
| MTE2 搬完 → MTE3 感知 | `MTE2_MTE3` | 迭代内 |
| V 算完 → MTE3 开始搬 | `V_MTE3` | 迭代内 |

### 7.6 xHighHalfTensor 复用安全性

`xHighHalfTensor` 被两个阶段使用：
- 高 4 位：Cast(1365) 写入、Muls(1367) 读写、Cast(1370) 读取
- 低 4 位：Adds(1391) 写入、Cast(1394) 读取

安全保证来自三层：
1. **同流水线**：所有读写都在 V 流水线上，不与 MTE2/MTE3 竞争
2. **PipeBarrier**：行 1368 的 `PipeBarrier<PIPE_V>()` 确保高 4 位的 V 操作全部排空后，标量处理器才继续
3. **WAR 无冲突**：高 4 位最后访问是读，低 4 位首次访问是写，中间 V 排空，不存在数据竞争

### 7.7 Ping-Pong 重叠示意

```
时间 ────────────────────────────────────────────────────────▶

Iter 0 (ID6, buffer0):
  MTE2: [CopyIN row0→buf0]           ← 冷启动，不等
  V:                     [高4]  [低4]
  MTE3:                         [写高4][写低4]

Iter 1 (ID7, buffer1):              ← ★ 与 Iter0 的 V/MTE3 重叠 ★
  MTE2: [CopyIN row1→buf1]           ← buf1 ≠ buf0，无冲突
  V:                     [等高4][高4] [等低4][低4] ← 等Iter0的MTE3
  MTE3:                                [写高4][写低4]

Iter 2 (ID6, buffer0):              ← ★ 与 Iter1 的 V/MTE3 重叠 ★
  MTE2: [CopyIN row2→buf0]           ← buf0=Iter0的buf0, Iter0早已完成
  V:                     [等高4][高4] ...
```

重叠发生在两个维度：
1. **输入维度**：Iter N+1 的 MTE2 用 buffer1，Iter N 的 V 用 buffer0，不冲突
2. **输出维度**：Iter N+1 的 V 通过 `WaitFlag<MTE3_V>` 等 Iter N 的 MTE3 释放单缓冲
