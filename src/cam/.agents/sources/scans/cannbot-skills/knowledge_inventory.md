# CANNBot knowledge inventory（AscendC, P1+P2 only）

- Source: `/home/wangyibo/cannbot-skills`
- Commit: `c4c425641d11127945d22e5a752fcad77646be97`
- License: CANN OSL v2.0
- Role: **Knowledge discovery source**, not Case source
- This scan: P1 `npu-arch` / `ascendc-tiling-design` / `ascendc-api-best-practices`；P2 `ascendc-precision-debug` / `ascendc-runtime-debug` / `ascendc-crash-debug`
- Out of scope: templates, docs-gen, GitCode, PyPTO/TileLang/Triton, Blaze/MC2/RegBase/SIMT best-practice skills

CANNBot 是二次整理。表里的 `kind` 先按它自己的口气分类，**不自动升成 Weaver 硬件事实**。

| kind | 含义 |
| --- | --- |
| fact | 它写成硬约束：容量、对齐、API 合法性、错误码 |
| rule | 决策树 / 必须做的工程步骤 |
| recommendation | “优先 / 通常 / 最佳实践” |
| workflow | 排查步骤、工具命令、尝试上限 |
| template | 可复制代码骨架、Agent 编排 |

| evidence | 含义 |
| --- | --- |
| platform-ini-claimed | 自称来自 CANN `platform_config/*.ini` + `PlatformAscendC` |
| official-doc-cited | 指向 asc-devkit / 官方 API 文档 |
| cannbot-secondary | 只见 cannbot 正文，未见一手出处 |
| source-code-cited | 指向具体官方仓实现（本轮 P1/P2 很少） |

`Weaver existing` 只写 **已入库 Knowledge / Case / Skill 正文**。空目录和 `cannbot_analysis.md` 不算已有资产。

---

## P1 `npu-arch`

入口：`ops/npu-arch/SKILL.md`  
厚文件：`references/npu-hardware-params.md`、`npu-arch-guide.md`、`simt-arch-guide.md`

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| NpuArch / SocVersion / `__NPU_ARCH__` / arch22·arch35 映射 | fact | platform-ini-claimed | none |
| A2/A3=`DAV_2201`，A5/950=`DAV_3510` | fact | cannbot-secondary（与官方命名一致，仍待 ini/头文件核对） | none（Cases 已默认 950，无映射文） |
| `__DAV_C310__` ≡ DAV_3510，禁止按数字字面推断 | rule | cannbot-secondary | none |
| 运行时用 `PlatformAscendC` / `GetCoreMemSize`，禁止硬编码容量 | rule | official-doc-cited | [CanImplement](../../../knowledge/engineering-experience/canimplement-is-not-all-constraints.md) 只说“检查≠完整”，不教获取接口 |
| DAV_3510：L1 512KB，L0C 256KB，UB 248KB，BT 4KB | fact | platform-ini-claimed | none（43/73 Case 有 L1 tile 数字，不是 hierarchy 文） |
| 跨代际 L0A/L0B 通常 64KB；Cube MAC 16×16×16 | fact | platform-ini-claimed | none |
| DAV_2201 vs 3510：UB 192→248，L0C 128→256，稀疏 4:2 取消 | fact | platform-ini-claimed | none |
| CubeCore:VectorCore = 1:2（2201/3510） | fact | platform-ini-claimed | MC2/GEMM Cases 默认 AIC+AIV，无独立文 |
| 950PR Server vs PCIE：核数/L2/HBM/频率不同 | fact | platform-ini-claimed（SKU 级；ini 可能含非量产） | none |
| 3510 新通路：L0C→UB、UB→L1、NDDMA、SIMT、RegBase、BufferID | fact | cannbot-secondary（guide 标公开资料/经验） | FA/MC2 Cases 用了部分通路，无硬件卡 |
| “L1→GM、GM→L0A/L0B 已删除” | fact | cannbot-secondary | none；**promote 前必须一手核对** |
| Memory 带宽 1.4/1.6 TB/s、Vector 算力×双发 | recommendation | cannbot-secondary（自称公开资料） | none；保持 recommendation |
| Kirin / 310P / 910 全代际表 | fact | platform-ini-claimed | none；Weaver 不需要 |
| SIMT register/DCache/warp 细节 | fact | cannbot-secondary | none（本轮不吸 SIMT） |
| Triton 架构备注 `npu-arch-guide-triton.md` | recommendation | cannbot-secondary | none |

