# Close-read: `all_gather_matmul_v2` fp16/bf16

Not a Case. Reverse coupling vs native RS：comm 产 A，Cube 消费。

```yaml
op: all_gather_matmul_v2
commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
hw: ascend950
canonical_kernel: AllGatherMatmulFP16BF16
canonical_comm: Hccl::AllGather<true>
v1: all_gather_matmul 不是 950 live
```

入口：`mc2/all_gather_matmul_v2/op_kernel/arch35/all_gather_matmul_v2_apt.cpp`  
实现：`.../arch35/all_gather_matmul_fp16_bf16.h`  
Host：`op_host/op_tiling/all_gather_matmul_tiling_base.cpp`、`arch35/all_gather_matmul_tiling_v2.cpp`

---

## 1. Exact 950 tiling key

Kernel（fp16/bf16）：

```text
INPUT_IS_BF16FP16 == true
OUTPUTDTYPE == OUTPUT_TYPE_IS_FP16_BF16     // Host: outputIsFp8_ ? 1 : 0
QUANTMMMODE == TPL_DEFAULT_MODE
SCALETYPE == SCALE_TYPE_NOT_IS_MX
HCCL_COMM_MODE ∈ {CCU, AICPU}
→ AllGatherMatmulFP16BF16
```

950 Host：`commMode` 字符串 `"ccu"` → CCU，否则 AICPU。  
没有 RS 那种 4P/8P 改 A2A 的 Host 分叉。job 与机制都是 AllGather。

`KERNEL_TYPE_MIX_AIC_1_2`。`--cce-auto-sync=off` = 编译条件。

Quant / MX 分派是 sibling，本文不读。

---

## 2. Chunk coordinate

Host 切 **本 rank 的 M**（`DoSplitMTiling`）：`tileCnt` / `tileMValue_`，可选 tail。  
Gather 后远端 Cube 的 M：`tileM * (rankDim - 1)`（`all_gather_matmul_tiling_v2.cpp` 写进 `tCubeTiling.M`）。

Kernel `StartNotify`：

```text
tilingM = tCubeTiling.M / (rankDim - 1)     # = 本 rank 该 tile 的 M
tileSendCount = tilingM * Ka
stride = tileSendCount * tileCnt + tailSendCount * tailCnt
hHandles_[i]     = AllGather<true>(aGM + i*tileSendCount, gatherAddr + i*tileSendCount, ...)
hHandles_[tileCnt+j] = tail 同形
```

`MAX_HANDLE = 16`（比 RS 的 64 小）。Host 注释 HCCL 最多 16 次通信。

`isGather=true` 的 `UpdateOffset`：**跳过 `i == rankID`**，只给远端 rank 填 A/C 偏移。

---

## 3. Producer / consumer timeline

全程 `if ASCEND_IS_AIC`（AIV 不跑这条 Process）。

```text
Init
  hccl_.InitV2 + SetCcTilingV2
  gatherAddr_ = gatherOut；若 gatherLen!=0 或 gatherOut 空 → workspace
  notifyFlag_ = true（AIC 且非 ONLY_CUBE）

Process (AIC only)
  StartNotify          // 全部 AG tile 先 AllGather<true>，留下 handles
  InnerProcess
    InnerProcessLocalMM     // 本卡 A，不 Wait
    InnerProcessGatherMM    // 每 tile Wait 再 MM
  EndNotify
    全 AIC：flag 3 Set/Wait
    AIC 0：Finalize
```

远端 tile：

```text
Wait(hHandles_[i+shift])
UpdateSlice(i)
Process
```

Comm **先于** 远端 Cube；本卡 MM 夹在 AG launch 和远端 Wait 之间。

---

## 4. Handle lifetime

- `hHandles_[MAX_HANDLE]`，`MAX_HANDLE = 16`。
- 一个 AG tile 一个 handle（主块 0..tileCnt-1，尾块接在后面）。
- `AllGather<true>` 在 `StartNotify` **一次性发完**，不是 MM 循环里发。
- Wait：远端 MM 循环里 **每个 tile 用之前** Wait 该 handle。闲核 / `rankN==0` 只 Wait 不 MM。
- Finalize 在全部 MM 之后、AIC 0。

---

## 5. Buffer ownership

| 角色 | 位置 |
| --- | --- |
| 本 rank A send | `aGM_` |
| gathered A | `gatherAddr_`（`gatherOut` 或 `workspace += gatherLen`） |
| B | `bGM_` replicated |
| 本卡 C | `cGM_ + rankId * rankM * N` |
| 远端 C | `cGM_`；`UpdateOffset` 按远端 rank 槽 |

Local MM 读 `aGM_`，写 C 的 **本 rank 条带**。  
Gather MM 读 `gatherAddr_`（debug ONLY_CUBE 才读 `aGM_`）。

`gatherLen==0` 且提供了 `gatherOut`：API gather 输出即 MM 输入。否则 gather 在 workspace（relocated）。

---

## 6. AIC / AIV

| | fp16/bf16 |
| --- | --- |
| AIV | 不进入 `Process` |
| AIC | StartNotify + local MM + Wait+gather MM + EndNotify |
| Flag 3 | `EndNotify` 里全 AIC Set/Wait，然后 0 号 Finalize。注释：防 block0 过快清 RcvCnt |

Flag 3 **不是** AG 的 consumer gate。跨 rank gate 是 `hccl_.Wait(hHandles_[i])`。

---

## 7. Local shortcut

**有。** `InnerProcessLocalMM`：

- 输入：本卡 `aGM_`，不 Wait
- 输出：`cGM` 上 `rankId * rankM * N` 条带
- 与 `StartNotify` 已发出的远端 AG **重叠**

远端路径才 Wait。`UpdateOffset` 跳过本 `rankID`，避免 gather Cube 再算一遍本卡。

RS **没有**对偶的 local RS shortcut。

---

## 不要做

- 把 v1 `all_gather_matmul` 当 950 SoT。
- 把 local MM 的 skip-Wait 写成 “没有通信依赖”（本卡 A 本来就在；远端仍要 Wait）。
- 把 EndNotify flag 3 写成跨 rank。
- 展开 quant AG。
- 为 AG 再开一张 Case。Contrast 已在 [RS Case](../../../../../cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md)。
