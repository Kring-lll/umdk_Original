# AscendWeaver curation status

Frozen snapshot **2026-08-21**。

**Attention 线 FREEZE**（Sparse **consumer-side** teaching / FA Knowledge / 49 / FIA）。不要回改。  
Indexer 作为 **selection producer** 是 Source 4 P1，不是把 Sparse Case 重开一张。  
**MC2 第一阶段 FREEZE**（RS Case / M2 Case / AR close-read+verdict / MC2 Knowledge `accepted`）。不开 AG/AR Case。不要读 M3。  
**ops-transformer FREEZE**（GMM finalize-routing contrast `review: passed`，结果 A，不开 Case）。不要再榨这个仓。

**vllm-ascend Source 4 FREEZE**（Scout / P2 / P3 / compress epilog）。不要再修 Scout 四件套，不要为涂绿继续深挖。  
P1 indexer：**mechanism passed**；**canonical runtime-evidence explicit pending**；**不写 Case**。P3 Knowledge **accepted**。Compress epilog **结果 A**（existing-pattern-composition）。  
NOW：Source 4 已收敛。唯一 leftover = P1 indexer canonical `.so` / OPP resolve。不要开第五源，除非另起一轮。

本文件是当前 Case / Knowledge **coverage map**。不替代 Case Card，不替代 catalog。  
原则已经在第三源兑现：**新仓库中的新实现 ≠ 新的工程机制。**

```text
Sources scanned
  1. catlass            → 6 GEMM/GMM Cases + 2 Knowledge
  2. ops-transformer    → 4 MoE Cases + 2 Knowledge; 0 new GEMM Case
                          FIA 950 = FA-pipeline peer, no Case
                          MC2 matmul-collective = 2 Cases accepted（RS + M2）；AR = M1 member，no Case
                          MC2 Knowledge accepted
                          GMM finalize-routing = 60/A/C/53 吸收，no Case
                          **source frozen**
  3. sgl-kernel-npu     → 1 Attention Case + FA Knowledge（accepted）
                          DeepEP split/fused absorbed as B / D alternatives
  4. vllm-ascend        → **source frozen**
                          Scout / P2 / P3 / compress-epilog FREEZE
                          P1 indexer canonical runtime-evidence **explicit pending**
                          Knowledge: custom-op-delivery-call-and-selected-path
                          compress epilog = existing-pattern-composition（no Case）

Live teaching assets
  13 Cases
   7 Knowledge
```

---

## Attention freeze（不要再动）

```text
FA pipeline Knowledge     accepted
  knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md

Block-sparse selected-page policy
  → Sparse Case，仍是 selection SoT
  → coverage: single-source
  → 不要把 Sparse Case 自身改成 cross-validated

FA cross-KV-tile pipeline
  → cross-validated Knowledge
  → Sparse + CATLASS 49 + FIA 950
  → 不含 select_idx；不含 PV-lag-QK；不含 FIA 独立 rescale-O

CATLASS 49                contrast done / no Case
ops-transformer FIA       contrast done / no Case

Contrast
  sources/scans/attention-cross-source/sparse_vs_49_vs_fia.md
  review: passed
```

`select_idx` 驱动的 non-contiguous page selection 目前仍只有 SGLang 这一张 Case。被跨源验证的是更高层 FA pipeline，不是 Sparse selection。

---

## Status vocabularies

不要把两套标签混成一张表。

**Case file `status`**（写在 `metadata.yaml` 里，历史用词不统一）：

| value | 本盘点怎么读 |
| --- | --- |
| `accepted` | 内容 Review 通过，并完成 **该 Case 自己定义的** acceptance gates。是否要求 upstream identity verify，看各卡 `metadata.yaml`，不倒逼历史卡。Sparse Attention 要求 `sgl-project` verify；MegaMoe 与 **MC2 native RS 不要求** upstream verify（fork SoT）；M2 Case 同样 **不要求** |
| `under-review` | 卡已写，等人 Review；**还不是** live teaching |
| `curated` / `reviewed` | 仍是 **live teaching Case**，不是 backlog。CATLASS 与 MoE A–C 用的是这套词 |