---

## P1 `ascendc-tiling-design`

入口：`ops/ascendc-tiling-design/SKILL.md`  
这是本轮最接近“从零设计普通 kernel”的资料。它自己规定：**每个算子族必须先读 `patterns.md`**；四要素对所有类别强制。

### 跨族规则（真正可抽成通用 Knowledge）

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| 四要素：多核切分 / UB 切分 / Buffer 规划 / 分支覆盖 | rule | cannbot-secondary | none。Cases 有具体 tile，无系统设计文 |
| UB 复用：生命周期不重叠 → 同一 `TBuf`；tile 尽量贴近 `GetCoreMemSize(UB)` | rule | cannbot-secondary | [46 UB footprint](../catlass/close_reads/46_ub_footprint.md) 是 Fixpipe 特例，不是通用规划 |
| 分支维：dtype / shape 大小 / 32B 对齐 / 边界 | rule | cannbot-secondary | none |
| 先规范化逻辑轴，再切核 / on-chip tile（cannbot 常写成“合轴/展平”） | rule | cannbot-secondary | GEMM Cases 从 MN/K 进，不教 Vector 这条。**Promote 时不得写成“所有族先 flatten”**：reduce 只合并相邻同角色轴；broadcast 先标 stride=0 |
| 全载 vs 分载：整行/整块能否进 UB | rule | cannbot-secondary | 73 full-K A 是 Cube 特例 |
| API / 容量 / 对齐会回传到 tiling（如 Reduce `repeatTimes≤255` → `R_max`） | rule | official-doc-cited（repeat 类型） | none。**Promote 时写成 proposal → constraint check → revise，不是线性填表** |

### Elewise

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| shape 全同 → 展平 1D；否则转 Broadcast | rule | cannbot-secondary | none |
| 每核至少 ~4KB；`blockFormer` 对齐 512 元素 | recommendation | cannbot-secondary | none |
| `ubFormer` 按 256B / Vector repeat 对齐 | fact+rule | official-doc-cited + cannbot 公式 | none |
| 首 block / 尾 block 分开记 loop 与 tail | rule | cannbot-secondary | none |
| FP16/BF16 Add/Sub 默认升 FP32，UB 加 K 份中间 buffer | recommendation | cannbot-secondary | none |

### Reduction

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| 合轴：标 A/R，消 size=1，合并相邻同型 | rule | cannbot-secondary | none |
| A0=1 → AR（Level 2 Reduce）；A0>1 → ARA（`Pattern::Reduce::RA`） | rule | official-doc-cited | none |
| AR/ARA：能装整行/整块 → FullLoad，否则 ColSplit/RowSplit | rule | cannbot-secondary | none |
| `rLength` vs `rLengthAlign`：Copy/Reduce count 用有效长，UB 偏移/分配用对齐长 | rule | cannbot-secondary | none |
| Group Reduce：A 不够喂核且 R 太大 → workspace + `SyncAll` 二阶段 | rule | cannbot-secondary | 68 SplitK 是 GEMM K 二阶段，不是 Vector reduce |
| 全局 reduce：AtomicAdd vs core0 merge | recommendation | cannbot-secondary | none |
| Welford / 二分累加 / with-index | recommendation | cannbot-secondary | none |
| `tmpBufSize` 公式、`a0TileBase = VECTOR_REG_WIDTH/sizeof(T)` | fact | official-doc-cited + cannbot 公式 | none |

