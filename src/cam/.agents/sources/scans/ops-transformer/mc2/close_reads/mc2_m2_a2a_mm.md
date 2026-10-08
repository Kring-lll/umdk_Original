# Close-read: M2 dense AllToAll ↔ Matmul（fp16/bf16）

Not a Case. Pin **one live stack**, two orders. Quant / MX / KC / arch22 不在本文。

```yaml
commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
hw: ascend950
canonical_op: allto_all_matmul
canonical_pipeline: AlltoAllMatmulPipeLine
canonical_comm: HcclCommunicator AICSync=false SendCnt=0 RecvCnt=1
canonical_primitive: AlltoAll<false> + Commit/Wait
sibling_op: matmul_allto_all          # 同栈反序
not_this_file: KC/MX quant pipelines; ProcessKC; arch22
```

入口：

- `mc2/allto_all_matmul/op_kernel/arch35/allto_all_matmul_apt.cpp`
- `mc2/matmul_allto_all/op_kernel/arch35/matmul_allto_all_apt.cpp`

实现：`allto_all_matmul_pipeline.h` / `matmul_allto_all_pipeline.h`  
包装：`allto_all_matmul_arch35.h` / `matmul_allto_all_arch35.h`  
栈：`mc2/common/op_kernel/mc2_templates/`  
Cube：`mc2_fp_matmul.h` → `Mc2MatmulAswKernel`（**compute baseline = 43**，不重教）  
AIV permute：`mc2_vec_transpose.h`  
Comm：`hccl_communicator.h` → `MC2AlltoAllPrimitives`

Host：`allto_all_fp_matmul_tiling_base.cpp` / `fp_matmul_allto_all_tiling_base.cpp`

---

## 1. Exact 950 live variant

`KERNEL_TYPE_MIX_AIC_1_2`。`--cce-auto-sync=off` 是编译条件，不是 sync 原语。

fp16/bf16 分派（两 op 相同口径）：

```text
ORIG_DTYPE_X1 == ORIG_DTYPE_X2 == fp16|bf16
QUANTMODE == NON_QUANT_MODE
COMMTYPE/COMMMODE ∈ {CCU, AICPU}
→ AlltoAllMatmulPipeLine / MatmulAlltoAllPipeLine ProcessDefault
```

Host `comm_mode` 默认 **`ai_cpu`**；`"ccu"` → CCU。**没有** RS 那种 4P/8P 改 A2A+vec 的分叉。job 名 AllToAll 与机制一致。

Host 配置串：`AlltoAll=level0:fullmesh;level1:pairwise`。  
`withReduceType(..., HCCL_REDUCE_SUM)` 是 builder API，**不是** AllReduce。

Agent trap：模板槽 **`X2TRANSPOSE`** = Cube B 是否转置。**不是** AIV `MC2VecTranspose` 那一阶段。

---

## 2. 这套栈和 M1 差在哪（先读这个）

| | M1 RS/AG | 本 M2 |
| --- | --- | --- |
| 调度 | AIC 循环里直接 `Hccl::*` | `pipeline_builder` 三阶段 |
| 通信控制核 | AIC（AIV 空转） | **`AICSync=false` → AIV block 0** |
| launch | `ReduceScatter/AllGather<true>` | `AlltoAll<false>` Prepare，再 **Commit（progress）** / **Wait（consumer gate）** |
| chunk 字段 | ASW `tileCnt` 上的 handle | 同一 Host `tileCnt`，communicator 用 **`taskIndex`** |
| 中间阶段 | 无 | **AIV layout permute** |
| same-device | flag **3** PIPE_FIX（AIC 内） | flag **8/9** + `SyncAll<true>`（**AIC↔AIV**） |

Cube 仍是 ASW。**不要**开第二张 43。

---

## 3. Chunk / task coordinate

Host formulaic 切 **M**（`AlltoAllMM` / `OneCalcOneCommBase`）：`tileCnt` 主块 + 可选 `tailCnt`。

- A2A→MM：通信内轴按 **K**（`SetCommShapeLen(kValue)`）
- MM→A2A：通信内轴按 **N**（`SetCommShapeLen(nValue)`）

Kernel：`ProcessTile(tileCnt)` 再 `ProcessTail(tailCnt)`，每次 `pipeLine_->Process(taskCnt)`。  
`taskIndex` ∈ `[0, taskCnt)` 对应一个 M-chunk 上的 **一整轮** comm / permute / MM。

