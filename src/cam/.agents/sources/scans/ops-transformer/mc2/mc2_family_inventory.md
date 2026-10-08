# MC2 family inventory（ops-transformer, Ascend 950）

Focused Scout. **Not a Case. Not Knowledge.** Attention / GEMM / MoE 线冻结。

Question this folder must answer:

```text
AG / AR / RS / A2A + Matmul
到底有几种真正不同的工程机制？
（collective 与 Matmul 的耦合方式，不是 op 名清单）
```

```yaml
status: scout-passed
review: passed
new_case: cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/
first_case: matmul_reduce_scatter_v2 native ReduceScatter  # accepted
close_read: sources/scans/ops-transformer/mc2/close_reads/
source:
  registry_ref: ops-transformer
  commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scan_path: /home/wangyibo/ops-transformer_bak
live_gate: >
  op_host CMakeLists has CONDITION_UNIT==ascend950
  AND op_host/config/ascend950/ exists
  AND 950 kernel (arch35 *_apt.cpp or shared apt-style MIX kernel)
out_of_inventory:
  - moe_distribute_* / mega_moe / moe_ep_*   # already Cases B/D
  - DeepEP dispatch/combine                  # A3 on this checkout; 950 in sgl
  - distribute_barrier* / engram_* / ATT↔FFN P2P
```

证据种类：`source-code`。本文件 **没有** `benchmark`。  
`examples/mc2/all_gather_add/` 是 A2/A3 的 **AllGather+Add**，产品表无 950，**不是** 本 Scout SoT。

---

## 1. 950 上哪些 MC2 family 是 live？

Live gate 过了的 **matmul + collective**：

| job（op 名） | 950 live op | 耦合顺序 |
| --- | --- | --- |
| AllGather + Matmul | `all_gather_matmul_v2` | AG(A shards) → MM → **C full** |
| Matmul + ReduceScatter | `matmul_reduce_scatter_v2` | MM → RS → **C shard** |
| Matmul + AllReduce | `matmul_all_reduce` | MM → AR → **Y full** |
| AllToAll + Matmul | `allto_all_matmul` | A2A → transpose → MM |
| Matmul + AllToAll | `matmul_allto_all` | MM → transpose → A2A |
| AllToAllV + Grouped MM | `allto_allv_grouped_mat_mul` + quant sibling | A2Av → GMM |
| Grouped MM + AllToAllV | `grouped_mat_mul_allto_allv` + quant sibling | GMM → A2Av |

**同名 v1 不是 950 live：** `all_gather_matmul`、`matmul_reduce_scatter`（CMake 只有 910b / 910_93；无 `config/ascend950/`）。950 用 **v2**。

**950 未编：** `matmul_all_reduce_add_rms_norm`、`inplace_*`、`grouped_mat_mul_all_reduce`、`batch_mat_mul_reduce_scatter_allto_all`、`allto_all_all_gather_batch_mat_mul`。

**live 但不是 matmul-collective：**

| op | 为什么列出 |
| --- | --- |
| `quant_reduce_scatter` | AIV-only MTE window；**没有** Cube MM |
| `quant_all_reduce` | 同上 |

不要把这两张当第一张 MC2 Case，也不要把它们并进 EP Case B（window 像，dataflow 不是 token routing）。

---

## 2. 每个 live family：Host / tiling / kernel；AIC / AIV / comm

### 2.1 AllGather + Matmul — `all_gather_matmul_v2`

| | path |
| --- | --- |
| Host | `mc2/all_gather_matmul_v2/op_host/`（CMake `ascend950`） |
| Tiling | `op_host/op_tiling/arch35/all_gather_matmul_v2_tiling_a5.cpp` 等 |
| Kernel | `op_kernel/arch35/all_gather_matmul_v2_apt.cpp` → `all_gather_matmul_fp16_bf16.h` |
| Cube | `mc2/common/op_kernel/mc2_mat_mul_asw_kernel.h` → `3rd/mat_mul_v3/.../mat_mul_asw_kernel.h` |
| Task | `KERNEL_TYPE_MIX_AIC_1_2` |