### Broadcast

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| 补维 + flag 位图 + 合并同 flag 轴；广播轴 stride=0 | rule | cannbot-secondary | none |
| 从内轴累乘找 `ubSplitAxis`，外层展平分核 | rule | cannbot-secondary | none |
| 核不够时缩小 maxElemNum（减 CACHE_LINE）再切 | recommendation | cannbot-secondary | none |
| DAV_3510：GM→UB 用 NDDMA vs UB 内 Broadcast 的优先级表 | rule | cannbot-secondary | none；**平台分流，需一手 API 核对** |
| NDDMA >5 轴要 Kernel 外层 loop | fact | official-doc-cited | none |

### Conversion / Sort / FA / MatMul（本轮只登记，不作为第一批 promote）

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| small-channel transpose：合成 `[C,N]→[N,C]`，当前仅 C≤16 一支 | rule | cannbot-secondary | none |
| Sort：M-way `MrgSort`、两级归并 | rule | official-doc-cited | none |
| FA tiling 树（shape/resources/roofline/sparse） | rule | cannbot-secondary | [FA pipeline](../../../knowledge/engineering-experience/fa-pipeline-online-softmax-and-paged.md) 已覆盖 KV-tile/online-softmax；cannbot 是另一套设计手册 |
| MatMul：2201 → 高阶 API；3510 → Blaze，禁止 `MatmulImpl` | rule | cannbot-secondary | GEMM Cases 走 CATLASS/Blaze 事实；不要再吸 A2 tiling 长文 |

Random / Conv / 部分 NN：SKILL 标「规划中」。不入库。

---

## P1 `ascendc-api-best-practices`

入口是索引。知识在 `references/api-*.md`。适合拆成短 Card，不适合合成一篇大文。

| topic | source file | kind | evidence | Weaver existing |
| --- | --- | --- | --- | --- |
| 生产数据路径不要用 `GlobalTensor::GetValue/SetValue` 逐元素搬 | SKILL + `api-datacopy.md` | recommendation | cannbot-secondary（性能 anti-pattern）。**未证明 illegal，不得标 fact** | none |
| GM↔UB 非 32B 对齐必须 `DataCopyPad` | `api-datacopy.md` | fact | official-doc-cited | none |
| 优先 `DataCopyExtParams` / `DataCopyPadExtParams<T>` | `api-datacopy.md` | recommendation | cannbot-secondary（称同一条 MTE） | none |
| DataCopy 后 Vector 前必须同步：EnQue/DeQue 或 `PipeBarrier<PIPE_MTE2>` | `api-pipeline.md` | fact | official-doc-cited | precision-debug 会再讲；Weaver Knowledge 无 |
| MTE2 搬入 / Vector 算 / MTE3 搬出 异步模型 | `api-pipeline.md` | fact | official-doc-cited | none |
| `TQue` 给 MTE↔Vector；`TBuf<VECCALC>` 给纯 Vector | `api-buffer.md` | rule | official-doc-cited | none |
| Double Buffer = `InitBuffer(que, num=2)`，**不是** 模板 `depth` | `api-buffer.md` | fact | official-doc-cited | 43 pingpong 是 Cube L1 槽，不是这条 UB 规则 |
| `TQue` `depth=1` 默认；`depth=0` 仅原地 Tensor | `api-buffer.md` | rule | official-doc-cited | none |
| `repeatTime` 为 `uint8_t` 时 ≤255，超了会 silently 错 | `api-repeat-limits.md` | fact | official-doc-cited | none |
| Kernel 禁止 `std::` 数学、`new/malloc`、乱混 Host 头 | `api-restrictions.md` | fact | official-doc-cited | none |
| Compare 256B 对齐 | `api-restrictions.md` | fact | official-doc-cited | none |
| Cast / 混合精度 / RoundMode | `api-precision.md` | rule | official-doc-cited | none |
| Adds/Muls 标量广播；`src1RepStride=0` 行广播 | `api-arithmetic.md` | rule | official-doc-cited | none |
| Reduce / Reduce pattern / tmpBuf | `api-reduce*.md` | rule | official-doc-cited | none |
| LoadData2D、NZ、MX（偏 Cube） | `api-loaddata.md` | rule | official-doc-cited | 53 MX Case 有 layout，无 API Card |
| `MatmulImpl` / GMM 高阶 API **限 DAV_2201** | `api-matmul.md` `api-gmm.md` | fact | official-doc-cited | 与 Weaver 950 默认路径冲突 → 只作负例 |
| GatherMask / RoPE 拆分 **限 2201** | `api-gathermask.md` | fact | official-doc-cited | none |
| MrgSort / Host `aclrtGetDeviceInfo` | `api-mrgsort.md` `api-host-runtime.md` | rule | official-doc-cited | none |

