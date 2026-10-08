# Verdict: vLLM P1-next compress epilog

Focused close-read after P3 FREEZE. **Not a Case. Not Knowledge.**

证据：[compress_epilog_a5.md](compress_epilog_a5.md)

```yaml
status: verdict-written
review: passed
result: A                         # existing-pattern-composition；不开卡
new_case: none
new_knowledge: none
classification: existing-pattern-composition
references:
  - knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md  # paged-addressing only
not:
  - indexer Case
  - second Sparse Case
  - 53 / C clone
  - compressor / store_kv_block walk
  - P3 Knowledge filling P1 runtime-live
  - FA Knowledge teaching the full epilog
```

`classification` 不是 “FA Knowledge 完整教学该 op”。  
FA Knowledge 吸收 **paged-addressing** 部分；quant + scatter 属于无需单独资产化的融合包装。

---

## 结果

**结果 A。** A5 `indexer_compress_epilog_v2` / `kv_compress_epilog` 是 **quant + physical-slot scatter** 的融合包装，不是新的 paged-cache write family，也不是 Indexer selection。

```text
Indexer            = 谁被选中？
Compress epilog    = 数据怎么量化并写进后续要读的 cache？
```

两条证据链继续独立。P3 不能替 P1 补 runtime-live。也不用 epilog 去填 Indexer Case。

API handoff 被压缩 ≠ 新的 execution family。

---

## 四问（短答）

1. **Materialize** — token-row `x`（K_index 或 KV / compressed-KV），group FP8/MXFP8，scatter 进 paged cache。不写 raw hidden。不产 top-k。
2. **Coordinate** — kernel 输入是 Host 算好的 **physical `slot_mapping`**（`-1` skip）。`block_table` 在 Host `BlockTable.compute_slot_mapping`，不进 kernel。
3. **Fusion** — 删掉 quant 中间 tensor、第二条 scale scatter（Indexer Part3 no-op）。不是新的 ownership / visibility 协议。
4. **Absorb** — FA Knowledge 吸收 paged-addressing 部分；quant + scatter 无需单独资产。53/C 的 MX 是 GEMM 机制，这里只是 Vector cache-write quant 形态；Indexer 是另一份 job。

没有出现需要单独开卡的：

```text
quantize + indexed paged-cache materialization
    as a new ownership / scheduling mechanism
```

---

## 不要做

- 把 epilog 写进 Indexer Case。
- 把 `slot_mapping` 写成 Sparse `select_idx`。
- 把 MXFP8 group-64 cache write 升级成第二张 53。
- 展开 `compressor` / `store_kv_block` / 整条 KV-cache。
- 把 `_C_ascend` dispatched 写成 A5 observed。
- 为了把 candidate 全部涂绿继续深挖。

---

## Status

```text
vllm-ascend Source 4

Scout                              ✅ FREEZE

P1 Indexer
  runtime paths                    ✅
  selection contract               ✅
  mechanism gate                   ✅
  canonical-instance gate          ⏸ explicit pending
  Case                             ⏸ 不写

Sparse consumer                    ✅ no second Case

P2 MoE / MC2                       ✅ absorbed / FREEZE

P3 custom-op surfaces
  Knowledge                        ✅ accepted
  phase                            ✅ FREEZE

P1-next compress epilog
  close-read                       ✅ PASSED
  verdict                          ✅ result A
  Case / Knowledge                 ❌ none

Source 4                           ✅ FREEZE
```

唯一显式 leftover：P1 indexer canonical runtime-evidence pending。不要再扫本仓其它 candidate 只为涂绿。
