# FusedDeepMoe Grouped Matmul 内核分析（Q&A）

> 内核文件：`grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h`（下文简称 **kernel文件**）

---

## Q1: GemmIdentityBlockSwizzle 是怎么实现的？它有哪些功能，作用是什么？

文件：[block_swizzle.hpp](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp)

### 模板参数

[block_swizzle.hpp:24](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L24)

| 参数 | 默认值 | 含义 |
|---|---|---|
| `SwizzleOffset` | `1` | swizzle 的分组粒度（行/列方向上的偏移步长） |
| `SwizzleDirection` | `0` | `0` = Zn（沿 N 方向蛇形排列），`1` = Nz（沿 M 方向蛇形排列） |

### 数据成员

[block_swizzle.hpp:28-30](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L28-L30)

```cpp
GemmCoord problemShape;   // 总问题规模 (M, N, K)
MatrixCoord tileMN;       // 每个 tile 的大小 (行, 列)
MatrixCoord loopsMN;      // M/N 方向各需要多少个 tile = CeilDiv(problemShape.MN, tileMN)
```

### 关键方法

**构造与更新** [block_swizzle.hpp:34-64](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L34-L64)：提供默认构造、两参数 `(problemShape, tileMN)` 和三参数 `(problemShape, tileMN, loopsMN)` 构造，以及对应的 `Update()` 方法。两参数构造自动计算 `loopsMN = CeilDiv({M, N}, {tileM, tileN})`。

**`GetCoreLoops()`** [block_swizzle.hpp:66-70](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L66-L70) — 返回总 tile 数：`loopsMN.row() * loopsMN.column()`。

**`GetBatchIdx(taskIdx)`** [block_swizzle.hpp:72-76](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L72-L76) — 按总 tile 数切分得到 batch 索引：`taskIdx / GetCoreLoops()`。

**`GetBlockCoord(taskIdx)`** [block_swizzle.hpp:78-113](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L78-L113) — 核心方法，将线性 taskIdx 映射为 `GemmCoord{mIdx, nIdx, 0}`：

- 将 M 方向的 loops 按 `SwizzleOffset` 切分成 `tileBlockLoop` 个分组
- `tileBlockIdx = innerIdx / (SwizzleOffset × loopsMN.column())` — 确定在第几个 M 分组
- `inTileBlockIdx = innerIdx % (SwizzleOffset × loopsMN.column())` — 组内偏移
- 组内按行优先映射：`mIdx = tileBlockIdx × SwizzleOffset + inTileIdx % nRow`，`nIdx = inTileIdx / nRow`
- **蛇形翻转** [block_swizzle.hpp:93-95](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L93-L95)：奇数分组 `tileBlockIdx % 2 == 1` 时，N 坐标翻转 `nIdx = nCols - nIdx - 1`，改善数据访问局部性

`SwizzleDirection == 1`（Nz 模式）[block_swizzle.hpp:97-112](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L97-L112) 逻辑对称，沿 N 方向分组、沿 M 方向蛇形翻转。

**`GetActualBlockShape(blockCoord)`** [block_swizzle.hpp:115-124](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L115-L124) — 处理边界残余：

```cpp
mActual = (mIdx == loopsMN.row() - 1)    ? (currentM - mIdx * tileM) : tileM;
nActual = (nIdx == loopsMN.column() - 1) ? (N - nIdx * tileN)        : tileN;
kActual = K;  // K 维在此层不切分
```

### 作用

将 1D task 序列映射为 2D 的 M×N 分块坐标，通过蛇形翻转优化数据访问模式。它本质上是一个 "identity" swizzle（不做实际数据重排），只提供任务到坐标的映射。

### 派生类（均在同文件 block_swizzle.hpp 中）

| 类 | 行号 | 场景 |
|---|---|---|
| `DynamicGemmIdentityBlockSwizzle` | [127-189](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L127-L189) | `swizzleOffset` 和 `direction` 运行时动态指定 |
| `SplitkGemmIdentityBlockSwizzle` | [192-300](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L192-L300) | Split-K 场景，M×N 之上增加 K 维切分和 splitk slice 管理 |
| `SingleCoreSplitkGemmIdentityBlockSwizzle` | [304-429](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L304-L429) | 单核 Split-K，增加首块边界融合逻辑 |
| `StreamkGemmIdentityBlockSwizzle` | [704-886](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L704-L886) | Stream-K 算法，支持 normal block 和跨 K 边界的 streamk block |
| `GemmIdentityBlockSwizzleL1FullLoad` | [504-633](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L504-L633) | 多核 L1 满载调度，将 task 按 core 均匀分配后再做 swizzle 映射 |