**`coverage_status`**（本文件机制层）：

| tag | 含义 |
| --- | --- |
| `canonical` | 已有稳定 teaching Case；新 source 默认只做 alternative / wrapper 证据 |
| `cross-validated` | 第二 source 已验证同一机制（或同一 Knowledge 分类仍成立） |
| `single-source` | 只有一张（或同一 source 的一组）Case；暂不抽 Knowledge |
| `candidate` | catalog / close-read 有，未成 Case |
| `no-case` | 对照后明确 **不开 Case**（peer / alternative 证据） |
| `deferred` | 已知存在，当前不扫；950 CMake 关掉、非 live path、或排在对照之后 |
| `scouting` | family Scout 进行中；**还没有** Case |

---

## 1. Case inventory

| ID | pattern | source | hw | file status | relation | teaching point |
| --- | --- | --- | --- | --- | --- | --- |
| `ascend950-basic-matmul` | Basic GEMM pingpong | catlass **43** | 950 | curated | **baseline** | CATLASS 分层；L1/L0 pingpong；实例化 ≠ 模板全集 |
| `ascend950-grouped-matmul-slice-m` | Grouped GEMM slice-M | catlass **60** | 950 | curated | **baseline** | irregular `groupList` → regular Cube tiles；prefix-sum 减法 |
| `ascend950-streamk-matmul` | StreamK | catlass **66** | 950 | reviewed | 43 **extension** | 只切 remainder MN tiles 的 K + AIV reduce |
| `ascend950-multi-core-splitk-matmul` | SplitK | catlass **68** | 950 | reviewed | 43 **extension** | 每个 MN tile 切 K + workspace C planes |
| `ascend950-fp8-mx-matmul` | MX GEMM | catlass **53** | 950 | reviewed | 43 **Block variant** | `MmadMx`；e8m0 group=32；不是又一张 Basic GEMM |
| `ascend950-matmul-full-loadA` | Full-K A + cross-N reuse | catlass **73** | 950 | reviewed | 43 **extension** | A full-K load；`CanImplement` 真实但非全部约束 |
| `ascend950-moe-local-permutation` | Local permute | ops-transformer **A** | 950 | curated | **sibling loop** of B | token-major ↔ expert-major；`expanded_row_idx`；Init **不是** Dispatch 前驱 |
| `ascend950-moe-ep-dispatch-combine` | EP dispatch/combine | ops-transformer **B** | 950 | curated | **canonical EP loop** | dest-push MTE window；`assist_info` triples；post-EP `expert_token_nums` |
| `ascend950-gmm-swiglu-mx-quant` | GMM1+SwiGLU+MX | ops-transformer **C** | 950 | curated | 60+53 **production extension** | MIX AIC GMM + AIV epilogue；谁生产 groupList；GMM1 `y` ≠ Combine payload |
| `ascend950-mega-moe-fused-ep-ffn` | Fused EP+FFN | ops-transformer **D** | 950 | accepted | B+C **compression** | API-visible handoff 内化；dest-**pull**；world sync 语义不能删 |
| `ascend950-block-sparse-paged-attention` | Block-sparse paged FA | sgl-kernel-npu | 950 | accepted | **new family** | `select_idx`→page→selected QK；running online-softmax + delayed PV + rescale-O |
| `ascend950-matmul-native-reduce-scatter` | Chunked MM→native RS | ops-transformer **MC2 M1** | 950 | **accepted** | **new family** | Cube C-tile → `Hccl::ReduceScatter`；per-tile Wait deferred；**RSᵢ overlaps MMᵢ₊₁**；compute baseline = 43 |
| `ascend950-alltoall-matmul-pipeline` | A2A + AIV permute + MM | ops-transformer **MC2 M2** | 950 | **accepted** | **new family** | 三阶段 pipeline；Commit=progress；Wait=consumer gate；**不是** M1 换名；RS = M1 mechanism contrast |

**Absorbed, no Case**：

