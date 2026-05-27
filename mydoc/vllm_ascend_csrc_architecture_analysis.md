# vllm-ascend C++ 扩展架构分析

## 项目总览

vllm-ascend 是 **vLLM 的华为昇腾 NPU 后端插件**，通过 PyTorch 自定义算子将 Ascend CANN 的加速能力注入 vLLM 推理框架。与 [wpz/umdk 的 pybind](../umdk/src/cam/comm_operator/pybind/) 相比，vllm-ascend 的 C++ 扩展层规模更大、架构更复杂，算子数量约 **50+**（对比 umdk 的 9 个）。

关键架构差异：

| 维度 | umdk CAM pybind | vllm-ascend csrc |
|---|---|---|
| 算子数量 | 9 个 | 50+ 个 |
| 注册方式 | `PYBIND11_MODULE` + `TORCH_LIBRARY` | `TORCH_LIBRARY_EXPAND(_C_ascend, ops)` |
| 编译器 | setuptools + NpuExtension | CMake（多架构） |
| 文件组织 | 每算子一个 .cpp | 单文件 + adapter 头文件 |
| Autograd | 完整 forward/backward + Meta | 无 backward（纯推理） |
| 硬件适配 | 单一 SOC | A2/A3/310P/A5 多架构 |
| Python 层 | 独立 wheel | vLLM 平台插件 |

---

## 一、构建系统层

### 1. [setup.py](setup.py) — 构建入口

**做什么：**

| 职责 | 实现 |
|---|---|
| SOC 检测 | 通过 `npu-smi` 自动获取芯片型号（910b/910c/310p/950），映射为 device_type |
| CMake 配置 | `cmake_build_ext` 类完整管理 CMake configure → build → install 三阶段 |
| Wheel 打包 | `setuptools-scm` 自动版本管理 |
| ACLNN 构建 | `build_and_install_aclnn` 命令调用 `csrc/build_aclnn.sh` |
| 环境变量注入 | 将 `ASCEND_HOME_PATH`、`PYTHON_EXECUTABLE`、`TORCH_NPU_PATH` 等传入 CMake |

**SOC 检测逻辑（`get_chip_type()`）：**
```
npu-smi info -l → 获取 NPU ID
npu-smi info -t board -i <id> → 获取 Chip Name / Chip Type
  ├── "310" + ChipType → e.g., "ascend310p1"
  ├── "910" + ChipType  → e.g., "ascend910b1" (A2)
  ├── "910" + no type + NPU Name → e.g., "ascend910_9391" (A3)
  └── "950" + NPU Name → e.g., "ascend950_xxx" (A5)
```

**CMake 配置流程（`cmake_build_ext.configure()`）：**
1. 查找 `ASCEND_HOME_PATH`（默认 `/usr/local/Ascend/ascend-toolkit/latest`）
2. 查找 `TORCH_NPU_PATH`（通过 `pip show torch-npu`）
3. 获取 `pybind11` 的 cmake 路径（`python -m pybind11 --cmakedir`）
4. 设置 `SOC_VERSION`（CANN 编译目标）
5. 设置 `FETCHCONTENT_BASE_DIR`（依赖缓存目录）
6. 可选 `VLLM_ASCEND_ENABLE_BATCH_MEMCPY`

**为什么用 CMake 而非 setuptools+NpuExtension：**
- vllm-ascend 需要编译 AscendC kernel（`.cpp` → NPU 二进制），需要 CANN 编译器（`cce`），不是普通 C++ 编译器
- CMake 支持多架构编译（A2→arch32, A3→arch32, A5→arch35, 310P→arch22），同一套源码按 SOC 生成不同的 NPU 字节码
- CMake 的 `FetchContent` / `ExternalProject` 机制可以管理第三方依赖（abseil-cpp, protobuf, gtest, makeself）

### 2. [pyproject.toml](pyproject.toml) — 项目元数据

