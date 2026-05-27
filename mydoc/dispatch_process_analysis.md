# Dispatch 分发过程详解

## 概述

Dispatch 是 MoE（Mixture of Experts）的第一步，负责将每个 token 按照其 `expert_ids` 路由到对应专家所在的 rank，并完成 per-expert 的 smooth quantization。Dispatch 运行在 **AIV 核心**上（`ASCEND_IS_AIV`），所有 AIV 核心协同工作，通过 HCCL window 实现跨 rank 数据交换。

**入口**: [CamMoeDistributeDispatch::Process()](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L1031-L1047)

---

## 一、整体流程（6 步）

```
Process() [仅 AIV 核心]:
  ┌─ 0. QuantForShareExpert()        ← 仅当 SHARED_EXPERT：对共享专家做 smooth quant
  ├─ 1. AlltoAllDispatch()           ← 计数 + 发送 token 到目标 rank window
  ├─ 2. SetStatus()                  ← 将 per-expert token 计数写入状态 window
  ├─ 3. WaitDispatch()               ← 忙等所有 rank 完成 SetStatus
  ├─ 4. LocalWindowCopy()            ← 从本地 window 读出收到的 token
  ├─ 5. [AllGatherSetStatusAndWait()  ← 仅当 TP：跨 TP 通信
  │      AllgatherProcessOut()]
  └─ 6. UpdataTokenNumsOut()         ← 写回各 expert 的 token 数量
```

---

## 二、关键数据结构

### 2.1 HCCL Window（跨 rank 通信窗口）

每个 rank 有两块 window 区域：

| Window | 用途 | 大小 |
|--------|------|------|
| **Data Window** (`windowsIn`) | 存放其他 rank 发来的 token 数据 | `moeSendNum_ * expertPerSizeOnWin_` |
| **Status Window** (`windowsExp`) | 存放其他 rank 发来的 per-expert 状态字 | `STATE_SIZE` (1MB) |

双缓冲设计：通过 `dataState_`（0 或 1）实现 ping-pong，交替使用两块 buffer 避免上一轮数据未消费就被覆盖。

```cpp
winDataSizeOffset_ = dataState_ * epWorldSize_ * expertPerSizeOnWin_ * moeExpertNumPerRank_;
// dataState_ 在每次 kernel 调用时翻转，实现双缓冲
```

### 2.2 Window 地址计算

每个 rank 的 window 通过 HCCL 提供的 remote 指针访问：

```cpp
GetWindAddrByRankId(ctxIdx, rankId):
  if curRankId == rankId:
    return localWindowsIn + winDataSizeOffset_ + rankId * OPT_RANK_OFFSET
  else:
    return remoteRes[rankId].nextDevicePtr->windowsIn + winDataSizeOffset_ + rankId * OPT_RANK_OFFSET
```

每个专家在 data window 中有一个固定大小的 slot：
```cpp
expertPerSizeOnWin_ = axisMaxBs_ * axisH_ * sizeof(XType)
// = 每 rank 最大 batch * hidden_size * dtype_size
```

Token 在 slot 内的偏移 = `expert_id * expertPerSizeOnWin_ + token_offset * hCommuSize_`。

### 2.3 Status Window 布局

每个 (src_rank, expert_id) 组合占用 `stateOffset_` 字节：
- `stateOffset_ = (recvWinBlockNum_ > 512) ? 256 : 512`
- `recvWinBlockNum_ = epWorldSize_ * moeExpertNumPerRank_`
- Status 值（float32）表示"本 rank 已发送完毕"的标志，被 `WaitDispatch` 轮询

### 2.4 核心分工

所有 AIV 核心均匀分配工作量：

```cpp
moeUsedAivNum_ = aivNum_ - sharedUsedAivNum_;  // 减去共享专家专用的核心

// 以 moeExpertNum_ 为例：
sendExpertNum_   = moeExpertNum_ / moeUsedAivNum_;
remainderRankNum = moeExpertNum_ % moeUsedAivNum_;
// 前 remainderRankNum 个核心多处理 1 个 expert
```

