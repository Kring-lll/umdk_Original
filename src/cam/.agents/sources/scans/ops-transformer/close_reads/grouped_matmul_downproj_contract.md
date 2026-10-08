# GroupedMatmul down-proj contract (GMM2)

Status: **interface footnote of Case C**, not a fourth Case Card.

Focused interface read. **Not** a kernel close-read of `grouped_matmul`. Does not re-teach Case 60/53.

Question: after `GroupedMatmulSwigluQuantV2`, which production GMM is the down-proj, and can its `y` be CombineV2 `expand_x`?

```yaml
source_identity:
  registry_ref: ops-transformer
  scanned_commit: 9311b63bcd065b9779eb08a7d634e9463ee41ef1
  scanned_branch: master
  scan_path: /home/wangyibo/ops-transformer_bak
  producer: gmm/grouped_matmul_swiglu_quant_v2/
  consumer: gmm/grouped_matmul/
  next: mc2/moe_distribute_combine_v2/
  catalog_ref: opsxf-gmm-grouped-matmul
  upstream_presence:
    verified: false
    upstream_commit: null
```

No in-tree graph that calls SwiGLU-quant V2 → GroupedMatmul → CombineV2. Edges below are API/layout, not a proven framework sequence.

---

## 1. Which production op consumes GMM1 `y` / `y_scale`?

**`gmm/grouped_matmul/`** (plain GroupedMatmul). Catalog already listed this as the production 60 counterpart. It is the only 950 M-group GEMM in this scan that:

- takes a single `x [M, K]` plus `groupList`
- accepts the **quantized** dtypes GMM1 emits (INT8 / FP8 / HiF8; MX e8m0 scales via `perTokenScale` / `scale`)
- can emit **fp16 / bf16** `y` (attr `dtype` 0 / 1 in `GMM_OUTPUT_DTYPE_MAP`)

Not: another SwiGLU-quant V2 (that is GMM1+activation again). Not: `grouped_matmul_add` (K-group). Not: `grouped_matmul_finalize_routing` (that fuses scatter-add toward token-major — EP uses Combine instead).

| GMM1 tensor | GMM2 input |
| --- | --- |
| `y [M, N/2]` quant | `x` (`groupType=0` M-group). K of GMM2 = GMM1’s `N/2`. |
| `y_scale` | `perTokenScale` (dynamic / MX) and/or the matching scale path. **Not** combine `expert_scales`. |
| `group_list` | **same buffer** — see §2 |

---

## 2. Same `group_list`?

Yes, **if the caller wants the same local-expert M slices**.

- GMM1: required `group_list` INT64 `[E]`, `group_list_type` 0=cumsum / 1=count.
- GMM2: optional `groupList` INT64; for **M-group single-x** (`groupType=0`, splitItem 2/3) it is **required**. Same 0/1 type (`grouped_matmul_def.cpp`: “cumsum or count”, default **0**).

Conditions (same as dispatch→GMM1):

- `E` = local experts (length match)
- `group_list_type` equal on GMM1 and GMM2
- `groupType=0` (M). `groupType=2` is K-group — **wrong axis** for expert-token rows.

GMM2 does **not** rewrite the list. Host does not convert count↔cumsum.

---

## 3. M row order?

**Preserved.** M-group GMM slices contiguous rows of `x` by `groupList`; it does not permute tokens. `y[m, :]` is the down-proj of `x[m, :]`.

So the local-expert-major order from Dispatch `expand_x` survives GMM1 **and** GMM2. Combine’s reverse map (`assist_info` triples) still indexes those rows.

---

## 4. Output dtype / width — can it be CombineV2 `expand_x`?

| check | GMM2 | Combine `expand_x` |
| --- | --- | --- |
| rank | 2-D `[M, N]` when `groupType=0` single-y | `[A, H]` |
| M vs A | same M if no drop of tail groups | infershape `A` from dispatch; must be `M == A` |
| N vs H | down-proj `N` must equal hidden `H` | `expand_x.dim1` |
| dtype | fp16 (`dtype=0`) / bf16 (`dtype=1`) / also int32/fp32/int8 | README **fp16 / bf16** |

**Verdict: conditional-direct** when:

- `groupType=0`, same `group_list` / type as GMM1
- `M == A`, `N == H`
- output dtype is fp16 or bf16
- `actType=0` (SwiGLU already happened in GMM1)

**Not** a direct-data-edge from GMM1 `y` (quant, width `N/2`). That stays **logical-edge** (E17).

If GMM2 `dtype` is int32 / int8, combine’s main path does not take it. Quant combine (`comm_quant_mode`) is a different branch — not claimed here.

---

## Chain this file closes

```
dispatch.expand_x + expert_token_nums
        ↓  (E14, E16)
GMM1 SwiGLU-quant  y, y_scale, same group_list
        ↓  this contract
GMM2 GroupedMatmul  y [M, H] fp16/bf16
        ↓  conditional-direct
combine.expand_x
```

Shared-expert ranks: `localExpertNum` may be 1; `E` must still match that rank’s groups, not global expert count.
