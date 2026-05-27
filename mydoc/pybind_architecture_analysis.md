# pybind 目录架构分析

## 目录总览

`pybind/` 目录是 **UMDK CAM 通信算子** 的 PyTorch C++ 扩展绑定层。它将华为昇腾 NPU 上的 MoE（Mixture of Experts）通信算子（dispatch / combine）暴露给 PyTorch，使上层 Python 代码能直接调用 NPU 加速的算子。

文件共 19 个，按职责分为 **4 层**。

---

## 一、基础设施层（4 个文件）

### 1. pybind.cpp — 模块入口 + Schema 注册

承担两个职责：

| 机制 | 作用 |
|---|---|
| `PYBIND11_MODULE` (L13-24) | 创建 Python 模块 `umdk_cam_op_lib`，绑定 9 个函数的 autograd 版本到 Python |
| `TORCH_LIBRARY` (L26-59) | 向 Torch 分发器注册所有算子的**类型签名**（schema），使 `Dispatcher::findSchemaOrThrow` 能找到它们 |

**设计理由：**
- 双重注册是必需的。`TORCH_LIBRARY` 定义 Torch 内部的算子 schema（类型安全的函数签名），有了它才能做设备分发（`PrivateUse1` → NPU，`Meta` → 形状推导），以及 `torch.compile` 捕获计算图。
- `PYBIND11_MODULE` 让 Python 端能 `import umdk_cam_op_lib` 并直接调用算子。两个路径最终都调到同一份 NPU 实现。

### 2. functions.h — 公开接口声明

声明 9 个 `*ImplAutograd` 函数签名，与 `pybind.cpp` 中的调用一一对应。

**设计理由：** 标准 C++ 头文件分离。`pybind.cpp` 只做注册，实际的 `ImplAutograd` 实现在各自的 `.cpp` 中。新增算子只需加一个 `.cpp` 文件 + 一行声明 + 一行注册，不碰其他代码。

### 3. torch_bind_exception.h — 自定义异常 + 断言宏

定义 `TorchBindException`（继承 `std::exception`），携带文件名、行号、条件表达式。宏 `TORCH_BIND_ASSERT(cond)` 在 cond 为 false 时抛出该异常。

**设计理由：**
- Ascend CANN API 的错误信息有时不够精确。在 binding 层做**前置校验**（维度、dtype、contiguous、参数范围），尽早报错，避免将非法输入传入 NPU kernel 后得到难以排查的运行时错误。
- 携带 `__FILE__:__LINE__` 能快速定位哪个算子入参不合法。

### 4. setup.py — 构建系统

用 setuptools + `torch_npu.utils.cpp_extension.NpuExtension` 编译所有 `.cpp` 文件为共享库 `umdk_cam_op_lib`。

关键配置：
- **Include 路径：** 串联了 torch、torch_npu、Ascend CANN（acl、hccl、experiment runtime）的头文件
- **链接库：** 链接 `torch_npu`、`ascendcl`、`runtime`、`hccl` 等 NPU 运行时
- **编译选项：** Release 模式 `-O2`，Debug 模式 `-g -O0`，支持 `-coverage`
- **依赖管理：** `BdistWheelBuild.dependencies` 声明 so 依赖（`libtorch.so`、`libtorch_npu.so` 等），打包 wheel 时排除这些系统库

**设计理由：**
- 不走 CMake，因为 PyTorch 生态的标准方式是 `torch.utils.cpp_extension` + setuptools。`NpuExtension` 继承自 `BuildExtension`，额外处理了 NPU 相关的编译标志。
- 版本号从环境变量 `CAM_WHL_VERSION` 读取，方便 CI/CD 注入。

---

## 二、辅助层（4 个文件，均在 `pytorch_extension/` 子目录）

### 5. pytorch_npu_helper.hpp — 核心宏 + 类型转换工具

整个目录最重要的基础设施文件。提供：

| 组件 | 功能 |
|---|---|
| `EXEC_NPU_CMD(aclnn_api, ...)` 宏 | 把 `aclnn*` CANN API 调用包装为标准执行流程 |
| `ConvertType()` 模板函数系列 | 将 PyTorch 类型（`at::Tensor`, `c10::optional<Tensor>`, `Scalar`, `IntArrayRef`, `TensorList` 等）转换为 Ascend ACL 类型（`aclTensor*`, `aclScalar*`, `aclIntArray*` 等） |
| `Release()` 系列 | 释放对应的 ACL 资源 |
| `GetOpApiFuncAddr()` | 通过 `dlsym` 动态加载 CANN 算子 API（先找 `libcust_opapi.so`，再 fallback 到 `libopapi.so`） |

