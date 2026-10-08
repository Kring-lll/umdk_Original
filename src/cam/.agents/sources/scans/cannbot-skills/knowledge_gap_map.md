# CANNBot × AscendWeaver Knowledge gap map

对照对象：

- CANNBot：`knowledge_inventory.md`（P1+P2 六 Skill）
- Weaver Knowledge（仅这 7 篇正文）：
  - `moe-token-expert-rank-data-contracts.md`
  - `moe-fusion-materialization-and-sync.md`
  - `fa-pipeline-online-softmax-and-paged.md`
  - `gemm-k-partition-strategies.md`
  - `canimplement-is-not-all-constraints.md`
  - `mc2-compute-collective-coupling.md`
  - `custom-op-delivery-call-and-selected-path.md`
- Weaver Cases：13 张，全是 GEMM / GMM / FA / MoE / MC2
- 空壳：`knowledge/hardware/ascend950/`、`knowledge/ascendc-api/{data-movement,vector,synchronization,cube}/`、`skills/operator-debug/`

判定：

```text
CANNBot topic
    ↓
Weaver 已有同构结论？
   yes → complement（补证据/边界）或 already-covered
   no  → candidate Knowledge / Skill / ignore
```

不在这里决定文件名。归位见 `knowledge_promotion_plan.md`。

---

## 1. Weaver 已经强、CANNBot 不要再写一篇

这些 topic cannbot 也沾边，但 Weaver 已有 **更高阶、有 Case 反证** 的文。再吸会重复。

| CANNBot topic | Weaver 已有 | 处理 |
| --- | --- | --- |
| FA tiling 树、KV tile、softmax | `fa-pipeline-online-softmax-and-paged.md` + Sparse Case | **already-covered**。cannbot FA 手册是另一套设计稿，不并进 FA Knowledge |
| MatMul 平台路由（950→Blaze，禁止 A2 `MatmulImpl`） | 43/53/66/68/73 Cases 已是 950 CATLASS 路径 | **already-covered** 作为负例一句即可，不吸 `api-matmul.md` / A2 tiling 长文 |
| GEMM K 切分、StreamK/SplitK | `gemm-k-partition-strategies.md` | **already-covered**。cannbot 无对等文 |
| L1A full-load / CanImplement 不完整 | `canimplement-is-not-all-constraints.md` + 73 | **already-covered**。cannbot 的 “用 GetCoreMemSize” 可 **complement** 一句 |
| MC2 chunk / visibility | `mc2-compute-collective-coupling.md` | **already-covered**。crash-debug 的 CrossCore flag 是核内 hang，不是这篇 |
| MoE token/expert/rank、fusion sync | 两篇 MoE Knowledge + Cases | **already-covered**。本轮 Skill 几乎不覆盖 |
| framework delivery ≠ call ≠ path | `custom-op-delivery-call-and-selected-path.md` | **already-covered**。561003 是 aclnn 安装，不是 vLLM 选路 |
| 46 Fixpipe UB 特例 | `sources/scans/catlass/close_reads/46_ub_footprint.md` | **already-covered** 作为 Cube epilogue 例。不能替代通用 UB 规划 |

---

## 2. 有具体例子、缺系统设计（最大缺口）

Weaver 能回答“这个 Case 怎么切”，不能回答“任意 Vector 算子从 shape 怎么切”。

| 已有 concrete | 缺的 generic | CANNBot 能否补 |
| --- | --- | --- |
| 43 MN tile + K loop | 拿到 shape 后选切哪一维 | 能。elewise 展平、reduce 合轴、broadcast `ubSplitAxis` |
| 60 grouped M | 多核喂不满时怎么办 | 部分。broadcast “缩小 tile 换核数”；reduce Group Reduce。不要和 60 的 groupList 混 |
| 66 StreamK / 68 SplitK | 核间二阶段 merge | 弱对应。Vector Group Reduce / 全局 reduce 是另一族 |
| 73 full-K A | 全载 vs 分载 | 能。reduce FullLoad/ColSplit 是同构问题，硬件层不同 |
| MC2 M chunk | 通信 chunk ≠ compute tile | 不能。别用 tiling-design 解释 MC2 |
| FA KV tile | online-softmax 跨 tile | 不能替代现有 FA Knowledge |

**candidate（P0 Knowledge）**：一篇站在算子开发者角度的 tiling 系统设计，Case 只当 worked examples。

CANNBot 可迁移的是决策材料，不是“先 flatten 再一路填参数”。Weaver 通例：

```text
problem shape
  → identify semantic axes（elewise 才可 flatten；reduce 标 A/R；
     broadcast 标 flag/stride=0；layout 敏感轴保留物理连续性）
  → collapse only semantics-preserving axes
  → proposal: core split + on-chip tile + buffer
  → constraint check: capacity / alignment / API legality / pipeline
  → revise split / tile / buffer（不是线性流水）
```

注意：cannbot elewise 的 `4KB/核`、`512 元素对齐` 是 **recommendation**，不是 950 硬件事实。写 Knowledge 时标清楚。

---

## 3. 完全空：基础工程 Knowledge