---

## 三、步骤详解

### 步骤 1：AlltoAllDispatch — 计数并发送 token

**位置**: [cam_moe_distribute_dispatch.h:583-610](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L583-L610)

```
AlltoAllDispatch():
  ① [可选] TokenActiveMaskCal():
     统计 x_active_mask 中有效 token 数
     activeMaskBsCnt_ = mask 中值为 true 的 token 数
     sendToMoeExpTokenCnt_ = activeMaskBsCnt_ * topK

  ② 从 GM 加载 expert_ids:
     DataCopyPad(expertIdsTensor_, expertIdsGMTensor_, ...)
     // expertIds [bs, topK] → expertIdsCnt = bs * topK 个 expert_id

  ③ 计数（两种模式）:
     [AIV优化模式 enableAivOpt_]: CountAndPrepareStatusWithAivOpt()
     [普通模式]: 逐 token 遍历，statusTensor_[expertId]++

  ④ SendToMoeExpert():
     将每个 token 量化后发送到目标 rank 的 data window
```

### 步骤 1a（AIV 优化模式）：CountAndPrepareStatusWithAivOpt

**位置**: [cam_moe_distribute_dispatch.h:512-580](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L512-L580)

优化条件：`bs <= 64 && moeExpertNum <= 256 && topK % 8 == 0`

使用二维表格 `table[bs+1][moeExpertNum]` 加速计数：

```
阶段 A — 标记（各行并行）:
  table[0][*] = 0  (第 0 行全零)
  for each token (row = tokenIndex/topK, col = expertId):
    table[row+1][col] = 1  // 标记此行此 expert 有一个 token

阶段 B — 前缀和（跨行累加）:
  for row = 1 to bs:
    table[row][*] += table[row-1][*]
  // 此时 table[row][expertId] = 前 row 行中发往 expertId 的 token 总数

阶段 C — 填 expertCount:
  FillExpertCountByRowRange():
    for each token:
      expertCount[tokenIndex] = table[row][expertId]
    // 即此 token 在对应 expert slot 内的偏移序号

阶段 D — 写 status:
  for each expert:
    statusTensor_[expertId * 8 + 1] = table[bs][expertId]  // 该 expert 收到的总 token 数
```

**核心思想**：通过二维前缀和，在 O(bs * moeExpertNum) 时间内算出每个 token 在其目标 expert slot 中的写入偏移量。

### 步骤 1b（普通模式）：逐 token 原子计数

```cpp
for each token:
  expertId = expertIdsTensor_[tokenIndex]
  expertCountTensor_[tokenIndex] = statusTensor_[expertId * 8 + 1]  // 当前计数值作为此 token 的偏移
  statusTensor_[expertId * 8 + 1]++                                  // 计数 +1
```

这是一个软件实现的"原子加"：先读后写，读到的值就是此 token 的序号。

### 步骤 1c：SendToMoeExpert — 写入远端 window

**位置**: [cam_moe_distribute_dispatch.h:441-495](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L441-L495)

```
每个 AIV 核心处理 [startTokenId, endTokenId) 范围的 token:
  for each token:
    ① 获取目标 expert_id:
       dstExpertId = expertIdsTensor_(tokenIndex)
       if dstExpertId < 0: skip  // pad token

    ② 计算目标 rank 和 window 地址:
       dstRankId = dstExpertId / moeExpertNumPerRank_
       expertLocalId = dstExpertId % moeExpertNumPerRank_
       offset = srcRank * expertPerSizeOnWin_ * moeExpertNumPerRank_
              + expertLocalId * expertPerSizeOnWin_
              + tokenOffset * hCommuSize_
       // tokenOffset = expertCountTensor_(tokenIndex)，即此 token 在目标 expert slot 内的序号

    ③ [量化模式] 从 GM 读原始 token，走 QuantProcess:
       xInTensor_ = xGMTensor_[tokenRow * hidden]   // 读原始 fp16/bf16 token
       QuantProcess(expertIndex=dstExpertId, scalesGMTensor_):
         Cast(fp16→fp32) → Mul(smoothScale[expertIndex]) → Dynamic Quant → fp16→int8
       xOutTensor_ = int8 token + scale
       DataCopy(dstWinGM, xOutTensor_, hCommuSize_)  // 写入远端 window

    ④ [非量化模式] 直接拷贝:
       DataCopy(dstWinGM, xGMTensor_[...], hCommuSize_)

    ⑤ 最后一个核心写 expandIdx:
       DataCopy(expandIdxOutGM, expertCountTensor_, expertIdsCnt)
       // 保存每个 token 在其 expert slot 内的偏移，供 combine 阶段还原顺序
```