---

## Q2: 结合 groupIdx 循环，详细讲解是如何分割从而进行矩阵乘的？

> 代码位置：kernel文件 [groupIdx 循环 L2330-L2389](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2330-L2389)
> 所属类：`GroupedMatmulSliceMPerTokenDequantSwigluQuantMultiStageWorkspaceWithShallowDispatch` [kernel文件 L2122](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2122)
> AIC operator() 完整实现 [kernel文件 L2248-L2401](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2248-L2401)

### Group 的含义

每个 `groupIdx` 对应 MoE 中的一个 Expert。`params.problemCount` = 专家数量。每个专家有自己的权重矩阵 B，所有专家共享输入 token 矩阵 A，但不同专家处理的 token 子集不同。

### M 维切分：groupList 前缀和

[kernel文件 L2335-L2336](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2335-L2336)

```cpp
uint32_t currentM = (groupIdx == 0) ? groupList.GetValue(groupIdx)
                                    : (groupList.GetValue(groupIdx) - groupList.GetValue(groupIdx - 1));
```

- `groupList[i]` = 前 i+1 个专家累计收到的 token 总数
- `currentM` = 第 `groupIdx` 个专家分配的 token 数

例如 5 个 token 分发到 3 个专家，`groupList = [2, 3, 5]`：Expert 0 拿 2 个 token，Expert 1 拿 1 个，Expert 2 拿 2 个。

### 每个 Group 构造独立的 GEMM

[kernel文件 L2337](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2337)

```cpp
GemmCoord inGroupProblemShape{currentM, params.problemShape.n(), params.problemShape.k()};
```

做矩阵乘法 `A[Mᵢ × K] × B[K × N] → C[Mᵢ × N]`。

### 数据寻址

[kernel文件 L2362-L2367](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2362-L2367)

- **A 矩阵**：`gmGroupOffsetA` 是跨 group 的累积偏移（每个 expert 处理完后 `+= currentM * K`，见 [L2383](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2383)），加上组内 tile 偏移
- **B 矩阵**：TensorList 模式时直接切换 B tensor（无需偏移，见 [L2331-L2334](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2331-L2334)），否则用 `gmGroupOffsetB` 累积（见 [L2384-L2386](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2384-L2386)）
- **C 矩阵**（workspace）：layout 为 `RowMajor{tileM × coreNum × WORKSPACE_STAGES, tileN}`（见 [L2271](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2271)），每个 `(stageId, coreIdx)` 写不同行区域，实现多缓冲

### 多核分配：Stride 分散

[kernel文件 L2346-L2348](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2346-L2348)

```cpp
uint32_t startLoopIdx = ((coreIdx < startCoreIdx) ? (coreIdx + coreNum) : coreIdx) - startCoreIdx;
for (uint32_t loopIdx = startLoopIdx; loopIdx < coreLoops; loopIdx += coreNum)
```

所有 core 以 `coreNum` 为步长均匀瓜分 tile。`startCoreIdx` 的跨组错位机制（见 [L2388](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2388)）避免某些 core 反复拿到不满的边界 tile，保证跨 expert 的负载均衡。

### Multi-Stage 流水线

AIC（计算 core）和 AIV（向量 core）之间通过 cross-core flag 实现生产者-消费者流水线，回调机制见 [L2353-L2359](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2353-L2359)，stage 轮转见 [L2380](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2380)：

```
Stage 0: [AIC matmul] → flag → [AIV dequant+swiglu+quant] → flag释放
Stage 1: [AIC matmul] → flag → [AIV dequant+swiglu+quant] → flag释放
Stage 2: [AIC matmul] → flag → [AIV dequant+swiglu+quant] → flag释放
```

### 完整流程

```
for groupIdx in 0..problemCount:                    // Expert 级  [L2330]
    1. 获取 currentM（groupList 前缀和差分）                     [L2335-L2336]
    2. 构造 inGroupProblemShape{currentM, N, K}                 [L2337]
    3. blockScheduler.Update(...)                                [L2342]
    4. for loopIdx = startLoopIdx .. coreLoops step coreNum:    // Core 级  [L2348]
         → GetBlockCoord → GetActualBlockShape                  [L2350-L2351]
         → 计算 GM 偏移                                          [L2362-L2367]
         → blockMmad(...)                                        [L2370-L2378]
    5. 推进 gmGroupOffsetA                                       [L2383]
    6. 推进 gmGroupOffsetB（非 TensorList 模式）                    [L2384-L2386]
    7. 更新 startCoreIdx（跨组错位）                                [L2388]
```

