#
# SPDX-License-Identifier: MIT
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# Description: precision test for the gmm_layered operator (A8W4 scenario)
# Create: 2026-09-22
# Note:
# History: 2026-09-22 create file
#
"""
Precision test for the gmm_layered operator (A8W4 scenario).

All inputs are randomly generated on every run (see gen_tensors), so the test
can be invoked repeatedly with fresh data. Dimensions are configurable via
CLI/env for quick sweeps.

Golden reference: the CANN built-in non-layered npu_grouped_matmul run with the
selected layer's weight/scale/bias. The layered op must reproduce it bit-close
for every layer_index. A wrong layer selection shows up as a large deviation
immediately, so the cross-layer difference assertion guards against a silently
dead selection path.

Requires the umdk_cam_op_lib wheel + the CAM run package (GroupedMatmulLayered)
installed on an NPU machine. Run it on the NPU host, not on a compile-only build box.

Run this in two phases:

  Phase 1 - single layer, migrated-operator precision only. LAYERS=1 makes the
  all_* lists single-element, so the layer-selection path is never exercised and
  the only question is whether our operator reproduces the built-in
  npu_grouped_matmul. If this fails, nothing downstream can be trusted.

      LAYERS=1 python3 test_gmm_multi_layer.py

  Phase 2 - layer selection. Once phase 1 passes, raise LAYERS and the same
  comparison runs for every layer index, followed by a cross-layer difference
  check that catches a selection path which silently always resolves layer 0.

      LAYERS=4 python3 test_gmm_multi_layer.py

Usage:
    python3 test_gmm_multi_layer.py                    # phase 2, default shape
    LAYERS=1 python3 test_gmm_multi_layer.py           # phase 1
    LAYERS=4 M=256 K=1024 N=2048 E=8 SEED=42 python3 test_gmm_multi_layer.py
    python3 test_gmm_multi_layer.py --loops 5          # fresh random data per loop
    DEVICE=npu:7 python3 test_gmm_multi_layer.py       # pick the device (default npu:7)
"""

import argparse
import os
import random

import torch
import torch_npu

import umdk_cam_op_lib

NZ_FORMAT = 29  # torch_npu FORMAT_FRACTAL_NZ
B4_PER_B32 = 8  # eight int4 nibbles per int32 word (aclnn UnpackB32ToB4)