**关键配置：**
- **构建依赖：** 锁定 `torch==2.10.0`、`torch-npu==2.10.0`、`triton-ascend==3.2.1`
- **构建后端：** `setuptools.build_meta`（标准 PEP 517）
- **入口点：**
  ```python
  [project.entry-points]
  "vllm.platform_plugins" = "ascend = vllm_ascend:register"
  "vllm.general_plugins" = [
      "ascend_kv_connector = vllm_ascend:register_connector",
      "ascend_model_loader = vllm_ascend:register_model_loader",
      ...
  ]
  ```
  这是 vLLM 的插件发现机制，安装 vllm-ascend 后自动注册 Ascend 平台后端。

**设计理由：** vllm-ascend 不修改 vLLM 源码，通过 entry_points 注入平台实现。vLLM 启动时扫描插件注册表，发现的 Ascend 后端就自动接管 NPU 设备。

### 3. [csrc/CMakeLists.txt](csrc/CMakeLists.txt) — CANN 算子编译体系

这是 CANN 自定义算子（"transformer" 包）的标准 CMake 构建。核心流程：

```
SOC_VERSION → ARCH_DIRECTORY
  ascend310p    → arch22
  ascend910b*   → arch32 (A2)
  ascend910_93* → arch32 (A3)
  ascend950*    → arch35 (A5)

每个算子目录:
  op_host/   → host 端代码（tiling 策略 + proto 生成）
  op_kernel/ → device 端代码（AscendC kernel 实现，按架构分发到 arch32/arch35）
```

**算子模块组织：**
```
mc2/       — 多卡通信（dispatch_layout, dispatch_ffn_combine, matmul_allreduce_add_rmsnorm, ...）
moe/       — MoE（moe_gating_top_k, moe_init_routing, causal_conv1d, ...）
gmm/       — 分组矩阵乘（grouped_matmul_swiglu_quant, ...）
attention/ — 注意力（sparse_flash_attention, lightning_indexer, compressor, ngram_spec_decode, ...）
ffn/       — 前馈网络
posembedding/ — 位置编码
common/    — 通用工具
```

**关键宏和编译选项：**
- `BUILD_WITH_3_8_PACKAGE` — Ascend950 使用 3~8 包分割编译
- `ENABLE_CCACHE` — 编译器缓存加速
- `ENABLE_STATIC` — 静态库模式
- `SOC_VERSION_LIST` — `ascend310p;ascend910b;ascend910_93;ascend950;kirinx90`

---

## 二、基础设施层（`csrc/` 根目录 + `aclnn_torch_adapter/`）

### 4. [torch_binding.cpp](csrc/torch_binding.cpp) — 巨型绑定文件（~2940 行）

**做什么：** 所有 C++ 算子实现 + Torch 注册的**单文件聚合**。