**`EXEC_NPU_CMD` 的执行流程：**
1. `dlsym` 获取 `aclnn_api` 和 `aclnn_apiGetWorkspaceSize` 的函数指针
2. 获取当前 NPU stream
3. 将全部参数通过 `ConvertTypes` 转为 ACL 类型
4. 调用 `GetWorkspaceSize` 确定所需 workspace 大小
5. 在 NPU 上分配 workspace 内存
6. 构造 lambda（内调用实际的算子 API），通过 `OpCommand::SetCustomHandler` + `Run()` 提交到 NPU stream
7. 释放转换过程中创建的临时 ACL 对象

**设计理由：**
- **宏而非函数**：每个 `aclnn*` API 有独特的函数签名（参数个数/类型各不同），C++ 模板无法优雅地处理"参数个数可变+类型各异"的通用调用。宏在编译期展开，每个调用点生成特化的代码。
- **动态加载（dlsym）而非静态链接**：CANN 算子是独立编译的 `.so` 文件（`libcust_opapi.so` / `libopapi.so`），不属于 PyTorch 编译体系。动态加载允许二进制级别的解耦，算子库独立升级无需重新编译 binding 层。
- **workspace 机制**：NPU 算子通常需要临时内存。先查询大小再分配，避免浪费显存。
- **`ConvertType` 模板重载**：PyTorch 的 optional tensor、TensorList、IntArrayRef 等类型在 CANN API 中各有对应的 ACL 结构体。通过重载实现自动类型派发，调用方只需传原始 PyTorch 类型。

### 6-7. NPUBridge.h + NPUBridge.cpp — NPU 存储桥接

提供 `static_cast` 桥接，将 PyTorch 的 `c10::StorageImpl*` / `at::Tensor` 转为自定义的 `NPUStorageImpl*`，从而访问 NPU 特有的存储描述符（format、storage sizes 等）。

**设计理由：**
- PyTorch 的 `StorageImpl` 不感知 NPU 的内存格式（如 `ACL_FORMAT_ND`、`ACL_FORMAT_NCHW`、`FRACTAL_NZ` 等）。`NPUStorageImpl` 继承 `StorageImpl`，附加了 `npu_desc_` 字段存储格式信息。
- `NPUBridge` 的存在是因为跨库场景：torch_npu 框架层的 `NPUStorageImpl` 类型对 binding 层不可见（仅通过不透明指针传递）。通过 `static_cast` 重新解释为 binding 层自己的 `NPUStorageImpl` 定义，可以安全地读取 format 字段（两个定义在 ABI 上兼容）。

### 8. NPUStorageImpl.h + NPUStorageImpl.cpp — 自定义存储实现

定义 `NPUStorageImpl`（继承 `c10::StorageImpl`），附加 `NPUStorageDesc` 结构体记录 NPU 特有的 format/sizes/strides 信息。提供 `make_npu_storage_impl()` 工厂函数创建实例。

`NPUStorageDesc` 关键字段：

- `base_sizes_` / `base_strides_` / `storage_sizes_` — 逻辑尺寸/步幅 vs 物理存储尺寸
- `origin_format_` / `npu_format_` — 原始格式 vs NPU 实际格式（如 ND、NCHW、FRACTAL_Z）
- `data_type_` — 数据类型（用于 CANN GE tensor 构造）

**设计理由：**
- 这是 NPU 设备上的 Tensor 正确表示所必需的。PyTorch 原生 `StorageImpl` 只知道"字节块"，不知道 NPU 的 `aclFormat`。没有 `NPUStorageImpl`，`ConvertType` 就无法正确判断 tensor 是 ND 格式还是特殊格式（如 FRACTAL_Z），类型转换就会出错。

### 9. bdist_wheel_build.py — Wheel 打包定制

继承 `bdist_wheel`，在 manylinux 环境下自动调用 `auditwheel repair`，将非系统 `.so` 依赖打包进 wheel，同时排除声明为外部依赖的库（`libtorch.so` 等）。

**设计理由：** 标准做法。`auditwheel` 确保 wheel 可以在目标 manylinux 环境自包含运行，`--exclude` 阻止将 PyTorch/ torch_npu 的 so 打包进去（这些由用户环境提供）。

---

## 三、通信算子实现层（9 个文件 + 1 个辅助头文件）

### 统一架构

所有算子文件采用**统一的 4 层架构**：

