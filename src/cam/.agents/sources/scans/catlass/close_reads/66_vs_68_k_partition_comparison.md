# 66 vs 68: K-partition comparison

Status: **reviewed (passed)**. Cases + index exist. Cards: `ascend950-streamk-matmul` + `ascend950-multi-core-splitk-matmul`. Index: `knowledge/engineering-experience/gemm-k-partition-strategies.md`.

Pair close-reads:

- `66_ascend950_streamk_matmul.md`
- `68_ascend950_multi_core_splitk_matmul.md`

Golden MN baseline: `cases/compute/gemm/ascend950-basic-matmul/` (43).

Evidence kinds: `source-code` | `design-doc` | `test` | `benchmark` | `inference`.

No `benchmark` numbers are attached to 66 or 68. README / practice-doc predicates stay `design-doc`.

---

## Shared vs different (relative to 43)

Both 66 and 68, live examples (`source-code`):

| | 43 | 66 StreamK | 68 SplitK |
| --- | --- | --- | --- |
| Block | `BlockMmadTla` pingpong | same | same |
| Policy | UnitFlag, L1/L0 2/2, L0C 1 | same (3-arg defaults) | same |
| fp32 tiles | 256/256/128 · 256/256/32 | same | same |
| Layout | RowMajor | same | same |
| Bias | void | void | void |
| `CanImplement` | true | true | true |
| Host AIC | `min(C, taskNum)` | **all C** | **all C** |
| Binary | **cube**, AIV empty | **mix** | **mix** |
| Workspace | 0 | per-AIC 2×`L1_M×L1_N` acc, min 2 MiB | `M×N×acc×factor`, min 2 MiB |
| Who writes GM C | AIC Fixpipe | AIC (normal tiles) + **AIV** (streamk tiles) | **AIV only** |
| Scheduler | `GemmIdentityBlockSwizzle<3, dir(M>N)>` | `Streamk…<3,0>` | `Splitk…<3,0>` |

Cube occupancy math is 43’s. Do not re-teach pingpong here.

Catalog vs live (both 66 and 68): catalog `fp16` + `cube`; examples `fp32` + `mix`. Record in the pair close-reads.

---

## K partition in one table

| | 43 MN tiling | 68 SplitK | 66 StreamK |
| --- | --- | --- | --- |
| Which K | not split across cores; one core walks full K | **every** MN tile’s K, into `splitkFactor` slices | **only** `B % C` tail MN tiles’ K; complete waves full-K |
| Split granularity | — | packs of L1-K, evened across factor | packs of L1-K, evened across **all** AIC |
| Partial location | none (C in GM) | workspace plane `s` = full `M×N` acc | workspace slots `2*aic` and `2*aic+1` (`L1_M×L1_N` each) |
| Reduce | none | AIV `SplitkReduceAdd`, **all** `M*N` | AIV inline Add, **only streamk MN tiles** |
| AIC→AIV sync | n/a | flag **after all** Cube work | flag **early** on streamk cores (after swapped streamk iter); idle-of-streamk cores flag at entry |
| Cube / Vector overlap | n/a | not in this source | loop-index swap enables AIV start while a streamk core still has a normal tile (`source-code`; “for overlap” = **inference**; stated as design for example 37 `design-doc`) |

---

## Trigger conditions (do not mix them)

Let `B = CeilDiv(M,m1)*CeilDiv(N,n1)`, `C = aicCoreNum`.

| Situation | What the **docs** say | What the **binaries** do |
| --- | --- | --- |
| `B` large, `B % C == 0` | 43 / Common. StreamK “usually unnecessary” (`design-doc` practice + 66 README inverse) | 66 still runs StreamK with `streamkBlocks=0` (no K split, AIV loop empty). 68 still SplitKs with `factor=min(2,kTileNum)` if `B>C`. |
| `B > C` but `B % C != 0` (tail round) | **StreamK** if also `B/C > 1` and `B%C <= 0.8C` (66 README, `design-doc`) | 66 implements that structure. 68 would still slice **all** tiles, paying full-C workspace+reduce. |
| `B <= C/2` (and K large enough) | **SplitK** (68 README, `design-doc`) | 68 `GetSplitkFactor ≈ min(C/B, kTileNum)`. 66 treats **all** MN tiles as streamk (`normalBlocks=0`). |
| `B < C` but `> C/2` | practice: consider smaller MN tiles first; SplitK docs use `<= C/2` | neither example gates |
| Short K (`kTileNum` small) | practice: SplitK/StreamK may lose to reduce cost (`design-doc`) | 68 factor capped by `kTileNum`; 66 may leave AIC with 0–1 L1-K |

**Neither example contains a selector.** Picking 66 vs 68 vs 43 is a host/product decision, not a `CanImplement` result.

Shipped shapes (`source-code` / `test` / README CLI):

| | M N K | `B` with 256×256 tiles | README window |
| --- | --- | --- | --- |
| 66 CLI + optest MN | 256×512, K=1024 or 3079 | **2** | StreamK wants `B/C>1`. This `B` is SplitK-shaped. |
| 68 CLI + optest | 256×512×1024 | **2** | Matches `B <= C/2` for typical `C>=4`. |