**文件结构：**
```
namespace vllm_ascend {
    // === 工具函数 ===
    DevicePrintPayload / device_print_callback   — NPU 设备打印（debug 用）
    swap_blocks_batch()                          — KV cache block 批量搬移
    get_dtype_from_torch()                       — PyTorch dtype → AscendType

    // === 直接 kernel 包装（#ifdef VLLM_ENABLE_ATB_AND_DIRECT_KERNELS）===
    swap_blocks()           — KV block swap
    get_masked_input_and_mask()  — VocabParallelEmbedding
    bgmv_shrink/expand()    — LoRA BGMV (batch grouped matrix-vector)
    sgmv_shrink/expand()    — LoRA SGMV (segmented grouped matrix-vector)

    // === MoE dispatch/combine 算子 ===
    dispatch_prefill()      — MoE token dispatch (all-to-all)
    // combine_prefill 在 adapter 头文件中实现

    // === CANN aclnn 算子包装（每个函数调用 EXEC_NPU_CMD）===
    npu_gemma_rms_norm()            — Gemma RMSNorm
    npu_hamming_dist_top_k()        — 汉明距离 top-k 检索
    npu_reshape_and_cache_bnsd()    — KV cache reshape
    npu_sign_bits_pack()            — 符号位压缩
    npu_copy_and_expand_eagle_inputs() — Eagle 投机解码
    npu_causal_conv1d_custom()      — 因果卷积1D
    moe_grouped_matmul()            — MoE 分组矩阵乘
    moe_gating_top_k_hash()         — MoE 门控 top-k（含 bias/input_ids/tid2eid）
    compressor()                    — KV cache 压缩（注意力压缩器）
    npu_quant_lightning_indexer()   — 量化闪电索引器
    npu_sparse_attn_sharedkv()      — 稀疏注意力（共享 KV）
    npu_hc_post/pre/sinkhorn/inv_rms() — HC (Hierarchical Context) 算子系列
    npu_rms_norm_dynamic_quant()    — RMSNorm + 动态量化融合
    indexer_compress_epilog()       — 索引器压缩收尾
    kv_compress_epilog()            — KV 压缩量化写入
    npu_kv_quant_sparse_attn_sharedkv() — 量化 KV + 稀疏注意力
    npu_swiglu_group_quant()        — SwiGLU + 分组量化
    npu_dequant_swiglu_quant()      — 反量化 → SwiGLU → 量化
    npu_scatter_nd_update_v2()      — 散列更新
    chunk_gated_delta_rule_fwd_h()  — 分块门控 Delta 规则（线性注意力）
    chunk_fwd_o()                   — 分块前向输出
}

// === Torch 算子注册 ===
#ifdef ASCEND_PLATFORM_310P
    TORCH_LIBRARY_EXPAND(_C_ascend, ops) { ... }  // 310P 仅有 2 个算子
#else
    TORCH_LIBRARY_EXPAND(_C_ascend, ops) { ... }  // 50+ 算子
#endif
```

**为什么是单文件而非多文件：**
- 编译效率：所有算子共享同一套头文件（`acl_torch_adapter/op_api_common.h`、`NPUBridge.h`），分成多文件会增加重复编译开销
- 但 2940 行对维护性有影响。注释中已表示部分算子计划迁移到 CANN PTA（Platform Training Adapter）后删除，届时文件会缩小

### 5. [ops.h](csrc/ops.h) — 直接 kernel 声明

**做什么：** 声明以下 AscendC 直接 kernel 的函数签名：

| 函数 | 用途 |
|---|---|
| `get_masked_input_and_mask_impl` | VocabParallelEmbedding 的 mask 生成 |
| `bgmv_shrink_impl / bgmv_expand_impl` | LoRA Batch GEMV (减秩/扩秩) |
| `sgmv_shrink_impl / sgmv_expand_impl` | LoRA Segmented GEMV |
| `mla_preprocess_impl` | MLA (Multi-head Latent Attention) 预处理 |
| `batch_matmul_transpose_impl` | 批量矩阵乘转置 |

这些是**绕过 CANN aclnn 框架**、直接在 AscendC 层面实现的 kernel。它们通过 `OpCommand::SetCustomHandler` 提交到 NPU stream，不经过 aclnn 的 workspace/executor 机制。

### 6. [utils.h](csrc/utils.h) — 工具宏

提供 `TORCH_BIND_ASSERT` 宏（在 `torch_binding.cpp` 中使用）：

```cpp
#define TORCH_BIND_ASSERT(cond)                                                \
    do {                                                                       \
        if (not(cond)) {                                                       \
            throw std::runtime_error("Assertion " #cond " failed");            \
        }                                                                      \
    } while (0)
```

### 7. [aclnn_torch_adapter/op_api_common.h](csrc/aclnn_torch_adapter/op_api_common.h) — ACLNN 调用框架

与 umdk 的 `pytorch_npu_helper.hpp` 功能完全相同，提供：

