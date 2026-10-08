# Verdict: native AllReduce 是否属于 M1

Scout 之后的 AR close-read 结论。**Not a Case. Not Knowledge.**  
quant A2A+AG / window / M3 / `MATMUL_WITH_ADD` 不展开。

证据：

- [mc2_mm_all_reduce_native.md](mc2_mm_all_reduce_native.md)
- 对照：[mc2_mm_reduce_scatter_v2_native.md](mc2_mm_reduce_scatter_v2_native.md)、[mc2_all_gather_mm_v2.md](mc2_all_gather_mm_v2.md)、[mc2_m1_ag_rs_verdict.md](mc2_m1_ag_rs_verdict.md)

```yaml
status: verdict-written
review: passed
op: matmul_all_reduce
canonical: MatmulAllReduce910General + MatmulAllReduceBase<false>  # TPL_A2A_RS_AG=0, no add
m1_membership: yes
morphology: one AllReduce(repeat=tileCnt) + per-turn Commit + Wait(same handle × N)
not: handle-per-M-tile
sibling: TPL_A2A_RS_AG=1  # 4P/8P DAV_3510；ai_cpu 也不强制 native
knowledge: knowledge/engineering-experience/mc2-compute-collective-coupling.md
new_case: none              # 不因第三个 collective 名再开 Case
```

---

## AR 是否真正属于 M1？

**是：** `ASW Cube MM + HCCL collective chunk coupling`。

判据不是 “job 名叫 AllReduce”，也不是 “和 RS 共用同一份 ASW 头文件”。判据是：

```text
Host 按 M 切 tileCnt / tailCnt
  → 每 turn 一块 Cube MM 写出 comm send
  → HCCL collective 以这块为通信单元
  → same-device Cube 就绪 ≠ cross-rank 完成
  → Wait 才是 consumer / completion gate
```

native AR 满足全部五条。`tileCnt` 同时是 MM turn 数和 `AllReduce(..., repeat=tileCnt)` 的 repeat 数。chunk 坐标是 **M-tile / turn**，不是 K 切分，也不是 “整张 Y 一次 AllReduce、MM 只是前缀”。

**“same ASW stack” 单独不够。** 若只有 Cube ASW、没有 HCCL 按 chunk 耦合，那只是 43 的消费者。AR 的耦合点是：Init 用 `cOffset`×`tileCnt` 把 MM 块登记成 AllReduce repeats，每 turn `Commit` 推进一块。

4P/8P sibling（`TPL_A2A_RS_AG=1`）job 仍叫 AllReduce，内层是 A2A+AG。**那条不是本 verdict 的 M1 证明**，和 RS 的 A2A+vec 一样是 job≠mechanism。

---

## 与 RS / AG 共用的 M1 invariant

这三条 live path 现在可以一起说（不再把 AR 排除在外）：

```text
1. Cube = ASW（compute baseline = 43，不开新 GEMM Case）
2. Host 按 M 切 tileCnt / 可选 tailCnt
3. 通信单元对齐 M-tile，不是整图一次 collective
4. producer readiness ≠ collective completion
   （有没有 local flag、flag 在哪一侧，由 coupling direction 决定；
     不要把 RS/AR 的 flag 3 写成 AG 也必须有）
5. Wait = collective completion / consumer gate
6. Commit 或 ReduceScatter/AllGather<true> = communication progress，不是 visibility
7. chunk ownership 沿 pipeline stage 转移；consumer 只有在对应
   readiness / completion gate 之后才能合法消费。
   最终 API ownership / shape 由具体 collective–compute 顺序决定
```

第 7 条是 ownership **转移规则**，不是 “MM partial 一定在 workspace”。具体形状在下面 morphology 表。不要拿 RS/AR 的 MM partial send buf 去套 AG。

方向：

```text
AG:  comm → MM     producer = AllGather      consumer = Cube（远端 tile）
RS:  MM → comm     producer = Cube           consumer = ReduceScatter
AR:  MM → comm     producer = Cube           consumer = AllReduce
```

AR 和 RS 都是 **MM 在前**。AR 没有 AG 的本卡 shortcut。

---

## AR 不同的 completion morphology

AG/RS close-read 钉死的是 **handle-per-chunk**。AR **不是**那种形态。这正是补 AR 的理由：M1 的普遍规律不是 “一个 M-tile 一个 HCCL handle”。

| | AG native | RS native | AR native |
| --- | --- | --- | --- |
| HCCL 调用次数 | 每 tile 一次 `AllGather` | 每 tile 一次 `ReduceScatter` | Init **一次** `AllReduce(..., tileCnt)` |
| handle | 每 tile 一个 | 每 tile 一个 | **一个**（tail 再一个） |
| `<true>/<false>` | `<true>` launch | `<true>` launch | `<false>` Prepare |
| 循环里 | 无 Commit；留下 handle | 无 Commit；留下 handle | 每 turn `Commit` |
| Wait | `Wait(handle_i)` 各一次 | `Wait(handle_i)` 各一次 | `Wait(同一 handle)` × `tileCnt` |
| ownership | local/remote A shard → gathered input → full C | MM C chunk → RS input → rank-local C shard | MM partial chunk → AR input → full Y |
| 4P/8P trap | 无此分叉 | CCU 默认 sibling；**ai_cpu 强制 native** | **即使 ai_cpu 也走 sibling** |

因此：

- **普遍（M1）：** chunk ownership、async comm lifetime、local ready vs remote Wait。
- **特殊（AG/RS handle 形态）：** 一 tile 一 handle、循环里 `Collective<true>`、Wait 不同 handle。

AR 的 overlap 资格来自 **per-turn Commit + deferred Wait**，不是来自 “handle 数组”。不要把 RS Case 上的 handle 图解原样套到 AR。

AR canonical 无 add：Init / Commit / Wait 在 **AIC 0**。这是 AR member morphology，不是 M1 = AIC0。RS 上 used AIC 都会调 `ReduceScatter`。M2 的 communicator 在 **AIV 0**。

Cube 头文件：AR 走 `mat_mul_v3` 的 `Mc2MatmulAswKernel`，RS/AG 走 `mc2_mat_mul_asw_kernel.h`。**同一 compute baseline，不是第二条 M1。**

---

## 和 M2 的边界（只为防串）

M2 也有 `AlltoAll<false>` + Commit/Wait + `tileCnt`。不要因此把 AR 并进 M2。

| | M1 AR | M2 |
| --- | --- | --- |
| 调度 | AIC 循环直接 `Hccl::AllReduce` | `pipeline_builder` 三阶段 |
| 中间阶段 | 无 AIV permute | AIV `MC2VecTranspose` |
| orchestration | AIC-side（本路径 Init/Commit/Wait 在 AIC 0） | AIV 0（`AICSync=false`） |
| collective | AllReduce repeats | AllToAll taskIndex |

Commit≠visibility 是两条都成立的 **progress vs gate**，不是把它们合成一个 family。

---

## Case / Knowledge

**不要**为 AR 再开一张 Case。第三个 collective 名不是新机制。RS Case 已经教 MM→comm + deferred Wait；AG 已是 reverse sibling。AR 的增量是 **repeat-AllReduce morphology**，放进 Knowledge 比再开卡合适。

Knowledge：[mc2-compute-collective-coupling](../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）。handle-per-chunk 是 AG/RS 特例，不是 M1 通例。
