# MC2 ↔ existing Cases / Knowledge

防重复 Case。Scout only。Identity：ops-transformer `9311b63bcd065b9779eb08a7d634e9463ee41ef1`。

**冻结：** GEMM 六卡、MoE A–D、Layer 1/2、Sparse Case、FA Knowledge。本文件不重教它们。

核心误区：

```text
里面用了 Matmul + 通信
    ≠ 既是 GEMM Case 又是通信 Case
```

真正要找的是 **collective 与 Matmul 的耦合方式**。

---

## vs 43 / 66 / 68（以及 53 / 73 / 60）

| 看到 | 不要写成 | 要写成 |
| --- | --- | --- |
| `mc2_mat_mul_asw_kernel.h` / `mat_mul_v3` ASW | 新的 Basic GEMM Case | **compute baseline = 43**；教学增量在 coupling / ownership |
| Cube M/N tile、`UpdateSlice` | 66 StreamK / 68 SplitK | MC2 的 **M-tile 是 comm 切片**，不是 remainder-K / every-MN-K |
| MX / FP8 出现在 AG/AR/RS | 第二张 53 | 同一 MX Block 上的 comm 路径；先教 fp16/bf16 耦合 |
| grouped A2Av+GMM | 第二张 60 或第二张 C | GMM **加上** AllToAllV；默认不是第一张 MC2 |

Cube tiling 或 K partition 相同 ≠ 新 GEMM Case。

---

## vs Layer 2 MoE fusion

都涉及 fusion / visibility。**dataflow family 不同。**

| Layer 2（MegaMoe / fused A5） | MC2 matmul-collective |
| --- | --- |
| 已经能接线的 **EP + expert FFN** 被压进一个 op | **TP/SP 风格** shard × collective × MM |
| dest-push / dest-pull window、token/expert 坐标 | M1–M3：HCCL primitive + Cube tiles；M4 才是 Win |
| 问：哪些 API tensor 被删、哪些 barrier 不能删 | 问：哪一块 C 何时对哪些 rank 可见 |

可以 **引用** Layer 2 三问（materialization / relocated metadata / visibility）。  
不要把 MC2 写进 MoE Knowledge，也不要为 MC2 先写第三份 MoE 文。

---

## vs Case B / D

HCCL / window / status 出现 ≠ EP routing。

| | Case B / D | MC2 AG/RS/AR/A2A+MM |
| --- | --- | --- |
| job 名 | README AllToAllV | AllGather / ReduceScatter / AllReduce / AllToAll |
| 950 搬运 | **MTE window** dest-push（B）/ dest-pull（D） | **`Hccl::` primitive** + `Wait`/`Commit`（quant-only 才是 Win） |
| 载荷 | token + `assist_info` / expert-major | A/B/C 矩阵 shard |
| 第一张 MC2 | 不要从 `moe_distribute_*` 再开 | 不要从 grouped A2Av 开第一张（太像 EP） |

`quant_reduce_scatter` / `quant_all_reduce` 的 window 读远端，**像** B 的引擎，**不是** B 的坐标。本 Scout 把它们标成 M4，不进第一张卡。

---

## vs FA Knowledge

Cube–Vector pipeline（QK / softmax / PV / O-merge）**不相关**。

MC2 的 AIC↔AIV `CrossCoreSetFlag` 是同设备 ordering，不要写成 FA 的 online-softmax 状态机。

---

## 本 Scout 自己的重复

| 不要 | 原因 |
| --- | --- |
| v1 `all_gather_matmul` / `matmul_reduce_scatter` 再开卡 | 950 不是 live；用 v2 |
| AG、RS、AR 各一张 | AG/RS/AR 同属 **M1 chunk coupling**；第一张 + contrast + AR morphology，不要三张 |
| `allto_all_matmul` 与 `matmul_allto_all` 各一张 | 同属 **M2**，顺序相反 |
| 四张 grouped A2Av/quant | 同属 **M3**，且 EP-adjacent |
| `examples/mc2/all_gather_add` Case | 非 950、非 Matmul |

理想形态（Case 已写，Knowledge `accepted`）：

```text
1 张 canonical MC2 Case（RS native, accepted）
      ↓
第二张（M2 A2A pipeline, accepted）
      ↓
AR close-read + M1 verdict（member，不开 Case）
      ↓
MC2 Knowledge（四条 invariant；accepted）
```

---

## Pointers

- Inventory：[mc2_family_inventory.md](mc2_family_inventory.md)
- Catalog：[mc2_candidate_catalog.yaml](mc2_candidate_catalog.yaml)
- Layer 2：[moe-fusion-materialization-and-sync.md](../../../../knowledge/engineering-experience/moe-fusion-materialization-and-sync.md)
- Case B：[ascend950-moe-ep-dispatch-combine](../../../../cases/compute-communication/moe/ascend950-moe-ep-dispatch-combine/case.md)
- First MC2 Case：[ascend950-matmul-native-reduce-scatter](../../../../cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md)（accepted）
- Second MC2 Case：[ascend950-alltoall-matmul-pipeline](../../../../cases/compute-communication/mc2/ascend950-alltoall-matmul-pipeline/case.md)（accepted）
- M2 close-read：[mc2_m2_a2a_mm.md](close_reads/mc2_m2_a2a_mm.md)
- Knowledge：[mc2-compute-collective-coupling](../../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)（`accepted`）
