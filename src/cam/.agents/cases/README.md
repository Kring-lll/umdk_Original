# Cases

`cases/` 是 **机制样例**，不是模板复制源。`metadata.yaml` 里的 `accepted` / `curated` / `reviewed` **都是 live teaching**；只有 `under-review` 不是。检索时不要只搜 `accepted`。

完整盘点见 [CURATION_STATUS.md](../CURATION_STATUS.md)。下面只回答：什么时候读、什么时候不要读。

## GEMM

| Case | 什么时候读 | 不要读成 |
| --- | --- | --- |
| [Basic Matmul](compute/gemm/ascend950-basic-matmul/case.md) | 950 Cube pingpong 数据路径 | 普通 Vector 的 tile 常数 |
| [StreamK](compute/gemm/ascend950-streamk-matmul/case.md) | 核不够时 remainder MN tiles 的二阶段 | 所有大 K 都 StreamK |
| [SplitK](compute/gemm/ascend950-multi-core-splitk-matmul/case.md) | 核间切 K | Vector reduce 也切 K |
| [FullLoadA](compute/gemm/ascend950-matmul-full-loadA/case.md) | full-K A load + 跨 N 复用 | “L1 resident A” 口号 |
| [MXFP8](compute/gemm/ascend950-fp8-mx-matmul/case.md) | MX 量化 MatMul 路径 | 所有 FP8 都走 MX |

索引：[GEMM K-partition](../knowledge/engineering-experience/gemm-k-partition-strategies.md)

## Grouped GEMM / MoE compute

| Case | 什么时候读 | 不要读成 |
| --- | --- | --- |
| [GMM Slice-M](compute/grouped-gemm/ascend950-grouped-matmul-slice-m/case.md) | group 维跟着 M 走；prefix-sum 减法 | 普通 Add 需要 `groupList` |
| [GMM+SwiGLU+MX](compute/grouped-gemm/ascend950-gmm-swiglu-mx-quant/case.md) | 专家 GMM1 融合 + 谁生产 groupList | 又一张基础 GEMM |

## MoE

| Case | 什么时候读 | 不要读成 |
| --- | --- | --- |
| [Local permutation](compute/moe/ascend950-moe-local-permutation/case.md) | 单卡 token↔expert 行重排 | EP 通信协议 |
| [EP Dispatch/Combine](compute-communication/moe/ascend950-moe-ep-dispatch-combine/case.md) | EP loop、window、post-EP nums | 融合后仍有同样的 API-visible handoff |
| [MegaMoe fused EP+FFN](compute-communication/moe/ascend950-mega-moe-fused-ep-ffn/case.md) | 哪些 GM handoff 被 deleted/relocated | 把 split 契约写回融合核 |

契约：[token-expert-rank](../knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md)、[materialization](../knowledge/engineering-experience/moe-fusion-materialization-and-sync.md)、[sufficient-state](../knowledge/engineering-experience/communicate-sufficient-state.md)

## Attention

| Case | 什么时候读 | 不要读成 |
| --- | --- | --- |
| [Block-sparse paged FA](compute/attention/ascend950-block-sparse-paged-attention/case.md) | `select_idx` 驱动的 page selection | 所有 FA 都有这套 selection |

流水：[FA pipeline](../knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md)

## MC2

| Case | 什么时候读 | 不要读成 |
| --- | --- | --- |
| [Matmul → ReduceScatter](compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md) | 通算耦合、compute 与 collective 的 visibility | 普通 Vector 也先走卡间 |
| [AllToAll → Matmul](compute-communication/mc2/ascend950-alltoall-matmul-pipeline/case.md) | A2A 与 MM 的 overlap 形态 | 和 MoE window 是同一套协议 |

索引：[MC2 coupling](../knowledge/engineering-experience/mc2-compute-collective-coupling.md)
