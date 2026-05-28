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

**核心问题**：代码中哪类事件是防止"跨迭代写覆盖"的，哪类只是本轮内部的流水线握手？

**答**：要跨迭代保护的核心是 `xHighI4Tensor` 和 `xLowI4Tensor` 这两个单缓冲——Iter N 的 MTE3 还在读它们往 GM 搬，Iter N+1 的 V 就不能提前覆写。其余 `MTE2_V`、`V_MTE3`、`MTE2_MTE3` 属于同一轮内 MTE2→V→MTE3 的前后依赖握手。此外双缓冲 `buffer0`/`buffer1` 的 `MTE3_MTE2` 和 `V_MTE2` 也是跨迭代的（隔轮复用），但因为隔了一轮，实际通常不卡。

---

**跨迭代保护**：针对会被"下一轮覆写、上一轮还在用"的物理 buffer。

单缓冲输出 buffer（**核心跨迭代保护**）：

| 保护目标 | 事件 | 设信号（谁、何时） | 等信号（谁、何时） |
|---|---|---|---|
| `xHighI4Tensor` | `MTE3_V(EVENT_ID7)` | Iter N 的 MTE3，排在高4位 DataCopy 之后（行 1377） | Iter N+1 的 V，在写 xHighI4Tensor 之前（行 1369） |
| `xLowI4Tensor` | `MTE3_V(EVENT_ID6)` | Iter N 的 MTE3，排在低4位 DataCopy 之后（行 1398） | Iter N+1 的 V，在写 xLowI4Tensor 之前（行 1393） |

> **为什么说这两个是核心？** 因为它们是仅有的两份物理空间，每轮都被覆写。如果 Iter N+1 的 V 不等 Iter N 的 MTE3 搬完就写，GM 输出的就是错数据。

双缓冲输入 buffer（跨迭代，但隔轮复用，实际通常不卡）：

| 保护目标 | 事件 | 设信号 | 等信号 |
|---|---|---|---|
| `buffer0` | `MTE3_MTE2(EVENT_ID6)` | Iter N MTE3（行 1361） | Iter N+2 MTE2（行 1353） |
| `buffer0` | `V_MTE2(EVENT_ID6)` | Iter N V（行 1386） | Iter N+2 MTE2（行 1354） |
| `buffer1` | `MTE3_MTE2(EVENT_ID7)` | Iter N+1 MTE3（行 1361） | Iter N+3 MTE2（行 1353） |
| `buffer1` | `V_MTE2(EVENT_ID7)` | Iter N+1 V（行 1386） | Iter N+3 MTE2（行 1354） |

---

**迭代内前后依赖**：同一轮内 MTE2 → V → MTE3 流水级之间的握手，不跨迭代：

| 依赖 | 事件 | 含义 |
|---|---|---|
| MTE2 搬完 → V 开始算 | `MTE2_V` | V 必须等 MTE2 把数据搬入 xTensor 后才能读 |
| MTE2 搬完 → MTE3 感知 | `MTE2_MTE3` | MTE3 需要知道输入 buffer 已就绪 |
| V 算完 → MTE3 开始搬 | `V_MTE3` | MTE3 必须等 V 把结果写入 xHighI4Tensor/xLowI4Tensor 后才能搬出 |

---

**直观理解**：把流水线想象成三个阶段——MTE2 搬入、V 计算、MTE3 搬出。`MTE2_V`、`V_MTE3` 是**同一轮内**前后阶段的握手信号。`MTE3_V` 才是**跨轮**的握手：上一轮的 MTE3 对下一轮的 V 说"单缓冲我已经用完，你可以写了"。

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

### 7.8 推论：如果输出 buffer 也是双份会怎样

**问**：如果 `xHighI4Tensor` / `xLowI4Tensor` 也是双份（out0/out1），两轮迭代是否可以完全并行？

**答**：可以减少 `MTE3_V` 的等待，但不会带来吞吐翻倍。

输出双份后，依赖关系从 N→N+1 变成 N→N+2：

```
单缓冲（当前）:
  out: Iter N → Iter N+1   ← 相邻迭代抢同一块空间，N+1 等 N 的 MTE3

双缓冲（假设）:
  out0: Iter 0 → Iter 2    ← 隔轮复用，跟输入 buffer 一样的模式
  out1: Iter 1 → Iter 3
```