这不是 66/68 的 K 切分，也不是 M1 里 “一个 handle = 一次 ReduceScatter、没有 AIV permute”。

`HcclCommunicator` `MAX_HCCL_HANDLE_ = 63`。`tileCnt > 63` 是否被 Host 挡住：**未**在本文证明。

---

## 4. `HcclCommunicator`：Wait-one vs Send-one

模板：

```text
allto_all_matmul : AICSync=false, SendCnt=0, RecvCnt=1  → COMMUNICATION_WAIT_ONE
matmul_allto_all : AICSync=false, SendCnt=1, RecvCnt=0  → COMMUNICATION_SEND_ONE
```

`notifyFlag_`：仅 **AIV `GetBlockIdx()==0`**。Prepare / Process / End 的 Wait·Commit·Finalize 只在这核。  
`Init()` **没有** `notifyFlag_` 守卫：AIC/AIV 都会 `InitV2` + `SetCcTilingV2`。不要写成 “只有 AIV0 碰过 HCCL”。

Primitive：

```text
Prepare  → hccl->AlltoAll<false>(send+i*off, recv+i*off, sendCount, dtype, stride, repeat)
SyncSend → AlltoAll<true>(...)     // 本 fp 路径的 Process 不用这条
```

`<false>` = 交出 handle，**不**在 Prepare 里当 launch。  
**Commit = communication progress**（启动该 handle 的搬运）。  
**Wait = completion / consumer gate**（这一块对下一阶段合法）。  
**Commit ≠ cross-rank visibility。** 远端对消费者可见的是 Wait，不是 Commit。

**WAIT_ONE（canonical A2A→MM）**

```text
PrepareAll(taskCnt)
  每个 i: hTasks_[i] = AlltoAll<false>
  然后全部 Commit          // progress：把所有 task 的搬运推出去
Process(i)
  Wait(hTasks_[i])         // consumer gate：这一块 recv 对 AIV permute 合法
End
  已 Wait 完的不再等；Finalize
```

**SEND_ONE（sibling MM→A2A）**

```text
PrepareAll(taskCnt)
  只填 handle，不 Commit
Process(i)
  Commit(hTasks_[i])       // progress：permute 写好之后推这一块 send
End
  Wait 所有尚未成功的 handle  // consumer gate 留到收尾
  Finalize
```

---

## 5. Canonical timeline：A2A → permute → MM

`AlltoAllMatmulPipeLine::Process`：

```text
PrepareAll(taskCnt)                 // AIV0：Commit 全部 = progress，不是 Wait

for taskIndex:
  AIV:
    Process(i)                      // Wait = consumer gate
    SyncAll<true>                   // AIV 之间：Wait 完再 permute
    MC2VecTranspose.Process(i)
    CrossCoreSetFlag<0, PIPE_MTE3>(8)
    CrossCoreWaitFlag(8)            // 本侧 MTE3 排干
    CrossCoreSetFlag<2, PIPE_MTE3>(9)
  AIC:
    CrossCoreWaitFlag(9)
    MC2FpMatmul.Process(i)          // ASW；AIV 直接 return
```

源码注释：matmul **内核**不消费 flag 8/9；8/9 是 **pipeline 外壳** 的 AIC↔AIV。不要当成 ASW 内部软同步。

WAIT_ONE 在循环前 Commit 全部：pipeline **permits** 后续 task 的搬运与当前 permute/MM 重叠；**不证明**已测到 overlap。进入 permuteᵢ 前必须 `Wait(i)`（consumer gate）。

---

## 6. Reverse sibling：MM → permute → A2A

`MatmulAlltoAllPipeLine::ProcessDefault`（fp 路径；**不要**读 `ProcessKC`）：

```text
PrepareAll(taskCnt)                 // 只 Prepare

for taskIndex:
  AIC:
    MC2FpMatmul.Process(i)
    CrossCoreSetFlag<0, PIPE_FIX>(8)
    CrossCoreWaitFlag(8)            // FIXPIPE 排干
    CrossCoreSetFlag<2, PIPE_FIX>(9)
  AIV:
    CrossCoreWaitFlag(9)
    MC2VecTranspose.Process(i)
    SyncAll<true>
    Process(i)                      // Commit = progress，不是 Wait
End: Wait leftover + Finalize         // leftover 的 consumer gate
```