AIC：`StartNotify` 发 AllGather，然后 `InnerProcessLocalMM`（本卡 A，不 Wait）再 `InnerProcessGatherMM`（`hccl_.Wait` 每块后再 ASW MM）。  
AIV：这条 fp16/bf16 `Process` **不进入**。`--cce-auto-sync=off` 是编译条件。  
Comm：`hccl_.template AllGather<true>(...)` per `tileCnt`/`tailCnt`。

### 2.2 Matmul + ReduceScatter — `matmul_reduce_scatter_v2`

| | path |
| --- | --- |
| Kernel | `op_kernel/arch35/matmul_reduce_scatter_v2_apt.cpp` → `matmul_reduce_scatter_fp16_bf16.h` |
| Cube | 同一套 `mc2_mat_mul_asw_kernel.h` |
| Task | MIX 1:2；Host `-DENABLE_CV_COMM_VIA_SSBUF=true` |

AIC：每个 M-tile `mmv3.Process` → `CrossCoreSetFlag/Wait` → `ReduceScatter<true>` 记下 handle。  
`PostProcess`：`hccl_.Wait(handles_[i])` 等所有 tile。  
另有 tiling key `TPL_CCU_ALL2ALL_VEC_REDUCE`：job 仍叫 RS，机制是 **A2A + Vector reduce**（不要一上来当 native RS）。

### 2.3 Matmul + AllReduce — `matmul_all_reduce`

| | path |
| --- | --- |
| Kernel | `op_kernel/arch35/matmul_all_reduce_apt.cpp` |
| Native fp16/bf16 | `matmul_all_reduce_based_all_reduce.h`：`hccl_.template AllReduce(...)` |
| Quant sibling | `matmul_all_reduce_based_a2a_rs_ag.h` 等：job 仍叫 AR，内层可走 **A2A + AG** |

Cube 仍是 ASW。Close-read passed：[mc2_mm_all_reduce_native.md](close_reads/mc2_mm_all_reduce_native.md)。native 一次 `AllReduce(..., tileCnt)` + per-turn Commit；**不是** AG/RS 的 per-handle Wait。

### 2.4 Dense AllToAll ↔ Matmul

| op | Kernel | 流水 |
| --- | --- | --- |
| `allto_all_matmul` | `op_kernel/arch35/allto_all_matmul_apt.cpp` + `allto_all_matmul_pipeline.h` | A2A → AIV transpose → AIC MM |
| `matmul_allto_all` | `op_kernel/arch35/matmul_allto_all_apt.cpp` + `matmul_allto_all_pipeline.h` | AIC MM → AIV transpose → A2A |

**另一套模板：** `mc2/common/op_kernel/mc2_templates/`  
`pipeline_builder.h` → `comm_stage.h` → `HcclCommunicator` → `MC2AlltoAllPrimitives` → `hccl->AlltoAll`。  
PrepareAll / Process / Commit / Wait。这两条 950 fp live：`AICSync=false`，通信控制核是 **AIV 0**（模板允许 AIC，本路径没用）。

Close-read：[mc2_m2_a2a_mm.md](close_reads/mc2_m2_a2a_mm.md)。

### 2.5 Grouped AllToAllV + GMM

`allto_allv_grouped_mat_mul` / `grouped_mat_mul_allto_allv` 及 quant 对。  
Op-local `HcclA2avOp`：`hccl_.template AlltoAllV<true>`。调度按 **expert/group chunk**，不是 dense TP tile。

教学上先当 **EP-adjacent sibling**，不要当第一张 MC2 Case。HCCL AllToAllV ≠ Case B 的 MTE window。

---

## 3. collective “job” vs byte-movement