| implementation | verdict |
| --- | --- |
| sgl DeepEP A5 split `Buffer.dispatch/combine` | Case **B alternative**（dest-push 没变；packaging / serving wrapper） |
| sgl `fused_deep_moe` A5 | Case **D alternative**（fused EP+FFN；内层是 B-style dest-push，不是 MegaMoe dest-pull）；Layer 2 taxonomy 仍分类，不新建 Knowledge |
| sgl CATLASS wrappers | **43 clone**；禁止再开 Basic/GMM/StreamK/SplitK/MX/FullLoadA |
| CATLASS 49 FA infer | FA-pipeline **peer**（连续 kvSeq）；[contrast](sources/scans/attention-cross-source/sparse_vs_49_vs_fia.md) passed；**不开 Case** |
| ops-transformer FIA 950 | FA-pipeline **peer**（mask-truncated S2）；同上；**不开 Case** |
| `all_gather_matmul_v2` fp16/bf16 | MC2 RS Case 的 **reverse sibling**；同一 M1 chunk coupling；**不开 AG Case** |
| `matmul_all_reduce` fp16/bf16 native | M1 **member / morphology evidence**；repeat-AllReduce，不是 handle-per-chunk；**不开 AR Case** |
| `matmul_allto_all` fp16/bf16 | M2 Case 的 **reverse sibling**（SEND_ONE）；**不开第二张 M2 卡** |
| CATLASS 71 / ops `grouped_matmul_finalize_routing` | 60+A+C+53 **fused composition**；[contrast](sources/scans/gmm-finalize-cross-source/catlass71_vs_ops_finalize_routing.md) passed；**不开 Case** |
| vLLM `MoECommType.MC2` dispatch/combine | **B contract alternative / framework packaging**；[P2](sources/scans/vllm-ascend/close_reads/p2_moe_mc2_absorb.md) passed；**不是** matmul-collective MC2；**不**重证 MTE dest-push |
| vLLM A5 GMM SwiGLU v2 + GMM2 | Case **C + 60**（MX → 53） |
| vLLM `FUSED_MC2` / MegaMoe | Case **D alternative**；A5 selector 不选为 eligible default |
| vLLM ALLGATHER init_routing | Case **A alternative** |
| vLLM `npu_mm_reduce_scatter_base` | MC2 Knowledge **四问**；MM→RS **semantic/job**；M1 morphology 未重证；**不开第三张 MC2 Case** |

---

## 2. Knowledge inventory

| Knowledge | status | supporting Cases | cross-source evidence | still single-source? |
| --- | --- | --- | --- | --- |
| [moe-token-expert-rank-data-contracts](knowledge/engineering-experience/moe-token-expert-rank-data-contracts.md) | accepted | **A, B, C**（60/53 只作减法 / MX Block 脚注） | **yes** — sgl DeepEP split：同一 EP loop、`expandIdx` = `assist_info` 寿命（Rule 1） | EP loop **no**；单卡 permute **yes**（A 尚无第二 source） |
| [moe-fusion-materialization-and-sync](knowledge/engineering-experience/moe-fusion-materialization-and-sync.md) | accepted | **D**（B/C 是被压的 split） | **yes** — sgl fused A5：Q1–Q3 仍分类；若干 **cells** 不同（push vs pull；`SetStatus/WaitDispatch` vs `CrossRankSyncInWorldSize`） | taxonomy **no**；MegaMoe dest-pull 形态本身仍主要是 D |
| [gemm-k-partition-strategies](knowledge/engineering-experience/gemm-k-partition-strategies.md) | live（无 `accepted` 字段） | **43, 66, 68** | **no** — 生产 GMM 里的 StreamK/SplitK **名字**被明确禁止当成 66/68 | **yes**（同一 source 的三张卡） |
| [canimplement-is-not-all-constraints](knowledge/engineering-experience/canimplement-is-not-all-constraints.md) | live | **43, 53, 73** | **no** as this Knowledge | **yes**（CATLASS）。Sparse 的 *Host fallback ≠ supported contract* 是 **同类 Agent trap、不同谓词**，先留在 Case，不并进本文 |
| [fa-pipeline-online-softmax-and-paged](knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md) | **accepted** | Sparse（pipeline 证据；**selection 不进本文**） | **yes** — CATLASS 49 + FIA 950 peers | pipeline **no**；`select_idx` **yes**（仍只 Sparse Case） |
| [mc2-compute-collective-coupling](knowledge/engineering-experience/mc2-compute-collective-coupling.md) | **accepted** | **RS + M2**（AG/AR close-read，不开卡） | **no** | **yes**（ops-transformer）。四条 invariant，不是四算子课；flag 3 / AIC0 **不是** M1 通例 |
| [custom-op-delivery-call-and-selected-path](knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md) | **accepted** | **none**（P1/P2 close-read 作反例；不开新 Case） | **no** | **yes**（vllm-ascend）。framework implementation-selection；**不是** kernel mechanism。**composition-capable ≠ runtime composition observed** |