- `EXEC_NPU_CMD(aclnn_api, ...)` 宏 — 标准 aclnn 调用流程
- `ConvertType()` 系列 — PyTorch → ACL 类型转换
- `Release()` 系列 — ACL 资源释放
- `GetOpApiFuncAddr()` — 动态加载 CANN 算子 API

这是华为昇腾生态中标配的"aclnn 绑定胶水层"，umdk 和 vllm-ascend 各有一份独立拷贝，因为它们是独立开发的包。

### 8. [aclnn_torch_adapter/NPUBridge](csrc/aclnn_torch_adapter/NPUBridge.cpp) + [NPUStorageImpl](csrc/aclnn_torch_adapter/NPUStorageImpl.cpp) — NPU 存储桥接

与 umdk 中的 `NPUBridge` / `NPUStorageImpl` 相同功能：
- `NPUBridge` — 将 `c10::StorageImpl*` 安全地 static_cast 为 `NPUStorageImpl*`，读取 NPU format 信息
- `NPUStorageImpl` — 继承 `StorageImpl`，附加 `NPUStorageDesc`（npu_format, storage_sizes, base_sizes, base_strides）

**注意命名空间差异：** vllm-ascend 中放在 `vllm_ascend::` 命名空间（vs umdk 的 `umdk::`），两个实现是独立维护的。

### 9. [camem_allocator.cpp](csrc/camem_allocator.cpp) — NPU 自定义内存分配器

vllm-ascend 特有的文件，为 NPU 设备提供 PyTorch 自定义分配器。这是 vllm-ascend 作为 vLLM 推理框架必须的组件——推理需要精细控制 NPU 显存分配/释放（KV cache、weight cache 等）。

---

## 三、算子实现层的三种模式

### 模式 1：ACLNN 算子（通过 `EXEC_NPU_CMD`）

**代表：** 绝大多数算子（`npu_gemma_rms_norm`、`npu_hc_pre`、`moe_gating_top_k_hash` 等）

**调用链：**
```
Python: torch.ops._C_ascend.npu_gemma_rms_norm(x, gamma, eps)
  → Torch Dispatcher
    → PrivateUse1 (NPU) → vllm_ascend::npu_gemma_rms_norm()
      → EXEC_NPU_CMD(aclnnGemmaRmsNorm, x, gamma, epsilon, y, rstd)
        → dlsym 获取 aclnnGemmaRmsNorm / aclnnGemmaRmsNormGetWorkspaceSize
        → ConvertTypes 转换为 ACL 类型
        → 查询 workspace 大小、分配 workspace
        → 提交到 NPU stream 执行
```

**特点：**
- 不参与 autograd（纯推理场景，无需 backward）
- 不注册 Meta dispatch key（纯推理，不需要 `torch.compile` 的图捕获）
- 算子实现在 CANN 的 `libcust_opapi.so` / `libopapi.so` 中，binding 层只做参数转换

### 模式 2：`_torch_adpt.h` 头文件适配器

**代表：** 19 个 `*_torch_adpt.h` 文件（`dispatch_ffn_combine_torch_adpt.h`、`grouped_matmul_swiglu_quant_torch_adpt.h` 等）

**实现方式：** 直接在头文件中实现完整的算子包装函数（inline），在 `torch_binding.cpp` 中 `#include` 并注册。

**示例：**[dispatch_ffn_combine_torch_adpt.h](csrc/mc2/dispatch_ffn_combine/dispatch_ffn_combine_torch_adpt.h)：
```cpp
namespace vllm_ascend {
std::tuple<at::Tensor&, at::Tensor&> dispatch_ffn_combine(...) {
    char *group_ep_ptr = const_cast<char *>(group.data());
    bool is_int8 = weight1[0].dtype() == at::kChar;
    bool is_int4 = weight1[0].dtype() == at::kInt;

    if (is_int8) {
        EXEC_NPU_CMD(aclnnDispatchFFNCombine, ...);      // INT8 路径
    } else if (is_int4) {
        EXEC_NPU_CMD(aclnnDispatchFFNCombineW4A8, ...);  // INT4 路径
    } else {
        EXEC_NPU_CMD(aclnnDispatchFFNCombineBF16, ...);  // BF16 路径
    }
    return {out, expert_token_nums};
}
}
```