| job 名 | 950 常见机制 | 不要读成 |
| --- | --- | --- |
| AllGather | `Hccl::AllGather<true>` + `SetCcTilingV2` | “Kernel 里调了 HCCL 高级 API 黑盒、无法 overlap” |
| ReduceScatter | native `ReduceScatter<true>` **或** A2A+vec reduce | 所有 RS 都是同一条搬运 |
| AllReduce | native `AllReduce` **或** A2A+AG 拼出来 | 所有 AR 都是一条 AllReduce |
| AllToAll | `HcclCommunicator` + `AlltoAll` + 可选 transpose | MoE EP window |
| AllToAllV | grouped `AlltoAllV<true>` | Case B dest-push |
| README / 教程 AllGatherAdd | A2/A3 Vector Add | 950 Matmul-collective |

与 Case B 同一条纪律：**job 名 ≠ 实现机制。**

本 Scout 的 950 live **matmul-collective M1–M3** 走 `Hccl::` / `HcclCommunicator` + HCCL context。  
**M4** quant-only collective 使用 remote window，且已被排除在第一张 matmul-collective Case 之外。  
不要用 kernel 直调 SHMEM/UDMA 的 “禁止 HCCL 高阶 API” 红线来否掉 M1–M3。

---

## 4. 通信和计算在哪里 overlap？

| family | overlap 单位（已钉） | 未钉 / 不要外推 |
| --- | --- | --- |
| AG+MM v2 | **M-tile / rank-M 切片**：先 Commit 全部 AG；本卡 MM 与远端 AG 重叠；远端 MM 前 `Wait` 该 tile | ping-pong buffer 名未作为本 Scout 不变量 |
| MM+RS v2 | **M-tile**：Cube 产出一块 → `ReduceScatter` 该块 → 下一块 Cube；全部 `Wait` 在 `PostProcess` | CCU vs AICPU server 枚举要 close-read |
| MM+AR | **M-tile / AllReduce repeat**：Init 一次 `AllReduce(..., tileCnt)`；每 turn Commit；Wait 同一 handle × N | 不要写成 AG/RS 的 handle-per-chunk |
| A2A↔MM | **taskIndex**：`PrepareAll(taskCnt)` 后逐 index Process；Wait-one vs Send-one 由 `SendCnt/RecvCnt` 决定 | 与 ASW 的 `tileCnt` 不是同一套字段 |
| GMM+A2Av | expert-chunk scheduler | 不要当 TP 第一课 |

`one_calc_two_comm` / 教程 ping-pong：**未**在上述 950 matmul-collective kernel 里作为 live 字符串钉死。mega_moe 里出现过，本 inventory 不引用。

Producer/consumer：

- AG：HCCL handle 是 comm producer，Cube `Wait` 后消费 gather 缓冲。
- RS：Cube 是 producer，`ReduceScatter` 消费刚写完的 C 切片。
- AR：Cube 是 producer，`AllReduce` repeat 消费 MM partial；`Wait` 后 `outputGM` 是 full Y。
- A2A pipeline：`HcclCommunicator::Process` 的 Wait vs Commit 是 comm progress；`CrossCoreSetFlag` 是核间，不是跨 rank。

---

## 5. 数据 ownership

```text
AG+MM
  local A shard  +  replicated B
      → AllGather A
      → local MM(local A) ∥ remote AG
      → MM(gathered A tiles)
      → C full  （本 rank 一份完整结果）

MM+RS
  local A shard  +  B
      → MM 写出 C 切片（workspace / gmToFloat）
      → ReduceScatter
      → C shard  （rank-local final）

MM+AR
  local A  +  B
      → MM partial
      → AllReduce
      → Y full

A2A→MM
  local x1 shards
      → AllToAll 拼成 MM 需要的布局（常先 transpose）
      → MM
      → y

MM→A2A
  相反：先 MM，再 AllToAll 把结果交出去
```

Grouped A2Av+GMM：ownership 跟 **expert-major token** 走，不是 TP shard 的 AG/RS。另册。

---

## 6. 哪些 sync 是 same-device，哪些是 cross-rank