```
Layer 1: *ImplNpu()        — 实际调用 aclnn* CANN API
Layer 2: *ImplMeta()       — Meta 设备上的形状推导（部分算子支持）
Layer 3: *Impl()           — 通过 Dispatcher 查找 schema 并 call
Layer 4: *ImplAutograd()   — 继承 torch::autograd::Function，forward 调 Impl()，backward 返回空梯度

注册:
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, PrivateUse1, ...)         — NPU 设备绑定
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, AutogradPrivateUse1, ...) — Autograd 绑定
TORCH_LIBRARY_IMPL(umdk_cam_op_lib, Meta, ...)                — Meta 设备绑定（可选）
```

### 完整调用链

```
Python调用
  → pybind11 → *ImplAutograd()
    → torch::autograd::Function::apply()
      → AutoDispatchBelowADInplaceOrView guard  // 防止递归
        → Dispatcher::call() → *Impl()
          → 根据设备分发:
              PrivateUse1/NPU → *ImplNpu()   → aclnn* CANN API
              Meta            → *ImplMeta()   → 只分配空tensor（形状推导）
              AutogradPrivateUse1 → *ImplAutograd() (反向时)
```

**关键设计点 `AutoDispatchBelowADInplaceOrView`：**
在 `forward()` 中设置此 guard 确保后续的 `Dispatcher::call()` 不会再次进入 autograd 层，而是直接路由到设备的物理实现（`PrivateUse1` → `ImplNpu`），避免无限递归。

---

### 核心算子分析

#### 10. get_dispatch_layout.cpp + get_dispatch_layout_a2.cpp — Dispatch 布局计算

**功能：** 根据 `topk_idx`（每个 token 选择的 top-k 专家编号），计算每个 expert 收到的 token 数量（`num_tokens_per_expert`）和按 rank 排序后的 token 索引（`send_token_idx_small`）。

**A2 变体差异：**

| 特性 | 基础版 | A2 版 |
|---|---|---|
| topk 范围 | 无限制 | `[2, 8]` |
| numRanks | 无限制 | `== 16` |
| 返回值 | `(num_tokens_per_expert, send_token_idx_small)` | `(num_tokens_per_expert, notify_send_data)` |
| 通信拓扑 | 普通 | 分层（server 内 HCCS + server 间网络），`localRankSize = 8` |

**必要性：** MoE dispatch 的第一步。需要知道每个 token 要发给哪个 rank 的哪个 expert，此算子负责把稀疏的 topk_idx 信息转化为结构化的通信元数据。

---

#### 11. moe_dispatch_prefill.cpp + moe_dispatch_prefill_a2.cpp — MoE 数据分发

**功能：** MoE **dispatch（分发）** 阶段。将输入的 token hidden states (`x`) 根据路由结果分发到各 rank 对应的 expert：

1. 先调用 `aclnnNotifyDispatch(Async)` 进行 all-to-all 通信，交换各 rank 的 token 数量信息
2. 再调用 `aclnnMoeDispatchNormal/A2` 真正搬运数据

**输出：** `(expandx_out, dynamic_scales_out, expand_idx_out, recv_count, recv_tokens_per_expert)`

**A2 变体差异：**
- 使用分层通信（HCCS + 网络拓扑感知），额外产生 `offset_inner`、`offset_outer`、`count_outer` 等分层路由元数据
- 支持 `expand_scales` 输出（量化缩放因子）
- 输入使用 `notify_send_data`（从 `get_dispatch_layout_a2` 产生）而非 `send_token_idx_small`
- **不支持 Meta 注册**（注释明确说明：Normal 类算子形状无法提前推导）

**两步 notify + dispatch 的设计理由：** All-to-all 通信需要先知道"我和每个 rank 交换多少数据"。notify 阶段交换 token count，dispatch 阶段根据 count 实际搬数据。这是标准的高效 all-to-all 模式。

---

#### 12. moe_combine_prefill.cpp + moe_combine_prefill_a2.cpp — MoE 结果聚合

**功能：** MoE **combine（聚合）** 阶段。Expert 计算完成后，将各 rank 上的结果按原始 token 位置汇总回去。调用 `aclnnMoeCombineNormal/A2`，使用 `epSendCounts`（各 rank 发回了多少数据）做反向的 all-to-all。

**输出：** 单个 `combined_x` tensor，形状为 `[num_tokens, hidden]`。

**A2 变体差异：** 额外输入 `expand_scales`、`offset_inner`、`offset_outer`、`count_outer`（都是 dispatch 阶段的产物），用于分层 combine 通信。

---

#### 13. moe_dispatch_shmem.cpp + moe_combine_shmem.cpp — 共享内存模式

**功能：** MoE **共享内存** 模式的 dispatch/combine。

**两种模式对比：**