**为什么用头文件而非 .cpp：**
- 每个 adapter 函数简短（通常 20-40 行），不值得独立编译单元
- inline 在 `torch_binding.cpp` 的编译上下文中展开，避免额外的符号导出
- 这些 adapter 文件也作为 CANN 算子的"示例/文档"——它们和 CANN kernel 源码放在一起，形成对照

### 模式 3：直接 AscendC kernel（通过 `OpCommand::SetCustomHandler`）

**代表：** `get_masked_input_and_mask`、`bgmv_shrink/expand`、`sgmv_shrink/expand`、`mla_preprocess`、`batch_matmul_transpose`

**调用链：**
```
Python: torch.ops._C_ascend.get_masked_input_and_mask(input, ...)
  → vllm_ascend::get_masked_input_and_mask()
    → at_npu::native::OpCommand cmd;
      cmd.Name("get_masked_input_and_mask");
      cmd.SetCustomHandler([captured params]() -> int {
          get_masked_input_and_mask_impl(stream, input_ptr, ...);
          return 0;
      });
      cmd.Run();
```

**与 EXEC_NPU_CMD 的差异：**

| 特性 | EXEC_NPU_CMD (aclnn) | OpCommand::SetCustomHandler |
|---|---|---|
| 算子来源 | CANN 预编译 so（`libcust_opapi.so`） | vllm-ascend 自己编译的 AscendC kernel |
| 加载方式 | dlsym 动态加载 | 静态链接（同编译单元） |
| workspace | 自动管理 | 自行处理 |
| 适用场景 | 标准 CANN 算子 | 需要定制 tiling 策略的性能敏感 kernel |

**为什么需要直接 kernel：**
- LoRA BGMV/SGMV：vLLM 特有的计算模式，CANNa 中没有对应算子
- MLA Preprocess：DeepSeek-V2/V3 的 MLA 结构需要特殊的预处理融合，通用 CANN 算子无法满足
- 性能：直接控制 Vector Core 的调度粒度（`aiv_num`），针对 vLLM 的小 batch 场景优化

**`bgmv_shrink_impl` 的 AscendC 调用模式：**
1. 查询设备 Vector Core 数量：`aclGetDeviceCapability(device_id, ACL_DEVICE_INFO_VECTOR_CORE_NUM, &aiv_num)`
2. 计算每个 core 处理的 token 数：`num_tokens_per_core = (batch_size + aiv_num - 1) / aiv_num`
3. 调用 AscendC kernel：`bgmv_shrink_impl(dtype, stream, x_ptr, weight_ptr, ...)`

---

## 四、算子注册层

### 注册机制：`TORCH_LIBRARY_EXPAND`

```cpp
TORCH_LIBRARY_EXPAND(CONCAT(_C, _ascend), ops) {
    ops.def("op_name(Tensor x, int param) -> Tensor");
    ops.impl("op_name", torch::kPrivateUse1, &vllm_ascend::op_name);
}
```

**与 umdk 的差异：**

| | umdk pybind | vllm-ascend |
|---|---|---|
| 注册宏 | `TORCH_LIBRARY` | `TORCH_LIBRARY_EXPAND` |
| 模块名 | `umdk_cam_op_lib` | `_C_ascend`（扩展 vLLM 的 `_C` 模块） |
| Python 绑定 | `PYBIND11_MODULE` 创建独立模块 | 不需要 `PYBIND11_MODULE`，算子通过 Torch dispatch 访问 |
| 设备 dispatch | `PrivateUse1` + `AutogradPrivateUse1` + `Meta` | 仅 `PrivateUse1`（推理场景，不需要 autograd 和 Meta） |