1369/1377/1393/1398 这四行跨迭代 `MTE3_V` 可以直接消失，流水线更干净。但真正的瓶颈是**硬件流水线本身**：

| 资源 | 共享 | 限制 |
|---|---|---|
| V 流水线 | 共享 | 只有一个 Vector ALU，两轮的 V 指令本质上串行 |
| MTE3 流水线 | 共享 | 只有一个 MTE3 引擎，两轮的 DataCopy 排队执行 |
| UB 空间 | 共享 | 多一份输出 buffer = 多 H bytes |

即使 buffer 全双份，实际流水线：

```
Iter 0:  MTE2[CopyIN→buf0]  V[高4][低4]  MTE3[写out0]
Iter 1:     MTE2[CopyIN→buf1]  V[ 高4 ][ 低4 ]  MTE3[ 写out1 ]
                                  ↑              ↑
                            V串行，等Iter0 V算完   MTE3串行，等Iter0 MTE3搬完
```

V 还是得等 V，MTE3 还是得等 MTE3。硬件才是真正的串行点。

当前设计用**空间换时间**：省下 H bytes UB，而 `MTE3_V` 等待的大部分开销被 V 自身的计算延迟掩盖，实际损失很小。交叉耦合设计本质上是用两个 EVENT_ID 模拟了两份输出 buffer 的信号，只不过高 4 位和低 4 位分的是"事件通道"而非物理空间。

---

### 7.9 为什么必须用两个 EVENT_ID

**问**：是否可以全都用 `EVENT_ID`，不显式区分 EVENT_ID6/EVENT_ID7？

**答**：不能。每个 EVENT_ID 内部每种事件类型（如 `MTE3_V`）只有一个状态位，跨迭代使用时会被错误消费，导致死锁。

**反证推演**（假设只用 EVENT_ID）：

```
Iter 0 的 MTE3:
  1377: SetFlag<MTE3_V>(EVENT_ID)   → 标志 = 1  (释放 xHighI4Tensor)
  1398: SetFlag<MTE3_V>(EVENT_ID)   → 标志 = 1  (释放 xLowI4Tensor, 是同一个位!)

Iter 1 的 V:
  1369: WaitFlag<MTE3_V>(EVENT_ID)  → 消费, 标志 = 0  ✓ 安全写 xHighI4Tensor
  1393: WaitFlag<MTE3_V>(EVENT_ID)  → 标志 = 0, 等不到 → 死锁!
         ↑ 这里需要的"xLowI4Tensor 已释放"信号在 Iter 0 行 1398 设过，
         但被行 1369 先消费掉了
```

**死锁原因**：跨迭代场景中，两个消费者（高 4 位 V 和低 4 位 V）不是同时到达 Wait，前一个消费掉标志后，后一个永远等不到——而下一次 SetFlag 来自 Iter 1 自己的 MTE3，V 还卡在 MTE3 之前。

**两个 EVENT_ID 如何解决**：给高 4 位和低 4 位各分配独立的 `MTE3_V` 通道：

```
高4位 MTE3→V:  EVENT_ID7.MTE3_V  → Iter N MTE3(行1377)设, Iter N+1 V(行1369)等
低4位 MTE3→V:  EVENT_ID6.MTE3_V  → Iter N MTE3(行1398)设, Iter N+1 V(行1393)等
```

两条通道的标志位互相独立，不会交叉消费。

同样，输入双缓冲的 `MTE3_MTE2` 和 `V_MTE2` 也需要两个 ID——buffer0 和 buffer1 的生命周期独立，必须用不同的标志位。

**核心判断标准**：

```
能共用同一个 EVENT_ID:  Set 和 Wait 在同一迭代内成对闭环，Wait 消费后标志归 0
必须用不同 EVENT_ID:    Set 在一轮迭代，Wait 在下一轮迭代，中间有另一个 Set/Wait 会污染
```

---

### 7.10 V_MTE3 两对是否可以互换 EVENT_ID

**问**：1371/1372 成对（高 4 位 V→MTE3），1395/1396 成对（低 4 位 V→MTE3），使用的 EVENT_ID 是否可以互换？