**关键**：这里不需要显式的 `send`/`recv` 原语。数据直接通过 HCCL 提供的 remote GM 指针写入目标 rank 的 data window 内存。这本质上是一个 **RDMA write** 操作。

### 步骤 2：SetStatus — 写入状态通知

**位置**: [cam_moe_distribute_dispatch.h:613-645](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L613-L645)

```
SetStatus():
  SyncAll<true>()  // 等所有核心完成 SendToMoeExpert

  每个 AIV 核心处理 [startExpertId_, endExpertId_) 范围的 expert:
    for each expert:
      ① 确定目标 rank 和 status window offset:
         dstRankId = expertIndex / moeExpertNumPerRank_
         offset = (epRankId_ + expertIndex % moeExpertNumPerRank_ * epWorldSize_) * stateOffset_

      ② 写入 status word（float32 = 1.0f）:
         dstStatusAddr = remote rank 的 localWindowsExp + offset
         DataCopy(dstStatusAddr, statusTensor_[expertIndex * 8], 8 bytes)
         // 写入 float32 值 1.0，表示 "本 rank 向此 expert 的数据已发送完毕"
```

每个 rank 向每个 expert 对应的 status slot 写一个标志。status 地址编码了 `(src_rank, expert_id)`：
- `offset = (src_rank + expert_local_id * epWorldSize) * stateOffset_`
- 当 `moeExpertNumPerRank_ == 1` 时简化为 `offset = stateOffset_ * epRankId_`

### 步骤 3：WaitDispatch — 忙等所有 rank 就绪

**位置**: [cam_moe_distribute_dispatch.h:692-734](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L692-L734)

```
WaitDispatch():
  每个核心负责 [startStatusIndex, startStatusIndex + recStatusNumPerCore) 范围的 status slot

  轮询等待:
    while true:
      ① DataCopy 读出对应范围的 float32 status 值
      ② GatherMask + Sum: 统计值为 1.0 的 status 数量
      ③ if sum ≈ recStatusNumPerCore * sumTarget_: break
         else: SPIN_WAIT_CYCLES()  // 硬件忙等

  sumTarget_ = 0.0 或 1.0（交替翻转），用于区分当前轮和上一轮的数据
```

**工作原理**：每个 rank 需要收到所有源 rank 发来的 status 标志，确认对应 expert 的数据已经全部写入 window。当所有 N 个 status slot 都为 1.0 时，收集完成。

`GatherMask` 指令是一个硬件原语，从多个不连续地址收集数据并按 mask 筛选，比逐个读取效率高。

### 步骤 4：LocalWindowCopy — 从 window 读出并整理

**位置**: [cam_moe_distribute_dispatch.h:789-868](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L789-L868)