**`TORCH_LIBRARY_EXPAND` vs `TORCH_LIBRARY`：**
- `TORCH_LIBRARY_EXPAND` 向已存在的 Torch library 添加算子，不会覆盖已有定义
- vLLM 本身的 C++ 扩展注册在 `_C` library 中，vllm-ascend 通过 `_C_ascend` library 扩展
- Python 端通过 `torch.ops._C_ascend.op_name(...)` 访问

### 310P 平台特化

```cpp
#ifdef ASCEND_PLATFORM_310P
// 310P 仅暴露 2 个算子：npu_causal_conv1d_310 + npu_recurrent_gated_delta_rule_310
TORCH_LIBRARY_EXPAND(CONCAT(_C, _ascend), ops) { ... }
#else
// A2/A3/A5 暴露 50+ 算子
TORCH_LIBRARY_EXPAND(CONCAT(_C, _ascend), ops) { ... }
#endif
```

**理由：** 310P 是低端推理芯片（Atlas 300I），计算能力有限，很多 MoE/注意力融合算子不适用。编译时直接排除，减小二进制体积。

---

## 五、kernel 源码组织（`csrc/kernels/` 和 `csrc/mc2/` 等目录）

### 直接 kernel 实现

| 文件 | 实现内容 |
|---|---|
| [kernels/get_masked_input_and_mask_kernel.cpp](csrc/kernels/get_masked_input_and_mask_kernel.cpp) | VocabParallelEmbedding mask + input 生成 |
| [kernels/bgmv_shrink.cpp](csrc/kernels/bgmv_shrink.cpp) | LoRA BGMV 减秩 |
| [kernels/bgmv_expand.cpp](csrc/kernels/bgmv_expand.cpp) | LoRA BGMV 扩秩 |
| [kernels/sgmv_shrink.cpp](csrc/kernels/sgmv_shrink.cpp) | LoRA SGMV 减秩（带 seq_len） |
| [kernels/sgmv_expand.cpp](csrc/kernels/sgmv_expand.cpp) | LoRA SGMV 扩秩（带 seq_len） |
| [kernels/math_utils.h](csrc/kernels/math_utils.h) | 数学工具（vec 操作、量化转换） |
| [kernels/utils.h](csrc/kernels/utils.h) | kernel 通用工具 |
| [kernels/types.h](csrc/kernels/types.h) | 类型定义（`AscendType::FP16/FP32/BF16`） |

### 复杂 kernel：MLA Preprocess

[mla_preprocess/](csrc/mla_preprocess/) 是 DeepSeek-V3 MLA 结构的预处理 kernel：

```
mla_preprocess_torch_adpt.h    — PyTorch adapter
op_host/mla_preprocess.h       — host 端 tiling 策略
op_kernel/
  ├── mla_preprocess_kernel.cpp              — kernel 入口
  ├── mla_preprocess_mix_bf16.hpp            — BF16 混合精度
  ├── mla_preprocess_mix_bf16_nq.hpp         — BF16 + NQ (no quant)
  ├── mla_preprocess_mix_bf16_qdown.hpp      — BF16 + Q-down projection
  ├── mla_preprocess_mix_fp16.hpp            — FP16 混合精度
  └── kernel/                                — 通用 kernel 原语
      ├── common.h, iterator.h, layout.h, mem.h, mma.h, simd.h, ...
      └── iterators/  — 数据搬运迭代器 (GM→L1, L1→UB, L0C→GM, ...)
```

每个 `mix_*.hpp` 是不同精度/配置组合的特化 kernel。这种"模板 + 显式实例化"的方式是 AscendC kernel 编译的标准模式——CANN 编译器需要知道具体的 tiling 参数和数据类型才能生成最优的 NPU 指令。