66’s **implementation** is tail-wave StreamK. 66’s **shipped problem** has zero complete waves. Do not use `256 512 1024` as a picture of “last round of a large GEMM.”

---

## Agent question: MN tiling, SplitK, or StreamK?

Decision order, sourced from 43 + these kernels + CATLASS practice docs. Steps 1–2 are `source-code` occupancy; steps 3–5 are `design-doc` heuristics unless a later Case adds `benchmark`.

```text
1. Start from 43.
   One MN tile = one AIC walking full K. Workspace 0. Cube-only if AIV is empty.

2. Count B and kTiles with the live L1 tile (here 256×256, k1=128 fp32 example).
   Occupancy must still fit L1/L0/L0C (43 formulas). Shrinking m1/n1 to
   manufacture blocks is a different knob; practice says try that before
   K-split when B is tiny (design-doc).

3. If B >= C and B % C == 0:
      stay on MN tiling (43). StreamK adds mix launch + AIV for no tail.
      SplitK writes extra workspace planes for every tile.

4. If B <= C/2 and kTiles >= 2:
      SplitK (68) is the matching CATLASS template.
      Cost: workspace M×N×acc×factor; AIV reduce of the whole C;
      AIC never Fixpipes to C.

5. If B > C and B % C != 0 (especially B%C <= 0.8C, and B/C > 1):
      StreamK (66) is the matching CATLASS template.
      Cost: 2×L1 tile acc per AIC; AIV reduce of the tail MN tiles only;
      complete waves still 43-style to GM C.

6. If B < C but you wanted StreamK because “K is large”:
      that is 68’s problem, not 66’s README. 66 will mark every MN tile
      as streamk (normalBlocks=0) and still help, but workspace/reduce
      shape is StreamK’s 2-slot-per-core, not 68’s full planes.
      Do not name it SplitK.

7. If kTiles is 1:
      neither K-split creates extra Cube work. Stay on 43 or change the tile.
```

**Wrong aliases to stop:**

- “StreamK = SplitK with a fancier name.” False (`source-code` schedulers).
- “StreamK splits all K.” False; only `B%C` tiles.
- “SplitK only splits the tail.” False; every MN tile.
- “README predicate is compiled in.” False.
- “66 CLI is a StreamK-regime shape.” `B=2` is not `B/C>1`.

---

## Case vs Reference vs Knowledge

### Judgment

| Artifact | Verdict |
| --- | --- |
| 66 | **Independent Case**, lead of the pair. Distinctive: tail-only K scatter, 2-slot workspace, inline AIV reduce, early flag + loop swap. |
| 68 | **Independent sibling Case**, not a footnote and not a second 43. Distinctive: `GetSplitkFactor`, full `M×N` planes, `SplitkReduceAdd`. Weaker than 66; still required so Agents do not StreamK a `B<=C/2` problem (or SplitK a fat tail). |
| One Case + one Reference | Acceptable **only if** 68 is a short Reference attached to 66 **and** the knowledge file exists. A 66-only Case will be copy-pasted onto small-MN GEMMs. |
| Two unrelated golden Cases | **No.** Cube Block is 43; splitting them in the catalog without a joint decision doc repeats 43 twice. |

Recommended ingest order (not done this batch):

1. Knowledge file (index / decision).
2. 66 Case Card (StreamK live path only).
3. 68 Case Card (SplitK live path only), `competes_with` 66 `sibling` / `selected`.
4. Both Case Cards point at 43 for pingpong and at the knowledge file for “which one.”

Do **not** open 69 (tail multi-core SplitK) or 37 (A2 StreamK) in the same batch. 66 README’s `37_ascend950_tail_multi_core_splitk_matmul` string is an unresolved naming clash (`design-doc` vs tree).

### Proposed Engineering Knowledge (do not create the file yet)

Path: `knowledge/compute/gemm/gemm-k-partition-strategies.md`

Audience: Agent choosing a CATLASS-style GEMM parallel axis on Ascend 950.

Must contain:

1. Definitions: MN tile `B`, core `C`, L1-K `kTiles`, tail `B%C`.
2. The decision tree above, with evidence kinds on every predicate.
3. Workspace / reduce cost **formulas** from 66/68 source, not performance claims.
4. Explicit “43 vs 66 vs 68” table (who writes C, what is split).
5. What NOT To Copy: README heuristics, catalog fp16, JIT K scaler, 192 KiB reduce constant, example 37 types.
6. Pointers: Case 43; future Cases 66/68; non-goals 09/22/34/52/69.
7. Open conflicts left unresolved: example vs JIT K; 66 CLI vs StreamK README window.

Must **not** contain: invented speedups; a compiled selector presented as CATLASS runtime; dualDst / 46; GMM / 60.

---

## Open pair-level conflicts (do not resolve)

1. Catalog `fp16`/`cube` vs live `fp32`/`mix` for both IDs.
2. Example L1-K 128 vs optest scaler L1-K 64 on fp32.
3. 66 README heuristic vs 66 CLI `B=2`.
4. 66 README contrast name vs examples 37 and 69.
5. `SplitkReduceAdd` 192 KiB vs 950 UB 248 KiB.
6. Practice-doc StreamK overlap story written for example 37; 66 has the swap in source without that README sentence.