```
当前:  1371 SetFlag<V_MTE3>(EVENT_ID6)    1395 SetFlag<V_MTE3>(EVENT_ID7)
       1372 WaitFlag<V_MTE3>(EVENT_ID6)    1396 WaitFlag<V_MTE3>(EVENT_ID7)
互换:  1371 SetFlag<V_MTE3>(EVENT_ID7)    1395 SetFlag<V_MTE3>(EVENT_ID6)
       1372 WaitFlag<V_MTE3>(EVENT_ID7)    1396 WaitFlag<V_MTE3>(EVENT_ID6)
```

**答**：可以互换，功能仍然正确。

V_MTE3 是**迭代内握手**（V 算完 → 通知 MTE3 搬出），不跨迭代。同一 EVENT_ID 内的 `V_MTE3` 和 `MTE3_V` 是两种不同的事件类型，各有独立的标志位，互不干扰。

互换后，每个 EVENT_ID "专管"一个 buffer 的完整生命周期：

```
互换后:
  EVENT_ID6:  MTE3_V(低4位 MTE3→V)  +  V_MTE3(低4位 V→MTE3)  ← 全管低4位
  EVENT_ID7:  MTE3_V(高4位 MTE3→V)  +  V_MTE3(高4位 V→MTE3)  ← 全管高4位
```

以 EVENT_ID7 专管高 4 位为例，时序分析：

```
Iter N:
  V:   1369 WaitFlag<MTE3_V>(ID7)     ← 等Iter N-1 MTE3释放
       1370 Cast→xHighI4Tensor        ← 写
       1371 SetFlag<V_MTE3>(ID7)      ← "V写完了"
  MTE3:1372 WaitFlag<V_MTE3>(ID7)     ← 等V写完 → 消费V_MTE3标志
       1373 DataCopy(xHighI4Tensor)    ← MTE3读
       1377 SetFlag<MTE3_V>(ID7)      ← "MTE3读完了，释放"

Iter N+1:
  V:   1369 WaitFlag<MTE3_V>(ID7)     ← 等Iter N MTE3释放 → 消费MTE3_V标志
```

序列严格交替：V_MTE3 消费 → MTE3_V 设置 →（下一轮）MTE3_V 消费 → V_MTE3 设置 → ...，没有冲突窗口。每条 V_MTE3 的 Set/Wait 都在同一个迭代内闭环，不存在跨迭代残留。

原本用交叉耦合可能只是演化路径自然形成，而非设计约束。各管各的（互换后）反而更直观——读代码时一眼就能看出 ID7 负责高 4 位的完整生命周期。

---

### 7.11 V_MTE3 两对使用同一个 EVENT_ID 是否正确

**问**：不互换，V_MTE3 两对都用同一个 EVENT_ID（如都用 EVENT_ID6），是否正确？

**答**：可以，完全正确。

V_MTE3 是迭代内握手，Set 和 Wait 在同一 Iter 内成对闭环：

```
同一 Iter N 内:
  V:    1371 SetFlag<V_MTE3>(EVENT_ID)   → 标志 = 1
  MTE3: 1372 WaitFlag<V_MTE3>(EVENT_ID)  → 消费, 标志 = 0  ← 消费掉了
         ...
  V:    1395 SetFlag<V_MTE3>(EVENT_ID)   → 标志 = 1
  MTE3: 1396 WaitFlag<V_MTE3>(EVENT_ID)  → 消费, 标志 = 0  ← 再消费掉
```

1372 消费掉 1371 的标志后标志归 0，然后 1395 再 Set，1396 再消费。两对 Set-Wait 严格交替，不存在残留和错误消费。

**对比 MTE3_V**：MTE3_V 是跨迭代的（Iter N 的 MTE3 Set，Iter N+1 的 V Wait），中间隔了高 4 位 V 计算和低 4 位 V 计算，如果用同一个 ID 会死锁（见 7.9 节）。

**结论**：判断能否共用 EVENT_ID 的唯一标准——Set 和 Wait 是否在同一迭代内成对闭环：

```
能共用:   迭代内握手 (V_MTE3, MTE2_V, MTE2_MTE3)
不能共用: 跨迭代保护 (MTE3_V, MTE3_MTE2, V_MTE2)
```
