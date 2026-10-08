# ops-transformer Scout notes

Status: **source frozen** (2026-08-21). Do not reopen MoE close-reads, Cases A–D, Layer 1/2, MC2 first phase, FIA contrast, or GMM finalize-routing contrast unless Review reopens them. Next work is a **different source**.

## Identity

- Official SoT: `cann/ops-transformer`. This scan used `/home/wangyibo/ops-transformer_bak` (fork `wang_yibo/ops-transformer`, master `9311b63bc`). `upstream_presence.verified` remains **false**.
- Catalog schema `2.1-ops-transformer-scout`. Extra field `line`: `frozen-done` | `backlog`. Do not run `tools/build_catalog.py`.

## Frozen (do not regress)

Split pipeline, not InitRouting → Dispatch → GMM → Combine → Finalize:

- Case A: Gating → InitRouting → expert compute → Finalize. Map: `expanded_row_idx`.
- Case B: Gating → DispatchV2 → expert compute → CombineV2. Map: `assist_info`. Combine already unpermutes; EP path does **not** then call Finalize.
- Case C: GMM SwiGLU MX as 60+53 production extension.
- Case D / MegaMoe: fused EP+FFN; compute-communication, not a fourth GEMM Case.

Layer 1: `knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md`  
Layer 2: `knowledge/engineering-experience/moe-fusion-materialization-and-sync.md`

## Catalog after freeze

| id | status | line |
| --- | --- | --- |
| opsxf-moe-gating-topk | **curated** (folded into A) | frozen-done |
| opsxf-moe-init-routing | **curated** (Case A) | frozen-done |
| opsxf-moe-distribute-ep | **curated** (Case B) | frozen-done |
| opsxf-moe-finalize-routing | **curated** (Case A) | frozen-done |
| opsxf-gmm-swiglu-mx | **curated** (Case C) | frozen-done |
| opsxf-mega-moe | **accepted** (Case D) | frozen-done |
| opsxf-gmm-grouped-matmul | candidate | **backlog** |
| opsxf-gmm-finalize-routing | **no-case** (contrast passed) | frozen-done |
| opsxf-fa-fused-infer | **no-case** | contrast-done |
| opsxf-deepep-a3 | candidate | **backlog** (950 DeepEP lives in sgl-kernel-npu) |
| opsxf-moe-routing-map | candidate | **backlog** |

## What was *not* done on this source

- No kernel walk of MLA / cgmct StreamK live path as a Case.
- No GMM `FullLoadA` / `KernelMultiBlockStreamK` as shipped 950 default.
- GMM+finalize-routing fused：**contrast passed**；absorb 60/A/C/53；**no Case**.
- MC2 M3：**not read**.

## Next

**Stop this source.** Switch to cann-ops-adv or RecSDK / DrivingSDK. Do not squeeze more ops-transformer (no M3, no another Attention family).