| 特性 | prefill（网络）版本 | shmem（共享内存）版本 |
|---|---|---|
| 通信模式 | All-to-all over HCCL | Shared memory（NPU 间共享显存） |
| 参数数量 | ~9 个 | ~22 个 |
| EP+TP 混合 | 仅 EP | 支持 EP + TP 联合 |
| Shared Expert | 不支持 | 支持（某些 rank 承担共享 expert） |
| Meta 支持 | 无（形状无法提前推导） | 有（`ImplMeta` 注册了 Meta dispatch key） |
| A2 分层通信 | 有变体 | 无（shmem 不需要分层） |
| 输出形状推导 | 依赖通信结果 | `GenerateDispatchOutputTensor` 纯输入推导 |

**shared expert 的处理逻辑（`GenerateDispatchOutputTensor`）：**

```cpp
bool isSharedExpert = epRankId < sharedExpertRankNum;
if (epWorldSize == sharedExpertRankNum) {
    // 错误：所有 rank 都是 shared expert，没有 rank 处理普通 expert
}
int expertPerRank = moeExpertNum / (epWorldSize - sharedExpertRankNum);
// shared expert rank 的输出大小 = bs * epWorldSize / sharedExpertRankNum
// 普通 expert rank 的输出大小 = bs * epWorldSize * min(expertPerRank, k)
```

**为什么需要两种模式：**
- **Prefill（网络）模式**：适用于分布式多节点场景，通过 HCCL 网络做 all-to-all
- **Shmem（共享内存）模式**：适用于单机多卡场景，利用 NPU 间共享显存带宽（远高于网络带宽）做数据交换

---

#### 14. fused_deep_moe.cpp — 融合 Deep MoE 前向算子

**功能：** 融合的 Deep MoE 前向算子，调用 `aclnnFusedDeepMoe`，在一次 kernel 调用中完成：
- GMM1（Gate MLP layer 1）+ GMM2（Gate MLP layer 2）
- 共享 expert 的 GMM1 + GMM2
- 支持量化（`quant_mode`）、smooth scales、bias

**参数数量：** 23 个（包括多个 optional tensor 和 TensorList）。

**返回值：** `(output, share_output, expert_token_nums)`

**为什么注册了 Meta 实现：** 输出形状可以从输入形状完全推导：
- `output` / `shareOutput` 形状 = `[bs, h]`
- `expertTokenNums` 形状 = `[localExpertNum]`
不需要运行时信息。支持 Meta dispatch 意味着 `torch.compile` 的图捕获可以正确处理。

---

## 四、整体设计原则总结

### 1. 为什么分层

- **设备无关性**：上层 Python 代码用 `torch.ops.umdk_cam_op_lib.xxx(...)` 调用，Torch Dispatcher 自动按设备 dispatch。同一套代码在 CPU（Meta）上做图捕获，在 NPU 上做真实计算。
- **Autograd 兼容**：`torch::autograd::Function` 使这些自定义算子能参与 PyTorch 的自动微分图。虽然 MoE dispatch/combine 的 backward 当前返回空 tensor（梯度不需要通过通信算子传播，而是通过 expert 计算图传播），但框架上已经支持。

### 2. 为什么动态加载 CANN API

通过 `dlsym` 加载 CANN 算子 so（`libcust_opapi.so` → `libopapi.so`），编译时不需要链接具体的 `aclnn*` 实现。新版本 CANN 算子可以直接替换 so 而不需要重新编译 binding 层。这种二进制解耦对于硬件厂商的独立发布节奏至关重要。

### 3. 为什么 Meta 支持可选化

prefill 类算子输出形状依赖网络通信结果（all-to-all 后才知道收到多少 token），无法在 meta 阶段确定，因此**不注册** Meta dispatch key。而 shmem 和 fused_deep_moe 输出形状可推导，就注册了 Meta key 以支持 `torch.compile`。这体现了**按能力注册**的务实策略。

### 4. 为什么 backward 返回空梯度

MoE dispatch/combine 是纯通信算子，不涉及数学运算。路由决策（gate 输出）的梯度通过 expert 前向计算的 autograd 图传播，不需要通过通信算子本身反向传播。因此 backward 返回空的 `at::Tensor()`，这并非未实现，而是**语义上就不需要梯度**。

### 5. 为什么有两套 dispatch/combine（prefill vs shmem, 基础 vs A2）

- **prefill vs shmem**：对应不同的物理通信方式（网络 vs 共享显存），shmem 延迟更低但限于单机
- **基础 vs A2**：A2 是更复杂的拓扑感知版本（分层通信，HCCS + 网络两级），适合大规模集群部署。基础版更简单通用
- 两套变体虽然核心语义相同，但输入输出 tensor 数量和通信元数据格式差异大，因此各自独立实现而非模板化
