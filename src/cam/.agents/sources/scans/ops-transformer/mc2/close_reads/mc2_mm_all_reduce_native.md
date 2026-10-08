# Close-read: `matmul_all_reduce` native AllReduce（fp16/bf16）

Not a Case. Pin **one live variant**. Sibling `TPL_A2A_RS_AG=1`（A2A + vec reduce + AG）不在本文。

```yaml
op: matmul_all_reduce
commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
hw: ascend950
canonical_kernel: MatmulAllReduce910General + MatmulAllReduceBase<false>
canonical_comm: Hccl::AllReduce<false> + Commit + Wait(same handle × tileCnt)
not_this_file: MatmulAllReduceBase<true>  # job 仍叫 AllReduce
status: close-read-written
review: passed
```

入口：`mc2/matmul_all_reduce/op_kernel/arch35/matmul_all_reduce_apt.cpp`  
实现：`.../arch35/matmul_all_reduce_910_general.h` + `matmul_all_reduce_based_all_reduce.h`  
Cube：`mc2/3rd/mat_mul_v3/op_kernel/arch35/mat_mul_asw_kernel.h`（`Mc2MatmulAswKernel`）  
Host：`op_host/op_tiling/arch35/matmul_all_reduce_tiling_950.cpp`  
Tiling key：`op_kernel/matmul_all_reduce_apt_tiling_key.h`

---

## 1. Exact 950 tiling key（native）

Kernel 分派（fp16/bf16，`ORIG_DTYPE_X1 == ORIG_DTYPE_X2`）：

```text
APT_MM_TYPE == MMTYPE_FP_MM
TPL_A2A_RS_AG == 0                 → MatmulAllReduceBase<…, false, commMode>
                                     hccl_.AllReduce(...)
TPL_A2A_RS_AG == 1                 → MatmulAllReduceBase<…, true, commMode>
                                     A2A + vec reduce + AG     // 不要
```

Canonical 再收一层：**无 x3 add**。

```text
MATMUL_WITH_ADD = 0
Mc2CoreType     = ON_CUBE
KERNEL_TYPE     = MIX_AIC_1_0
TPL_COMM_MODE   = CCU 或 AICPU
dtype           = fp16 | bf16
```

`fp_matmul_all_reduce`：`!MATMUL_WITH_ADD` → `ON_CUBE`；`MATMUL_WITH_ADD` → `ON_CUBE_AND_VECTOR`（`MIX_AIC_1_2`，notify 改到 AIV 0）。**不是本文路径。**

Host `GetTilingKey()`：`isA2ARSAG = IsUseA2APath(rankDim, npuArch_)`。  
`IsUseA2APath`：**DAV_3510 且 rankDim ∈ {4, 8}**。

**和 RS 不同：** AR **没有** `comm_mode == ai_cpu` 强制 native。默认 attr 仍是 `"ai_cpu"`，但 4P/8P 950 照样走 sibling。本文是：

```text
DAV_3510 且 rankDim ∉ {4, 8}
TPL_A2A_RS_AG = 0
```

Agent trap：job 名 AllReduce、4P/8P 默认路径、`MATMUL_WITH_ADD`、quant `matmul_all_reduce_based_a2a_rs_ag.h` —— 都不是这篇。

`--cce-auto-sync=off` 是 950 编译条件，不是 sync 原语。

Host 通信配置（native）：`AllReduce=level0:fullmesh`，`HCCL_CMD_ALLREDUCE`，reduce = SUM。  
Sibling 才是 `AlltoAll` + `AllGather`（`SetMc2HcommTwoShot`）。

---

## 2. Chunk coordinate

Host formulaic 切 **M**（`DoSplitMTiling` / `GetTilingResult`）：`tileCnt` 个主块 + 可选 `tailCnt` 尾块。若干强制不切（`enableSplitK`、K=0 等）时 `tileCnt=1, tailCnt=0`。

Kernel 一个 MM turn `i`：

