# P1 close-read: `_C_ascend` A5 registration (DSA indexer)

Narrow follow-up after the three P1 files. **Not a Case. Not Knowledge. Not P3 lifecycle.**

Question:

```text
A5  enable_custom_op() == False
        ↓
skip import vllm_ascend.vllm_ascend_C
        ↓
why does DSA still write
torch.ops._C_ascend.npu_vllm_quant_lightning_indexer ?
```

Evidence: `vllm-project/vllm-ascend` `main` @ `d8268faea90dd9b999a0a82f6d133751d57b5770`.

```yaml
status: close-read-written
review: pending
scope: registration-chain-only
canonical_instance_gate: pending   # .so load success not observed on a live A5 process
not: P3-two-surfaces-Knowledge
not: Case-write
```

---

## Answers (four options)

| # | hypothesis | result |
| --- | --- | --- |
| 1 | another registration path? | **yes** — `camem.py` module-level import of `vllm_ascend_C` |
| 2 | vendor OPP registers the same Python namespace? | **no** — OPP is ACLNN delivery (`ASCEND_CUSTOM_OPP_PATH`), not `torch.ops._C_ascend` |
| 3 | unconditional import? | **yes, on serving worker** — `NPUWorker` imports `camem`; MiniMax has a second, explicit A5 lazy import (not DSA) |
| 4 | DSA A5 path actually unreachable because of the gate? | **not because of `enable_custom_op`** — DSA never checks that gate. Reachable **iff** `vllm_ascend_C` actually loads |

Remaining live bit is only: does that `.so` import succeed on the **target A5 process**. This checkout did not execute it.

---

## What registers `_C_ascend`

Python name `torch.ops._C_ascend.npu_vllm_quant_lightning_indexer` is created by loading the C extension:

```text
import vllm_ascend.vllm_ascend_C
        ↓  .so static init
TORCH_LIBRARY(_C_ascend)     # torch_binding.cpp, non-310P branch
        ↓
ops.def / ops.impl npu_vllm_quant_lightning_indexer
        ↓  call
npu_vllm_quant_lightning_indexer_npu
        ↓
EXEC_NPU_CMD(aclnnVllmQuantLightningIndexer, ...)
```

That last hop is why Path 2’s **mechanism source** is in-tree `csrc/attention/vllm_quant_lightning_indexer` (ACLNN), not `torch_npu.npu_lightning_indexer`.

Vendor `_cann_ops_custom` / `bootstrap_custom_op_env()` only prepends `ASCEND_CUSTOM_OPP_PATH` so that **ACLNN** can find the kernel. README examples use `torch.ops.custom.*`. That is a different Python namespace. It does **not** populate `_C_ascend`.

`platform.import_kernels()` still calls `bootstrap_custom_op_env()` and **does not** import `vllm_ascend_C` (commented; PR `#884` / `ASCEND_RT_VISIBLE_DEVICES`).

---

## Official gate vs side doors

### Gate (does not load C on A5)

`enable_custom_op()` (`utils.py`):

```text
A5 or VLLM_BATCH_INVARIANT
        ↓
_CUSTOM_OP_ENABLED = False
        ↓
return   # never reaches import vllm_ascend.vllm_ascend_C
```

Issue `#7157`: “temporarily disable all custom ops” on 950. That is this function only.

DSA / DeepSeek-V4 / `dsa_cp.py` never call `enable_custom_op()`. They just name `torch.ops._C_ascend.*`.

### Side door 1 — serving worker (DSA rides this)

```text
platform: worker_cls = vllm_ascend.worker.worker.NPUWorker   # not 310P
        ↓
worker.py import-time:
  from vllm_ascend.device_allocator.camem import CaMemAllocator
        ↓
camem.py:
  try:
      from vllm_ascend.vllm_ascend_C import init_module, python_create_and_map, ...
  except ImportError:
      camem_available = False   # sleep mode off; _C_ascend also not registered
```

Loading the allocator symbols loads the **whole** `.so`. `TORCH_LIBRARY(_C_ascend)` therefore registers the indexer as a side effect of sleep-mode plumbing.

`NPUPlatform.is_cumem_allocator_available()` **avoids** importing camem during ModelConfig validation (“before custom-op init”). The extension is meant to come in later via worker → camem. That comment is evidence the authors know camem is an extension load point.

A5 worker `__init__` still skips `_register_atb_extensions()`. Dummy fusion (`ops.register_dummy_fusion_op`) only plants a few `_C_ascend.rms_norm*` names. It does **not** register the indexer.

### Side door 2 — MiniMax only (not DSA)

`msa_m3_npu._npu_k2q_csr`:

```text
# A5 disables the generic custom-op loader, so register the in-tree
# MiniMax M3 operators lazily after the NPU runtime has been initialized.
import vllm_ascend.vllm_ascend_C
enable_custom_op()   # still False on A5; import already happened
```

Authors already knew the generic loader is off on A5. They added an **explicit** bypass for MiniMax. DSA has **no** equivalent comment or import.

---

## What this does / does not prove for the Case gate

```text
                  runtime live              mechanism source
SFA torch_npu          ✅ Python call             ?  (still not this kernel)
DSA _C_ascend          ⏳ .so load attempted       ✅  TORCH_LIBRARY → aclnnVllmQuantLightningIndexer
                       on every NPUWorker import
                       success not observed here
```

Closed:

- Why the source can *write* `_C_ascend` on A5: it assumes someone else loaded the `.so`. On serving, that someone is **camem**, not `enable_custom_op`.
- Vendor OPP ≠ `_C_ascend` namespace.
- `enable_custom_op()==False` is **not** a proof that Path 2 is dead.

Still open (canonical-instance gate):

- On the **target A5** process: after `import vllm_ascend.worker.worker` (or `import vllm_ascend.device_allocator.camem`), is `hasattr(torch.ops._C_ascend, "npu_vllm_quant_lightning_indexer")` true?
- If **yes** → Path 2 is the canonical instance; write the Indexer Case; SFA is sibling (`torch_npu`, implementation identity not assumed).
- If **no** (ImportError / missing op) → Path 2 not live on that runtime; do **not** invent another registration; fall back to Case-write option A (SFA `torch_npu` + its own mechanism evidence).

Do not expand this into P3 Knowledge. Do not execute a model.

**Paused:** P1 canonical-instance gate is **not** pursued this source round. Next was P2 MoE/MC2 absorb. Resume this probe only if a later Case write picks Path 2 as canonical.