**暂不创建**

- 第三份 MoE Knowledge：Layer 1/2 已够；A5 是验证，不是新层。
- 第四张 GEMM Case / 第二张 MX / 第二张 GMM：饱和。
- 49 Case / FIA Case：contrast 已否决。
- 第二篇 Attention Knowledge（`select_idx` / indexer）：仍只有 Sparse 一张卡。
- AG Case / AR Case / 第二张 M2 Case：sibling 或 morphology，不开卡。
- 第二篇 MC2 Knowledge：不要为 M3 再抽一篇。第一篇已 accepted。
- 第二篇 framework surfaces Knowledge：P3 已够；不要为每个 `_C_ascend` 再开一篇。
- Compress epilog Case / Knowledge：结果 A；FA 只覆盖 Host paged-addressing。

---

## 3. Coverage matrix

### compute — GEMM / GMM / MX / K-partition

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| Basic GEMM (MN pingpong) | **canonical** | 43 | sgl wrappers = duplicate |
| Grouped GEMM slice-M | **canonical** | 60 | C 是生产融合 extension，不是第二张 60 |
| MX block-scale GEMM | **canonical** | 53 | C / MegaMoe / 生产 GMM 用 MX，不克隆 53 |
| StreamK (remainder-tile K) | **canonical** | 66 | 禁止把 GMM/cgmct 的 StreamK **名字**当 66 |
| SplitK (every-MN K) | **canonical** | 68 | 同上 |
| Full-K A + cross-N reuse | **canonical** | 73 | 生产 `TryFullLoadA` = 思想 extension，不开第二张 |
| GMM+SwiGLU+MX MIX | **canonical** | C | 60+53 之上的生产融合 |
| Fixpipe / example 46 | **deferred** | — | close-read 有，明确不进 K-partition 树 |
| CATLASS 65 MX GMM+SwiGLU | **candidate** | — | 对照 C，默认 alternative |
| CATLASS 71 / ops GMM finalize-routing | **no-case** | — | [contrast](sources/scans/gmm-finalize-cross-source/catlass71_vs_ops_finalize_routing.md) **passed**；60+A+C+53 fused composition；**不是 EP**；`row_index` ≠ Case A `expanded_row_idx` |

### routing — local / EP / fusion

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| Local permute (Init/Finalize) | **canonical** | A | sibling of B；无第二 source |
| EP dest-push + assist triples | **cross-validated** | B | sgl DeepEP A5 split |
| Fused EP+FFN compression | **cross-validated** | D | sgl fused A5；内层 push vs D 的 pull = 形态差，loop 同类 |
| InitRouting → Dispatch 生产链 | **forbidden** | — | 两套 loop，不是一条链 |
| DeepEP Buffer / strategy switch | **saturated** | — | packaging；不是新 window 协议 |
| `DISPATCH_FFN_COMBINE` | **deferred** | — | 950 skip |
| DeepEP A2 | **deferred** | — | 非本阶段 950 SoT |