| 层级 | 分割方式 | 调度器 | 代码位置 |
|---|---|---|---|
| Expert 级 | `groupIdx` 循环 | `groupList` 前缀和切分 M 维 | [L2330-L2389](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2330-L2389) |
| Tile 级 | M×N 二维切分 | `BlockScheduler` (swizzle) | [L2342-L2351](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2342-L2351) |
| Core 级 | Stride 分发 | `startLoopIdx + coreNum` 步进 | [L2346-L2348](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2346-L2348) |
| Stage 级 | AIC/AIV 多缓冲 | `WORKSPACE_STAGES` 轮转 | [L2353-L2380](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2353-L2380) |
| K 级 | 内部二级 tiling | `blockMmad` (L1 → L0) | blockMmad 内部 |

---

## Q3: 具体如何根据 inGroupProblemShape 及其他参数计算得到 coreLoops？

### Step 1：计算 loopsMN

调用点见 [kernel文件 L2342](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2342)：

```cpp
blockScheduler.Update(inGroupProblemShape, MakeCoord(L1TileShape::M, L1TileShape::N));
```

在 `GemmIdentityBlockSwizzle::Update` [block_swizzle.hpp:50-56](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L50-L56) 内部：

```cpp
void Update(GemmCoord const &problemShape_, MatrixCoord const &tileMN_) {
    problemShape = problemShape_;
    tileMN = tileMN_;
    loopsMN = CeilDiv(MatrixCoord(problemShape.GetCoordMN()), tileMN);
}
// = CeilDiv({currentM, N}, {tileM, tileN})
// loopsMN.row()    = ceil(currentM / tileM)
// loopsMN.column() = ceil(N / tileN)
```