```
LocalWindowCopy():
  ① GetCumSum(statusTensor_, outCountLocal):
     从 status window 读回所有 status 值（此时已包含所有源 rank 的数据）
     对 recvWinBlockNum_ 个 status 做前缀和，算出每个 expert 的 token 起始偏移
     totalCnt_ = 全部接收的 token 总数

  ② 对每个 expert（当前核心负责的范围）:
     count = statusTensor_[expertIdx * 8 + 1]  // 此 expert 收到的 token 数
     beginIdx = prefixSum[expertIdx] - count    // 这些 token 在输出中的起始位置

     计算 window 地址:
       winOffset: 将 (rank, expert) 展开为线性索引
         当 moeExpertNumPerRank_ > 1:
           winOffset = rank % epWorldSize * moeExpertNumPerRank_ + rank / epWorldSize
         wAddr = windowGM_ + winOffset * expertPerSizeOnWin_

     对每个 token:
       ① 从 window 读: DataCopy(tokGlobal[j * hCommuSize_])
       ② 解包 scale（量化模式）
       ③ 写入 expandXOut: DataCopy(expandXOutGM_ + (beginIdx + j) * h, token_data)
       ④ [TP 模式] 同时拷贝到 TP gather window

     ④ 写 sendCountsOut: 记录每个 expert 收到的累计 token 数
```

**数据整理**：所有 rank 发来的 token 在 data window 中按 (src_rank, expert) 组织。`LocalWindowCopy` 遍历每个 expert slot，将 token 按序拷贝到连续的输出 buffer `expandXOut` 中，供后续 GMM1 使用。

`expandIdxOut`（步骤 1 写的）记录了每个 token 在 expert slot 中的序号，供后续 Combine 阶段恢复原始顺序。

### 步骤 5（TP 模式）：跨 TP 通信

仅当 `IsNeedAllgater == true`（即存在 tensor parallel）时执行：

```
AllGatherSetStatusAndWait():
  各 TP rank 将自己的 gatherCount 写入对端 status window
  等待对端 status = 1.0

AllgatherProcessOut():
  从对端 window 读出 token，追加到 expandXOut 尾部
  更新 sendCountsOut
```

### 步骤 6：UpdataTokenNumsOut — 写回 expert token 数量

**位置**: [cam_moe_distribute_dispatch.h:953-1028](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L953-L1028)

```
UpdataTokenNumsOut():
  [多 expert per rank]:
    对每个 local expert:
      tokenSums = sendCounts[expertLocalId * epWorldSize + epWorldSize - 1] + gatherCount
      expertTokenNumsOut[expertLocalId] = tokenSums  // 前缀和形式
      expertTokenNums[expertLocalId] = tokenSums      // 各自数量

  [单 expert per rank]:
    expertTokenNumsOut[0] = totalCnt_ + gatherCount
```

`expertTokenNums` 用作后续 GMM1/GMM2 的 group list，控制每个 expert group 处理多少 token。

---

## 四、量化处理：QuantProcess

