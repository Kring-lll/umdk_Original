# SNC NPU-L1 选路 Hash 覆盖规划设计文档

> 本文档定义在现有 `planPathsCoverage` 基础上新增接口 `planPathsCoverageEx` 的设计与实现口径。
> 现有接口只覆盖 **L1SW↔L2SW** 之间的出端口选路（hash）；新接口在覆盖链路域中
> **增加 NPU↔L1SW 之间的选路 hash 与链路覆盖**，使覆盖率口径覆盖 NPU→L1→L2→L1→NPU
> 全链路出端口。
>
> **NPU→L1SW 选口使用标准二元组 `(DstCNA, jettyId)`**（jettyId 对应物理端口，取值范围
> `[32, 1023]`），复用既有 `libubswitch` 原生 hash（dll/so）；**被选中的 NPU 端口 CNA 作为
> SCNA** 供后续 L1SW/L2SW 选口使用；**ACK 方向**（以源 CNA 为目的 CNA、**源 NPU 端口的
> jettyId** 选路——与正向同一 jettyId）。测试侧输出 Markdown 覆盖率报告
> （`target/coverage-intra-chassis-report.md`）。

---

## 目录

1. [概述](#1-概述)
2. [现状分析](#2-现状分析)
3. [需求与接口定义](#3-需求与接口定义)
4. [总体设计](#4-总体设计)
5. [数据结构设计](#5-数据结构设计)
6. [算法设计](#6-算法设计)
7. [详细流程](#7-详细流程)
8. [结果输出与统计](#8-结果输出与统计)
9. [兼容性与影响分析](#9-兼容性与影响分析)
10. [测试设计](#10-测试设计)
11. [文件清单与实施步骤](#11-文件清单与实施步骤)
12. [开放问题](#12-开放问题)
13. [框间/框内 两阶段覆盖与示例](#13-框间框内-两阶段覆盖与示例)

---

## 1. 概述

### 1.1 业务背景

SNC（Supernode Network Controller）的覆盖规划能力由 `planPathsCoverage` 提供：给定超节点拓扑
（含路由表），挑选一组 EID 对（src/dst NPU 端口），使其**正反向 hash 选路结果**能够遍历
目标出端口集合，输出 EID 对 → 覆盖链路的映射，以及覆盖率、重复率、EID 均匀度等统计。

### 1.2 问题描述

现有实现（`CoveragePlanEngine`）的**覆盖链路域**由 `collectBidirectionalLinks()` 构建，只包含：

| 方向 | 设备 | 过滤条件 | 说明 |
|:---|:---|:---|:---|
| L1SW → L2SW | L1SW | 路由出端口 `remoteDevice` ∈ L2SW 集合 | 全部出端口（含单端口路由） |
| L2SW → L1SW | L2SW | 非默认路由（maskLength != 0）且出端口数 > 1，出端口 `remoteDevice` 为 L1SW | 仅 ECMP 路由 |

而 NPU↔L1SW 这一段：

1. **NPU→L1SW**：源 NPU 端口由候选（`NpuCandidate`）**固定**，其 L1SW 入端口由物理连接
   `npuPortConns` 唯一确定，**不参与 hash 选路**；NPU 上连出端口也不在覆盖链路域内，不计入覆盖率。
2. **L1SW→NPU**：`traceForwardPath()` / `traceReversePath()` 末跳直接取
   `dstL1swOutPorts.get(0)` / `srcL1swRevOutPorts.get(0)`，**无 hash**，且该出端口同样不在覆盖链路域内。

当 NPU 存在**多条上连到 L1SW 的物理链路**（ECMP）时，真实硬件会按 hash 在 NPU 出端口集合中
选路；若不建模，则：

- 覆盖率统计口径不完整（漏掉 NPU↔L1SW 出端口）；
- 生成的 EID 对无法保证覆盖 NPU↔L1SW 的所有出端口；
- 末跳 `get(0)` 与硬件 hash 行为不一致，覆盖结论失真。

### 1.3 设计目标

| 编号 | 目标 |
|:---:|:---|
| G1 | 新增北向接口 `planPathsCoverageEx`（`SncService` → `PathService` → `CoveragePlanEngine`），**现有 `planPathsCoverage` 行为与签名完全不变** |
| G2 | **NPU→L1SW 出端口按 hash 选路**：以 NPU 路由表（按目的 CNA 做 LPM）的出端口集合为 ECMP 成员集，用与 L1↔L2 相同的 hash 元组选出实际出端口 |
| G3 | **L1SW→NPU 出端口按 hash 选路**：替换现有 `get(0)` 的确定性取值，成员集为 L1SW 路由到目的 NPU 的出端口集合 |
| G4 | 把 NPU→L1SW、L1SW→NPU 出端口纳入**覆盖链路域**，`totalLinks` / `coveredCount` / `coverageRate` 覆盖全链路 |
| G5 | 输出**分层统计**（`layerStats`），可分别查看 NPU↔L1SW 与 L1SW↔L2SW 的覆盖率 |
| G6 | 复用现有配置（`hashFunc` / `fixedDataUdpPort` / `fixedAckUdpPort` / `HashTuple`），**不新增配置项** |

### 1.4 非目标

- 不修改 `planPathsCoverage`（含其链路域、hash 点位、返回结构语义）。
- 不修改 NPU 内部转发模型、框内拓扑模型、`planPath` 相关链路。
- 不引入新的 hash 算法；仍走 `HashUtils.nativeHash`（`libubswitch`）。
- 不改变 L1SW↔L2SW 段的 ECMP 成员集与选路规则。

### 1.5 术语

| 术语 | 含义 |
|:---|:---|
| 覆盖链路域 | 需要被 EID 对 hash 选路"踩到"的出端口全集（`linkMap`） |
| 出端口 | 某设备某转发芯片上的一条出接口（`LinkInfo` / `CoveredLinkDetail`） |
| 成员集（ECMP member set） | 某次路由查找命中的出端口集合，hash 对 `size()` 取模选其一 |
| NPU 上连 | NPU 转发芯片上朝向 L1SW 的出端口 |
| 分层 | `NPU_L1`（NPU↔L1SW）、`L1_L2`（L1SW↔L2SW） |
| 跳（hop） | 一段设备间转发：NPU→L1SW、L1SW→L2SW、L2SW→L1SW、L1SW→NPU |

### 1.6 实现口径（最终，2026-09-13 落地）

| 项 | 最终口径 | 落地位置 |
|:---|:---|:---|
| NPU→L1SW 选口 hash | 标准二元组 **`(DstCNA, jettyId)`**；新增 `HashUtils.nativeHashDstCnaJetty(dstCna, jettyId, ecmpCnt, hashFunc)`，复用既有 `libubswitch`（`ubswitch_Hash_ecmp`，dll/so）映射为 `dip←DstCNA`、`sport←jettyId`（业界标准中 QP/jetty 熵字段位置）、`sip/dport/protocol←0` | `util/HashUtils.java` |
| jettyId 取值 | **`[32, 1023]`**，每个物理端口一个；入口校验越界抛 `IllegalArgumentException` | `HashUtils.JETTY_ID_MIN/MAX`、`isValidJettyId` |
| topo 输入字段 | 超节点 JSON `jettyId`（`TestDataLoader`）；模板 JSON `jetty_id`（`128_npu_rack.json`）+ `PortLoader`/`SncPort`；`FullRackTopologyGenerator` 用**固定分配** `JETTY_ID_BASE + portIndex`（32..39，每次生成完全一致，由 `FullRackTopologyJettyIdTest` 钉死）；缺失时回落 `32 + portId` 并计入 `jettyIdFallback` | `entity/NpuPortEntity.jettyId`、`TestDataLoader`、`TopoTemplateService`、`FullRackTopologyGenerator.JETTY_ID_BASE` |
| NPU 出端口成员集 | 该 NPU **转发芯片**上按 `DstCNA` 做 LPM 命中条目的出端口中，对端为 L1SW 的子集（`ecmpCnt = size`） | `CoveragePlanEngine.selectNpuEgress` |
| SCNA 链接 | **选中 NPU 端口的 CNA 作为 SCNA**，供后续 L1SW→L2SW、L2SW→L1SW、L1SW→NPU 的 hash 选口使用 | `traceForwardPathEx` |
| ACK 反向 | **`DstCNA = 源 CNA`**、**`jettyId = 源 NPU 端口的 jettyId`**（与正向同一 jettyId）选目的 NPU 出端口；其 CNA 作为反向 SCNA，驱动反向 L1SW 选口 | `traceReversePathEx` / `traceIntraReversePathEx` |
| 覆盖链路域 | NPU→L1SW、L1SW→NPU（`layer = NPU_L1`）＋ L1SW→L2SW、L2SW→L1SW（`layer = L1_L2`）；单出端口路由同样纳入 | `collectNpuL1Links` + 既有 `collectBidirectionalLinks` |
| 每个 EID 对 | **8 条**覆盖链路（4 正向 + 4 ACK），顺序见 §8.1 | `PathService.planPathsCoverageEx` |
| 北向接口 | `SncService.planPathsCoverageEx` → `PathService.planPathsCoverageEx` → `CoveragePlanEngine.findCoverageEx`；旧接口零行为变更 | 见 §3.1 |
| 覆盖率测试报告 | 集成测试 `CoverageIntraChassisTest`（单机框夹具）生成 Markdown 报告（含分层覆盖率、CRC8 选路与 SCNA 链接证据、框内 2+2 链路、诊断计数） | `target/coverage-intra-chassis-report.md` |

---

## 2. 现状分析

### 2.1 现有调用链

```
SncService.planPathsCoverage(CoveragePathsRequest)      // 状态校验 DATAREADY + null 校验
  └─ PathService.planPathsCoverage(request)             // 参数校验、DTO 转换
       └─ CoveragePlanEngine.findCoverage(superNode, dataUdpSrcPort, ackUdpSrcPort,
                                           requirement)
            ├─ collectNpuCandidates(devices)            // NPU 端口候选（eid + cna）
            ├─ collectL2swNames(devices)
            ├─ collectBidirectionalLinks(...)           // ★ 覆盖链路域：仅 L1→L2 / L2→L1
            ├─ precomputeTopology(devices, l2swNames)   // npuPortConns / l2swChipRemotePorts
            └─ findCoverageDiffPath(...)                // 逐 (src,dst) 追踪正反向路径
                 ├─ traceForwardPath(...)               // 4 跳，L1→L2、L2→L1 处 hash
                  ├─ traceReversePath(...)               // 4 跳，ACK jettyId=源端口，L1→L2、L2→L1 处 hash
                 ├─ runGreedyCoverage(...)              // MIN_COVERAGE
                 └─ runGreedyCoverageDualDisjoint(...)  // REDUNDANT
```

### 2.2 现有 hash 使用点

hash 统一封装在 `CoveragePlanEngine.nativePortIdx(scna, dcna, sport, dport, ecmpCnt)`：

| 序号 | 位置 | 方向 | scna / dcna | sport / dport | 成员集 |
|:---:|:---|:---|:---|:---|:---|
| H1 | `traceForwardPath` | L1SW→L2SW | `src.cna` / `dst.cna` | `dataUdpSrcPort` / `ackUdpSrcPort` | L1SW 路由出端口中 remote 为 L2SW 的集合 |
| H2 | `traceForwardPath` | L2SW→L1SW | `src.cna` / `dst.cna` | `dataUdpSrcPort` / `ackUdpSrcPort` | L2SW 入端口所属 chip 上通往 `dst.remoteL1sw` 的端口集合 |
| H3 | `traceReversePath` | L1SW→L2SW（反） | `dst.cna` / `src.cna` | `ackUdpSrcPort` / `dataUdpSrcPort` | 目的 L1SW 路由出端口中 remote 为 L2SW 的集合 |
| H4 | `traceReversePath` | L2SW→L1SW（反） | `dst.cna` / `src.cna` | `ackUdpSrcPort` / `dataUdpSrcPort` | 反向 L2SW 入端口所属 chip 上通往 `src.remoteL1sw` 的端口集合 |
| — | `traceForwardPath` | **NPU→L1SW** | 无（由 `npuPortConns` 固定） | 无 | **无 hash** |
| — | `traceForwardPath` | **L1SW→NPU** | 无（`get(0)`） | 无 | **无 hash** |
| — | `traceReversePath` | **L1SW→NPU（反）** | 无（`get(0)`） | 无 | **无 hash** |

### 2.3 现有链路域与路径产物

- `LinkInfo.getKey()` = `switchDevice + ":" + outPortName`，`linkMap` 以该 key 去重（同 key 取
  `totalOutPorts` 较大者）。
- `CoveredPair` 携带 `forwardLinkDetails`（2 条：L1→L2、L2→L1）与 `reverseLinkDetails`（2 条），
  **每个 EID 对固定 4 条覆盖链路**。
- `PathService.planPathsCoverage()` 把 `forwardLinkDetails` + `reverseLinkDetails` 顺序拼接为
  `CoveredEidPair.coveredLinks`（4 条），`CoverageDiagramGenerator` 依赖该固定顺序
  （`li == 0/1/2/3` → 图中类型 1/2/3/4）渲染。

### 2.4 差距分析

| 维度 | 现状（`planPathsCoverage`） | 目标（`planPathsCoverageEx`） |
|:---|:---|:---|
| 覆盖链路域 | L1→L2、L2→L1 | L1→L2、L2→L1 **＋ NPU→L1、L1→NPU** |
| NPU 出端口选择 | 由候选固定（物理连接） | NPU 路由 LPM + **hash** 选择 |
| L1SW 末跳出端口 | `get(0)` 确定性 | **hash** 选择 |
| 每个 EID 对链路数 | 4（2 正 + 2 反） | 8（4 正 + 4 反） |
| 统计口径 | 单层（合计） | 合计 + **分层（NPU_L1 / L1_L2）** |
| 失败诊断 | `fwdFail1..12` / `revFailA..H` | 追加 NPU 段失败计数 |
| 北向接口 | `planPathsCoverage` | **新增 `planPathsCoverageEx`**（旧接口不变） |

---

## 3. 需求与接口定义

### 3.1 新增接口签名

```java
// com.huawei.umdk.snc.SncService
/**
 * 覆盖规划（扩展版）：在 L1SW↔L2SW 覆盖的基础上，增加 NPU↔L1SW 的 hash 选路与链路覆盖。
 *
 * <p>与 {@link #planPathsCoverage(CoveragePathsRequest)} 的差异：
 * <ul>
 *   <li>覆盖链路域增加 NPU→L1SW、L1SW→NPU 出端口；</li>
 *   <li>NPU 上连出端口按 hash 选择（NPU 路由命中的出端口集合为 ECMP 成员集）；</li>
 *   <li>L1SW→NPU 末跳出端口按 hash 选择（不再取首个出端口）；</li>
 *   <li>返回结果附加 {@code scope} 与 {@code layerStats} 分层统计。</li>
 * </ul>
 *
 * @param request 覆盖规划请求，{@code superNodeName} 必填；{@code coverageRequirement} 为
 *                null 时按 {@link CoverageRequirement#MIN_COVERAGE} 处理
 * @return 覆盖规划结果；状态为 {@code SUCCESS} / {@code COVERAGE_INCOMPLETE} /
 *         {@code TOPO_NOT_FOUND}
 * @throws SNCStateException 当前状态不是 DATAREADY
 * @throws IllegalArgumentException request 为 null
 */
public CoveragePathsResult planPathsCoverageEx(CoveragePathsRequest request);
```

```java
// com.huawei.umdk.snc.service.PathService
public CoveragePathsResult planPathsCoverageEx(CoveragePathsRequest request);
```

```java
// com.huawei.umdk.snc.engine.CoveragePlanEngine
public CoverageSearchResult findCoverageEx(SuperNode superNode,
                                           int fixedDataUdpSrcPort,
                                           int fixedAckUdpSrcPort,
                                           CoverageRequirement requirement);
```

### 3.2 分层职责

| 层 | 职责 |
|:---|:---|
| `SncService` | 状态机校验（`DATAREADY`）、null 校验、日志、委托 `PathService` |
| `PathService` | 请求参数校验、调用引擎、把 `CoverageSearchResult` 转为 `CoveragePathsResult`（含分层统计、`coveredPairs` 反向索引） |
| `CoveragePlanEngine` | 覆盖链路域构建、NPU/L1/L2 hash 选路、正反向路径追踪、贪心选对、统计计算 |
| `CoverageRouteAugmentor`（测试） | 为 NPU 与 L1SW 增强多出端口路由，构造 NPU↔L1SW ECMP 场景 |

### 3.3 状态码与异常

| 场景 | 行为 |
|:---|:---|
| 状态非 `DATAREADY` | `SncService` 抛 `SNCStateException` |
| `request == null` | `SncService` 抛 `IllegalArgumentException` |
| `superNodeName` 为空 | 返回 `PlanStatus.TOPO_NOT_FOUND`（与旧接口一致，不抛异常） |
| SuperNode 不存在 | 返回 `PlanStatus.TOPO_NOT_FOUND` |
| 全覆盖 | `PlanStatus.SUCCESS` |
| 未全覆盖 | `PlanStatus.COVERAGE_INCOMPLETE` + `errorMessage`（含分层覆盖率明细） |

> **说明：** 新接口复用 `PlanStatus`，不新增状态码，避免北向契约扩散；"未覆盖的是哪一层"
> 由 `layerStats` 与 `errorMessage` 表达。

### 3.4 与现有接口的关系

| 接口 | 覆盖链路域 | 每个 EID 对链路数 | 是否 hash NPU↔L1 |
|:---|:---|:---:|:---:|
| `planPathsCoverage` | L1↔L2 | 4 | 否 |
| `planPathsCoverageEx` | L1↔L2 ＋ NPU↔L1 | 8 | 是 |

两个接口**并存**，`planPathsCoverageEx` 不改变旧接口任何行为（见 §9.1），便于灰度与回归对比。

---

## 4. 总体设计

### 4.1 分层改动总览

| 模块 | 文件 | 改动类型 | 说明 |
|:---|:---|:---|:---|
| DTO | `dto/CoverageLinkScope.java` | 新增 | 覆盖域枚举 `L1_L2` / `NPU_L1_L2` |
| DTO | `dto/CoverageLinkLayer.java` | 新增 | 链路分层枚举 `NPU_L1` / `L1_L2` |
| DTO | `dto/CoverageLayerStats.java` | 新增 | 分层统计 |
| DTO | `dto/CoverageLink.java` | 扩展 | 追加 `deviceType`、`layer` |
| DTO | `dto/CoveragePathsResult.java` | 扩展 | 追加 `scope`、`layerStats` |
| Engine | `engine/CoveragePlanEngine.java` | 扩展 | `LinkInfo`/`CoveredLinkDetail` 分层与 deviceType、`chipIndexOfPort`/`jettyIdOf`、NPU 上连预计算、`findCoverageEx`、NPU 段 `(DstCNA, jettyId)` hash、SCNA 链接、末跳 hash、分层统计、`getExDiagnostics` |
| Util | `util/HashUtils.java` | 扩展 | 新增 `JETTY_ID_MIN/MAX`、`isValidJettyId`、`nativeHashDstCnaJetty`（复用 `libubswitch` dll/so） |
| Entity | `entity/NpuPortEntity.java` | 扩展 | 新增 `jettyId` 字段（保留既有 2 参构造器） |
| Service | `service/PathService.java` | 扩展 | 新增 `planPathsCoverageEx`（复用转换逻辑，抽公共私有方法） |
| Service | `SncService.java` | 扩展 | 新增北向方法 + `History` 注释更新 |
| Test | `CoverageRouteAugmentor.java` | 扩展 | `augmentNpuRouting`：NPU 上连多出端口、L1→NPU 多出端口 |
| Test | 新增 `CoverageNpuL1MainSuccessTest` 等 | 新增 | 见 §10 |

> **不改动：** `CoveragePathsRequest`、`CoverageStats`、`CoveredEidPair`、`CoveredEidPairRef`、
> `CoverageRequirement`、`HashTuple`、`SNCConfig`、`HashUtils`。

### 4.2 覆盖域扩展（`CoverageLinkScope`）

```
CoverageLinkScope
 ├─ L1_L2       // 旧接口：L1SW→L2SW、L2SW→L1SW
 └─ NPU_L1_L2   // 新接口：NPU→L1SW、L1SW→L2SW、L2SW→L1SW、L1SW→NPU
```

- 链路域枚举值同时作为结果回填（`CoveragePathsResult.scope`）与内部分支开关，避免在
  Service 层散落 `boolean` 开关。
- 引擎内部用 `CoverageLinkScope scope` 参数传入 `collectBidirectionalLinks` 与
  `traceForwardPath/traceReversePath`，使新旧两条链路共用同一套贪心与统计代码。

### 4.3 分层统计

```
CoveragePathsResult
 ├─ scope         : CoverageLinkScope
 ├─ layerStats    : Map<CoverageLinkLayer, CoverageLayerStats>
 └─ stats         : CoverageStats（合计，语义与旧接口一致）
```

- 合计 `stats` 与分层 `layerStats` 都从同一份 `linkMap` 计算，保证
  `sum(layerStats[*].totalLinks) == stats.totalLinks`。
- `fullCoverage = ∀ layer: layerStats[layer].coveredCount == layerStats[layer].totalLinks`。
- `NPU_L1_L2` 域下 `totalLinks = NPU↔L1 出端口数 + L1↔L2 出端口数`。

### 4.4 设计决策

| 决策点 | 选定方案 | 理由 / 备选 |
|:---|:---|:---|
| 接口形态 | **新增独立方法** `planPathsCoverageEx` | 需求为"新增一个接口"；旧接口零变更，便于回归对比。备选：在请求中加 `scope` 字段并复用 `planPathsCoverage`（会改变旧接口行为边界，不采用） |
| 接口命名 | `planPathsCoverageEx`（扩展） | 保留 `planPathsCoverage` 前缀便于发现，`Ex` 可承载后续更多层（如框内）扩展。备选：`planPathsCoverageWithNpuL1`（语义更显式）、`planPathsFullCoverage`。**改名只需替换 §3.1 三处签名 + §11 文件清单** |
| 请求 DTO | **复用 `CoveragePathsRequest`** | 参数完全一致（`superNodeName` + `coverageRequirement`），避免新增 DTO 与重复校验；不新增 `scope` 字段，覆盖域由方法名决定 |
| 结果 DTO | **复用 `CoveragePathsResult`，追加 `scope` / `layerStats`** | 调用方可沿用既有渲染逻辑（`eidPairs` / `coverageLinks` / `stats`）；代价是 `@AllArgsConstructor` 参数个数变化（见 §5.6） |
| 分层统计字段 | 新增 `CoverageLayerStats`，**不改 `CoverageStats`** | 把 `@AllArgsConstructor` 变更面收敛到 `CoveragePathsResult` 一个类，`CoverageStats` 与其测试不受影响 |
| NPU 上连成员集来源 | **NPU 路由表 LPM 命中条目的出端口集合**（过滤 remote 为 L1SW） | 与硬件"NPU 查路由 → 多出端口 ECMP"一致；不依赖 `npuPortConns` 的单端口假设 |
| 旧接口是否复用新 hash | 否 | `planPathsCoverage` 保持既有 `get(0)` 与固定连接语义，保证历史结果可复现 |
| 配置项 | 复用 `hashFunc` / `fixedDataUdpPort` / `fixedAckUdpPort` / `HashTuple` | 不新增配置；`HashTuple` 决定 hash 元组宽度，两个接口共享同一实例配置 |

### 4.5 端到端数据流（新接口）

```
CoveragePathsRequest(superNodeName, coverageRequirement)
        │
        ▼
SncService.planPathsCoverageEx        [状态 DATAREADY 校验]
        │
        ▼
PathService.planPathsCoverageEx       [参数校验 + 取 SNCConfig 固定端口]
        │
        ▼
CoveragePlanEngine.findCoverageEx(superNode, data, ack, requirement)
        │
        ├─ collectNpuCandidates(devices)              # 候选仍为 NPU 端口（EID/CNA）
        ├─ collectL2swNames(devices)
        ├─ collectBidirectionalLinks(..., NPU_L1_L2)   # ★ 覆盖域含 NPU↔L1
        ├─ precomputeTopology(devices, l2swNames)      # NPU 上连映射 / L2 远端端口映射
        └─ findCoverageDiffPath(..., NPU_L1_L2)
              ├─ traceForwardPath  → CoveredLinkDetail × 4（NPU→L1, L1→L2, L2→L1, L1→NPU）
               ├─ traceReversePath  → CoveredLinkDetail × 4（ACK，jettyId=源端口）
              └─ runGreedyCoverage / runGreedyCoverageDualDisjoint   # 复用，不感知分层
        │
        ▼
CoverageSearchResult（含 layerTotals/layerCovered/fullCoverage）
        │
        ▼
PathService 构建 CoveragePathsResult(scope, eidPairs, coverageLinks, stats, layerStats)
```

---

## 5. 数据结构设计

> 与 §4.1 一致：新增 DTO 一律使用 Lombok 注解（`@Getter`、`@Setter`、`@NoArgsConstructor`、
> `@AllArgsConstructor`、`@EqualsAndHashCode`、`@ToString`），与现有 DTO 风格保持一致。

### 5.1 枚举 `CoverageLinkScope`

```java
package com.huawei.umdk.snc.dto;

/** 覆盖链路域。 */
public enum CoverageLinkScope {
    /** 仅 L1SW↔L2SW 出端口（旧接口 planPathsCoverage 的域）。 */
    L1_L2,

    /** NPU↔L1SW ＋ L1SW↔L2SW 出端口（新接口 planPathsCoverageEx 的域）。 */
    NPU_L1_L2
}
```

### 5.2 枚举 `CoverageLinkLayer`

```java
package com.huawei.umdk.snc.dto;

/**
 * 链路分层，用于把出端口归入 NPU↔L1SW 或 L1SW↔L2SW。
 *
 * <p>方向不在此枚举中表达：由链路两端设备类型推导——
 * 设备为 NPU 即 NPU→L1SW；设备为 L1SW 且对端为 NPU 即 L1SW→NPU。
 */
public enum CoverageLinkLayer {
    /** NPU↔L1SW（NPU 上连出端口 / L1SW 的 NPU 向出端口）。 */
    NPU_L1,

    /** L1SW↔L2SW。 */
    L1_L2
}
```

### 5.3 分层统计 `CoverageLayerStats`

```java
package com.huawei.umdk.snc.dto;

/** 单个分层（{@link CoverageLinkLayer}）的覆盖率统计，字段语义对齐 {@link CoverageStats}。 */
public class CoverageLayerStats {

    private Integer totalLinks;      // 该层出端口总数
    private Integer coveredCount;    // 该层已覆盖出端口数
    private Double coverageRate;     // coveredCount / totalLinks
    private Integer minRepeatCount;  // 该层单链路最小覆盖次数（未覆盖的链路不计入）
    private Integer maxRepeatCount;  // 该层单链路最大覆盖次数
    private Double avgRepeatCount;   // 该层单链路平均覆盖次数
    private Double repeatRate;       // (Σ覆盖次数 - coveredCount) / totalLinks
}
```

**字段语义与计算口径**（与 `CoverageStats` 完全一致，保证可对比）：

| 字段 | 计算 |
|:---|:---|
| `totalLinks` | 该层在 `linkMap` 中的链路数 |
| `coveredCount` | 该层 `coverCount > 0` 的链路数 |
| `coverageRate` | `totalLinks > 0 ? coveredCount / totalLinks : 0` |
| `minRepeatCount` | 该层已覆盖链路 `coverCount` 的最小值；无已覆盖链路时为 0 |
| `maxRepeatCount` | 该层已覆盖链路 `coverCount` 的最大值；无已覆盖链路时为 0 |
| `avgRepeatCount` | 该层已覆盖链路 `ΣcoverCount / coveredCount`；`coveredCount == 0` 时为 0 |
| `repeatRate` | `(ΣcoverCount - coveredCount) / totalLinks` |

### 5.4 `CoverageLink` 扩展

```java
public class CoverageLink {

    /** 出端口所属设备名。**语义扩展：** 既可能是 SW（L1SW/L2SW），也可能是 NPU。 */
    private String switchDevice;

    private Integer chipIndex;
    private String outPort;
    private String remoteSwitch;
    private String remotePort;
    private Integer outPortIndex;
    private Integer totalOutPorts;
    private List<CoveredEidPairRef> coveredPairs;
    private Integer coverCount;

    /** 新增：出端口所属设备类型，取值 "NPU" / "SW"（由 DeviceType.name() 转换，dto 层不依赖 entity 包）。 */
    private String deviceType;

    /** 新增：链路分层（NPU_L1 / L1_L2）；旧接口不设置（保持 null）。 */
    private CoverageLinkLayer layer;
}
```

> **架构约束：** `dto` 层不可依赖 `entity` 包（见《SNC Framework Architecture Design》§3.3）。
> `deviceType` 采用 `String` 而非 `DeviceType`，由 Service 层从 `DeviceType.name()` 转换，
> 与 `HopInfo.deviceType` 的既有做法一致。
>
> **兼容说明：** `switchDevice` 字段名保留（避免破坏 JSON 契约与既有解析），语义由
> "交换机设备名"放宽为"出端口所属设备名"。

### 5.5 `CoveragePathsResult` 扩展

```java
public class CoveragePathsResult {

    private PlanStatus status;
    private String errorMessage;
    private List<CoveredEidPair> eidPairs;
    private List<CoverageLink> coverageLinks;
    private CoverageStats stats;

    /** 新增：本次覆盖所用的链路域；旧接口固定为 L1_L2。 */
    private CoverageLinkScope scope;

    /** 新增：分层统计（NPU_L1 / L1_L2）；仅新接口设置，旧接口保持 null。 */
    private Map<CoverageLinkLayer, CoverageLayerStats> layerStats;
}
```

**字段语义：**

| 字段 | 说明 |
|:---|:---|
| `status` | `SUCCESS` / `COVERAGE_INCOMPLETE` / `TOPO_NOT_FOUND` |
| `errorMessage` | 未全覆盖时携带分层明细，例如：<br>`物理遍历覆盖率未达100%: 96.88% (2016/2080 出端口已覆盖) [NPU_L1: 98.4% 1008/1024; L1_L2: 95.3% 1008/1056]` |
| `eidPairs` | 每个 EID 对的 `coveredLinks` 为 8 条（4 正向 + 4 反向，顺序见 §8.1） |
| `coverageLinks` | 全量出端口及其 `coverCount` / `coveredPairs` / `layer` / `deviceType` |
| `stats` | 合计统计（口径不变） |
| `scope` | `NPU_L1_L2` |
| `layerStats` | 两个分层各一份 `CoverageLayerStats` |

### 5.6 引擎内部结构扩展

#### 5.6.1 `LinkInfo` 追加分层

```java
public static class LinkInfo {
    public final String switchDevice;
    public final Integer chipIndex;
    public final String outPortName;
    public final String remoteSwitch;
    public final String remotePort;
    public final int outPortIndex;
    public final int totalOutPorts;
    public final String dstChassis;
    public int coverCount;

    /** 新增：链路分层。 */
    public final CoverageLinkLayer layer;

    public LinkInfo(String switchDevice, Integer chipIndex, String outPortName,
                    String remoteSwitch, String remotePort,
                    int outPortIndex, int totalOutPorts) {
        this(switchDevice, chipIndex, outPortName, remoteSwitch, remotePort,
             outPortIndex, totalOutPorts, null, CoverageLinkLayer.L1_L2);
    }

    /** 新增重载：带 dstChassis 与 layer；旧构造器保留，内部委托到此。 */
    public LinkInfo(String switchDevice, Integer chipIndex, String outPortName,
                    String remoteSwitch, String remotePort,
                    int outPortIndex, int totalOutPorts,
                    String dstChassis, CoverageLinkLayer layer) {
        // ... 字段赋值
    }

    /** key 不变：deviceName + ":" + outPortName。 */
    public String getKey() {
        return switchDevice + ":" + outPortName;
    }
}
```

> `getKey()` 不变，因此 NPU 链路与 SW 链路天然不冲突（设备名全局唯一）。`linkMap` 的
> `merge` 去重策略不变。

#### 5.6.2 `CoveredLinkDetail` 追加分层

与 `LinkInfo` 对应，追加 `CoverageLinkLayer layer` 字段（新增构造器，保留旧构造器），
使 Service 层可直接把它回填到 `CoverageLink.layer`。

#### 5.6.3 NPU 上连连接预计算

现有 `NpuPortConn`（NPU 端口 → 唯一 L1SW/端口/chip/机框）保留不变（旧接口与候选定位仍使用）。
新增按**设备**聚合的上连视图，供 hash 成员集使用：

```java
static class NpuUplink {
    String l1swName;      // 对端 L1SW 设备名
    String l1swPortName;  // 对端 L1SW 端口名（NPU 出端口的 remotePort）
    int l1swChipIdx;      // 对端 L1SW 端口所属 chip
    String l1swChassis;   // 对端 L1SW 机框
    int npuChipIdx;       // NPU 侧转发芯片索引
}

static class PrecomputedTopo {
    Map<String, NpuPortConn> npuPortConns = new HashMap<>();          // 既有：npuDevice:port -> conn
    Map<String, List<String>> npuL1FacingPorts = new HashMap<>();     // 新增：npuDevice -> NPU 上连出端口名列表
    Map<String, NpuUplink> npuUplinkByPort = new HashMap<>();         // 新增：npuDevice:port -> NpuUplink
    Map<String, List<String>> l1NpuFacingPorts = new HashMap<>();     // 新增：l1swDevice -> NPU 向出端口名列表
    // 既有 L2 映射保持不变
    Map<String, Map<Integer, Map<String, List<String>>>> l2swChipRemotePorts = new HashMap<>();
    Map<String, DeviceEntity> devices;
    Set<String> l2swNames;
}
```

- `npuL1FacingPorts` / `npuUplinkByPort`：遍历 NPU 设备端口（`NpuPortEntity`，`remoteDevice`
  为 L1SW 且能定位到对端端口）得到。
- `l1NpuFacingPorts`：遍历 L1SW 设备端口（`remoteDevice` 为 NPU）得到。
- 以上映射仅用于**候选/默认顺序**；真正参与 hash 的成员集来自**路由表 LPM 命中条目**（§6.1），
  两者通过端口名交集取得。

#### 5.6.4 `CoverageSearchResult` 追加分层统计

```java
public static class CoverageSearchResult {
    // ... 既有字段保持
    /** 新增：各层链路总数（层 -> count）。 */
    public final Map<CoverageLinkLayer, Integer> layerTotalLinks;
    /** 新增：各层已覆盖数（层 -> count）。 */
    public final Map<CoverageLinkLayer, Integer> layerCoveredCount;
}
```

> `CoverageSearchResult` 为引擎内部结构，构造函数为包内可见，追加字段不影响北向契约。

#### 5.6.5 芯片定位与 jettyId 读取（无需修改 `NpuCandidate`）

实现改为**不改动** `NpuCandidate`：候选端口所属 NPU 芯片由新增私有助手
`CoveragePlanEngine.chipIndexOfPort(dev, portName)` 定位（遍历 `forwardingChips`，按端口名
命中取 `chipIndex`），候选端口本身由既有 `findPortInDevice` 取得。

`jettyId` 从 `NpuPortEntity.getJettyId()` 读取；缺失或越界时回落
`HashUtils.JETTY_ID_MIN + portId` 并累加 `exJettyFallback` 诊断计数，保证旧拓扑输入
（未携带 jettyId）仍可完成选路，且缺失情况在测试报告中可见。

```java
private static Integer chipIndexOfPort(DeviceEntity dev, String portName) {
    if (dev == null || dev.getForwardingChips() == null) return null;
    for (ForwardingChip chip : dev.getForwardingChips().values()) {
        if (chip.getPorts() != null && chip.getPorts().containsKey(portName)) {
            return chip.getChipIndex();
        }
    }
    return null;
}

private int jettyIdOf(PortEntity port) {
    if (port instanceof NpuPortEntity) {
        Integer jettyId = ((NpuPortEntity) port).getJettyId();
        if (jettyId != null && HashUtils.isValidJettyId(jettyId)) return jettyId;
    }
    exJettyFallback++;                     // 拓扑缺失计数
    Integer id = port == null ? null : port.getId();
    return HashUtils.JETTY_ID_MIN + (id == null ? 0 : id);   // 32 + portId
}
```

### 5.7 DTO 兼容性（`@AllArgsConstructor` 影响）

现有 DTO 使用 Lombok `@AllArgsConstructor`，新增字段会**改变全参构造器签名**（编译期错误），
需要同步修改的既有测试如下（改动是机械的：在实参末尾补齐新字段）：

| DTO | 现有全参构造器 | 受影响位置 |
|:---|:---|:---|
| `CoveragePathsResult` | 5 参 | `test/.../dto/CoveragePathsResultTest.java`（`allArgsConstructor`） |
| `CoverageLink` | 9 参 | `test/.../dto/CoverageLinkTest.java`（`allArgsConstructor`、`equalsHashCodeToString`） |
| `CoverageStats` | 20 参 | **不变**（本次不修改） |
| `CoveragePathsRequest` | 2 参 | **不变**（本次不修改） |
| `CoveredEidPair` / `CoveredEidPairRef` | 9 / 2 参 | **不变** |

> **备选方案（零编译影响）：** 若希望完全不触碰既有测试，可改成新增包装结果
> `CoveragePathsExResult { CoveragePathsResult coverage; CoverageLinkScope scope;
> Map<CoverageLinkLayer, CoverageLayerStats> layerStats; }`，但会让北向多一层嵌套、
> 并分裂两个接口的结果类型。本设计**推荐直接扩展 `CoveragePathsResult`**
> （调用方复用成本最低，测试改动仅 2 个文件）。

---

## 6. 算法设计

### 6.1 覆盖链路域构建（`collectBidirectionalLinks` 扩展）

新增 `CoverageLinkScope scope` 参数，`L1_L2` 时行为与现状完全一致；`NPU_L1_L2` 时追加两段：

```java
private void collectBidirectionalLinks(SuperNode superNode,
                                       Map<String, DeviceEntity> devices,
                                       Set<String> l2swNames,
                                       CoverageLinkScope scope,
                                       List<LinkInfo> links) {
    // ① [既有] L1SW 出端口，remote ∈ L2SW  → layer = L1_L2
    // ② [既有] L2SW 出端口，非默认路由且出端口数 > 1，remote 为 L1SW → layer = L1_L2

    if (scope == CoverageLinkScope.NPU_L1_L2) {
        // ③ [新增] NPU 出端口，remote 为 L1SW → layer = NPU_L1
        for (DeviceEntity dev : devices.values()) {
            if (dev.getDeviceType() != DeviceType.NPU) continue;
            for (chip : dev.getForwardingChips().values()) {
                RoutingTable rt = superNodeStore.getRoutingTable(new RoutingTableKey(
                    superNode.getName(), dev.getDeviceName(), chip.getChipIndex()));
                if (rt == null || rt.getRoutes() == null) continue;
                for (RoutingEntry entry : rt.getRoutes().values()) {
                    if (entry.getOutPortInfos() == null) continue;
                    List<OutPortInfo> ports =
                        new ArrayList<>(entry.getOutPortInfos().values());
                    for (int idx = 0; idx < ports.size(); idx++) {
                        PortEntity p = chip.getPorts().get(ports.get(idx).getPortName());
                        if (p == null || p.getRemoteDevice() == null) continue;
                        if (!isL1Sw(devices.get(p.getRemoteDevice()))) continue;
                        links.add(new LinkInfo(dev.getDeviceName(), chip.getChipIndex(),
                            p.getPortName(), p.getRemoteDevice(), p.getRemotePort(),
                            idx, ports.size(), null, CoverageLinkLayer.NPU_L1));
                    }
                }
            }
        }

        // ④ [新增] L1SW 出端口，remote 为 NPU → layer = NPU_L1
        for (DeviceEntity dev : devices.values()) {
            if (!isL1Sw(dev)) continue;
            for (chip : dev.getForwardingChips().values()) {
                RoutingTable rt = superNodeStore.getRoutingTable(new RoutingTableKey(
                    superNode.getName(), dev.getDeviceName(), chip.getChipIndex()));
                if (rt == null || rt.getRoutes() == null) continue;
                for (RoutingEntry entry : rt.getRoutes().values()) {
                    if (entry.getOutPortInfos() == null) continue;
                    List<OutPortInfo> ports =
                        new ArrayList<>(entry.getOutPortInfos().values());
                    for (int idx = 0; idx < ports.size(); idx++) {
                        PortEntity p = chip.getPorts().get(ports.get(idx).getPortName());
                        if (p == null || p.getRemoteDevice() == null) continue;
                        DeviceEntity remote = devices.get(p.getRemoteDevice());
                        if (remote == null || remote.getDeviceType() != DeviceType.NPU) continue;
                        links.add(new LinkInfo(dev.getDeviceName(), chip.getChipIndex(),
                            p.getPortName(), p.getRemoteDevice(), p.getRemotePort(),
                            idx, ports.size(), null, CoverageLinkLayer.NPU_L1));
                    }
                }
            }
        }
    }

    // 既有 linkMap 去重逻辑不变：
    // linkMap.merge(link.getKey(), link,
    //               (a, b) -> a.totalOutPorts >= b.totalOutPorts ? a : b);
}
```

**口径说明：**

| 段 | 纳入条件 | 是否要求 outPorts > 1 |
|:---|:---|:---:|
| L1SW→L2SW（既有） | remote ∈ L2SW | 否 |
| L2SW→L1SW（既有） | 非默认路由 且 outPorts > 1 且 remote 为 L1SW | 是 |
| NPU→L1SW（新增） | remote 为 L1SW | 否 |
| L1SW→NPU（新增） | remote 为 NPU | 否 |

单出端口（`totalOutPorts == 1`）的路由虽无 ECMP，但该出端口是**必踩链路**，属于覆盖率分母，
必须纳入。L2SW→L1SW 沿用既有"仅 ECMP 路由"口径以保持旧结果可复现。

### 6.2 NPU→L1SW 选路 hash（新增 H5 / H7a）

**成员集定义：** 源 NPU 的**转发芯片**（候选端口所属 chip）上，按目的 CNA 做 LPM 命中的
路由条目出端口集合中，`remoteDevice` 为 L1SW 的子集。

```java
/** 正向：源 NPU → 源 L1SW（新增 H5）。 */
private List<String> npuUplinkMembers(SuperNode superNode, NpuCandidate src,
                                      String dstCna, PrecomputedTopo topo) {
    RoutingEntry entry = lookupRoute(superNode, src.deviceName, src.chipIndex, dstCna);
    if (entry == null || entry.getOutPortInfos() == null) return null;   // 路由缺失
    DeviceEntity npu = topo.devices.get(src.deviceName);
    ForwardingChip chip = npu.getForwardingChips().get(src.chipIndex);
    List<String> members = new ArrayList<>();
    for (OutPortInfo opi : entry.getOutPortInfos().values()) {
        PortEntity p = chip.getPorts().get(opi.getPortName());
        if (p == null || p.getRemoteDevice() == null) continue;
        if (isL1Sw(topo.devices.get(p.getRemoteDevice()))) members.add(p.getPortName());
    }
    return members;                                                       // 空 => 无上连
}
```

选路（实现：`CoveragePlanEngine.selectNpuEgress`）：

```
chipIdx  = chipIndexOfPort(srcNpu, src.portName)
srcPort  = srcNpu.getForwardingChips().get(chipIdx).getPorts().get(src.portName)
jettyId  = jettyIdOf(srcPort)                     // NpuPortEntity.jettyId；缺失回落 32+portId
M        = npuUplinkMembers(srcNpu, chipIdx, dst.cna)   // LPM 命中条目的 L1SW 向出端口
if M.isEmpty()  → 失败 NPU_ROUTE_NOT_FOUND / NPU_UPLINK_NOT_FOUND
idx      = HashUtils.nativeHashDstCnaJetty(dst.cna, jettyId, M.size(), hashFunc)   // ★ H5
outPort  = M.get(idx)
scna     = port(srcNpu, chipIdx, outPort).getCna()      // ★ 要求 3：选中端口 CNA → SCNA
l1Dev    = port(...).getRemoteDevice()                  // 源 L1SW
l1InPort = port(...).getRemotePort()                    // 源 L1SW 入端口
l1Chip   = findPortInDevice(l1Dev, l1InPort).getChipIndex()
detail#1 = CoveredLinkDetail(src.deviceName, chipIdx, outPort, l1Dev, l1InPort,
                             idx, M.size(), null,
                             CoverageLinkLayer.NPU_L1, DeviceType.NPU)
```

> **与旧接口的关键差异：** 旧实现用 `npuPortConns.get(src.deviceName + ":" + src.portName)`
> 以候选端口**物理绑定**源 L1SW 且不参与 hash；新实现改为**路由 LPM + `(DstCNA, jettyId)`
> hash** 推导 NPU 出端口及其对端 L1SW，并把**选中端口的 CNA 作为 SCNA** 传给后续所有
> L1SW/L2SW 选口（`nativePortIdx(scna, dst.cna, ...)`）。候选端口
> （`src.portName` / `src.cna` / `src.eid`）仍承载流的源端点身份，用于构造 EID 对与 NPU 段
> hash 的 jettyId 来源。

**退化行为：** `M.size() == 1` 时 `floorMod(hash, 1) == 0`，选路结果确定，等价于单上连场景。

### 6.3 L1SW→NPU 选路 hash（新增 H6 / H7d）

**成员集定义：** 目的 L1SW 的入端口所属 chip 上，按目的 CNA 做 LPM 命中的路由条目出端口集合中，
`remoteDevice == dst.deviceName` 的子集。

```
entry = lookupRoute(superNode, dstL1swName, dstL1swChipIdx, dst.cna)
if entry == null   → 失败 NPU_DST_ROUTE_NOT_FOUND
M = [opi.portName | opi ∈ entry.outPortInfos.values(),
       port(dstL1sw, dstL1swChipIdx, opi.portName).getRemoteDevice() == dst.deviceName]
if M.isEmpty()     → 失败 NPU_DST_PORT_NOT_FOUND
idx = nativePortIdx(scna, dst.cna, dataUdpSrcPort, ackUdpSrcPort, M.size())   // ★ H6（scna = 选中 NPU 端口 CNA）
outPort = M.get(idx)                                                        // 取代既有 get(0)
covered = LinkInfo(dstL1swName, dstL1swChipIdx, outPort, dst.deviceName,
                   port(dstL1sw, dstL1swChipIdx, outPort).getRemotePort(),
                   idx, M.size(), null, CoverageLinkLayer.NPU_L1)
```

> **替换既有 `dstL1swOutPorts.get(0)`：** 旧实现取首个出端口，多端口时其余出端口永远无法被覆盖，
> 且与硬件 hash 不一致。新实现按 hash 选择，使 `M` 中每个出端口都能被不同 EID 对踩到。

### 6.4 反向路径规则（H7 / H8）

反向追踪（目的 NPU → … → 源 NPU）的四跳：

| 反向跳 | 设备 | 成员集来源 | hash 输入（scna, dcna, sport, dport） | layer |
|:---|:---|:---|:---|:---|
| H7a | **目的 NPU → 目的 L1SW** | 目的 NPU 路由表 LPM(`src.cna`) 命中的 L1SW 向出端口 | **`(DstCNA=src.cna, jettyId=源 NPU 端口 jettyId)`**（`nativeHashDstCnaJetty`，与正向同一 jettyId） | `NPU_L1` |
| H7b | 目的 L1SW → L2SW | 既有 H3 成员集，SCNA = **H7a 选中端口 CNA**（`scnaRev`） | `nativePortIdx(scnaRev, src.cna, ack, data)` | `L1_L2` |
| H7c | L2SW → 源 L1SW | 既有 H4 成员集，SCNA 同 H7b | 同 H7b | `L1_L2` |
| H7d | **源 L1SW → 源 NPU** | 源 L1SW 路由表 LPM(`src.cna`) 命中的 NPU 向出端口 | 同 H7b（`scnaRev` 驱动，取代既有 `get(0)`） | `NPU_L1` |

> **ACK jettyId 变更说明（2026-09）：** ACK 方向 NPU 选口的 jettyId 从
> "目的 NPU 端口 jettyId"改为"**源 NPU 端口 jettyId**"（与正向同一 jettyId）。
> 这符合真实硬件行为：NPU1 端口 A ping NPU2 端口 B 时，正向选口用 A 的 jettyId，
> B 回 ACK 给 A 时选口也用 A 的 jettyId。`DstCNA` 仍为源 CNA，`SCNA` 仍为选中端口 CNA。

**正反向不变量：** 正反向 hash 输入仅 **SCNA/DCNA 与 sport/dport 互换**；jettyId 在两个方向
**相同**（均取自源 NPU 端口），因此两个方向独立选路但共享 jettyId 熵源。

### 6.5 覆盖判定与 `fullCoverage`

贪心选择算法（`runGreedyCoverage` / `runGreedyCoverageDualDisjoint` / `trimRedundantPairs` /
`rebalanceEidUsage`）**完全复用，不感知分层**——`CoveredPair` 的
`forwardCoveredKeys` / `reverseCoveredKeys` 扩展为 4 条 key 后，贪心自然把 NPU 链路纳入
`uncovered` 集合。

```java
// buildResult 中：
boolean fullCoverage = true;
for (Map.Entry<CoverageLinkLayer, Integer> e : layerTotalLinks.entrySet()) {
    if (layerCoveredCount.getOrDefault(e.getKey(), 0) < e.getValue()) {
        fullCoverage = false;
        break;
    }
}
```

- **MIN_COVERAGE：** 每层每条链路 `coverCount ≥ 1`。
- **REDUNDANT：** 每层每条链路 `coverCount ≥ 2`（`runGreedyCoverageDualDisjoint` 现有 `min=2`
  判定改为按层校验）。

### 6.6 失败分支与诊断

在既有 `fwdFail1..12` / `revFailA..H` 之外新增计数（仅 `NPU_L1_L2` 域累加，打印策略与既有
`logFwdFail()` 一致：首次 + 每 500 次打印）：

| 计数 | 触发条件 |
|:---|:---|
| `npuFwdFail1` | 源 NPU 路由 LPM 未命中（`NPU_ROUTE_NOT_FOUND`） |
| `npuFwdFail2` | 源 NPU 路由出端口中无 L1SW 向端口（`NPU_UPLINK_NOT_FOUND`） |
| `npuFwdFail3` | 选定 NPU 出端口的对端 L1SW 端口定位失败 |
| `npuFwdFail4` | 目的 L1SW 路由 LPM 未命中（`NPU_DST_ROUTE_NOT_FOUND`） |
| `npuFwdFail5` | 目的 L1SW 路由出端口中无指向 `dst.deviceName` 的端口（`NPU_DST_PORT_NOT_FOUND`） |
| `npuRevFail1..5` | 反向对应失败分支（ACK 方向同样需解析源端口 jettyId，失败计入 `exFailRevNpu`） |

> 上述失败仅表示"该 EID 对不可用"（与既有 `fwdFail*` 语义一致，直接 `continue` 跳过该对），
> 不改变接口返回状态；若全场无可用 EID 对，则 `linkMap` 无法覆盖，返回
> `COVERAGE_INCOMPLETE`。

### 6.7 关键不变量

| 编号 | 不变量 |
|:---:|:---|
| I1 | NPU↔L1 成员集只来自**路由表 LPM 命中条目**，不来自物理连接全量枚举（与硬件转发一致） |
| I2 | 成员集 `size == 1` 时 hash 退化为确定选路，与旧接口同场景结果一致 |
| I3 | `linkMap` key 仍为 `deviceName:outPortName`，无跨层冲突；同 key 去重取 `totalOutPorts` 较大者 |
| I4 | `L1_L2` 域下链路域、hash 点位、`CoveredPair` 链路数、统计全部与旧实现逐位一致 |
| I5 | `sum(layerStats[*].totalLinks) == stats.totalLinks`，`sum(layerStats[*].coveredCount) == stats.coveredCount` |
| I6 | 每个 EID 对的正向/反向覆盖链路数相等（各 4 条），jettyId 在两方向相同（均取自源端口） |
| I7 | 贪心与统计逻辑不引入分层分支；分层只出现在 `collectBidirectionalLinks`（域名）与 `buildResult`（统计） |

---

## 7. 详细流程

### 7.1 服务层流程

```java
// SncService.planPathsCoverageEx
public CoveragePathsResult planPathsCoverageEx(CoveragePathsRequest request) {
    if (state != State.DATAREADY) {
        throw new SNCStateException("SNC is not in DATAREADY state, current state: " + state);
    }
    if (request == null) {
        throw new IllegalArgumentException("CoveragePathsRequest must not be null");
    }
    CoveragePathsResult result = pathService.planPathsCoverageEx(request);
    LOG.info("planPathsCoverageEx: status=" + result.getStatus()
        + ", scope=" + result.getScope());
    return result;
}
```

```java
// PathService.planPathsCoverageEx —— 与既有 planPathsCoverage 共用校验与转换
public CoveragePathsResult planPathsCoverageEx(CoveragePathsRequest request) {
    return planPathsCoverageInternal(request, CoverageLinkScope.NPU_L1_L2);
}

public CoveragePathsResult planPathsCoverage(CoveragePathsRequest request) {
    return planPathsCoverageInternal(request, CoverageLinkScope.L1_L2);   // 行为不变
}
```

`planPathsCoverageInternal(request, scope)` 抽取自现有 `planPathsCoverage` 方法体，
仅两处按 `scope` 分支：

| 位置 | 分支行为 |
|:---|:---|
| 调用引擎 | `L1_L2` → `coveragePlanEngine.findCoverage(...)`；`NPU_L1_L2` → `findCoverageEx(...)` |
| 结果回填 | 设置 `result.setScope(scope)`；`NPU_L1_L2` 时额外调用 `buildLayerStats(searchResult)` 设置 `layerStats` |
| `errorMessage` | `NPU_L1_L2` 时附加分层明细（§5.5） |
| `CoverageLink` 转换 | 回填 `deviceType`（由 `link.deviceType` 映射：NPU/SW）与 `layer` |

其余（`eidPairs` 构建、`coverageLinks` 构建、`coveredPairs` 反向索引、`stats` 回填）逻辑
完全共享，避免双份实现。

### 7.2 引擎流程 `findCoverageEx`

```java
public CoverageSearchResult findCoverageEx(SuperNode superNode,
                                           int fixedDataUdpSrcPort,
                                           int fixedAckUdpSrcPort,
                                           CoverageRequirement requirement) {
    Map<String, DeviceEntity> devices = superNode.getAllDevices();
    if (devices == null || devices.isEmpty()) return emptyResult();

    List<NpuCandidate> npuCandidates = collectNpuCandidates(devices);
    if (npuCandidates.isEmpty()) return emptyResult();

    Set<String> l2swNames = collectL2swNames(devices);

    List<LinkInfo> links = new ArrayList<>();
    collectBidirectionalLinks(superNode, devices, l2swNames,
                              CoverageLinkScope.NPU_L1_L2, links);      // ★ 域名扩展
    if (links.isEmpty()) return emptyResult();

    Map<String, LinkInfo> linkMap = new HashMap<>();
    for (LinkInfo link : links) {
        linkMap.merge(link.getKey(), link,
                      (a, b) -> a.totalOutPorts >= b.totalOutPorts ? a : b);
    }
    int totalLinks = linkMap.size();

    PrecomputedTopo topo = precomputeTopology(devices, l2swNames);      // ★ 含 NPU 上连视图

    boolean dual = (requirement == CoverageRequirement.REDUNDANT);
    return findCoverageDiffPath(superNode, linkMap, npuCandidates, totalLinks,
                                fixedDataUdpSrcPort, fixedAckUdpSrcPort, topo,
                                CoverageLinkScope.NPU_L1_L2, dual);     // ★ scope 透传
}
```

> `findCoverageEx` 与 `findCoverage` 的唯一差别是 `scope`；其余步骤完全一致，
> 因此可以先重构 `findCoverage(...)` 内部为 `findCoverageInternal(..., scope)`
> 以减少重复代码（见 §11 实施步骤）。

### 7.3 正向路径追踪（8 步）

`traceForwardPath(superNode, src, dst, dataUdpSrcPort, ackUdpSrcPort, topo, scope)`
返回 `PathTrace`，`forwardLinkDetails` 由 2 条扩展为 4 条。

| Step | 操作 | 产物 / 失败分支 |
|:---:|:---|:---|
| 1 | 源 NPU 路由 LPM（`src.chipIndex`，目的 `dst.cna`）→ 过滤 L1SW 向出端口 = `M_npu` | `npuFwdFail1/2` |
| 2 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_npu.size())` → `npuOutPort`；定位对端 L1SW 端口/chip | `npuFwdFail3`；**覆盖链路 ① `NPU_L1`** |
| 3 | 源 L1SW 路由 LPM（Step 2 得到的 chip，`dst.cna`）→ 过滤 L2SW 向出端口 = `M_l1l2` | 既有 `fwdFail2` |
| 4 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l1l2.size())` → `l1OutPort`；定位 L2SW 入端口/chip | 既有 `fwdFail3a/3b`；**覆盖链路 ② `L1_L2`** |
| 5 | L2SW 路由到 `dst.remoteL1sw` → `M_l2l1`（既有 `l2swChipRemotePorts`） | 既有 `fwdFail8` |
| 6 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l2l1.size())` → `l2OutPort`；定位目的 L1SW 入端口/chip | 既有 `fwdFail9/10/11`；**覆盖链路 ③ `L1_L2`** |
| 7 | 目的 L1SW 路由 LPM（chip，`dst.cna`）→ 过滤 `remoteDevice == dst.deviceName` 的出端口 = `M_l1npu` | `npuFwdFail4/5` |
| 8 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l1npu.size())` → `dstOutPort`；校验对端为 `dst.deviceName` | 既有 `fwdFail3c`；**覆盖链路 ④ `NPU_L1`** |

> Step 2 与 Step 7 为新增；Step 3~6 为既有逻辑，仅把 Step 2 得到的 L1SW/chip 替换原来
> `npuPortConns` 提供的 L1SW/chip（`scope == NPU_L1_L2` 时）。

### 7.4 反向路径追踪（8 步）

`traceReversePath(superNode, src, dst, ackUdpSrcPort, dataUdpSrcPort, fwdTrace, topo, scope)`
的 `reverseLinkDetails` 同样扩展为 4 条：

| Step | 操作 | 区域 |
|:---:|:---|:---|
| R0 | 解析**源 NPU 端口**，读取其 jettyId（ACK 选口使用源端口 jettyId，与正向同一） | **新增** |
| R1 | 目的 NPU 路由 LPM（目的侧 chip，`src.cna`）→ 过滤 L1SW 向出端口；`nativeHashDstCnaJetty(src.cna, 源端口jettyId)` | **新增**，覆盖链路 ① `NPU_L1` |
| R2 | 目的 L1SW → L2SW（既有 H3） | 既有 `revFailA/B/C` |
| R3 | L2SW → 源 L1SW（既有 H4） | 既有 `revFailD/E` |
| R4 | 源 L1SW 路由 LPM（`src.cna`）→ 过滤 `remoteDevice == src.deviceName` 的出端口；hash(`dst.cna, src.cna, ack, data`) | **替换既有 `get(0)`**，覆盖链路 ④ `NPU_L1` |

**正反向覆盖链路组合：**

```
正向： ① 源NPU→源L1   ② 源L1→L2   ③ L2→目的L1   ④ 目的L1→目的NPU   (jettyId = 源端口)
反向： ① 目的NPU→目的L1 ② 目的L1→L2 ③ L2→源L1    ④ 源L1→源NPU      (jettyId = 源端口，与正向同)
```

### 7.5 贪心选择（复用，不变）

`findCoverageDiffPath` 中对每个 `(src, dst)` 交叉组合：

```java
PathTrace fwd = traceForwardPath(superNode, src, dst, data, ack, topo, scope);
if (!fwd.viable) continue;
ReversePathTrace rev = traceReversePath(superNode, src, dst, ack, data, fwd, topo, scope);
if (!rev.viable) continue;

List<String> fwdKeys = fwd.forwardLinkDetails.stream()
                           .map(CoveredLinkDetail::getLinkKey).collect(toList());   // 4 条
List<String> revKeys = rev.reverseLinkDetails.stream()
                           .map(CoveredLinkDetail::getLinkKey).collect(toList());   // 4 条
allPairs.add(new PairCoverage(src, dst, fwdKeys, revKeys,
    fwd.forwardLinkDetails, rev.reverseLinkDetails, fwd.srcChassis, fwd.dstChassis));
```

后续 `runGreedyCoverage`（MIN）/ `runGreedyCoverageDualDisjoint`（REDUNDANT）与
`trimRedundantPairs` / `rebalanceEidUsage` 完全复用；仅 `fullCoverage` 判定按层（§6.5）。

### 7.6 结果构建

1. `buildResult(...)` 计算 `allLinkInfos`、逐链路 `coverCount`、合计指标与**分层指标**
   （按 `LinkInfo.layer` 分组后套用同一套公式）。
2. `CoverageSearchResult` 回传 `layerTotalLinks` / `layerCoveredCount`。
3. `PathService`：
   - `eidPairs[*].coveredLinks` = 正向 4 条 + 反向 4 条（顺序见 §8.1）；
   - `coverageLinks[*].layer` / `deviceType` 回填；
   - `result.layerStats` = 按层构建的 `CoverageLayerStats`；
   - `result.scope = NPU_L1_L2`；
   - `errorMessage`（未全覆盖时）追加分层明细。

---

## 8. 结果输出与统计

### 8.1 `coveredLinks` 顺序约定

`CoveredEidPair.coveredLinks` 在 `NPU_L1_L2` 域下固定 **8 条**，顺序如下
（与 `forwardLinkDetails` + `reverseLinkDetails` 的拼接顺序一致）：

| 下标 | 方向 | 设备（出端口） | 对端 | layer |
|:---:|:---|:---|:---|:---|
| 0 | 正向 | 源 NPU | 源 L1SW | `NPU_L1` |
| 1 | 正向 | 源 L1SW | L2SW | `L1_L2` |
| 2 | 正向 | L2SW | 目的 L1SW | `L1_L2` |
| 3 | 正向 | 目的 L1SW | 目的 NPU | `NPU_L1` |
| 4 | 反向 | 目的 NPU | 目的 L1SW | `NPU_L1` |
| 5 | 反向 | 目的 L1SW | L2SW | `L1_L2` |
| 6 | 反向 | L2SW | 源 L1SW | `L1_L2` |
| 7 | 反向 | 源 L1SW | 源 NPU | `NPU_L1` |

> `L1_L2` 域下仍为 **4 条**（下标 0..3 = 正 2 条 + 反 2 条），**顺序与语义完全不变**。
>
> **渲染适配：** `CoverageDiagramGenerator` 现按 `li == 0/1/2/3 → 类型 1/2/3/4` 硬编码映射，
> 需改为按 `CoverageLink.layer` + 出端口/对端设备类型判断类型，才能同时支持两个接口
> （见 §9.2）。

### 8.2 `CoverageLink` 增补字段回填

```java
private CoverageLink toCoverageLink(CoveragePlanEngine.LinkInfo li) {
    CoverageLink cl = new CoverageLink();
    cl.setSwitchDevice(li.switchDevice);
    cl.setChipIndex(li.chipIndex);
    cl.setOutPort(li.outPortName);
    cl.setRemoteSwitch(li.remoteSwitch);
    cl.setRemotePort(li.remotePort);
    cl.setOutPortIndex(li.outPortIndex);
    cl.setTotalOutPorts(li.totalOutPorts);
    cl.setCoverCount(li.coverCount);
    cl.setLayer(li.layer);                       // 新增
    cl.setDeviceType(li.deviceType.name());      // 新增：NPU / SW
    return cl;
}
```

`LinkInfo` 需追加 `DeviceType deviceType` 字段（`LinkInfo` 位于 `engine` 包，可直接引用
`entity` 包的 `DeviceType`）。

### 8.3 统计与分层

```java
// 合计（既有口径）：覆盖全部 layer
stats.totalLinks    = Σ linkMap.size()
stats.coveredCount  = Σ link.coverCount > 0 ? 1 : 0
stats.coverageRate  = coveredCount / totalLinks
// ... 其余重复率 / EID 均匀度字段保持不变

// 分层（新增）：
layerStats[NPU_L1].totalLinks   = 该层出端口数
layerStats[NPU_L1].coveredCount = 该层已覆盖数
layerStats[L1_L2].totalLinks    = 该层出端口数
layerStats[L1_L2].coveredCount  = 该层已覆盖数
```

`NPU_L1_L2` 域下典型关系：

```
totalLinks(NPU_L1) ≈ NPU 上连出端口数（去重后） + L1SW 的 NPU 向出端口数（去重后）
totalLinks(L1_L2)  = 既有 planPathsCoverage 的 totalLinks（拓扑未变时应相等）
```

### 8.4 输出示例

```json
{
  "status": "COVERAGE_INCOMPLETE",
  "errorMessage": "物理遍历覆盖率未达100%: 99.42% (2068/2080 出端口已覆盖) [NPU_L1: 100.00% 1024/1024; L1_L2: 98.86% 1044/1056]",
  "scope": "NPU_L1_L2",
  "eidPairs": [
    {
      "srcEid": "000000000000000000000000dfdf0011",
      "dstEid": "000000000000000000000000dfdf0091",
      "srcDevice": "rack1#board1#npu1",
      "srcPort": "400GUB 1/2/1",
      "srcCna": "223.223.0.17",
      "dstCna": "223.223.0.145",
      "destDevice": "rack1#board5#npu1",
      "destPort": "400GUB 5/2/1",
      "coveredLinks": [
        { "switchDevice": "rack1#board1#npu1", "deviceType": "NPU", "layer": "NPU_L1",
          "chipIndex": 0, "outPort": "400GUB 1/2/2", "remoteSwitch": "rack1#l1sw1",
          "remotePort": "400GUB 1/0/3", "outPortIndex": 5, "totalOutPorts": 8 },
        { "switchDevice": "rack1#l1sw1", "deviceType": "SW", "layer": "L1_L2",
          "chipIndex": 0, "outPort": "400GUB 1/0/65", "remoteSwitch": "l2sw2",
          "remotePort": "400GUB 1/0/33:0", "outPortIndex": 17, "totalOutPorts": 32 }
      ]
    }
  ],
  "coverageLinks": [
    { "switchDevice": "rack1#board1#npu1", "deviceType": "NPU", "layer": "NPU_L1",
      "outPort": "400GUB 1/2/2", "coverCount": 3,
      "coveredPairs": [ { "srcEid": "...", "dstEid": "..." } ] }
  ],
  "stats": { "totalLinks": 2080, "coveredCount": 2068, "coverageRate": 0.9942 },
  "layerStats": {
    "NPU_L1": { "totalLinks": 1024, "coveredCount": 1024, "coverageRate": 1.0,
                "minRepeatCount": 1, "maxRepeatCount": 4, "avgRepeatCount": 2.1,
                "repeatRate": 1.1 },
    "L1_L2":  { "totalLinks": 1056, "coveredCount": 1044, "coverageRate": 0.9886,
                "minRepeatCount": 1, "maxRepeatCount": 5, "avgRepeatCount": 2.4,
                "repeatRate": 1.37 }
  }
}
```

### 8.5 与旧接口输出差异速查

| 输出现象 | `planPathsCoverage` | `planPathsCoverageEx` |
|:---|:---|:---|
| `scope` | `L1_L2` | `"NPU_L1_L2"` |
| `eidPairs[*].type` | `CROSS_L2` | `CROSS_L2` / `LOCAL_L1` |
| `layerStats` | `null` | 两个分层 |
| `coverageLinks[*].layer` | `null` | `NPU_L1` / `L1_L2` |
| `coverageLinks[*].deviceType` | `null` | `"NPU"` / `"SW"` |
| `totalLinks` | 仅 L1↔L2 | NPU↔L1 ＋ L1↔L2 |
| `coveredLinks` 条数 | 4 / 对 | 8（框间）/ 4（框内） |

---

## 9. 兼容性与影响分析

### 9.1 现有接口零行为变更

| 变更点 | 对 `planPathsCoverage` 的影响 |
|:---|:---|
| `collectBidirectionalLinks` 新增 `scope` 参数 | `L1_L2` 时执行分支与现状逐行一致，不追加 NPU 段 |
| `traceForwardPath` / `traceReversePath` 新增 `scope` 参数 | `L1_L2` 时仍用 `npuPortConns` 固定源 L1SW、末跳仍取 `get(0)` |
| `LinkInfo` / `CoveredLinkDetail` 新增字段 | 旧构造器保留并委托到新构造器；`L1_L2` 链路 `layer = L1_L2` |
| `CoveragePathsResult` / `CoverageLink` 新增字段 | 既有字段（`status`/`errorMessage`/`eidPairs` 旧字段/`coverageLinks` 旧字段/`stats`）取值完全不变；新增字段中 `scope = L1_L2`、`eidPairs[*].type = CROSS_L2` 会被填写（纯增量、可直接判断来源），`layerStats` 与 `coverageLinks[*].layer/deviceType` 保持 `null` |
| `runGreedyCoverage*` / `buildResult` | `fullCoverage` 判定在 `L1_L2` 域下与单层等价；统计公式未变 |
| `SncService` / `PathService` 公开方法 | 旧方法签名与内部实现抽取为 `planPathsCoverageInternal(..., L1_L2)`，语义不变 |

> **回归基线：** 既有 `PlanPathsCoverageIntegrationTest`、`CoverageMainSuccessTest`、
> `PathServiceTest` 中的覆盖用例必须**全部原样通过**，作为"零行为变更"的验收标准。

### 9.2 必须同步修改的既有测试与工具

| 文件 | 改动原因 | 改动方式 |
|:---|:---|:---|
| `test/.../dto/CoveragePathsResultTest.java` | `CoveragePathsResult` 全参构造器 5 → 7 参 | 在实参末尾补 `null, null`，并新增 `scope`/`layerStats` 的 setter/getter 断言 |
| `test/.../dto/CoverageLinkTest.java` | `CoverageLink` 全参构造器 9 → 11 参 | 在实参末尾补 `null, null`，并新增断言 |
| `test/.../service/CoverageDiagramGenerator.java` | 硬编码 `li == 0/1/2/3 → 类型 1/2/3/4` | 改为按 `link.getLayer()` + `deviceType` 判定；NPU 链路渲染为 NPU↔L1SW 段 |
| `test/.../dto/CoverageStatsTest`（若后续新增） | `CoverageStats` 未改动 | 无需改动 |

> 其余测试（`CoveragePathsRequestTest`、`CoveredEidPairTest`、`CoverageRequirementTest`、
> `SncServiceIntegrationTest`、`PathServiceTest` 的非覆盖用例等）不受影响。

### 9.3 性能影响

| 维度 | 变化 | 说明 |
|:---|:---|:---|
| EID 对候选枚举 | **不变**（O(n²)） | 候选集合仍为 NPU 端口，未因分层扩大 |
| 单次路径追踪 | 每次 +2 次路由 LPM（正向/反向各两处 NPU 段） | `lookupRoute` 为 O(掩码种类数)，典型 ≤ 5 |
| 单次路径追踪 | 每次 +2 次 `nativeHash`（NPU 段） | 与既有 L1/L2 hash 同量级 |
| `linkMap` 规模 | 约 +1 倍（NPU↔L1 出端口） | 覆盖链路域由"L1↔L2"扩为"NPU↔L1 ∪ L1↔L2" |
| 贪心选择 | 与 `uncovered` 规模同阶增长 | `greedySelect` 内层扫描为 `linkToPairIdx` 驱动，新增链路会带来更多 `uncovered` 项 |
| 内存 | `CoveredPair` 链路列表 2 → 4（正/反各），`CoveredLinkDetail` 数量翻倍 | 全量 EID 对的内存约 +2x 链路明细 |

> **建议：** 对全机架拓扑（128 NPU / 16 L1SW / 4 L2SW）先做一次基准测量（`CoverageDiagramGenerator`
> 已有耗时打印），据此决定是否需要加候选裁剪（如仅保留跨机框对）。

### 9.4 线程安全

- `CoveragePlanEngine` 本身无状态，`hashFunc` / 固定端口 / `hashTuple` 为 final 字段。
- **既有隐患：** 失败计数器 `fwdFail1..12` / `revFailA..H` 为 `private static int`，并发调用
  会相互污染。新增 `npuFwdFail*` / `npuRevFail*` 时**建议一并改为实例字段**（或在 `uninit`
  时重置），避免新接口引入新的共享可变状态。
- `SuperNode` / `SuperNodeStore` 由调用方保证并发安全（既有约束不变）。

### 9.5 配置兼容

| 配置 | 是否新增 | 说明 |
|:---|:---:|:---|
| `SNCConfig.hashFunc` | 否 | NPU 段 hash 复用同一函数选择器 |
| `SNCConfig.fixedDataUdpPort` / `fixedAckUdpPort` | 否 | NPU 段复用固定端口（四/五元组参与 hash 时生效） |
| `SNCConfig.hashTuple` | 否 | NPU 段复用同一元组宽度 |
| 新增 NPU 段开关 | **不加** | 是否启用由调用哪个接口决定（`planPathsCoverage` vs `planPathsCoverageEx`） |

---

## 10. 测试设计

### 10.1 测试夹具增强

现有机架拓扑中 NPU 路由**每个目的 CNA 只有一个出端口**（`FullRackTopologyGenerator`：
"each 1 outPort"），无法驱动 NPU 段 hash。需在 `CoverageRouteAugmentor` 增加：

```java
/**
 * 为 NPU 增强 NPU↔L1SW 多出端口路由，用于驱动 NPU 段 hash 选路。
 *
 * <p>对每个 NPU：把其到"对端 L1SW 向目的 CNA"的路由出端口扩展为
 * 该 NPU 上连到目标 L1SW 的多个端口（ECMP），并同步维护 maskLengths。
 * 同时为 L1SW 的 NPU 向 /32 路由扩展多出端口（当 L1SW 到同一 NPU 有多端口时）。
 */
public static void augmentNpuRouting(SuperNode superNode);

/** 统计 NPU→L1SW 链路的出端口总数（与 countL1swToL2swLinks 同风格）。 */
public static int countNpuToL1swLinks(SuperNode superNode);

/** 统计 L1SW→NPU 链路的出端口总数。 */
public static int countL1swToNpuLinks(SuperNode superNode);
```

> 若真实拓扑不存在 NPU 多上连，则 NPU 段成员集恒为 1，hash 退化——此时测试重点转为
> **覆盖率分母纳入 NPU↔L1 链路**（`totalLinks = countNpuToL1swLinks + countL1swToNpuLinks
> + countL1swToL2swLinks + countL2swToL1swLinks`）。

### 10.2 单元测试（`CoveragePlanEngineNpuL1Test`，新增）

| 用例 | 场景 | 断言 |
|:---|:---|:---|
| T1 | NPU 单上连（成员集 size = 1） | NPU 出端口 = 唯一上连端口；`coveredLinks` 含 `NPU_L1` 链路；与旧接口同 EID 对结果一致 |
| T2 | NPU 多上连（size ≥ 2），固定端口 `(0,0)` 二元组 | 同一 EID 对两次调用结果稳定；不同 EID 对可落到不同 NPU 出端口 |
| T3 | `HashTuple.FIVE`，不同 sport/dport | NPU 出端口选择随端口变化（证明 NPU 段参与 hash） |
| T4 | 源 NPU 路由缺失 | `traceForwardPath` 不可用，`npuFwdFail1` 累加，该对不入选 |
| T5 | 源 NPU 路由出端口无 L1SW 向端口 | `npuFwdFail2` 累加 |
| T6 | L1SW 末跳多出端口 | 不同 EID 对可覆盖不同 L1SW→NPU 出端口（证明替换 `get(0)` 生效） |
| T7 | L1SW 末跳路由缺失/无 NPU 向端口 | `npuFwdFail4/5` 累加 |
| T8 | 旧接口回归 | `findCoverage(..., L1_L2)` 的 `totalLinks` / `coveredCount` / 每对 4 条链路与改动前一致 |
| T9 | 分层统计一致性 | `sum(layerStats[*].totalLinks) == stats.totalLinks`；`sum(coveredCount)` 同理 |
| T10 | `fullCoverage` 按层判定 | 构造仅 NPU_L1 层未覆盖的夹具 → 必须返回 `COVERAGE_INCOMPLETE` |

### 10.3 北向集成测试（`PlanPathsCoverageExIntegrationTest`，已落地）

夹具：`FullRackTopologyGenerator` 生成的全机架拓扑 → 裁剪为**单机框**（rack1 NPU + rack1 L1SW，
无 L2SW），经 `augmentL1swRouting` / `augmentL2swRouting` / `augmentNpuRouting` /
`augmentL1swNpuRouting` 增强，再经 `SncService.init + setSuperNode` 接入
（2 机框场景保留给老接口，见 §13.1）。

| 用例 | 断言 |
|:---|:---|
| `minCoverage` | `status ∈ {SUCCESS, COVERAGE_INCOMPLETE}`；`scope == NPU_L1_L2`；`eidPairs` 非空；每对 `type == LOCAL_L1` 且 `coveredLinks.size() == 4`；`L1_L2.totalLinks == 0`；`sum(layerStats) == stats`；NPU_L1 `coveredCount > 0` |
| `redundant` | 同上，且每条 `layer == NPU_L1` 且 `coverCount > 0` 的链路 `coverCount ≥ 2` |
| `notDataReady` | 未 `setSuperNode`（非 DATAREADY）时抛 `SNCStateException` |
| `nullRequest` | `request == null` 抛 `IllegalArgumentException` |
| `missingSuperNode` | `superNodeName` 不存在 → 返回 `TOPO_NOT_FOUND` |
| `afterUninit` | `uninit` 后调用抛 `SNCStateException` |

**全机架（4 框）覆盖率报告工具：** `test/.../service/CoverageExRackReport`（`main`，为不拖慢回归
**不作为 CI 用例**）：

```bash
mvn -o -DskipTests test-compile
java -Xmx6g -Djna.library.path=<resources> \
     -cp "target/classes;target/test-classes;<jna.jar>;<fastjson2.jar>" \
     com.huawei.umdk.snc.service.CoverageExRackReport MIN_COVERAGE target/coverage-rack4-report.md
```

实测（见 §13.7）：全机架 4096/4096 条出端口 **100%** 覆盖、`SUCCESS`、598 个框间 EID 对、耗时 ≈189s。

### 10.4 断言矩阵（新增字段）

| 断言项 | 表达式 |
|:---|:---|
| 分层合计 | `layerStats.values().stream().mapToInt(CoverageLayerStats::getTotalLinks).sum() == stats.getTotalLinks()` |
| 每对链路数 | `eidPairs.stream().allMatch(p -> p.getCoveredLinks().size() == 8)` |
| NPU 链路设备类型 | `coverageLinks` 中 `layer == NPU_L1 && "NPU".equals(deviceType)` 的项，其 `remoteSwitch` 必为 L1SW 设备名 |
| L1→NPU 链路设备类型 | `layer == NPU_L1 && "SW".equals(deviceType)` 的项，其 `remoteSwitch` 必为 NPU 设备名 |
| `coveredPairs` 一致性 | 既有断言（`coveredPairs.size() == coverCount`）对新增层同样成立 |
| 覆盖率单调性 | `stats.coverageRate` ≤ 同拓扑下 `layerStats[L1_L2].coverageRate + layerStats[NPU_L1].coverageRate`（分层加权关系） |

### 10.5 回归对比测试

保留现有 `planPathsCoverage` 集成用例不动；建议补充一个 **对拍用例**：同一拓扑、同一配置下，
`planPathsCoverageEx` 的 `layerStats[L1_L2]`（totalLinks / coveredCount）应与
`planPathsCoverage` 的对应值在拓扑增强前后保持可比（若 `augmentNpuRouting` 不改动 L1SW↔L2SW
路由，则应完全相等）。

---

## 11. 文件清单与实施步骤

### 11.0 已落地内容（2026-09-13 完成，477 个测试全部通过）

| 文件 | 状态 | 说明 |
|:---|:---|:---|
| `src/snc/.../util/HashUtils.java` | 已改 | `nativeHashDstCnaJetty` + `JETTY_ID_MIN/MAX` + `isValidJettyId` |
| `src/snc/.../entity/NpuPortEntity.java` | 已改 | `jettyId` + 显式 2 参/3 参构造器 |
| `src/snc/.../dto/CoverageLinkScope.java`、`CoverageLinkLayer.java`、`CoverageLayerStats.java` | 新增 | 覆盖域 / 分层 / 分层统计 |
| `src/snc/.../dto/CoverageLink.java`、`CoveragePathsResult.java` | 已改 | `+deviceType`/`+layer`、`+scope`/`+layerStats` |
| `src/snc/.../engine/CoveragePlanEngine.java` | 已改 | 扩展路径（`findCoverageEx`/`traceForwardPathEx`/`traceReversePathEx`/`selectNpuEgress`/`collectNpuL1Links`/`getExDiagnostics`），旧路径零改动 |
| `src/snc/.../service/PathService.java` | 已改 | `planPathsCoverageInternal` + `planPathsCoverageEx` + `buildLayerStats`/`formatLayerDetail` |
| `src/snc/.../SncService.java` | 已改 | 北向 `planPathsCoverageEx` |
| `src/snc/.../route/topo/template/loader/PortLoader.java`、`model/SncPort.java`、`service/TopoTemplateService.java` | 已改 | 模板输入 `jetty_id` → `SncPort.jettyId`（缺失回落 `32 + portId`） |
| `src/snc/src/main/resources/128_npu_rack.json` | 已改 | 256 个 NPU 端口写入 `"jetty_id": 32 + port_id` |
| `test/snc/resources/topo_data_2npu_1port.json`、`topo_data_4npu_8port.json` | 已改 | NPU 端口写入 `"jettyId"` |
| `test/.../TestDataLoader.java` | 已改 | 解析 `jettyId` |
| `test/.../FullRackTopologyGenerator.java` | 已改 | 每个 NPU 物理端口**固定**分配 `jettyId = JETTY_ID_BASE + 端口序号`（32..39，跨生成完全一致） |
| `test/.../FullRackTopologyJettyIdTest.java` | 新增 | 钉死 jettyId 固定分配：区间/NPU 内唯一/精确映射/两次生成一致（3 用例） |
| `test/.../service/CoverageExRackReport.java` | 新增 | 全机架 4 框覆盖率报告工具（`main`，输出 `target/coverage-rack4-report.md`，非 CI 用例） |
| `test/.../CoverageRouteAugmentor.java` | 已改 | `augmentNpuRouting`（NPU 多上连 ECMP）+ `countNpuToL1swLinks` / `countL1swToNpuLinks` |
| `test/.../util/HashUtilsJettyTest.java` | 新增 | 6 个用例：区间/越界/取模范围/可复现/分布（2 成员 ECMP 下 128 次采样命中 2 个成员，64:64）/**CRC8 与 Java 参考实现逐位一致** |
| `test/.../CoverageIntraChassisTest.java` | 新增 | **新接口 PathService 层测试（单机框夹具）**：框间 0 对、框内 2 跳覆盖（SUCCESS、64/64、18 对）＋ CRC8 选口/SCNA 链接端到端校验 ＋ 分层统计一致性 ＋ 路由范围校验，产出报告 |
| `test/.../PlanPathsCoverageExIntegrationTest.java` | 新增 | **新接口北向集成测试（`SncService` + 全机架生成器单机框子集，类比老接口）**：MIN_COVERAGE/REDUNDANT（含"被覆盖链路 ≥2"）＋ 非 DATAREADY/null/不存在/uninit 契约，6 用例 |
| `test/.../dto/CoverageLinkTest.java`、`CoveragePathsResultTest.java` | 已改 | 全参构造器补齐新字段 + 新字段断言 |
| `ubswitch.c`（仓库根目录） | 已改 | 新增导出符号 `ubswitch_Hash_dieEcmp`（CRC-8/ATM，(DstCNA, jettyId) 二元组） |
| `build_ubswitch.ps1`（仓库根目录） | 新增 | 重新编译 `libubswitch.dll` + 两个 `.so`（MinGW gcc / clang `-fuse-ld=lld --target=…-linux-gnu -nostdlib`） |
| `umdk/src/snc/src/main/resources/libubswitch*.{dll,so}` | 已更新 | 重新编译（含新符号）：dll 66048B、x86_64 so 3152B、aarch64 so 3144B |
| `src/snc/.../dto/CoveragePathType.java` | 新增 | 路径类型标识（CROSS_L2 / LOCAL_L1，按 NPU/L1/L2 层级命名，不依赖物理 chassis 概念） |
| `src/snc/.../dto/CoveredEidPair.java` | 已改 | `+type` |
| `test/.../dto/CoveredEidPairTest.java` | 已改 | 全参构造器补齐 `type` |
| `src/snc/.../engine/CoveragePlanEngine.java` | 已改（二） | 两阶段覆盖（框间→框内）、`npuL1Peers`/`l2PortsTowardsL1Peers`/`l1PortsTowardsDevice`、框内 2 跳追踪、路径类型标记 |
| `test/.../CoverageRouteAugmentor.java` | 已改（二） | `augmentL1swNpuRouting`（L1SW 路由按 NPU 设备范围扩展）+ `checkL1swNpuRouteScope`/`npuCnasOf` |

**实测结果：** 见 §13.7。单机框（新接口唯一自动化集成测试）：`SUCCESS`、框内 18 对、NPU_L1
**100%（64/64）**；2 机框场景仅用于老接口回归（`PlanPathsCoverageIntegrationTest` /
`CoverageMainSuccessTest`），NPU 段失败计数为 0，`jettyIdFallback` 为 0（拓扑输入已携带 jettyId）。

### 11.1 文件清单

| 文件 | 类型 | 说明 |
|:---|:---|:---|
| `doc/ch/snc/SNC NPU-L1 Coverage Path Planning Design.ch.md` | 新增 | 本文档 |
| `doc/en/snc/SNC NPU-L1 Coverage Path Planning Design.md` | 新增 | 英文对照版（按仓库 ch/en 一对一约定） |
| `src/snc/.../dto/CoverageLinkScope.java` | 新增 | 覆盖域枚举 |
| `src/snc/.../dto/CoverageLinkLayer.java` | 新增 | 分层枚举 |
| `src/snc/.../dto/CoverageLayerStats.java` | 新增 | 分层统计 DTO |
| `src/snc/.../dto/CoverageLink.java` | 修改 | +`deviceType`、+`layer` |
| `src/snc/.../dto/CoveragePathsResult.java` | 修改 | +`scope`、+`layerStats` |
| `src/snc/.../engine/CoveragePlanEngine.java` | 修改 | `LinkInfo`/`CoveredLinkDetail` 分层、NPU 上连视图、`findCoverageEx`、H5/H6/H7a/H7d、`buildResult` 分层、失败计数 |
| `src/snc/.../service/PathService.java` | 修改 | `planPathsCoverageInternal`、`planPathsCoverageEx`、`layerStats`/`deviceType` 回填 |
| `src/snc/.../SncService.java` | 修改 | `planPathsCoverageEx` + `History` 注释 |
| `test/snc/java/.../CoverageRouteAugmentor.java` | 修改 | `augmentNpuRouting` / `countNpuToL1swLinks` / `countL1swToNpuLinks` |
| `test/snc/java/.../engine/CoveragePlanEngineNpuL1Test.java` | 新增 | 单元测试（§10.2） |
| `test/snc/java/.../PlanPathsCoverageExIntegrationTest.java` | 新增 | 集成测试（§10.3） |
| `test/snc/java/.../dto/CoverageLayerStatsTest.java` | 新增 | 分层统计 DTO 测试（按既有 DTO 测试模板） |
| `test/snc/java/.../dto/CoveragePathsResultTest.java` | 修改 | 全参构造器补齐新字段 |
| `test/snc/java/.../dto/CoverageLinkTest.java` | 修改 | 全参构造器补齐新字段 |
| `test/snc/java/.../service/CoverageDiagramGenerator.java` | 修改 | 链路类型判定改为按 `layer`/`deviceType` |
| `doc/ch/snc/SNC API Guide.ch.md` / `doc/en/snc/SNC API Guide.md` | 修改 | 补充 `planPathsCoverageEx` 接口说明与结果字段 |
| `doc/ch/snc/SNC Test Specification.ch.md` / `doc/en/snc/SNC Test Specification.md` | 修改 | 补充新接口测试用例矩阵 |

### 11.2 实施步骤（建议顺序）

1. **DTO 先行**：新增 `CoverageLinkScope` / `CoverageLinkLayer` / `CoverageLayerStats`，
   扩展 `CoverageLink` / `CoveragePathsResult`，同步修正 2 个 DTO 测试 → 编译通过。
2. **引擎重构（无行为变更）**：把 `findCoverage` / `findCoverageDiffPath` /
   `collectBidirectionalLinks` / `traceForwardPath` / `traceReversePath` 增加 `scope` 参数，
   `L1_L2` 分支保持逐行一致；`buildResult` 增加分层统计（`L1_L2` 时单层）。
3. **跑旧回归**：既有覆盖测试全绿，证明第 2 步零行为变更。
4. **新增 NPU 段**：`precomputeTopology` 增加 NPU 上连视图；`collectBidirectionalLinks`
   增加第 ③④ 段；`traceForwardPath`/`traceReversePath` 接入 H5/H6/H7a/H7d 与失败计数。
5. **Service 接入**：抽取 `planPathsCoverageInternal`，新增 `planPathsCoverageEx`
   （`PathService` + `SncService`），回填 `scope` / `layerStats` / `deviceType` / `layer`。
6. **测试夹具与用例**：`augmentNpuRouting` + 单元/集成测试；更新 `CoverageDiagramGenerator`。
7. **文档**：更新 API Guide 与 Test Specification（中英同步），补充本设计文档英文版。

---

## 12. 开放问题

| 编号 | 问题 | 影响 | 建议 |
|:---:|:---|:---|:---|
| Q1 | 接口正式命名（`planPathsCoverageEx` vs `planPathsCoverageWithNpuL1` vs `planPathsFullCoverage`） | 影响 §3.1 签名与全部文档 | 评审时定名；改名成本仅为 3 处签名 + 文档替换 |
| Q2 | NPU 出端口 hash 是否应按 **NPU 端口级 CNA**（上连端口自身 CNA）而非流的源 CNA 作为 SCNA | 影响 hash 输入语义与结果 | 当前设计沿用既有约定（源 CNA）；若硬件按出端口 CNA 参与 hash，需按实际行为调整 |
| Q3 | `L1SW→NPU` 末跳是否也应纳入 `REDUNDANT` 的"每链路 ≥ 2" | 影响 EID 对数量与冗余度 | 当前设计按层统一 ≥2，如有更细粒度需求可分层配置 |
| Q4 | 是否需要支持"仅 NPU↔L1、不含 L1↔L2"的中间域 | 影响 `CoverageLinkScope` 取值 | 当前仅 `L1_L2` / `NPU_L1_L2`；如需可加 `NPU_L1` |
| Q5 | 失败计数器是否借本次改造由 `static` 改为实例字段 | 影响并发语义 | 建议一并改造（§9.4） |
| Q6 | `CoverageLink.switchDevice` 是否随语义扩展改名为 `deviceName` | 影响 JSON 契约 | 建议保留原名并在注释中说明语义（避免破坏兼容） |

---

## 13. 框间/框内 两阶段覆盖与示例

> 本章为 2026-09-13 落地的补充说明：两个覆盖率（框间 / 框内）、路由范围扩展、EID 与出端口的关系、
> NPU→L1 的 CRC8 选口算法，以及两类流程的具体示例。

### 13.1 两个覆盖率与输出范围

| 接口 | 覆盖范围 | 输出内容 |
|:---|:---|:---|
| `planPathsCoverage`（旧） | **仅框间**（L1SW↔L2SW） | 每 EID 对 4 条链路，`CoveredEidPair.type = null` |
| `planPathsCoverageEx`（新） | **框间 + 框内** | 两类 EID 对同时出现在 `eidPairs` 中，用 `type` 区分 |

| 阶段 | 路径 | 每 EID 对链路数 | 标识 |
|:---|:---|:---:|:---|
| 阶段 1 **框间** | `NPU → L1SW → L2SW → L1SW → NPU` | 8（4 正向 + 4 ACK） | `CoveredEidPair.type = CROSS_L2`；`CoverageLink.layer ∈ {NPU_L1, L1_L2}` |
| 阶段 2 **框内** | `NPU → L1SW → NPU`（同一机框，不经 L2SW） | 4（2 正向 + 2 ACK） | `CoveredEidPair.type = LOCAL_L1`；`CoverageLink.layer = NPU_L1` |

执行顺序（`CoveragePlanEngine.findCoverageEx`）：

1. **先做框间全覆盖**：枚举**跨机框** EID 对，追踪 4 跳正/反向路径，贪心选出覆盖 L1SW↔L2SW 的 EID 对；
2. **再看缺口**：在阶段 1 的覆盖结果中筛出 `layer == NPU_L1 && coverCount < required` 的出端口
   （`required`：MIN_COVERAGE = 1，REDUNDANT = 2）；
3. **用框内补齐**：仅当存在缺口时，枚举**同机框** EID 对并追踪 2 跳正/反向路径，以"缺口集合"为
   目标再贪心选对；
4. **合并统计**：阶段 1 + 阶段 2 的 EID 对合并后，在**完整链路域**上重算覆盖率/重复率等统计。

> 因此某些拓扑下阶段 2 可能产出 0 对（框间已覆盖全部 NPU↔L1SW 出端口），这是正常结果；
> 单机框（或跨机框路径不可用）时则全部由框内覆盖。
>
> **测试约定（2026-09-13）：** **2 机框场景只用于老接口 `planPathsCoverage` 的回归测试**
> （`PlanPathsCoverageIntegrationTest`、`CoverageMainSuccessTest`）；新接口 `planPathsCoverageEx`
> 的自动化测试全部基于 `FullRackTopologyGenerator` 生成的拓扑、裁剪为**单机框**子集：
> - `PlanPathsCoverageExIntegrationTest`（**北向 `SncService`**，类比老接口集成测试）：MIN/REDUNDANT
>   覆盖率 + 状态机/空参/SuperNode 不存在/uninit 四类契约用例；
> - `CoverageIntraChassisTest`（`PathService` 层）：框内 2 跳 + CRC8 选口 + SCNA 链接 + 分层统计 +
>   路由范围端到端校验，并产出框内覆盖率报告。

### 13.2 示例 A：框间流程（4 跳 + ACK）

> 示例数据取自 2 机框拓扑的规划输出。按 §13.1 的测试约定，2 机框场景只做老接口回归，
> 因此本示例作为**流程走查**；新接口的自动化断言见 §13.3（单机框）。

拓扑：2 机框子集（8 NPU + 8 L1SW + 4 L2SW）。EID 对：`rack2#board1#npu1:400GUB 1/2/1` ↔
`rack1#board1#npu2:400GUB 1/4/1`。

| # | 方向 | 设备 | 出端口 | 对端 | 对端端口 | layer |
|--:|:---|:---|:---|:---|:---|:---|
| 0 | 正向 | rack2#board1#npu1 | 400GUB 1/2/1 | rack2#l1sw1 | 400GUB 1/0/1 | NPU_L1 |
| 1 | 正向 | rack2#l1sw1 | 400GUB 1/0/78 | l2sw1 | 400GUB 1/0/7:2 | L1_L2 |
| 2 | 正向 | l2sw1 | 400GUB 1/0/7:2 | rack1#l1sw1 | 400GUB 1/0/78 | L1_L2 |
| 3 | 正向 | rack1#l1sw1 | 400GUB 1/0/3 | rack1#board1#npu2 | 400GUB 1/4/1 | NPU_L1 |
| 4 | ACK | rack1#board1#npu2 | 400GUB 1/4/2 | rack1#l1sw1 | 400GUB 1/0/4 | NPU_L1 |
| 5 | ACK | rack1#l1sw1 | 400GUB 1/0/94 | l2sw1 | 400GUB 1/0/15:2 | L1_L2 |
| 6 | ACK | l2sw1 | 400GUB 1/0/47:2 | rack2#l1sw1 | 400GUB 1/0/94 | L1_L2 |
| 7 | ACK | rack2#l1sw1 | 400GUB 1/0/1 | rack2#board1#npu1 | 400GUB 1/2/1 | NPU_L1 |

流程要点：
1. 源 NPU 用 **CRC8 `(DstCNA, jettyId)`** 在"到目的所经 L1SW 的上连端口"里选出口（第 0 跳）；
2. 该选中端口的 **CNA 成为 SCNA**，参与 L1SW→L2SW（第 1 跳）与 L2SW→L1SW（第 2 跳）的选口；
3. 目的侧 L1SW 按 `DstCNA` 查表，在"到目的 NPU 的全部端口"里 hash 选出口（第 3 跳）；
4. ACK 方向（第 4~7 跳）：`DstCNA = 源 CNA`，**jettyId = 源 NPU 端口 jettyId**（与正向同一 jettyId）。

### 13.3 示例 B：框内流程（2 跳 + ACK，真实测试输出）

拓扑：单机框（rack1：4 NPU + 4 L1SW，无 L2SW）→ 阶段 1 无可用 EID 对，全部由阶段 2 覆盖。
EID 对：`rack1#board1#npu2:400GUB 1/4/1` ↔ `rack1#board1#npu1:400GUB 1/2/1`。

| # | 方向 | 设备 | 出端口 | 对端 | 对端端口 | layer |
|--:|:---|:---|:---|:---|:---|:---|
| 0 | 正向 | rack1#board1#npu2 | 400GUB 1/4/1 | rack1#l1sw1 | 400GUB 1/0/3 | NPU_L1 |
| 1 | 正向 | rack1#l1sw1 | 400GUB 1/0/2 | rack1#board1#npu1 | 400GUB 1/2/2 | NPU_L1 |
| 2 | ACK | rack1#board1#npu1 | 400GUB 1/2/1 | rack1#l1sw1 | 400GUB 1/0/1 | NPU_L1 |
| 3 | ACK | rack1#l1sw1 | 400GUB 1/0/4 | rack1#board1#npu2 | 400GUB 1/4/2 | NPU_L1 |

### 13.4 路由范围扩展与接收语义

**接收语义（关键前提）**：**目的 CNA 属于某 NPU 设备，该 NPU 就能接收该报文**，即使报文到达的
端口并不是该 CNA 自己对应的物理端口。因此路由/选口的约束从"CNA ↔ 端口一一对应"放宽为
"**CNA 所属 NPU 设备可达**"。

| 位置 | 旧口径 | 扩展后口径 | 落地 |
|:---|:---|:---|:---|
| L1SW 路由 | 仅为"物理相连端口"的 CNA 建 /32 路由 | 对**有端口的每个 NPU**，为其**每个 CNA** 建 /32 路由，出端口 = 该 L1SW 到该 NPU 的**全部**端口 | `CoverageRouteAugmentor.augmentL1swNpuRouting`，校验 `checkL1swNpuRouteScope` |
| L2SW→L1SW 选口 | 固定用 `dst.remoteL1sw`（目的端口所在的 L1SW） | 任意"能到达目的 NPU 设备的 L1SW" | `PrecomputedTopo.npuL1Peers` + `l2PortsTowardsL1Peers` |
| L1SW→NPU 选口 | 路由仅 1 个出端口（无 ECMP） | 该 L1SW 到目的 NPU 的全部端口（≥2 → hash 可选） | `l1PortsTowardsDevice` + `nativePortIdx(scna, dstCna, …)` |

副作用（正向）：L1SW→NPU 与 L2SW→L1SW 两跳的 ECMP 成员集不再退化，覆盖规划可以真正"踩"到
这些出端口，NPU↔L1SW 层覆盖率可达 100%。

### 13.5 EID 与出端口的关系（"已指定 EID 但报文不一定从该端口出去"）

- `srcPort / srcCna / srcEid` 标识的是**端点身份**（哪台设备的哪个逻辑端口发起/接收该流），
  **不约束**报文的物理出口；
- 源 NPU 收到报文后，按 **CRC8 `(DstCNA, jettyId)`** 在自己的上连端口（对目的所经 L1SW 的全部
  端口）中选择真实出口；**被选中端口的 CNA 才是后续 L1/L2 选口使用的 SCNA**；
- 因此 `CoveredEidPair.srcPort`（选定的端点身份）与 `coveredLinks[0].outPort`（真实出口）**可以不同**，
  这是设计上的有意行为，测试用例 `CoverageIntraChassisTest.verifyForwardIntraChain` /
  `verifyAckIntraChain` 对此有显式校验；
- ACK 方向同理：`destPort` 是 ACK 的发送端点身份，真实出口由 `(srcCna, 源端口 jettyId)` 决定
  （jettyId 取自源 NPU 端口，与正向同一）。

### 13.6 原生库：NPU→L1 的 CRC8 选口算法

`ubswitch.c`（仓库根目录）新增导出符号：

```c
int ubswitch_Hash_dieEcmp(const char *dst_cna, int jetty_id, int ecmp_cnt);
/* CRC-8/ATM: poly 0x07, init 0x00, 无反转、无最终异或
   字节流 = DstCNA 的 ASCII（不含结尾 NUL）+ jettyId 低字节 + jettyId 高字节
   ecmp_cnt == 0 → 返回原始 CRC（0..255）；ecmp_cnt > 0 → CRC % ecmp_cnt */
```

| 用途 | 原生符号 | Java 入口 |
|:---|:---|:---|
| 框间 L1SW↔L2SW 选口、L1SW→NPU 选口 | `ubswitch_Hash_ecmp` | `HashUtils.nativeHash(...)` / `nativePortIdx(...)` |
| **NPU→L1SW 选口（CRC8）** | **`ubswitch_Hash_dieEcmp`** | **`HashUtils.nativeHashDstCnaJetty(dstCna, jettyId, ecmpCnt, hashFunc)`** |

二进制更新：`build_ubswitch.ps1`
（MinGW `gcc -O2 -shared` 生成 `libubswitch.dll`；`clang --target={x86_64,aarch64}-unknown-linux-gnu
-fuse-ld=lld -nostdlib -shared` 生成两个 `.so`），产物落在
`umdk/src/snc/src/main/resources/`。CRC8 正确性由 `HashUtilsJettyTest.crc8MatchesReference`
用 Java 参考实现逐位比对（含 `ecmpCnt` 取模）。

### 13.7 实测结果

| 场景 | 接口 | status | 框间对数 | 框内对数 | NPU_L1 覆盖率 | L1_L2 覆盖率 | 报告 |
|:---|:---|:---|--:|--:|--:|--:|:---|
| **全机架 4 框**（148 设备） | **新接口** `planPathsCoverageEx` | **`SUCCESS`** | **598** | 0 | **100.00% (2048/2048)** | **100.00% (2048/2048)** | `target/coverage-rack4-report.md`（耗时 ≈189s） |
| 2 机框子集 | **老接口** `planPathsCoverage`（仅框间） | 见 `PlanPathsCoverageIntegrationTest` | — | — | — | — | 控制台输出（`CoverageMainSuccessTest`） |
| 单机框（无 L2SW） | **新接口** `planPathsCoverageEx` | `SUCCESS` | 0 | 18 | **100% (64/64)** | 0/0 | `target/coverage-intra-chassis-report.md` |

> 全机架 4 框 = `FullRackTopologyGenerator` 生成内容（128 NPU × 8 端口 + 16 L1SW + 4 L2SW），
> 覆盖链路域 4096 条（NPU_L1 2048 = NPU→L1 1024 + L1→NPU 1024；L1_L2 2048 = L1→L2 1024 + L2→L1 1024），
> **全部 4096 条被 598 个框间 EID 对覆盖**，10 项引擎诊断计数全为 0、`jettyIdFallback` 为 0。
> 运行方式见 §10.4（`CoverageExRackReport`，非 CI 用例以免拖慢回归）。

> 2 机框子集若用新接口规划，实测框间 161 对、NPU_L1 100%（128/128）、L1_L2 55.27%（566/1024）、
> 框内 0 对（框间已覆盖全部 NPU_L1）——该结果仅作参考，**不作为自动化测试用例**。