其中 `GetCoordMN()` [gemm_coord.hpp:138-140](m:\code\catlass\include\catlass\gemm_coord.hpp#L138-L140) 从 `inGroupProblemShape{currentM, N, K}` 提取 `{currentM, N}`。`CeilDiv` [alignment.hpp:75](m:\code\catlass\include\catlass\detail\alignment.hpp#L75) 做逐元素向上取整除法。

### Step 2：计算 coreLoops

调用点见 [kernel文件 L2343](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2343)，实现见 [block_swizzle.hpp:66-70](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L66-L70)：

```cpp
uint32_t GetCoreLoops() const {
    return loopsMN.row() * loopsMN.column();
}
// = ceil(currentM / tileM) * ceil(N / tileN)
```

K 维度不参与此层 tiling——K 的切分在 `blockMmad` 内部用 `L1TileShape::K` → `L0TileShape::K` 的二级 tiling 完成。

### 数值示例

假设 `currentM=37, N=512, tileM=16, tileN=128, coreNum=4`：

```
loopsMN.row()    = ceil(37/16)  = 3
loopsMN.column() = ceil(512/128) = 4
coreLoops = 3 × 4 = 12
```

12 个 tile 分给 4 个 core，每 core 3 个 tile：

```
Core 0: loopIdx = 0, 4, 8
Core 1: loopIdx = 1, 5, 9
Core 2: loopIdx = 2, 6, 10
Core 3: loopIdx = 3, 7, 11
```

---

## Q4: 每个 loopIdx 得到的 blockCoord 和 actualBlockShape 有何不同？如何被使用？

### blockCoord = 逻辑坐标

`GetBlockCoord(loopIdx)` [block_swizzle.hpp:78-113](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L78-L113) 返回 `GemmCoord{mIdx, nIdx, 0}`，表示该 tile 在 M×N 网格中的位置。

```
           nIdx=0    nIdx=1    nIdx=2    nIdx=3
        ┌─────────┬─────────┬─────────┬─────────┐
mIdx=0  │ tile 0  │ tile 1  │ tile 2  │ tile 3  │  ← 每 tile 16×128 (满)
        ├─────────┼─────────┼─────────┼─────────┤
mIdx=1  │ tile 4  │ tile 5  │ tile 6  │ tile 7  │  ← 每 tile 16×128 (满)
        ├─────────┼─────────┼─────────┼─────────┤
mIdx=2  │ tile 8  │ tile 9  │ tile 10 │ tile 11 │  ← m 方向残余！只有 5 行
        └─────────┴─────────┴─────────┴─────────┘
```

### actualBlockShape = 实际尺寸

`GetActualBlockShape(blockCoord)` [block_swizzle.hpp:115-124](m:\code\catlass\include\catlass\gemm\block\block_swizzle.hpp#L115-L124)：

```cpp
GemmCoord GetActualBlockShape(GemmCoord blockCoord) {
    uint32_t mActual = (blockCoord.m() == (loopsMN.row() - 1)) ?
        (problemShape.m() - blockCoord.m() * tileMN.row()) : tileMN.row();
    uint32_t nActual = (blockCoord.n() == (loopsMN.column() - 1)) ?
        (problemShape.n() - blockCoord.n() * tileMN.column()) : tileMN.column();
    uint32_t kActual = problemShape.k();
    return GemmCoord{mActual, nActual, kActual};
}
```

K 方向始终是 `problemShape.k()`（K 维的 tiling 在 blockMmad 内部完成）。

| loopIdx | blockCoord {m,n} | actualBlockShape {m,n,k} | 说明 |
|---|---|---|---|
| 0 | {0, 0} | {16, 128, 4096} | 内部 tile |
| 3 | {0, 3} | {16, 128, 4096} | 末列但 M 方向未到底 |
| 8 | {2, 0} | {5, 128, 4096} | 末行 m=37-2×16=5 |
| 11 | {2, 3} | {5, 128, 4096} | 末行末列 |

### 两者用途

调用点见 [kernel文件 L2350-L2371](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2350-L2371)。

- **`blockCoord`** → 定位数据：用完整 tile 尺寸 × block 索引算出该 tile 在 A、B 矩阵中的起始地址。即使边界 tile 不满，起始地址仍按 tileM/tileN 对齐计算

```cpp
// kernel文件 L2362-L2363
MatrixCoord offsetA{blockCoord.m() * L1TileShape::M, blockCoord.k() * L1TileShape::K};
//                = mIdx * tileM                        = 0
MatrixCoord offsetB{blockCoord.k() * L1TileShape::K, blockCoord.n() * L1TileShape::N};
//                = 0                                  = nIdx * tileN
```

- **`actualBlockShape`** → 限定计算：传给 `blockMmad`（[L2371](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2371)），告诉它该 tile 的真实范围，在边界处正确处理残余，避免越界读/写

形象地说：`blockCoord` 是"房间号"（301 号房，按标准尺寸定位）；`actualBlockShape` 是"房间实际大小"（301 号只有标准间的 5/16 宽）。一个带你走到正确的门，一个告诉你这间房只有多大。

---

## Q5: `offsetA` 和 `gmOffsetA` 这两行实现了什么？MatrixCoord 是什么？GetOffset 什么作用？blockCoord.m() 获取的是啥？

> 核心代码：[kernel文件 L2362-L2367](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2362-L2367)

```cpp
MatrixCoord offsetA{blockCoord.m() * L1TileShape::M, blockCoord.k() * L1TileShape::K};
int64_t gmOffsetA = layoutA.GetOffset(offsetA);
```

### 涉及的数据结构

**GemmCoord** [gemm_coord.hpp:67-154](m:\code\catlass\include\catlass\gemm_coord.hpp#L67-L154)：继承自 `Coord<3, uint32_t>`，内部存 `uint32_t idx[3]`。

```cpp
struct GemmCoord : public Coord<3, uint32_t> {
    // M_INDEX=0, N_INDEX=1, K_INDEX=2
    Index const &m() const { return this->At(0); }  // idx[0]
    Index const &n() const { return this->At(1); }  // idx[1]
    Index const &k() const { return this->At(2); }  // idx[2]
};
```

**MatrixCoord** [matrix_coord.hpp:37-67](m:\code\catlass\include\catlass\matrix_coord.hpp#L37-L67)：继承自 `Coord<2, uint32_t>`，内部存 `uint32_t idx[2]`。

```cpp
struct MatrixCoord : public Coord<2, uint32_t> {
    // ROW_INDEX=0, COLUMN_INDEX=1
    Index const &row()    const { return this->At(0); }  // idx[0]
    Index const &column() const { return this->At(1); }  // idx[1]
};
```

**RowMajor** [layout/matrix.hpp:24-82](m:\code\catlass\include\catlass\layout\matrix.hpp#L24-L82)：

```cpp
struct RowMajor {
    Shape shape_;   // {rows, cols}
    Stride stride_; // {ldm, 1}  leading dimension

    LongIndex GetOffset(MatrixCoord const &coord) const {
        return LongIndex(coord.row()) * stride_[0] + LongIndex(coord.column());
    }
};
```

### 第一行做了什么

[kernel文件 L2362](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2362)

```cpp
MatrixCoord offsetA{blockCoord.m() * L1TileShape::M, blockCoord.k() * L1TileShape::K};
```

- `blockCoord.m()` = `mIdx`（tile 在 M 方向的行号），类型 `uint32_t`
- `blockCoord.k()` = `0`（L1 调度层不切 K，恒为 0）
- 结果：`offsetA = {mIdx * tileM, 0}` — 二维坐标，表示该 tile 在 A 矩阵中的起始位置

### 第二行做了什么

[kernel文件 L2365](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2365)

`layoutA` 来自 [L2339](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2339) 的 `params.layoutA.GetTileLayout(inGroupProblemShape.GetCoordMK())`，是一个 `RowMajor{shape={currentM, K}, stride={lda, 1}}`。

```cpp
int64_t gmOffsetA = layoutA.GetOffset({mIdx * tileM, 0});
// = (mIdx * tileM) * lda + 0
// = mIdx * tileM * lda
```

这是该 tile 在 A 矩阵 GM buffer 中的**起始线性偏移量**（以元素个数为单位）。最终 GM 地址为 `gmA[gmGroupOffsetA + gmOffsetA]`，见 [L2371](m:\code\wpz\umdk\src\cam\comm_operator\ascend_kernels\fused_deep_moe\op_kernel\fused_deep_moe\gemm\kernel\grouped_matmul_slice_m_per_token_dequant_swiglu_quant_multistage_workspace.h#L2371)。

### 完整计算流程图示

以 `currentM=128, K=4096, tileM=16, mIdx=3` 为例：

```
A 矩阵在 GM 中的布局 (RowMajor, lda=4096):

    col=0    col=1    ...    col=4095
row=0  [0]    [1]     ...    [4095]       ← mIdx=0, offsetA={0,0}
row=1  [4096] [4097]  ...    [8191]
...
row=48 [196608][196609]...    [200703]     ← mIdx=3, offsetA={48,0}
                                           ↑ gmOffsetA = 48*4096 = 196608
```

```cpp
offsetA   = {mIdx * tileM, 0} = {3*16, 0} = {48, 0}       // 逻辑坐标
gmOffsetA = 48 * 4096 + 0 = 196608                          // 线性偏移
gmA[gmGroupOffsetA + 196608]                                 // 最终 GM 地址
```

**`offsetA` 是逻辑二维坐标，`gmOffsetA` 是线性一维偏移**。RowMajor 的 `GetOffset` 完成了 `(row, col) → row × leading_dimension + col` 的转换。

---

## Q6: offsetA = {mIdx * tileM, 0} 这跟 swizzle 算法的不同会有不同吗？GetBlockCoord(loopIdx) 的结果有考虑过 swizzle 吗？

### 两层映射关系

```
loopIdx  ──[swizzle 映射]──→ {mIdx, nIdx}  ──[几何映射]──→ offsetA = {mIdx*tileM, 0}
     ↑         GetBlockCoord()                     ↑              ↑
     ↑    [block_swizzle.hpp:78-113]               ↑    这步只是纯几何计算
     ↑                                     这步受 swizzle 影响
```

**`GetBlockCoord` 内部做了 swizzle，输出已经是 swizzle 后的坐标。** `offsetA = {mIdx * tileM, 0}` 公式本身恒定不变——不管 mIdx 是经由何种 swizzle 映射来的，`mIdx=3` 就对应 A 矩阵第 `3×tileM` 行。这是确定的几何关系。

### 无 Swizzle vs Zn Swizzle 对比

以 `loopsMN={3,4}`, `coreNum=4` 为例，Core 0 拿到的 tile：

| | 无 swizzle | Zn swizzle |
|---|---|---|
| loopIdx | 0, 4, 8 | 0, 4, 8 |
| mIdx | 0, 1, 2 | 0, 1, 2 |
| nIdx | 0, 0, 0 | 0, **3**, 0 |
| offsetA | {0,0}, {tileM,0}, {2tileM,0} | **完全一致** |
| offsetB | {0,0}, {0,0}, {0,0} | {0,0}, {0,**3tileN**}, {0,0} |

### 结论

- **`offsetA` 不受 swizzle 影响** — 它只依赖 mIdx，而 mIdx 的取值本身跟有无 swizzle 无关（只是分配给了不同的 loopIdx）
- **`offsetB` 间接受 swizzle 影响** — swizzle 改变了每个 core 拿到的 nIdx 序列，从而改变了 B 矩阵列访问模式
- **Swizzle 的真正作用**：改变每个 core 处理哪些 `{mIdx, nIdx}` 组合，从而改变各 core 对 A、B 数据的访问序列，优化 L2 cache 和带宽的利用率