### 复杂 kernel：Batch Matmul Transpose

[batch_matmul_transpose/](csrc/batch_matmul_transpose/) 批量矩阵乘+转置融合：
```
batch_matmul_transpose_torch_adpt.h   — PyTorch adapter
op_host/
  ├── batch_matmul_transpose.h        — host 端接口
  ├── common.h, common_tiling.h        — tiling 公共逻辑
  └── tiling/tiling_data.cpp/h        — tiling 数据管理
op_kernel/
  └── batch_matmul_transpose_kernel.cpp  — AscendC kernel 实现
```

### 复杂 kernel：Attention Compressor

[attention/compressor/](csrc/attention/compressor/) 是 KV cache 注意力压缩器：

```
op_host/
  ├── compressor_def.cpp     — 算子定义
  ├── compressor_proto.cpp   — proto 生成
  ├── compressor_tiling.cpp  — tiling 策略
  └── compressor_tiling.h
op_kernel/
  ├── compressor.cpp                               — kernel 入口
  ├── compressor_comm.h, compressor_kernel.h        — kernel 通信
  ├── compressor_template_tiling_key.h              — tiling key 模板
  ├── arch32/                                       — A2/A3 架构
  │   ├── compressor_block_cube_perf.h               — Cube 单元优化
  │   ├── compressor_block_vec_perf.h                — Vector 单元优化
  │   ├── compressor_vector_comm.h                   — Vector 通信
  │   ├── rms_norm.h, rope.h, soft_max.h             — 融合算子组件
  │   └── arch35/                                   — A5 架构
  │       ├── compressor_block_cube_perf.h
  │       ├── compressor_block_vec_perf.h
  │       └── vf/                                    — Vector Fusion 原语
  │           ├── vf_add.h, vf_mul.h, vf_rms_norm.h
  │           ├── vf_rope.h, vf_softmax.h
```

**arch32 vs arch35 的差异：**
- arch32 (Ascend910B/C/93): 更成熟，kernel 实现经过充分优化
- arch35 (Ascend950): 新一代架构，引入了 Vector Fusion (VF) 指令——可以将 add/mul/rms_norm 等操作融合为一条 VF 指令，大幅减少指令发射和寄存器压力

---

## 六、Python 层对接

### 算子调用路径

Python 代码通过 `torch.ops._C_ascend.op_name(...)` 调用：

```python
# vllm_ascend/ops/fused_moe/ 等模块中
import torch
output = torch.ops._C_ascend.dispatch_ffn_combine(x, weight1, weight2, ...)
```

### ASCEND_PLATFORM_310P 的编译时条件

`setup.py` 中通过 `gen_build_info()` 生成 `_build_info.py`：
```python
# Auto-generated
__device_type__ = 'A2'  # 或 'A3', 'A5', '_310P'
```

这个变量在编译时影响 `#define VLLM_ENABLE_ATB_AND_DIRECT_KERNELS` 等宏定义，控制哪些 kernel 被编译进 so。

### 模块结构

```
vllm_ascend/
  __init__.py        — register() 函数，注册 vLLM 平台插件
  ops/               — 算子 Python 封装层
    fused_moe/       — 融合 MoE 算子封装
    triton/          — Triton DSL 实现的算子（triton-ascend）
      fla/           — Flash Linear Attention
      mamba/         — Mamba SSM 算子
      activation/    — 激活函数
      spec_decode/   — 投机解码
  worker/            — Worker 实现
  models/            — 模型适配（Qwen, DeepSeek, GLM, Hunyuan, ...）
  attention/         — 注意力后端（context_parallel, kvcomp_attn）
  distributed/       — 分布式通信（kv_transfer, device_communicators）
  compilation/       — 图编译优化 pass
  quantization/      — 量化方法（FP8, INT8, INT4, MXFP, ...）
  eplb/              — Expert Parallelism Load Balancing
  lora/              — LoRA 后端
  _310p/             — 310P 平台特化算子封装
```