### attention — FROZEN

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| FA pipeline（QK → online-softmax → PV → **normalization-aware O merge**；optional logical→physical `block_table`） | **cross-validated** | Sparse + Knowledge | 49 / FIA 是 **peers**；**不含** PV-lag-QK；**不含** FIA 独立 rescale-O stage |
| Block-sparse paged attention（`select_idx`） | **single-source** | Sparse | MMA 前 page 选择；不进 FA Knowledge；**Case 本身不是 cross-validated** |
| CATLASS 49 FA infer | **no-case** | — | 连续 kvSeq peer；contrast passed |
| CATLASS 70 FA prefill | **candidate** | — | Attention freeze 后不扫；49 sibling |
| ops-transformer FIA | **no-case** | — | 950 PFA/IFA peer；`sparse_mode` ≠ `select_idx` |
| CATLASS 19 / ops MLA prolog | **deferred** | — | freeze |
| sgl lightning indexer / MLA preprocess | **deferred** | — | 950 CMake **off** |
| sgl laser / ada / FLA | **deferred** | — | 非 950-first |

### communication / MC2

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| MTE window dest-push (EP) | **cross-validated** | B | DeepEP A5 同族；**不是** MC2 AG/RS/AR |
| MTE window dest-pull (fused) | **canonical** | D | 尚未被第二 source 复制（A5 fused 用 push） |
| World / cross-rank visibility | **cross-validated** | D + Layer 2 | A5 换原语，不删语义；MC2 **Wait = consumer gate**；**Commit = progress，不是 visibility** |
| MC2 chunked MM–collective（M1） | **single-source** | [RS Case](cases/compute-communication/mc2/ascend950-matmul-native-reduce-scatter/case.md) `accepted` | AG reverse sibling；AR member（repeat morphology）；**compute baseline = 43**；不是 EP |
| MC2 A2A↔MM pipeline（M2） | **single-source** | [M2 Case](cases/compute-communication/mc2/ascend950-alltoall-matmul-pipeline/case.md) `accepted` | WAIT_ONE canonical；MM→A2A reverse sibling；RS = **M1 mechanism contrast** |
| MC2 Knowledge（ownership / progress / visibility / transform） | **canonical** | RS + M2 + AG/AR close-read | 不是 RS 扩写；handle-per-chunk / flag 3 / AIC0 **不是** M1 通例 |
| MC2 M3 | **deferred** | — | 继续不读 |
| HCCL AllToAllV job 名 | — | B | job ≠ mechanism |

### quant

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| MX FP8 GEMM（E4M3 payload + E8M0 / `MmadMx` group-32） | **canonical** | 53 (+ C, D) | 禁止第四张 GEMM；e5m2 不在 53 dtypes |
| Non-MX FP8 GEMM（identity，无 block scale） | **gap** | — | 无 Case；**不能**用 53 代表 |
| FP8 attention FullQuant | **single-source sibling** | Sparse variant `10003` | 同一 sparse loop，不开第二张卡 |
| FP4 / 更低 | **gap** | — | 无 Case |
| Routing-side MX dtype | **footnote** | A/B | dtype on tokens，不是 GEMM Case |

### framework — custom-op integration

| mechanism | coverage_status | live Case | notes |
| --- | --- | --- | --- |
| Delivery ≠ call ≠ selected path | **canonical** | — | [Knowledge](knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md)；**composition-capable ≠ observed**；**不开 Case** |
| Indexer as selection producer | **paused** | — | P1 mechanism passed；canonical instance **explicit pending**；**不写 Case** |
| Compress epilog（quant + physical scatter） | **no-case** | — | existing-pattern-composition；FA Knowledge 只覆盖 Host paged-addressing |

---

## 4. Duplication / saturation

以后新 source **默认只做 alternative evidence**，禁止再开 Case：