def gen_tensors(device, m, k, n, e, layers):
    """Random A8W4 scenario tensors.

    - x: int8 [m, k] per-token-quantized activation (values in the int8 range;
      the op dequantizes with pertoken_scale)
    - pertoken_scale: float32 [m]
    - expert_tokens: int64 [e] per-expert token counts (group_list_type=1),
      positive and summing to m so the per-group outputs tile the rows
    - all_weight[l]: int32 [e, k, n // 8] where each int32 packs 8 int4 nibbles
      (the aclnn unpacks to int4 [e, k, n]); stored in FRACTAL_NZ. Nibbles are
      generated in [-8, 7] then packed little-endian per word so the payload is
      a realistic signed-int4 weight regardless of the unpack convention.
    - all_scale[l]: int64 [e, n] (converted to uint64 by the aclnn); bit pattern
      built from small float halves so the packed dequant scale stays in a sane
      numeric range
    - all_bias[l]: float32 [e, n]

    Both the layered op and the built-in golden consume the identical tensors,
    so any distribution is sufficient for consistency; the ranges above simply
    keep the dequantified arithmetic well conditioned.
    """
    # Seed the generator explicitly from the process seed. A freshly constructed
    # torch.Generator does NOT inherit torch.manual_seed - it starts from a fixed
    # default - so without this every run produces byte-identical tensors and
    # --seed / SEED= / --loops all silently do nothing.
    gen = torch.Generator(device="cpu").manual_seed(torch.initial_seed())

    def rand_int8(shape):
        return torch.randint(-128, 128, shape, generator=gen, dtype=torch.int8).to(device)

    def rand_float(shape, lo, hi):
        return (torch.rand(shape, generator=gen) * (hi - lo) + lo).to(torch.float32).to(device)

    x = rand_int8((m, k))
    pertoken_scale = rand_float((m,), 5e-3, 2e-2)

    # per-expert token counts: positive, summing to m
    base = m // e
    counts = [base] * e
    for i in range(m - base * e):
        counts[i % e] += 1
    random.shuffle(counts)
    expert_tokens = torch.tensor(counts, dtype=torch.int64, device=device)

    all_weight = []
    all_scale = []
    all_bias = []
    for _ in range(layers):
        nibbles = torch.randint(-8, 8, (e, k, n // B4_PER_B32, B4_PER_B32), generator=gen,
                                dtype=torch.int32)
        packed = sum((nibbles[..., j] & 0xF) << (4 * j) for j in range(B4_PER_B32))
        # view as int32; cast for NZ storage
        weight = packed.to(torch.int32).to(device)
        weight = torch_npu.npu_format_cast(weight, NZ_FORMAT)
        all_weight.append(weight)

        # uint64 packed scale: two float32 halves (low/high 32 bits) with small
        # magnitudes; built from int64 bit pattern so the aclnn INT64->UINT64
        # reinterpret keeps the payload
        hi = torch.rand((e, n), generator=gen) * 1e-3 + 1e-4
        lo = torch.rand((e, n), generator=gen) * 1e-3 + 1e-4
        hi_bits = hi.view(torch.int32).to(torch.int64) & 0xFFFFFFFF
        lo_bits = lo.view(torch.int32).to(torch.int64) & 0xFFFFFFFF
        scale = (hi_bits << 32) | lo_bits
        all_scale.append(scale.to(device))

        all_bias.append(rand_float((e, n), -0.5, 0.5))

    return {
        "x": x,
        "pertoken_scale": pertoken_scale,
        "expert_tokens": expert_tokens,
        "all_weight": all_weight,
        "all_scale": all_scale,
        "all_bias": all_bias,
    }


def run_layered(tensors, layer_index):
    """Call the layered op. group_list is passed host-side (the op
    converts it to a device tensor internally); per-token scale is required by
    the A8W4 MSD dequant path.

    x is cloned per call: the A8W4 MSD pre-quantization pass rewrites the
    activation in place (the kernel binds the same buffer as both its input and
    its output - GMMA8W4PreProcess::Init(x, x, ...)), so without a private copy
    the golden below would be compared against an already-quantized x, and each
    further layer would re-quantize the previous result."""
    x = tensors["x"].clone()
    group_list = tensors["expert_tokens"].reshape(-1).to(torch.int64).tolist()
    out = torch.ops.umdk_cam_op_lib.gmm_layered(
        x=[x],
        all_weight=tensors["all_weight"],
        all_bias=tensors["all_bias"],
        all_scale=tensors["all_scale"],
        layer_index=layer_index,
        per_token_scale=tensors["pertoken_scale"],
        group_list=group_list,
        group_list_type=1,
        split_item=3,
    )
    # split_item=3 yields one y per group; concatenate rows for a single
    # [sum(group_list), n] tensor to compare against the golden
    return torch.cat(list(out), dim=0) if isinstance(out, (list, tuple)) else out


def run_builtin_reference(tensors, layer_idx):
    """Golden: CANN built-in npu_grouped_matmul on the selected layer's tensors.

    The built-in A8W4 tiling requires a 3-D pergroup scale [e, G, n] (it reads
    dim(1) as the K-group count unconditionally); our perchannel [e, n] scale
    is memory-identical to [e, 1, n], so an unsqueeze(1) view (no copy) feeds
    the built-in op without changing the payload the layered op consumes.

    x is cloned for the same reason as in run_layered: the built-in op runs the
    same in-place pre-quantization, so sharing one buffer would let whichever op
    ran first change what the other sees."""
    x = tensors["x"].clone()
    golden = torch_npu.npu_grouped_matmul(
        x=[x],
        weight=[tensors["all_weight"][layer_idx]],
        scale=[tensors["all_scale"][layer_idx].unsqueeze(1)],
        bias=[tensors["all_bias"][layer_idx]],
        per_token_scale=[tensors["pertoken_scale"]],
        group_list=tensors["expert_tokens"],
        split_item=3,
        output_dtype=torch.bfloat16,
        group_type=0,
        group_list_type=1,
    )
    return torch.cat(list(golden), dim=0) if isinstance(golden, (list, tuple)) else golden


def compare(tag, actual, expect):
    """Quantized-op tolerance comparison with detailed stats."""
    a = actual.to(torch.float32)
    e = expect.to(torch.float32)
    torch_npu.npu.synchronize()

    print(f"\n[{tag}] expect {tuple(e.shape)} {expect.dtype} | actual {tuple(a.shape)} {actual.dtype}")
    if a.shape != e.shape:
        print(f"[{tag}] FAIL: shape mismatch")
        return False

    finite = torch.isfinite(a)
    if not bool(finite.all()):
        print(f"[{tag}] FAIL: output contains NaN/Inf ({int((~finite).sum())} elements)")
        return False

    diff = (a - e).abs()
    rel = diff / e.abs().clamp_min(1e-6)
    max_abs = float(diff.max())
    mean_abs = float(diff.mean())
    p99_abs = float(diff.flatten().kthvalue(max(1, int(diff.numel() * 0.99))).values)
    max_rel = float(rel.max())
    mean_rel = float(rel.mean())
    print(f"[{tag}] abs diff  max={max_abs:.6f} mean={mean_abs:.6f} p99={p99_abs:.6f}")
    print(f"[{tag}] rel diff  max={max_rel:.6f} mean={mean_rel:.6f}")

    ok = torch.allclose(a, e, rtol=2e-2, atol=2e-2)
    print(f"[{tag}] allclose(rtol=2e-2, atol=2e-2): {'PASS' if ok else 'FAIL'}")
    return ok


def one_round(device, m, k, n, e, layers, seed):
    """One full round with fresh random data: dump-style layer + full sweep."""
    if seed is None:
        seed = random.randrange(2**31)
    random.seed(seed)
    torch.manual_seed(seed)

    tensors = gen_tensors(device, m, k, n, e, layers)
    print(f"\n########## round seed={seed} shape m={m} k={k} n={n} e={e} layers={layers} ##########")
    print(f"x: {tuple(tensors['x'].shape)} {tensors['x'].dtype}, "
          f"pertoken_scale: {tuple(tensors['pertoken_scale'].shape)}, "
          f"expert_tokens: {tensors['expert_tokens'].tolist()}")
    # Fingerprint the data so a silently-constant generator is visible in the log:
    # with the seed wired through, consecutive rounds/loops must differ here.
    print(f"x fingerprint: first4={tensors['x'].flatten()[:4].tolist()} "
          f"sum={int(tensors['x'].to(torch.int32).sum())}")
    for i, (w, s, b) in enumerate(zip(tensors["all_weight"], tensors["all_scale"], tensors["all_bias"])):
        print(f"layer {i}: weight {tuple(w.shape)} {w.dtype}, scale {tuple(s.shape)} {s.dtype}, "
              f"bias {tuple(b.shape)} {b.dtype}")
    torch_npu.npu.synchronize()

    ok_all = True
    outs = {}
    for li in range(layers):
        layer_index = torch.tensor([li], dtype=torch.int64, device=device)
        outs[li] = run_layered(tensors, layer_index)
        golden = run_builtin_reference(tensors, li)
        ok_all &= compare(f"layer_index={li}", outs[li], golden)

    # cross-layer difference: catches a silently dead layer selection. Skip
    # pairs whose references are identical only by coincidence of tiny layers.
    if layers >= 2:
        d01 = (outs[0].to(torch.float32) - outs[1].to(torch.float32)).abs().mean()
        print(f"\ncross-layer mean |out0 - out1| = {float(d01):.6f} (must be >> 0)")
        if float(d01) < 1e-3:
            print("WARN: layers nearly identical - layer selection may not be taking effect")
            ok_all = False

    return ok_all


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--m", type=int, default=int(os.environ.get("M", 256)))
    parser.add_argument("--k", type=int, default=int(os.environ.get("K", 1024)))
    parser.add_argument("--n", type=int, default=int(os.environ.get("N", 2048)))
    parser.add_argument("--e", type=int, default=int(os.environ.get("E", 8)))
    parser.add_argument("--layers", type=int, default=int(os.environ.get("LAYERS", 2)))
    parser.add_argument("--seed", type=int, default=int(os.environ["SEED"]) if "SEED" in os.environ else None)
    parser.add_argument("--loops", type=int, default=1, help="rounds of fresh random data")
    args = parser.parse_args()

    if args.k % 16 != 0 or args.n % 64 != 0:
        raise SystemExit(f"shape constraints: k % 16 == 0 and n % 64 == 0 required (NZ layout), "
                         f"got k={args.k} n={args.n}")
    if args.m < args.e:
        raise SystemExit(f"m ({args.m}) must be >= e ({args.e}) so every expert gets tokens")

    torch_npu.npu.config.allow_internal_format = True
    device = torch.device(os.environ.get("DEVICE", "npu:7"))

    ok = True
    for loop in range(args.loops):
        seed = args.seed + loop if args.seed is not None else None
        ok &= one_round(device, args.m, args.k, args.n, args.e, args.layers, seed)

    print("\n=== RESULT:", "PASS" if ok else "FAIL", "===")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
