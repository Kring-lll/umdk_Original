# CAM

 **CAM**  is short for  **C**ommunication  **A**cceleration for  **M**atrix on Ascend NPU. CAM provides EP (Expert Parallelism) communication kernels, high performance KVCache transfer for PD disaggregation and KVC pooling, AFD communication kernels, RL weights transfer and so on. CAM is easily to be run in single kernel mode or integrated into vllm or SGLang framework. 

## Compact routed-only communication

Dispatch and combine use exactly `R` routed expert slots per MoE rank, with
zero-based local expert IDs and no shared-expert slot. This wire layout is
incompatible with the original shared-expert and reserved-slot layouts. Upgrade
the CANN kernel package, Python extension, and every participant together.

| Python interface | Compact contract |
|------------------|------------------|
| `moe_dispatch_send_async` | Routed expert IDs are in `[0, moe_rank_num * R)`. |
| `moe_dispatch_recv_async` | Returns `[expand_x, dynamic_scales, batch_info, ep_recv_count_routed]`. |
| `moe_combine_send_async` | Takes `expand_x, comm_args, batch_info`; the shared-expert tensor argument is removed. |
| `moe_combine_recv_async` | Produces the weighted sum of routed expert outputs. |

`dispatch_recv` and `combine_send` take `max_seq_len` in the position previously
named `batch_size`. `batch_info` has `5 + tp_size + R * tp_size` int64 elements;
its expert range is zero-based and inclusive. Pass the returned metadata to
combine without adding a reserved slot. Empty MoE ranks must still call combine
send to publish completion.

The communication kernels obtain peer windows from the HCCL context associated
with `group_name`; `comm_args` remains a FLOAT16 interface placeholder. The
receive-side `x` / `expand_x` inputs are floating-point tensors of shape `[1]`
that select the output dtype. MoE global rank IDs start at `attn_rank_num`. The legacy
`CommArgs` shared-header layout is retained for compatibility with existing
users. The extension exposes
`CAM_ROUTED_ONLY_CONTRACT = "cam_async_routed_only_compact_v2"`; this identifies
the Python ABI and does not verify the separately installed CANN kernel binary.

The single communication interface check is:

```bash
python test/cam/test_moe_async_interfaces.py
# After installing matching torch, torch_npu, and the built extension:
python test/cam/test_moe_async_interfaces.py --installed
```

The default mode checks source interfaces. `--installed` also calls all four
Python and Torch interfaces on Meta tensors to check output order, shapes, and
dtypes. Device compilation and multi-NPU numerical validation require an Ascend
environment. Detailed migration documents and CPU verification models stay
in the local migration workspace and are excluded from this repository update.

# Roadmap

- [x]  **EP Communication: Dispatch & Combine** 
  - [x] Support A2
  - [x] Support A3
  - [x] Support low latency mode
  - [ ] Support high throughput mode
  - [ ] Support BF16/FP16 input
- [x]  **FusedDeepMoE: Dispatch + GEMM + Combine** 
  - [ ] Support A2
  - [x] Support A3
  - [x] Support low latency mode
  - [ ] Support high throughput mode
  - [ ] Support BF16/FP16 input
  - [x] Support W8A8 for GEMM
  - [ ] Support W4A8 for GEMM
- [ ]  **KVCache Transfer** 
- [ ]  **RL Weights Transfer** 
- [ ]  **AFD Communication**
 
# Performance
(To be done)

# Directory Structures
```text
UMDK/
|--- build/
|    |--- cam/                      # build script for cam project
|    |    |--- comm_operator/       # build script for cam communication operator
|--- src/
|    |--- cam/  
|    |    |--- comm_operator/       # code of communication operator
|    |    |    |--- ascend_kernels/ # code of ascend kernels
|    |    |    |    |--- operator_registry.json  # operator -> SOC mapping + build metadata
|    |    |    |--- pybind/         # code of python interface bindings
|    |    |--- examples/            # examples for different kernels
|    |    |--- third_party/         # third party dependencies (git submodules)
|    |    |    |--- catlass/        # catlass (git submodule, pinned commit)
|--- test/
|    |--- cam/                      # UT test code of cam
```