```text
mVal     = (tileCnt==1 && tailCnt==0) ? rankM : mmTiling->M
cOffset  = mVal * N                       # 元素个数
cAddrOff = cOffset * sizeof(Y)
aAddrOff = mVal * Ka * sizeof(X)          # fp16/bf16
```

`tileCnt` 对 communication 的真实含义：

```text
AllReduce(send=cGM, recv=outputGM, count=cOffset, SUM, repeat=tileCnt)
```

一次调用，**repeat = tileCnt**。count 是 **一块** M×N，不是整张 Y。

所以：

| 问 | 答 |
| --- | --- |
| AR 是不是按 M-tile / turn？ | **是。** MM 循环 `i < turnCnt`，每 turn 一块 M。 |
| 是否和 RS 一样“一 tile 一 collective”？ | **不是。** RS：每 tile 一次 `ReduceScatter<true>` → 一个 handle。AR：Init **一次** `AllReduce(..., tileCnt)` → **一个** handle，tileCnt 个 repeat。 |
| tail？ | 另一次 `AllReduce(..., tailCnt)`，另一个 handle。 |

这不是 66/68 的 K 切分。

---

## 3. Producer / consumer timeline

源码决定的顺序（native，`basedA2aRsAg=false`）：

```text
Init
  hccl_.InitV2 + SetCcTilingV2
  cGM := workspace + nd2NzWorkLen + biasLen     // 3510；window 不在本文
  AllReduce<false>(cGM, outputGM, cOffset, SUM, tileCnt)   // Prepare，一个 handle
  if tail: AllReduce<false>(cGM+off, outputGM+off, tailCnt)

Process
  InnerProcess(tileCnt, tileInfo)
    for i in 0 .. tileCnt-1:
      used AIC: ASW MM_i → 当前 cGM
      PostProcEachTurn(same handle)
        Mc2SyncAll                         // same-device
        Commit(handle)                     // 这一 repeat 的 comm progress
        aGM += aOff; cGM += cOff
  if tail: InnerProcess(tailCnt, tailInfo)  // 同一形态
  HcclFinalize
    Wait(tile handle) × tileCnt
    if tail: Wait(tail handle) × tailCnt
    Finalize
```

**Wait 全部在 HcclFinalize**，不在 MM 循环里。

因此 **不是** 预设的：

```text
MM_i → AllReduce(handle_i) → next MM
```

而是：

```text
MM_i → Commit(repeat_i) → MM_{i+1} → … → Wait × tileCnt
```

`Commit_i` 之后可以继续 `MM_{i+1}`；`AR_i` 与 `MM_{i+1}` **有资格** overlap。本文 **没有** 测过重叠拍数。

没有 AG 那种本卡 local shortcut。每一块都是 MM → Commit。

---

## 4. Async morphology

HCCL stub：`template <bool commit = false> AllReduce(..., uint8_t repeat = 1)`。  
源码写 `hccl_.template AllReduce(...)`，**没有**写 `<true>` → **`AllReduce<false>`**。

| | native AR | native RS（对照，不是本文） |
| --- | --- | --- |
| launch | Init 一次 `AllReduce<false>(…, tileCnt)` | 每 tile `ReduceScatter<true>(…, 1)` |
| handle | **一个**（tail 再一个） | **每 tile 一个**（最多 64） |
| Commit | 每 turn `hccl_.Commit(handle)` | 无独立 Commit；`<true>` 即 launch |
| Wait | **同一 handle** `Wait` **tileCnt 次** | 对 **每个** handle `Wait` 一次 |

`<false>` = Prepare，不在 Init 里把 comm 跑完。  
`Commit` = 这一 repeat 的 communication **progress**，不是 cross-rank visibility。  
`Wait` = 这一 repeat 的 completion / consumer gate。`Wait` 完之前不要把 `outputGM` 当已经 AllReduce 好的 Y。

不要把 “Wait 调用了 tileCnt 次” 读成 “tileCnt 个 handle”。循环变量只是重复 Wait **同一个** `tileInfo_.hcclHandleId`。