**位置**: [cam_moe_distribute_dispatch.h:648-689](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe/raw_distributed/cam_moe_distribute_dispatch.h#L648-L689)

每个 token 在发送前经过量化处理：

```
QuantProcess(expertIndex, scalesGMTensor):
  step 1: Cast(fp16/bf16 → float32)
          floatLocalTemp = Cast(xInTensor_)
  
  step 2: [可选] Smooth Quant
          smoothScale = scalesGMTensor[expertIndex * hidden]  // per-expert scale
          floatLocalTemp *= smoothScale  // element-wise mul

  step 3: Dynamic Quant
          rowMax = ReduceMax(abs(floatLocalTemp))
          dynamicScale = 127.0 / rowMax
          floatLocalTemp *= dynamicScale

  step 4: Cast + Round
          int32Temp = Cast(floatLocalTemp, RINT)   // round to nearest int
          halfTemp  = Cast(int32Temp, ROUND)        // int32 → fp16
          int8Out   = Cast(halfTemp, TRUNC)         // fp16 → int8

  step 5: 存储 dequant scale
          int8Out[hidden] = 1.0 / dynamicScale      // 附在 int8 token 数据后面
```

输出格式：`[int8_token (hidden bytes)] [dequant_scale (4 bytes)]`，共 `hCommuSize_ = hidden * sizeof(int8) + 128(pad)` 字节。

---

## 五、双缓冲（Ping-Pong）机制

Dispatch 使用双缓冲来避免数据竞争：

```
第一次调用 kernel: dataState_ = 0 → 使用 window 前半段
  SetStatus 写入 sumTarget_ = 1.0
  WaitDispatch 等待 sumTarget_ = 1.0

第二次调用 kernel: dataState_ = 1 → 使用 window 后半段  
  SetStatus 写入 sumTarget_ = 0.0
  WaitDispatch 等待 sumTarget_ = 0.0
```

`Process()` 入口处（第 245-250 行）翻转 dataState：

```cpp
dataState_ = selfDataStatusTensor(aivId_ * UB_ALIGN);  // 读到上一轮的值
selfDataStatusTensor(aivId_ * UB_ALIGN) = (dataState_ == 0);  // 翻转
```

---

## 六、完整数据流图

```
每个 rank 的输入:
  x [bs, hidden]           → token 数据
  expert_ids [bs, topK]    → 每个 token 的路由目标
  smooth_scales [expert_num, hidden] → per-expert smooth quant scale

═══════════════════════════════════════════════════════════════════

Step 1: AlltoAllDispatch()

  expert_ids: [token0→expert5, token1→expert2, token2→expert5, ...]
              │                │                │
              ▼                ▼                ▼
        计数+量化          计数+量化          计数+量化
              │                │                │
              ▼                ▼                ▼
     ┌──────────────────────────────────────────────────┐
     │          HCCL Data Window (远端 GM)               │
     │                                                  │
     │  Rank0: [expert_slot_0] [expert_slot_1] ...     │
     │  Rank1: [expert_slot_0] [expert_slot_1] ...     │
     │  ...                                             │
     │                                                  │
     │  每个 slot = expertPerSizeOnWin_ bytes           │
     │  每个 token = hCommuSize_ bytes                  │
     │  (int8_token + scale_pad)                        │
     └──────────────────────────────────────────────────┘

Step 2: SetStatus()

     ┌──────────────────────────────────────────────┐
     │        HCCL Status Window                    │
     │                                              │
     │  [rank0→expert0] [rank0→expert1] ...        │
     │  [rank1→expert0] [rank1→expert1] ...        │
     │  ...                                         │
     │  每个 slot = stateOffset_ bytes              │
     │  写入 float32 1.0 表示数据已就绪              │
     └──────────────────────────────────────────────┘

Step 3: WaitDispatch()
     轮询 status window，等待所有 slot = 1.0

Step 4: LocalWindowCopy()

     ┌──────────────────────┐        ┌─────────────────────┐
     │   HCCL Data Window   │  ──►   │   expandXOut (GM)    │
     │   (本 rank 收到的     │        │                      │
     │    所有 token)       │        │ [expert0_token0]     │
     │                      │        │ [expert0_token1]     │
     │   expert0: tok0,tok1 │        │ ...                  │
     │   expert1: tok0      │        │ [expert1_token0]     │
     │   ...                │        │ ...                  │
     └──────────────────────┘        └─────────────────────┘

     同时输出:
       expandIdxOut: 每个 token 在 expert slot 中的偏移序号
       dynamicScalesOut: 每个 token 的 per-token dequant scale
       sendCountsOut: 每个 (expert, rank) 收发的 token 数量前缀和
       expertTokenNums: 每个 local expert 的 token 总数（用于 GMM group list）
```

---

## 七、与 Deep Fuse 模式的区别

在 deep fuse 模式下（`EXEC_FLAG & EXEC_FLAG_DEEP_FUSE`），dispatch 被部分融合进 GMM1 kernel：

- **非 deep fuse**：阶段一的 dispatch 独立运行在 AIV 核心上，完成后再由 AIC 核心执行 GMM1。两者通过 `CrossCoreBarrier/SetFlag/WaitFlag` 同步（[fused_deep_moe.h:484-504](src/cam/comm_operator/ascend_kernels/fused_deep_moe/op_kernel/fused_deep_moe.h#L484-L504)）
- **deep fuse**：dispatch 的 `AlltoAllDispatch`/`SetStatus`/`WaitDispatch`/`LocalWindowCopy` 仍在 AIV 上运行，但 `WaitDispatch` 之后 AIV 直接设置 flag 通知 AIC 开始 GMM1，省去 `Process()` 中的显式 barrier