| 已够 | 新实现应判成 |
| --- | --- |
| Basic GEMM / pingpong / CATLASS wrappers | 43 duplicate |
| GMM slice-M / prefix-sum `groupList` | 60 extension 或 C 的消费者 |
| MX Identity `MmadMx` | 53 |
| StreamK / SplitK **教学形态** | 66 / 68；生产同名 tiling ≠ 新 Case |
| FullLoadA 思想 | 73；生产 sum-vs-L1 检查可进 CanImplement 证据，不开卡 |
| EP dest-push + triples + MTE window | B alternative |
| Fused EP+FFN（handoff 内化 + 保留 visibility） | D alternative；填 Layer 2 三问 |
| DeepEP Python Buffer / HCCS/RDMA/low-latency | packaging |
| FA pipeline（online-softmax 跨 tile + O-merge invariant + logical→physical `block_table`） | 49 / FIA **peer evidence**；不开第二张 FA 卡 |
| GMM + finalize-routing（expert-major → token-major scatter-add fused into GMM） | 60/A/C/53 **fused composition**；不开卡；`row_index` ≠ Case A `expanded_row_idx` |
| vendor OPP + `vllm_ascend_C`（delivery ≠ call ≠ selected path） | P3 Knowledge；composition-**capable** ≠ observed；不开 Case |
| A5 compress epilog（quant + `slot_mapping` scatter） | existing-pattern-composition；Host 已做 logical→physical；不开卡 |

饱和的成功样例就是 sgl-kernel-npu：**扫了一个生产仓，只新增一张 Sparse Attention。** 49/FIA 对照后仍 **不开** 第二张 Attention Case。

MC2 **第一阶段已冻结**（Knowledge `accepted`）。M3 未展开，**不读**。里面用了 Matmul + 通信 ≠ 既是 GEMM Case 又是 EP Case。

---

## 5. Open gaps

### 完全没 Case

- MC2 M3（继续不读）
- 生产 plain `grouped_matmul` / cgmct infra（有意不克隆 60）
- CATLASS 70 FA prefill / MLA / indexer（Attention freeze）
- FP4 及更低量化 GEMM
- 46 Fixpipe（有 close-read，明确不入库为 Case）

### 只有 catalog candidate

见 `sources/scans/{catlass,ops-transformer,sgl-kernel-npu}/candidate_catalog.yaml`。  
MC2 细节：`sources/scans/ops-transformer/mc2/`。

### 值得 cross-source、而不是再开 Case

1. **Sparse Attention ↔ CATLASS 49 ↔ ops-transformer FIA** — **done**（`review: passed`）。不开 49/FIA Case；FA Knowledge **accepted**。
2. 生产 GMM `FullLoadA` / `CanImplement` 文案 vs 73（可进现有 Knowledge）。
3. 下一份融合 Case（若出现）**直接填 Layer 2 三问**，不写第三份 MoE Knowledge。MC2 的 visibility 问题可以 *引用* Layer 2，但 dataflow family 不同，不要并进 MoE Knowledge。

---

## 6. Next-source / next-family priority

```text
Attention
  Sparse Case                     ✅
  Sparse ↔ CATLASS49 ↔ FIA        ✅
  FA pipeline Knowledge           ✅
  ─────────────────────────────── FREEZE

THEN
  MC2 family Scout                ✅
  RS native close-read            ✅
  AG reverse close-read           ✅
  AG ↔ RS verdict                 ✅
  第一张 MC2 Case（RS native）     ✅ accepted
  ─────────────────────────────── RS Case FREEZE
  M2 A2A↔MM close-read            ✅
  第二张 MC2 Case（A2A pipeline）  ✅ accepted
  ─────────────────────────────── M2 Case FREEZE
  AR native close-read            ✅ passed
  AR → M1 verdict                 ✅ passed
  MC2 Knowledge                   ✅ accepted
  ─────────────────────────────── MC2 第一阶段 FREEZE
  GMM finalize contrast           ✅ passed / no Case
  ─────────────────────────────── ops-transformer FREEZE
  M3                              ⏸ 不读

  Source 4 Scout                  ✅ FREEZE
  P1 indexer mechanism            ✅
  P1 canonical-instance           ⏸ explicit pending
  P1 Case                         ⏸ 不写
  Sparse consumer                 ✅ no second Case
  P2 MoE / MC2 absorb             ✅ FREEZE
  P3 custom-op Knowledge          ✅ accepted / FREEZE
  P1-next compress epilog         ✅ PASSED / result A / no Case
  ─────────────────────────────── vllm-ascend Source 4 FREEZE

NOW
  Source 4 frozen. Leftover = P1 indexer canonical runtime-evidence only.
  Do not keep scanning this repo to paint candidates green.
  Do not start Source 5 unless a new round is opened.

不要
  因为名字有 Sparse 就开第二张 Sparse Case
  把 indexer 和 Sparse Case 合成一张
  把 compress epilog 绑进 Indexer Case
  开第三张 MC2 Case
  再榨 ops-transformer（M3 / 再扫 fused 名）
  回改 RS / M2 / FA / MoE / GEMM 课
  把 vLLM 的 MoECommType.MC2 当成 matmul-collective 家族
  把 vendor OPP 与 vllm_ascend_C 画成互斥两叉（源码 composition-capable，不是 XOR）
  把 composition-capable 写成 A5 runtime composition observed
  看到 `_C_ascend` 就当成 in-tree kernel body
  把 enable_custom_op() A5=False 当成“950 没有这些 kernel”
  用 P3 Knowledge 去填 P1 indexer Case
  为把 Source 4 candidate 涂绿继续深挖
```