# Quick Start
## 1. Basic Environment Requirements
|Requirements|Type|Version|Description|
|---|---|---|---|
|Ascend Chip|Required|A2/A3|You can run CAM now only in an Ascend A2 or A3 SuperPod.|
|CANN|Required|8.3.RC1|Before using CAM, you need to install CANN ≥ 8.3.RC1 to offer basic toolkit functions. Please refer to “[Huawei Ascend-CANN](https://www.hiascend.com/cann)” and install CANN first.|
|Torch|Required|2.8.0|To compile Pybind whl packet in CAM, you need to install Torch first.|
|Torch-Npu|Required|2.8.0-7.2.0|Torch-Npu supports torch framework in Ascend Platform.|
|Ascend-SHMEM|Optional|1.0.0|If you want to compile and run SHMEM kernels, you need to install Ascend-SHMEM first. Please refer to "[Huawei Ascend-SHMEM](https://gitee.com/ascend/shmem)".
## 2. Compile and Install
### · Compile
CAM offers a basic ".run" packet for kernels and a ".whl" packet for python interface.

**Basic usage:**
```bash
# enter UMDK folder
cd UMDK/
# initialize git submodules (catlass, etc.)
git submodule update --init --recursive
# build: all registered SOC generations, full operator set
./build/cam/build.sh
```

**Options:**
|Option|Description|
|---|---|
|`-c <soc>`|Target SOC generation. Supported: `ascend910_93`. Omit to build all registered generations.|
|`-a <ops>`|Semicolon-separated operator list to compile (requires `-c`). Names must match the SOC support list in `operator_registry.json`. Omit to compile the full set.|
|`-q`|Select the `fused_deep_moe_w4a8` (quantization) variant instead of `fused_deep_moe`.|
|`-d`|Enable debug build.|
|`-x`|Extract the run package instead of packing it.|
|`-t`|Build unit tests only.|
|`-p`|Build pybind whl only.|
|`-r`|Build the run package only; skip the whl package. Mutually exclusive with `-p`.|
|`-m`|Build only the cam_comm library (`framework/communicator`); skip operator/run/whl. Mutually exclusive with `-p`/`-r`. (cam_feature only)|

**Examples:**
```bash
./build/cam/build.sh -c ascend910_93                # full set for ascend910_93
./build/cam/build.sh -c ascend910_93 -a "a2e;e2a"   # only a2e and e2a
./build/cam/build.sh -c ascend910_93 -q             # full set, w4a8 variant
./build/cam/build.sh -c ascend910_93 -q -a fused_deep_moe_w4a8
./build/cam/build.sh -c ascend910_93 -a "fused_deep_moe" -r   # only fused_deep_moe, run package only
./build/cam/build.sh -c ascend910_93 -m              # only the cam_comm library (cam_feature)
./build/cam/build.sh -d                             # debug, all SOCs
```

**Rules:**
- `-a` requires `-c` (specify a SOC generation first); `-a` cannot be used with the default all-SOC build.
- `-c` must be a registered SOC (`ascend910_93`). Unregistered values (e.g. `ascend910b4`) exit with an error.
- Each `-a` entry must be in the SOC's support list; unknown names exit with an error.
- `-p` (whl only) and `-r` (run only) are mutually exclusive; using both exits with an error. By default (neither flag) the run package and whl package are built together.
- `-m` (cam_comm only) is mutually exclusive with `-p` and `-r`. It builds only the `framework/communicator` library and produces no run/whl package. Only available on cam_feature (master has no cam_comm).
- `fused_deep_moe` and `fused_deep_moe_w4a8` are mutually exclusive (they share source filenames); `-q` switches to the w4a8 variant. `fused_deep_moe_fwk` is an independent operator and can always coexist with either.
- Operators that require SHMEM are automatically skipped when `SHMEM_HOME_PATH` is unset.

If the build succeeds, the two packets are placed under:
```bash
# whl packet, version/arch depend on the compile environment.
output/cam/comm_operator/dist/umdk_cam_op_lib_XXX.whl
# run packet, <soc>_<os>_<arch> depend on the compile environment.
output/cam/comm_operator/run/CAM_<soc>_<os>_<arch>.run
```
### · Install
To install these two packets, you may follow the commands below:
```bash
# Step 1: install run packet. The recommended install path is the opp folder in your environment.
./output/cam/comm_operator/run/CAM_<soc>_<os>_<arch>.run --install-path=/usr/local/Ascend/ascend-toolkit/latest/opp
# Step 2: enable environment variables. The path is provided from the output that install run packet.
source /usr/local/Ascend/ascend-toolkit/latest/opp/vendors/CAM/bin/set_env.bash
# Step 3: install whl packet.
pip install --force-reinstall ./output/cam/comm_operator/dist/umdk_cam_op_lib_XXX.whl
```