| 缺口 | CANNBot | 去重结论 |
| --- | --- | --- |
| 950 memory hierarchy（GM/L2/L1/L0/UB、谁能碰谁） | `npu-arch` | **candidate Knowledge**。数字必须再对 ini / `PlatformAscendC`，cannbot 不是最终真源 |
| 核类型 AIC:AIV=1:2、arch35 别名 | `npu-arch` | **candidate Knowledge**（短） |
| DataCopy vs DataCopyPad、32B | `api-datacopy` | **candidate Knowledge** |
| Get/SetValue 作生产搬运 | SKILL 黑名单 | **candidate recommendation**（并进 copy 文）。未证明 illegal 前不写“禁止使用” |
| EnQue/DeQue vs PipeBarrier | `api-pipeline` | **candidate Knowledge** |
| TQue/TBuf、Double Buffer=`num` | `api-buffer` | **candidate Knowledge** |
| mask / repeat≤255 | `api-repeat-limits` | **candidate Knowledge** |
| Kernel 禁 std:: / 动态分配 | `api-restrictions` | **candidate Knowledge**（短，coding-guidelines 也可） |
| Cast / 半精度升精度 | `api-precision` + elewise Step 2 | **candidate Knowledge** 或并进 vector card |
| 精度症状（全 0 = 同步，不是公式） | precision-debug | **candidate 短 Knowledge** + Skill 流程 |
| 错误码 161/361/507035/561 | runtime-debug | **Skill**。507035 只作访存/buffer **假说入口**，不要固化成对齐或 UB overflow |
| hang/crash 配对纪律 | crash-debug | **Skill**；Alloc/Free、Set/Wait 可各留一条 rule |

---

## 4. 部分重叠：只 complement，不开新主题

| CANNBot | 已有资产 | complement 什么 |
| --- | --- | --- |
| `GetCoreMemSize`，禁止硬编码 | CanImplement 文 | 在 CanImplement 或 hierarchy 文加：“运行时读平台，不要抄 Case 里的 248KB/512KB” |
| Cube pingpong / 2 slots | 43/66 Cases | 写 UB Double Buffer 时 **显式对比**：Case 的 L1/L0 槽 ≠ `InitBuffer(num=2)` |
| AIV reduce after SplitK | 66/68 | Vector Group Reduce 文必须写“不是 GEMM SplitK” |
| CrossCore flag | MC2 visibility | crash Skill 可链到 MC2，但核内 hang ≠ collective visibility |
| 561003 找不到 kernel | delivery/call/path | 一句：aclnn 安装失败 ≠ Python 选了另一条 kernel |

---

## 5. 不要当缺口去补

| CANNBot 内容 | 原因 |
| --- | --- |
| 全代际 Kirin/310P 表 | Weaver 目标 950 |
| A2 `MatmulImpl` / GMM 高阶 API 教程 | 与 950 路径冲突 |
| GatherMask 限 2201 的长教程 | 非 P0；以后 Case 用到再补 |
| FA cannbot 设计全书 | 已有 FA Knowledge |
| SIMT register/DCache 细表 | 本轮不做 SIMT |
| “带宽 1.6 TB/s”“Vector 双发 54T” | recommendation；无 benchmark 不进事实层 |
| “L1→GM 已删除” | 硬通路声称；未对一手文档前 **不 promote 为 fact** |
| DumpTensor/mssanitizer/plog 脚本 | workflow / template |
| Architect/Developer/Reviewer | orchestration |
| 规划中的 Random/Conv/NN tiling | 空 |

---

## 6. 按用户列的 8 类基础缺口对照

| 用户要补的类 | Weaver 现在 | CANNBot 本轮 | gap |
| --- | --- | --- | --- |
| 1. 硬件 / memory hierarchy | 无独立文 | 强（3510 表） | **开 Knowledge** |
| 2. Tiling 与多核切分 | 只有 Case 特例 | 强（四要素 + 三族树） | **开 Knowledge（第一篇）** |
| 3. UB/L1/L0 buffer 规划 | 46/73 特例 | 中（UB 通用强，L1/L0 弱） | UB 跟 tiling 文；L1/L0 继续靠 GEMM Cases |
| 4. AscendC API 约束 | 空目录 | 强 | **开若干短 Card** |
| 5. Pipeline / event / sync | 无基础文；有 MoE/MC2 高阶 sync | 强（MTE 异步） | **开 Knowledge** |
| 6. 精度定位 | 无 | 强 workflow + 几条 rule | **Skill 为主，短 Knowledge 为辅** |
| 7. runtime / crash / hang | 无 | 强 workflow | **Skill / workflow** |
| 8. 性能编码基本原则 | 无通用文；有 GEMM/MC2 高阶 | 本轮弱（在未读的 performance-best-practices） | 本轮只吸：对齐、GetValue 不作生产搬运、Double Buffer 定义。整本 perf 族下一轮 |

---

## 7. 一句话

现有 Knowledge 能让 Agent **读懂高级 Case**。CANNBot P1 能补的是让 Agent **从零设计一个普通 Vector kernel**。P2 补的是 **写坏了怎么查**，应进 Skill，不要稀释 `knowledge/engineering-experience/`。
