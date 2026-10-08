# P3 close-read: two custom-op surfaces (delivery ≠ call ≠ selected impl)

P2 absorb **PASSED / FREEZE**. This round is the leftover engineering layer Scout already named. **Not a Case. Not `cases/compute/`.**

Evidence dump. Knowledge: [custom-op-delivery-call-and-selected-path.md](../../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md).

Source: `vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`.

```yaml
status: close-read-written
review: passed
scope: three-layer-surfaces
freeze: p3
not: case
not: mutually-exclusive-forks
not: runtime-compose-observed
not: reopen-p1-canonical-probe
not: reopen-p2-kernels
```

Companion: [p3_custom_op_surfaces_verdict.md](p3_custom_op_surfaces_verdict.md).

P1/P2 already produced the examples. Do not re-read indexer/MoE kernels.

---

## 先问什么

```text
1. 这个 binary / OPP 是从哪条 Build / delivery artifact 来的？
2. Python 实际 call 的是哪个 namespace / 哪个 adaptor 分支？
3. runtime 选中的 path 是哪一条？（binding→ACLNN→kernel / vendor torch_npu / unresolved）
   不要用 1 或 2 自动填 3。
4. 源码上 composition-capable 吗？runtime composition / OPP resolution 是否 observed？
```

不要先问“这是不是 custom op”，也不要画成：

```text
Framework → torch.ops → ACLNN/csrc → kernel     # 禁止默认一条链
Surface A XOR Surface B                         # 禁止默认互斥两叉
```

不同 delivery **artifact** ≠ 互斥 runtime **branch**。源码可以 composition-capable。**composition-capable ≠ runtime composition observed。**

---

## Graph

```text
                   Build / delivery artifacts
             ┌──────────────┴──────────────┐
             │                             │
      vendor ACLNN OPP              torch extension .so
       (_cann_ops_custom)             (vllm_ascend_C)
             │                             │
             └────── may compose ──────────┘
                           ↓

                   Python call surface
             torch_npu.* / _C_ascend.* / torch.ops.custom.*
                           ↓

                selected runtime path
       binding → ACLNN dispatch → delivered kernel
       or vendor torch_npu implementation
       or unresolved
```

`DeviceOperator` / `A5DeviceAdaptor` chooses a **Python call**. That is layer 2, not layer 3.

`enable_custom_op()` is a **load gate** on importing `vllm_ascend_C` (layer 2 registration), not “950 没有这些 kernel” (layer 1) and not “this call ran the in-tree kernel” (layer 3).

---

## Layer 1 — Build / delivery

Two **independent** compile/delivery artifacts (inventory; do not collapse; **not** XOR runtime branches):

| Artifact | Build | Install / search |
| --- | --- | --- |
| vendor ACLNN OPP | `csrc/build_aclnn.sh` SOC-filtered `CUSTOM_OPS_ARRAY` | `vllm_ascend/_cann_ops_custom`；`bootstrap_custom_op_env` prepends `ASCEND_CUSTOM_OPP_PATH` |
| torch extension `.so` | `setup.py` + CMake `COMPILE_CUSTOM_KERNELS` (default 1) | `vllm_ascend.vllm_ascend_C`；`TORCH_LIBRARY(_C_ascend)` in `torch_binding.cpp` |

They can sit in one wheel. Source proves they are **composition-capable** on one selected path. That is **not** A5 runtime composition observed:

已钉死的 binding / dispatch chain：

```text
vllm_ascend_C
    ↓ TORCH_LIBRARY
_C_ascend.npu_vllm_quant_lightning_indexer
    ↓ torch_binding.cpp
EXEC_NPU_CMD(aclnnVllmQuantLightningIndexer)
    ↓
ACLNN dispatch
```

delivery composition candidate（unresolved without runtime evidence）：

```text
ACLNN dispatch
    ↓ runtime OPP resolution
ASCEND_CUSTOM_OPP_PATH may expose
_cann_ops_custom / vendor OPP
    ↓
selected kernel
```

A5 ACLNN SoT is the `^ascend950` list, **not** A3’s list, **not** a lone `AddConfig("ascend950")`.

A5 list includes `vllm_quant_lightning_indexer` and `kv_quant_sparse_attn_sharedkv`.  
A5 list does **not** include `sparse_attention_score` (A3 does). MiniMax still **calls** `_C_ascend.npu_sparse_attention_score` — that is a layer-2 fact, not “A5 ACLNN built it”.

Both artifacts can exist in one wheel. Having an op on the ACLNN list does not mean serving calls it. Having a `TORCH_LIBRARY` def does not mean A5 imported the `.so`. Independent artifacts ≠ mutually exclusive runtime branches.

---

## Layer 2 — Python call

Who is invoked:

| path (already pinned) | Python call | adaptor? |
| --- | --- | --- |
| SFA A5 indexer | `torch_npu.npu_lightning_indexer*` | `A5DeviceAdaptor.indexer_select_post_process` |
| DeepSeek-V4 / DSA indexer | `torch.ops._C_ascend.npu_vllm_quant_lightning_indexer` | **direct**; no DeviceOperator |
| A5 eligible EP (P2) | `torch_npu.npu_moe_distribute_dispatch(_v2)` / `combine(_v2)` | none |
| A5 GMM1 default (P2) | `torch_npu.npu_grouped_matmul_swiglu_quant_v2` | `A5DeviceAdaptor` |
| Base GMM1 custom | `torch.ops._C_ascend.grouped_matmul_swiglu_quant_weight_nz*` | Base; `enable_custom_op` gated in mlp |
| TP MMRS | `torch_npu.npu_mm_reduce_scatter_base` | A5 default `comm_mode="ai_cpu"` |
| MiniMax sparse | `torch.ops._C_ascend.npu_sparse_attention_score` | MiniMax lazy-imports `vllm_ascend_C` |

`get_device_adaptor()` returns `A5DeviceAdaptor` on A5. That only forks **some** calls. DSA indexer and MoE dispatch **bypass** it.

Registration of `_C_ascend` (P1, paused as canonical gate):

```text
enable_custom_op() A5 → False → skip import vllm_ascend_C
        ≠
serving worker → camem.py try-import vllm_ascend_C   # side door
        ≠
vendor OPP registering torch.ops._C_ascend           # it does not
```

`torch.ops.custom.npu_vllm_quant_lightning_indexer` appears in the **ACLNN README**. vLLM serving does not use that namespace.

---

## Layer 3 — runtime selected implementation

Only pin when the hop is in source. Otherwise **unresolved**.

| Python call | hop in this tree | layer 3 |
| --- | --- | --- |
| `_C_ascend.npu_vllm_quant_lightning_indexer` | `torch_binding.cpp` → `EXEC_NPU_CMD(aclnnVllmQuantLightningIndexer)` | `_C_ascend` **binding** → ACLNN custom-op **dispatch**；final kernel identity still depends on ACLNN/OPP resolution. Do **not** collapse binding and kernel into “in-tree ACLNN”. |
| `torch_npu.npu_lightning_indexer*` | **no** in-tree body | vendor / torch_npu；**not** proved = `vllm_quant_lightning_indexer` |
| `torch_npu.npu_moe_distribute_*` | **no** in-tree body this P3 | vendor；P2 already: contract ≠ MTE identity |
| `torch_npu.npu_grouped_matmul_swiglu_quant_v2` | in-tree `csrc/gmm/grouped_matmul_swiglu_quant_v2/` exists as **delivery** | A5 **call** is torch_npu, not `_C_ascend` |
| `_C_ascend.npu_sparse_attention_score` | torch binding exists; **not** on A5 `CUSTOM_OPS_ARRAY` | A5 runtime kernel **unresolved** / A3-shaped |

Do not write “compiled on A5 ⇒ A5 serving ran it”.

---

## Worked contradictions (why the three layers exist)

1. **Same semantic job, two Python calls** — SFA indexer vs DSA indexer. Path matrix already forbids one global “A5 indexer”.
2. **Same delivery surface, not on the A5 ACLNN list** — MiniMax `_C_ascend.npu_sparse_attention_score`.
3. **Same in-tree kernel family, different call** — GMM SwiGLU: A5 torch_npu v2 vs Base `_C_ascend` vs `csrc/` compile.
4. **Load gate ≠ delivery** — A5 `enable_custom_op()==False` while `build_aclnn.sh` still lists 950 ops and CMake still builds `vllm_ascend_C`.
5. **Load gate ≠ only registration** — camem import can still register `_C_ascend` (P1 registration close-read). Canonical live probe **paused**; do not reopen here.
6. **Delivery artifacts may compose** — source proves composition-capable（`_C_ascend` binding → `EXEC_NPU_CMD`），不是 XOR。A5 runtime composition / OPP resolution remains **unresolved**（P1 canonical paused）。

---

## What the Knowledge file teaches

Not op names. Five rules:

1. Ask the three layers separately.
2. Artifact presence ≠ call selection.
3. Python call ≠ kernel identity (`API contract aligned ≠ kernel mechanism identity proved`).
4. Load gate ≠ build availability.
5. Delivery artifacts may compose (composition-capable ≠ runtime composition observed).

Live file: [custom-op-delivery-call-and-selected-path.md](../../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md). **Never** `cases/compute/`.

---

## What this file does not do

- Close P1 indexer Case (canonical instance still paused).
- Re-prove torch_npu kernels or pin ACLNN OPP bytes to `csrc/` sources.
- Collapse DeviceOperator or `_C_ascend` into “the kernel”.
- Teach Surface A XOR Surface B.
- Write DSA binding/dispatch as A5 runtime OPP composition observed.