---

## 七、与 umdk CAM pybind 的对比总结

| 维度 | umdk CAM pybind | vllm-ascend csrc |
|---|---|---|
| **定位** | 通信算子库绑定 | vLLM 推理框架后端插件 |
| **算子数量** | 9（MoE dispatch/combine） | 50+（注意力、MoE、GMM、HC、量化...） |
| **架构** | 每算子独立 .cpp，统一 4 层模式 | 单文件 + adapter 头文件 + AscendC kernel |
| **Autograd** | 完整 forward/backward + Meta | 无（纯推理场景） |
| **注册方式** | `PYBIND11_MODULE` + `TORCH_LIBRARY` | `TORCH_LIBRARY_EXPAND` |
| **编译系统** | setuptools + NpuExtension | CMake + CANN 编译器 (cce) |
| **算子来源** | 全部来自 CANN aclnn | aclnn + 自研 AscendC kernel + ATB kernel |
| **硬件适配** | 单 SOC | A2(arch32) / A3(arch32) / A5(arch35) / 310P(arch22) |
| **NPU 桥接** | 相同机制（NPUBridge + NPUStorageImpl） | 相同机制（独立拷贝） |
| **ACLNN 胶水** | `pytorch_npu_helper.hpp` | `op_api_common.h`（功能一致，独立拷贝） |
| **Python 集成** | 独立 Python wheel | vLLM 平台插件（entry_points） |
| **内存管理** | 依赖 torch_npu | 自定义 NPU 分配器（`camem_allocator.cpp`） |
| **代码规模** | ~19 文件，~2000 行 C++ | ~100+ 文件，~2940 行核心 C++ + 大量 AscendC kernel |

### 关键设计取舍

**1. 为什么 vllm-ascend 不参与 autograd：**
vLLM 是推理框架，所有计算都在 `torch.no_grad()` 下执行。Autograd 的 forward/backward 机制是训练框架的需求，推理阶段不需要。省去 autograd 层减少了代码复杂度和运行时开销。

**2. 为什么 vllm-ascend 用 `TORCH_LIBRARY_EXPAND` 而非 `PYBIND11_MODULE`：**
vllm-ascend 是 vLLM 的插件，需要将算子注入到 vLLM 已有的 Torch library 命名空间。`TORCH_LIBRARY_EXPAND` 允许在不创建新 Python 模块的情况下添加算子，算子在 Python 端通过 `torch.ops._C_ascend.*` 统一访问。

**3. 为什么有两套 aclnn 胶水代码：**
umdk 和 vllm-ascend 是两个独立维护的项目。`op_api_common.h` 和 `pytorch_npu_helper.hpp` 功能完全一致（`EXEC_NPU_CMD`、`ConvertType`、`dlsym` 动态加载），但因为发布时间线不同、依赖版本不同，各自维护一份。未来可能会统一到 torch_npu 框架中。

**4. 为什么部分 kernel 绕过 aclnn：**
aclnn 框架提供的通用算子通常针对大 batch 训练场景优化。vLLM 推理面临的是小 batch、高并发、延迟敏感的场景，通用的 aclnn 算子无法覆盖所有优化空间（如 LoRA SGMV 的 `num_tokens_per_core` 调度、MLA Preprocess 的特殊融合模式）。直接写 AscendC kernel 可以精细控制 Vector Core 利用率。

**5. 为什么用单文件 torch_binding.cpp 而非多文件：**
vllm-ascend 的算子大多是薄包装（50-80 行），独立编译单元的开销大于收益。但注释中也提到部分算子"planned to be integrated into PTA soon"，届时会被删除，文件会自然缩小。对于较复杂的 kernel（MLA、batch_matmul_transpose、compressor），adapter 仍然是独立文件，通过 `_torch_adpt.h` 包含。