---

## Coverage status (scannable)

```yaml
coverage_status:
  Basic GEMM:                 canonical
  Grouped GEMM:               canonical
  MX GEMM:                    canonical          # 53 MX FP8, not identity FP8
  Non-MX FP8 GEMM:            gap
  StreamK / SplitK:           canonical          # CATLASS teaching shape only
  FullLoadA:                  canonical
  GMM+SwiGLU+MX:              canonical
  Local permutation:          canonical          # single-source Case A
  EP routing:                 cross-validated    # B + sgl DeepEP
  MoE fusion compression:     cross-validated    # D + sgl fused A5
  Block-sparse paged attn:    single-source      # select_idx；Case only；NOT cross-validated
  FA pipeline:                cross-validated    # Knowledge accepted; Sparse+49+FIA
  FIA / CATLASS 49:           no-case            # FA-pipeline peers; contrast passed
  CATLASS 70 prefill:         candidate          # frozen with Attention
  MLA / indexer:              paused             # mechanism passed; canonical not pursued
  MC2 matmul-collective:      single-source      # RS + M2 Cases; Knowledge accepted
  MC2 AG:                     no-case            # reverse sibling on the RS card
  MC2 AR:                     no-case            # M1 member / morphology; close-read passed
  MC2 M2 A2A↔MM:              single-source      # Case accepted
  MC2 M3:                     deferred
  GMM finalize-routing:       no-case            # contrast passed; absorb 60/A/C/53
  vllm-ascend:                source-frozen      # P1 canonical pending only; P3 Knowledge accepted
  custom-op surfaces:         canonical          # delivery ≠ call ≠ selected path; composition-capable ≠ observed; no Case
  compress epilog:            no-case            # existing-pattern-composition; Host slot_mapping
  cgmct / plain GMM:          deferred           # extension of 60, not a clone
  DeepEP packaging:           saturated
  FP4:                        gap
```

---

## Identity (sources, not reopened)

| source | scanned | intended SoT note |
| --- | --- | --- |
| catlass | `a8b4fffbd9cabc021dfd47253e907a1cde59de25` | CATLASS Cases 的 live commit |
| ops-transformer | fork `9311b63bcd065b9779eb08a7d634e9463ee41ef1` | `cann/ops-transformer` 未 fetch；MoE 线按此 checkout 冻结；MC2 Scout 同 commit |
| sgl-kernel-npu | fork `8d76a8da75315ae9add5cfd2458b8028289f1ada` | Sparse 三路径 verified on `sgl-project` `main` @ `ee334193e5278d7b3a64afcc2e3ee5243133c4a0` |
| vllm-ascend | official `d8268faea90dd9b999a0a82f6d133751d57b5770` | `vllm-project/vllm-ascend` `main`；Scout 四件套在 `sources/scans/vllm-ascend/` |

MoE 线、GEMM 六卡、Sparse Case **consumer-side** teaching、FA Knowledge、**MC2 第一阶段**（RS / M2 Case、AR close-read、MC2 Knowledge）、**GMM finalize-routing contrast**、**ops-transformer source**、**vllm-ascend Source 4** **不要回改**。P1 indexer canonical 是唯一显式 pending，不是继续扫仓的入口。