---

## P2 `ascendc-precision-debug`

主体是 **workflow**。能进 Knowledge 的是少数可独立存在的规律。

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| 先固定最小 case（对齐、优先 FP32）再改代码 | workflow | cannbot-secondary | none（Skill 空） |
| 禁止凭直觉改；先 docs-search | workflow | cannbot-secondary | `AGENTS.md` 有“先理解”，无调试步骤 |
| 改完输出不变 → 清 `build/` 与 kernel_cache | workflow | cannbot-secondary | none |
| 全 0 / 随机 → 先查 EnQue/DeQue，再查 32B，再查 Get/SetValue | rule | cannbot-secondary（与 pipeline 事实同根） | none |
| FP32 过、FP16/BF16 挂 → Cast/中间精度/容差 | rule | cannbot-secondary | none |
| 猜测修改无信息增益后改二分 / Dump / 打点 | workflow | cannbot-secondary | none。cannbot 写“>7 次”是 heuristic，**不是工程边界** |
| 容差：任务 Plan > 官方标准 > 缺省（FP16 1e-3 / FP32 1e-5） | rule | cannbot-secondary | none；标准真源应是 `ops-precision-standard`（本轮未读） |
| 多 dtype 交叉验证 | recommendation | cannbot-secondary | none |

---

## P2 `ascendc-runtime-debug`

几乎全是 **workflow + 错误码表**。

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| 161xxx 参数；361xxx runtime；561xxx 内部 | fact | official-doc-cited | none |
| 507035 → 访存 / buffer 相关假说（再对 plog / 地址 / copy shape） | workflow | cannbot-secondary | none。**不是** “507035 ≡ 对齐或 UB overflow” 的确定映射 |
| 561002 TilingKey/TilingFunc；561003 安装/环境 | rule | cannbot-secondary | none |
| `aclGetRecentErrMsg`、plog、`ASCEND_SLOG_PRINT_TO_STDOUT` | workflow | official-doc-cited | none |
| kernel 二进制 / SEL / 多版本冲突 | workflow | cannbot-secondary | [delivery/call/path](../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md) 是框架选路，不是 aclnn 错误码 |

---

## P2 `ascendc-crash-debug`

| topic | kind | evidence | Weaver existing |
| --- | --- | --- | --- |
| crash → coredump+GDB；hang → plog + Alloc/Free + EnQue/DeQue | workflow | cannbot-secondary | none |
| AIV hang → CrossCore Set/Wait 配对；drain hang → AIV→AIC flag | rule | cannbot-secondary | MC2 Knowledge 讲 visibility，不讲核内 hang |
| 偶发 → mssanitizer（越界/多核踩踏/非对齐） | workflow | official-doc-cited | none |
| `parse_plog.py` / memcheck 脚本 | template | cannbot-secondary | none；脚本不进 Knowledge |

---

## 本轮明确不登记为“知识型”

这些在六个 Skill 里出现，但属于 B/C 类：

- 直调 Architect → Developer → Reviewer
- DumpTensor 七步操作手册
- mssanitizer 三步编译安装
- `evals/evals.json`、skill 自身测试
- FA/MatMul 族里指向 `ascendc-blaze-best-practice` 的整本设计手册（另一 Skill）

---

## 一眼结论

CANNBot 这六个 Skill 里，**知识密度最高、Weaver 最缺**的是：

```text
tiling 四要素 + elewise/reduce/broadcast 决策树
    +
API：对齐 / DataCopyPad / queue-pipe / repeat255 / GetValue 作为搬运 anti-pattern（recommendation）
    +
950 memory hierarchy 数字（必须再对 ini/PlatformAscendC）
```

P2 三份 debug 主要应进 `skills/operator-debug/`，不要先写成 Knowledge 长文。