SEND_ONE 在 permute 后 Commit、Wait 留到 End：pipeline **makes overlap eligible**（当前 send 的搬运 vs 下一块 MM）。**不证明**已测到 overlap。和 M1 RS 的 Wait-deferred 同形态，但中间多 AIV permute，通信核在 AIV0。

---

## 7. Buffer ownership

**A2A→MM（canonical）**

| 角色 | 缓冲 |
| --- | --- |
| AlltoAll send | API `x1` |
| AlltoAll recv | workspace `commOutGM_`（`commLen`） |
| permute dst / Cube A | `all2all_out`；若为空则 workspace `commOut + commLen` |
| Cube B / C | API `x2` / `y` |

**MM→A2A**

| 角色 | 缓冲 |
| --- | --- |
| Cube A/B | API `x1` / `x2` |
| Cube C / permute src | workspace `tempComputeOut`（`mmResultLen`） |
| permute dst / AlltoAll send | workspace `transOut` |
| AlltoAll recv | API `y` |

中间 permute 缓冲不是下一张 API tensor（除非用户给了 `all2all_out`）。

---

## 8. Same-device vs cross-rank

| | 本栈 |
| --- | --- |
| same-device AIC↔AIV | flag **9**（producer 用 8 做本侧 pipe drain 再发 9） |
| same-device AIV↔AIV | `SyncAll<true>`（Wait/permute 与 Commit 之间） |
| communication progress | `hccl_.Commit`（仅 AIV0）。**不是** consumer gate，**不是** cross-rank visibility |
| completion / consumer gate | `hccl_.Wait`（仅 AIV0 Process/End）。**这才是** 下一段可读远端结果的点 |
| 编译条件 | `--cce-auto-sync=off` **不是** sync |

Flag 8/9 ≠ 远端可见。Commit ≠ 远端可见。不要写成 Case B window。

AIV permute 是 **rank-block layout permute**（`nextSrc/DstBlockOffset` + `innerOffsetIn/Out`），让 AlltoAll 缓冲变成 Cube 要的 A（或反过来）。不要写成 “又一个 `transpose_x2`”。

---

## 9. AIC / AIV

| | fp16/bf16 M2 |
| --- | --- |
| AIV 0 | HCCL Prepare/Process/End + 参与 permute |
| 其他 AIV | permute；不 Commit/Wait |
| AIC | 只 MM；等 flag 9 |
| AIV 在 `MC2FpMatmul::Process` | `return` |

M1 RS：AIV 在 Compute 开头直接 return。本栈 AIV **有活**。

---

## 相对 M1：是不是新 teaching mechanism？

**是。** 共用 “按 M 切 chunk、HCCL handle、ASW Cube”，但工程耦合换成：

```text
三阶段 pipeline
  + 通信控制在 AIV0（HcclCommunicator）
  + AIV layout permute
  + AIC↔AIV flag 8/9
  + AlltoAll<false> + 显式 Commit
```

这不是 “又一个 AG/RS，只是 collective 换成 AllToAll”。  
两 op **不要**各开一张卡：同一套栈、顺序相反。

**建议已执行：** 第二张 MC2 Case [`ascend950-alltoall-matmul-pipeline`](../../../../../cases/compute-communication/mc2/ascend950-alltoall-matmul-pipeline/case.md) `accepted`（canonical A2A→MM fp；MM→A2A reverse sibling）。Knowledge：[mc2-compute-collective-coupling](../../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）。不要把本栈并进 RS Case。

---

## 不要做

- 把 `X2TRANSPOSE` 读成 AIV permute。
- 把 flag 8/9 写成跨 rank，或写成 ASW 内核软同步。
- 把 **Commit 写成 cross-rank visibility**（Commit = progress；Wait = consumer gate）。
- 把 “pipeline permits overlap” 写成已测到的 overlap。
- 把 `AlltoAll<false>` 写成已经 launch（要等 Commit）。
- 把默认 `ai_cpu` 写成 “950 不是 live”（AICPU 与 CCU 都是本栈）。
- 把 `withReduceType(SUM)` 写成 AllReduce。
- 展开 KC/MX/`ProcessKC`/arch22。
- 用 SHMEM 直调红线否掉这条 `Hccl::` 生产路径。
- 写 Knowledge / 读 AR / 读 M3。
