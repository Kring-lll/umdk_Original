# Verdict: vLLM P3 custom-op surfaces

P2 freeze 之后。**Not a Case.** Knowledge **accepted**. P3 **FREEZE**.

证据：[p3_custom_op_surfaces.md](p3_custom_op_surfaces.md) · [Knowledge](../../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md)

```yaml
status: verdict-written
review: passed
result: knowledge-accepted
new_case: none
new_knowledge: knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md
preset_path: none
freeze: p3
```

分类仍是 Knowledge 而非 Case。MUST-FIX 已收：composition-capable ≠ runtime composition observed。

---

## 结果

vLLM-ascend 把三层 **实际拆开了**，不是 Scout 画的示意图。这是 Source 4 相对已有 Cases / Knowledge **还没被教过** 的工程层。

```text
Build / delivery artifacts
    ≠
Python call surface
    ≠
runtime selected implementation

composition-capable
    ≠
runtime composition observed
```

独立 delivery artifact **可以** compose，不要画成互斥两叉。源码 composition-capable **不是** A5 runtime 已 observed。

**不开 Case。** 没有新的 Cube / EP / FA / MC2 coupling。  
**Knowledge accepted**：framework implementation-selection 纪律，不是 kernel 说明书。

---

## 为什么够当 Knowledge，不够当 Case

Cases 教的是 compute / EP / FA / MC2 **mechanism**。P3 教的是 **implementation selection**：同一语义 op 可以走 torch_npu 或 `_C_ascend`，wheel 里可以同时有 vendor OPP 和 torch extension，源码上还可以 **composition-capable**；A5 的 load gate 还不等于 “kernel 不存在”。

已经有反例（P1/P2，不再展开）：

- SFA `torch_npu.npu_lightning_indexer*` ≠ DSA `_C_ascend.npu_vllm_quant_lightning_indexer`
- A5 GMM1 call 是 torch_npu v2，同时 `csrc/gmm/...` 作为另一条 delivery 存在
- MiniMax 调用 `_C_ascend.npu_sparse_attention_score`，该 op **不在** A5 ACLNN list
- `enable_custom_op()==False` 只跳过官方 import；camem 仍可能加载 `.so`
- DSA binding → `EXEC_NPU_CMD` 钉死 dispatched；vendor OPP resolve **unresolved**（不是 XOR，也不是 runtime compose observed）

同一条纪律已在 P2 MUST-FIX 用过：contract aligned ≠ identity proved。P3 把它从 MoE/MC2 特例收成 **framework 通例**。

---

## MUST-FIX applied

1. Surface A / B = **independent delivery artifacts**，不是互斥 runtime branches。
2. Layer 3：`_C_ascend` = binding → ACLNN dispatch；kernel identity 另钉。不要写 “in-tree ACLNN”。
3. DSA 源码链 = composition-**capable**；A5 OPP resolution / `.so` load = **unresolved**（P1 canonical paused）。

---

## 不要做

- `Framework → torch.ops → ACLNN/csrc → kernel` 画成默认一条管线。
- Surface A XOR Surface B 画成默认两叉。
- 把 composition-capable 写成 runtime composition observed。
- 用 DeviceOperator 分支直接填 “跑了哪个 kernel”。
- 看到 `_C_ascend` 就当成 in-tree AscendC kernel body。
- 用 `enable_custom_op` A5=False 说 “950 没有 custom kernel”。
- 开第三张 MC2 Case / 第二张 Sparse / Indexer Case。
- 重开 P1 canonical `.so` probe（paused）。

---

## Knowledge 教什么（已 accepted）

1. 三层分别问
2. artifact presence ≠ call selection
3. Python call ≠ kernel identity
4. load gate ≠ build availability
5. delivery artifacts may compose（composition-capable ≠ observed）

不写：完整 op 清单、P1 indexer Case、MoE 再吸收一遍。

---

## Status

```text
P2 MoE / MC2 absorb                ✅ PASSED / FREEZE
P3 three-layer close-read          ✅ PASSED / FREEZE
P3 Case                            ❌ no
P3 Knowledge                       ✅ accepted
P1 indexer canonical               ⏸ unchanged
```