notify 守卫：`ON_CUBE` 时 `notifyFlag_ = (GetBlockIdx() == 0)`。Init AllReduce / Commit / Wait / Finalize **只有 AIC 0**。used AIC 做 MM；idle AIC 仍走 `PostProcEachTurn` 的 `Mc2SyncAll`，不 Commit。

---

## 5. Ownership

addrs 初值：`{aGM, bGM, biasGM, addGM, cGM, workspaceGM, cGM}` → `outputGM` 一开始就是 API 的 Y。

3510 Init 改写 **`cGM`**，不改 `outputGM`：

| 角色 | 地址 | lifetime |
| --- | --- | --- |
| MM 写出 / AllReduce send | workspace 上的 `cGM`（偏过 nd2Nz + bias） | 每 turn 写一块；Commit 后这块是 comm 输入。Wait 之前不是 API Y |
| AllReduce recv / 最终 Y | `outputGM`（用户 cGM） | 每 rank 一份 **full Y**。Wait 之后才是 reduced 结果 |
| A / B | 本 rank `aGM` / `bGM` | 每 turn `aGM` 沿 M 前进 |

API 最终：每个 rank **完整 Y**（AllReduce 语义）。中间 MM partial 在 workspace，是 comm 的 send，不是下一张 API tensor。

非 3510 的 `WINDOW_IN` 把 `cGM` 指到 `GetWindowsInAddr`：**不在本文**。不要把 3510 native 写成 window。

`MC2_DEBUG_ONLY_CUBE` 不在这条 910 general 路径上。

---

## 6. Same-device vs cross-rank

`Mc2SyncAll<ON_CUBE>`：

```text
PipeBarrier<PIPE_ALL>
CrossCoreSetFlag<0x0, PIPE_FIX>(3)
CrossCoreWaitFlag(3)
```

在 **MM Process/End 之后、Commit 之前**。AIV 不在 `MIX_AIC_1_0` 这条 mix 里。把它读成 **Cube 写完 GM 的 same-device 就绪**，不要读成 cross-rank。

| gate | 原语 | 含义 |
| --- | --- | --- |
| Cube result ready（local） | flag 3 PIPE_FIX / `Mc2SyncAll` | 本卡 FIXPIPE 后再让 comm 读 send buf |
| collective completion（remote） | `hccl_.Wait` × tileCnt | reduced Y 对后续消费者可见 |
| comm 控制核 | AIC 0 | Init / Commit / Wait / Finalize |

`--cce-auto-sync=off` 只说明不能指望编译器代插 sync。

---

## 7. Relative to RS / AG

同属后面要判的 M1 候选（机制分类见 [mc2_m1_ar_verdict.md](mc2_m1_ar_verdict.md)）：

```text
ASW Cube + Host 按 M 切 tileCnt/tailCnt + HCCL collective + Wait 才是 cross-rank gate
```

**只是同一 ASW 栈、但形态不同：**

- 不是 handle-per-chunk
- `AllReduce<false>` + per-turn Commit，不是 `ReduceScatter<true>`
- Wait 同一 handle × N，不是 Wait N 个 handle
- 产出 full Y，不是 shard / gather-then-MM
- 4P/8P **即使 ai_cpu** 也不是这条 native

Cube 文件也不是 RS 的 `mc2_mat_mul_asw_kernel.h`，是 v3 `Mc2MatmulAswKernel`。compute baseline 仍是 43，不要开新 GEMM Case。

---

## 不要做

- 把 4P/8P 950 默认路径当成这篇 native AllReduce。
- 把 `comm_mode=ai_cpu` 读成 “一定 native”（那是 RS 的强制，不是 AR 的）。
- 写成 “一 tile 一个 AllReduce handle”。
- 把 Init 的 `AllReduce` 读成已经 launch 完（默认 `<false>`）。
- 把 `Commit` 写成 cross-rank visibility。
- 把 flag 3 写成跨 rank。
- 展开 quant / A2A+AG sibling / window / `MATMUL_WITH_ADD` / M3。
- 预设 `MM_i → AllReduce(handle_i) → MM_{i+1}`。