| 类别 | 例子 | 语义 |
| --- | --- | --- |
| same-device ordering | `CrossCoreSetFlag` / `WaitFlag`（RS flag 3；A2A flag 8/9）；`SyncAll` | AIC ↔ AIV / 核内显式顺序 |
| compile/runtime condition | `--cce-auto-sync=off` | **不是**同步原语；表示必须自己发显式 sync |
| cross-rank visibility | M1–M3：`hccl_.Wait(handle)` / `HcclCommunicator` Wait/Commit；**M4**：读 remote **Win** | 对端 rank 的数据/归约可见 |

Layer 2（MoE fusion）的句子仍适用：**可以换原语，不能删 visibility。**  
但 MC2 的 dataflow 不是 EP routing。引用 Layer 2，不要并进 MoE Knowledge。

---

## 7. workspace / communication window

| family | 中间缓冲角色 |
| --- | --- |
| AG+MM | optional `gatherOut` + MM workspace；HCCL CCU 缓冲经 `mc2CcTiling`。远端 A 先落到 gather 再当 MM 输入（**relocated** 给下一阶段，不是 API 再手递） |
| MM+RS | `gmToFloat_` workspace 当 RS send；debug-only-cube 可直接打到 `cGM` |
| MM+AR | MM 输出 workspace（send）；AR recv = `outputGM` full Y。close-read passed |
| A2A↔MM | `sendBuffer` / `recvBuffer` 在 `MC2AlltoAllContext`；transpose 吃 recv |
| quant RS/AR | **window 就是下一阶段输入**（`GetWinDataAddrGm`）；没有 Cube |

---

## 有几种真正不同的工程机制？（Scout 判断）

不要按四个 collective 名开四张卡。当前证据分成：

| ID | 机制 | 950 代表 op | 是否像第一张 Case |
| --- | --- | --- | --- |
| **M1** | **ASW Cube MM + HCCL collective chunk coupling** | AG / RS / AR **confirmed**；AR 不是 handle-per-chunk | AG+RS+AR 同一 family。**不要**把 handle-per-chunk 写成 M1 通例 |
| **M2** | `pipeline_builder` + `HcclCommunicator` + **transpose stage** + AllToAll | `allto_all_matmul` / `matmul_allto_all` | Case accepted |
| **M3** | AllToAllV + **grouped** MM（expert chunk） | `*grouped_mat_mul*allto_allv*` | 本轮不下钻 |
| **M4** | AIV-only MTE window collective | `quant_reduce_scatter` / `quant_all_reduce` | **不是** matmul-collective |

M1 拆开：

```text
confirmed
  AG v2 : M-tile / rank-M gather → MM
  RS v2 : M-tile MM → ReduceScatter（native 路径；4P/8P Host 默认可走 A2A+vec sibling）
  AR    : M-tile MM → AllReduce repeat（native；4P/8P 即使 ai_cpu 也走 A2A+AG sibling）
```

第一张 Case **已选**：MM+RS v2 fp16/bf16 native `ReduceScatter`。  
卡：[`ascend950-matmul-native-reduce-scatter`](../../../../cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md)（`accepted`）。  
AG v2 是 reverse sibling contrast，不开第二张卡。AR 是 M1 member / morphology evidence，不开 AR Case。  
M2 Case：[`ascend950-alltoall-matmul-pipeline`](../../../../cases/compute-communication/mc2/ascend950-alltoall-matmul-pipeline/case.md)（`accepted`）。  
Knowledge：[mc2-compute-collective-coupling](../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）。

---

## 不要做

- 回改 RS / M2 Case 技术内容。
- 四种 collective 各开一张卡。
- 现在展开 M3。
- 把 Cube ASW 读成新的 43 / 66 / 68。
- 把 HCCL Wait 读成 Case B 的 MTE dest-push。
- 把 `examples/mc2/all_gather_add` 当 950 SoT。
- 把 grouped A2Av+GMM 当第一张 TP MC2。
- 用 SHMEM 直调红线否定生产 **M1–M3** 的 `Hccl::` 路径。
- 把 M4 window 写进 “全部生产 MC2 都是 Hccl::” 那句总结。
- 把 handle-per-chunk 写成 M1 通例。
