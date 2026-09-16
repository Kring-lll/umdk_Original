# FullRackTopologyGenerator 测试调用链分析

> 适用范围：`src/snc` 模块及其测试目录 `umdk/test/snc/java`。
> 目的：弄清 `FullRackTopologyGenerator` 生成的全机架拓扑在测试中**由谁触发、用到哪些 Java 文件、走什么路径**，方便后续维护与测试替换。

---

## 1. 概述

`FullRackTopologyGenerator`（`umdk/test/snc/java/com/huawei/umdk/snc/FullRackTopologyGenerator.java:55`）是一个纯内存拓扑生成器，**不走 JSON 文件**，直接构造 `SuperNode` 对象图。

产物规模（`FullRackTopologyGenerator.java:60-67`）：

| 类型 | 数量 | 备注 |
|------|------|------|
| NPU 设备 | 128 | 4 rack × 8 board × 4 NPU |
| L1 交换 | 16 | 4 rack × 4 L1SW |
| L2 交换 | 4 | 4 个全局 L2SW |
| 合计 | 148 | 128×8 = 1024 个 NPU 物理端口 |

唯一对外入口：

```java
public static SuperNode buildRawTopology()   // FullRackTopologyGenerator.java:92
```

产出的 `SuperNode` 内**只有 /32 主路由**，没有 ECMP 默认路由、没有跨框 L1SW↔L2SW 路由，这些由 `CoverageRouteAugmentor` 在加载后补齐（见 §4 第 2 步）。

---

## 2. 触发入口（"谁出发的"）

按"直接调用"和"间接调用"两类。所有路径均位于 `umdk/test/snc/java` 下（pom.xml `testSourceDirectory` 指向此目录，`pom.xml:68`）。

### 2.1 直接调用 `FullRackTopologyGenerator.buildRawTopology()`

| 调用方 | 类型 | 触发方式 | 行号 |
|--------|------|---------|------|
| `FullRackTopologyJettyIdTest` | JUnit 5 测试 | `@Test` 方法直接调用，跑 3 个用例 | `FullRackTopologyJettyIdTest.java:63,99,125,126` |

### 2.2 通过 `RackTopologyLoader.loadRawTopology()` 间接调用

`RackTopologyLoader`（`RackTopologyLoader.java:19`）是薄包装，仅一行委托：

```java
public static SuperNode loadRawTopology() {
    return FullRackTopologyGenerator.buildRawTopology();   // RackTopologyLoader.java:30
}
```

调用方：

| 调用方 | 类型 | 触发方式 | 调用点 |
|--------|------|---------|--------|
| `CoverageIntraChassisTest` | JUnit 5 测试 | `@BeforeAll setUp` | `CoverageIntraChassisTest.java:81` |
| `PlanPathsCoverageIntegrationTest` | JUnit 5 测试 | `@BeforeAll setUp` | `PlanPathsCoverageIntegrationTest.java:39` |
| `PlanPathsCoverageExIntegrationTest` | JUnit 5 测试 | `@BeforeAll setUp` | `PlanPathsCoverageExIntegrationTest.java:64` |
| `service/CoverageMainSuccessTest` | JUnit 5 测试 | `@BeforeAll setUp` | `CoverageMainSuccessTest.java:43` |
| `service/CoverageExRackReport` | 可执行 `main` 类 | `java -cp ... CoverageExRackReport` | `CoverageExRackReport.java:60` |
| `service/CoverageDiagramGenerator` | 可执行 `main` 类 | `java -cp ... CoverageDiagramGenerator` | `CoverageDiagramGenerator.java:67` |

> 后两个不是 JUnit 测试，是手工触发的离线报告/绘图工具，通过 `public static void main` 启动。

### 2.3 入口分类汇总

```
JUnit 测试（mvn test 自动跑）
├─ FullRackTopologyJettyIdTest         (直接调)
├─ CoverageIntraChassisTest            (间接，单机框裁剪)
├─ PlanPathsCoverageIntegrationTest    (间接，双机框裁剪)
├─ PlanPathsCoverageExIntegrationTest  (间接，单机框裁剪)
└─ service/CoverageMainSuccessTest     (间接，全 4 框不裁剪)

离线 main 工具（手工 java -cp 启动）
├─ service/CoverageExRackReport        (间接，全 4 框)
└─ service/CoverageDiagramGenerator    (间接，全 4 框)
```

---

## 3. 涉及的 Java 文件清单

### 3.1 测试夹具层（`umdk/test/snc/java/com/huawei/umdk/snc/`）

| 文件 | 角色 |
|------|------|
| `FullRackTopologyGenerator.java` | 拓扑生成器本体 |
| `RackTopologyLoader.java` | 一行包装，统一对外入口 |
| `CoverageRouteAugmentor.java` | 路由增强器，补 ECMP 默认路由与跨框 /32 路由 |
| `FullRackTopologyJettyIdTest.java` | 校验 jettyId 固定分配的 JUnit 测试 |
| `CoverageIntraChassisTest.java` | 框内 2 跳覆盖测试 |
| `PlanPathsCoverageIntegrationTest.java` | `planPathsCoverage` 北向接口集成测试 |
| `PlanPathsCoverageExIntegrationTest.java` | `planPathsCoverageEx` 北向接口集成测试 |
| `service/CoverageMainSuccessTest.java` | §5 全局覆盖测试（全 4 框） |
| `service/CoverageExRackReport.java` | 离线 MD 报告生成器 |
| `service/CoverageDiagramGenerator.java` | 离线 HTML 拓扑图生成器 |

### 3.2 被测主代码（`umdk/src/snc/src/main/java/com/huawei/umdk/snc/`）

| 文件 | 作用 |
|------|------|
| `SncService.java` | 北向服务入口，`setSuperNode` / `planPathsCoverage` / `planPathsCoverageEx` |
| `service/PathService.java` | 内部服务，组装 `PathEngine` + `RouteLookupEngine` + `CoveragePlanEngine` |
| `store/SuperNodeStore.java` | `SuperNode` 仓库，`replace(sn)` 装入拓扑供引擎查询路由表 |
| `engine/PathEngine.java` | 单条路径规划引擎 |
| `engine/RouteLookupEngine.java` | LPM 路由查找引擎（按 mask 降序） |
| `engine/CoveragePlanEngine.java` | 覆盖规划核心引擎，两阶段搜索 |
| `util/HashUtils.java` | JNA 调原生库 `libubswitch` 做 CRC8 哈希 |
| `util/AddressUtils.java` | CNA / 掩码工具 |
| `util/DllLoader.java` | 原生库加载器（见 SNC 原生库加载方案文档） |
| `config/SNCConfig.java` | 配置项（hashFunc、hashTuple、UDP 端口等） |
| `config/HashTuple.java` | 二元组/五元组枚举 |

### 3.3 关键实体类（`umdk/src/snc/src/main/java/com/huawei/umdk/snc/entity/`）

| 文件 | 作用 |
|------|------|
| `SuperNode.java` | 顶层容器，含 `npuDevices` 与 `swDevices` |
| `DeviceEntity.java` | 设备基类 |
| `NpuDevice.java` / `SwDevice.java` | NPU / SW 设备 |
| `ForwardingChip.java` / `NpuForwardingChip.java` / `SwForwardingChip.java` | 转发芯片 |
| `PortEntity.java` / `NpuPortEntity.java` / `SwPortEntity.java` | 端口实体（NpuPortEntity 带 `jettyId`、`cna`） |
| `RoutingTable.java` / `RoutingEntry.java` / `RoutePrefix.java` / `OutPortInfo.java` | 路由表条目 |
| `SwitchLevel.java` | L1/L2 枚举 |
| `DeviceType.java` | NPU/SW 枚举 |

### 3.4 DTO（`umdk/src/snc/src/main/java/com/huawei/umdk/snc/dto/`）

| 文件 | 作用 |
|------|------|
| `CoveragePathsRequest.java` / `CoveragePathsResult.java` | 覆盖规划请求 / 结果 |
| `CoveredEidPair.java` / `CoveredEidPairRef.java` | 被覆盖的 EID 对 |
| `CoverageLink.java` / `CoverageLayerStats.java` / `CoverageLinkLayer.java` / `CoverageLinkScope.java` / `CoveragePathType.java` / `CoverageRequirement.java` | 覆盖链路与分层统计 |

---

## 4. 详细调用流程

所有调用方都遵循同一套四阶段流程，差别只在第 0 步是否裁剪、第 4 步走哪个北向接口。

### 阶段 0：拓扑裁剪（可选）

部分测试为聚焦场景，在 `setUp` 里裁剪 `SuperNode`：

| 测试 | 裁剪策略 | 调用点 |
|------|---------|--------|
| `CoverageIntraChassisTest` | 仅 rack1 的 4 NPU + 4 L1SW，**去掉所有 L2SW** | `CoverageIntraChassisTest.java:85-96` |
| `PlanPathsCoverageExIntegrationTest` | 同上（单机框，无 L2SW） | `PlanPathsCoverageExIntegrationTest.java:66-77` |
| `PlanPathsCoverageIntegrationTest` | rack1+rack2 各 4 NPU + 4 L1SW，**保留 4 个 L2SW** | `PlanPathsCoverageIntegrationTest.java:43-57` |
| `CoverageMainSuccessTest` | **不裁剪**，全 4 框 148 设备 | `CoverageMainSuccessTest.java:43` |
| `CoverageExRackReport` | **不裁剪**，全 4 框 | `CoverageExRackReport.java:60` |
| `CoverageDiagramGenerator` | **不裁剪**，全 4 框 | `CoverageDiagramGenerator.java:67` |
| `FullRackTopologyJettyIdTest` | 不裁剪（只校验生成器输出） | `FullRackTopologyJettyIdTest.java:63` |

裁剪方式：`sn.getNpuDevices().keySet().removeIf(...)` 与 `sn.getSwDevices().keySet().removeIf(...)`，直接改 SuperNode 内部 Map。

### 阶段 1：加载拓扑

```
调用方
  └─ RackTopologyLoader.loadRawTopology()                    // RackTopologyLoader.java:30
       └─ FullRackTopologyGenerator.buildRawTopology()       // FullRackTopologyGenerator.java:92
            ├─ buildNpuDevice(r,b,n)   × 128                 // :225
            │    ├─ buildNpuPorts(r,b,n)                     // :246  生成 8 端口，jettyId=32+p
            │    └─ buildNpuRouting(r,b,n)                   // :266  生成 1016 条 /32 路由
            ├─ buildL1swDevice(r,s) × 16                     // :287
            │    ├─ buildL1swPorts(r,s)                      // :305  64 NPU-facing + 64 L2-facing
            │    └─ buildL1swRouting(r,s)                    // :331  64 NPU /32 + 192 L2 /32
            └─ buildL2swDevice(s)   × 4                      // :377
                 ├─ buildL2swPorts(s,c)  × 2 chips           // :397
                 └─ buildL2swRouting(s,c) × 2 chips          // :414  4×64=256 条 /32
            → new SuperNode(NAME, VERSION, npuDevices, swDevices)
```

> **原生库副作用**：`FullRackTopologyGenerator.JETTY_ID_BASE = HashUtils.JETTY_ID_MIN`（`FullRackTopologyGenerator.java:79`）。`HashUtils` 静态初始化块（`HashUtils.java:26`）会通过 `DllLoader.load()` 加载 `libubswitch*.so/.dll`。因此**只要类被引用，原生库就会被加载**——加载失败时 `HashUtils.LIB` 为 `null`，后续 `nativeHash` 抛 `IllegalStateException`。

### 阶段 2：路由增强

加载后的 `SuperNode` 只有裸 /32 路由，覆盖规划需要 ECMP 默认路由和跨框路径。调用 `CoverageRouteAugmentor` 的 4 个静态方法补齐：

| 方法 | 作用 | 何时调用 |
|------|------|---------|
| `augmentL1swRouting(sn)` | L1SW 加 0.0.0.0/0 默认 ECMP（64 个 L2-facing 端口）；跨框 NPU CNA 加 /32 ECMP 路由 | 所有覆盖测试都调 |
| `augmentL2swRouting(sn)` | L2SW 加 0.0.0.0/0 默认 ECMP；每个 NPU CNA 加 /32 路由（出端口为本芯片连目的机框 L1SW 的 32 个端口） | 所有覆盖测试都调 |
| `augmentNpuRouting(sn)` | NPU 加默认路由 / 跨芯片 ECMP | 仅 `CoverageExRackReport`、`PlanPathsCoverageExIntegrationTest`、`CoverageIntraChassisTest` 调 |
| `augmentL1swNpuRouting(sn)` | L1SW 加 NPU-facing ECMP 路由，让框内 2 跳 NPU→L1SW→NPU 可达 | 同上三处 |

调用顺序固定：`L1sw → L2sw → Npu → L1swNpu`（见 `CoverageIntraChassisTest.java:99-102`、`PlanPathsCoverageExIntegrationTest.java:79-82`、`CoverageExRackReport.java:61-64`）。

> 老接口 `planPathsCoverage` 只关心 L1SW↔L2SW 链路覆盖，因此 `PlanPathsCoverageIntegrationTest` 与 `CoverageMainSuccessTest` 只调前两个 augment 方法（`PlanPathsCoverageIntegrationTest.java:40-41`、`CoverageMainSuccessTest.java:44-45`）。

### 阶段 3：装入仓库并构造服务

两种装配方式：

**方式 A：通过 `SncService`（北向接口测试用）**

```
SncService sncService = new SncService();
sncService.init(new SNCConfig());          // SncService.java:init
sncService.setSuperNode(sn);               // SncService.java:140  → 内部 SuperNodeStore.replace(sn)
```

调用点：`PlanPathsCoverageIntegrationTest.java:62-64`、`PlanPathsCoverageExIntegrationTest.java:84-86`。

`setSuperNode` 内部委托给 `SuperNodeStore`（`SncService.java:140-149`），状态机进入 `DATAREADY`，之后才能调 `planPathsCoverage` / `planPathsCoverageEx`。

**方式 B：直接构造 `PathService`（底座测试 / 离线工具用）**

```
SuperNodeStore store = new SuperNodeStore();
store.init();
store.replace(sn);                                       // SuperNodeStore.java

CoveragePlanEngine engine =
    new CoveragePlanEngine(store, hashFunc, dataPort, ackPort, hashTuple);

PathService pathService = new PathService(
    store, new PathEngine(), new RouteLookupEngine(), engine);
```

调用点：`CoverageIntraChassisTest.java:104-109`、`CoverageMainSuccessTest.java:64-85`、`CoverageExRackReport.java:67-73`、`CoverageDiagramGenerator.java:81-...`。

### 阶段 4：触发北向 / 引擎执行

| 调用方 | 入口方法 | 调用点 |
|--------|---------|--------|
| `PlanPathsCoverageIntegrationTest` | `sncService.planPathsCoverage(req)` | `PlanPathsCoverageIntegrationTest.java:98,107,119,129` |
| `PlanPathsCoverageExIntegrationTest` | `sncService.planPathsCoverageEx(req)` | `PlanPathsCoverageExIntegrationTest.java:140,152,173,183,191,203` |
| `CoverageIntraChassisTest` | `pathService.planPathsCoverageEx(req)` | `CoverageIntraChassisTest.java:120` |
| `CoverageMainSuccessTest` | `pathService.planPathsCoverage(req)` | `CoverageMainSuccessTest.java:103,153,184,204,222,240` |
| `CoverageExRackReport` | `pathService.planPathsCoverageEx(req)` | `CoverageExRackReport.java:81` |
| `CoverageDiagramGenerator` | `pathService.planPathsCoverage(req)` | （main 流程内） |
| `FullRackTopologyJettyIdTest` | **不调用引擎**，只断言 `SuperNode` 结构 | `FullRackTopologyJettyIdTest.java:60-129` |

引擎内部链路（`planPathsCoverageEx` 时）：

```
PathService.planPathsCoverageEx
  └─ CoveragePlanEngine
       ├─ 阶段 1：框间（inter-chassis）NPU→L1→L2→L1→NPU 4 跳覆盖
       │    └─ RouteLookupEngine.lookupRoute   (按 maskLengths 降序 LPM)
       │    └─ HashUtils.nativeHash / nativeHashDstCnaJetty  (调原生库)
       └─ 阶段 2：框内（intra-chassis）NPU→L1→NPU 2 跳覆盖
            └─ 同上
```

---

## 5. 三种典型路径

### 路径 A：纯夹具校验（不跑引擎）

`FullRackTopologyJettyIdTest`：

```
JVM 启动
  → HashUtils 静态块加载 libubswitch.so        (HashUtils.java:26)
  → @BeforeAll 隐式
  → FullRackTopologyGenerator.buildRawTopology()  (FullRackTopologyJettyIdTest.java:63)
  → 断言：
      - 128 NPU × 8 端口 = 1024 个 jettyId    (:89)
      - 每个 NPU 内 jettyId 唯一、落在 [32,1023]  (:78-83)
      - 端口序号 p → 32 + p 精确映射            (:104-112)
      - 两次生成结果完全一致                    (:125-128)
  → 不调 CoverageRouteAugmentor、不调 SncService、不调引擎
```

### 路径 B：北向 SncService 集成测试

以 `PlanPathsCoverageExIntegrationTest` 为例：

```
@BeforeAll setUp                                                  (:60-87)
  1. RackTopologyLoader.loadRawTopology()                         (:64)
       → FullRackTopologyGenerator.buildRawTopology()
  2. 裁剪为单机框：rack1 4 NPU + 4 L1SW，去掉所有 L2SW            (:66-77)
  3. CoverageRouteAugmentor.augmentL1sw/L2sw/Npu/L1swNpuRouting   (:79-82)
  4. new SncService().init(new SNCConfig())                       (:84-85)
  5. sncService.setSuperNode(sn)                                  (:86)
       → SuperNodeStore.replace(sn)
       → 状态 → DATAREADY

@Test minCoverage                                                 (:136-146)
  → sncService.planPathsCoverageEx(request(MIN_COVERAGE))         (:140)
       → CoveragePlanEngine 两阶段搜索
       → 断言 status / scope / eidPairs / layerStats

@AfterAll tearDown                                                (:89-92)
  → sncService.uninit()
```

### 路径 C：离线报告 / 绘图（main 入口）

以 `CoverageExRackReport` 为例（`CoverageExRackReport.java:54-90`）：

```
$ java -cp <cp> -Djna.library.path=<so目录> \
       com.huawei.umdk.snc.service.CoverageExRackReport MIN_COVERAGE out.md

main(args)                                                       (:54)
  1. RackTopologyLoader.loadRawTopology()                        (:60)  → 全 4 框
  2. CoverageRouteAugmentor.augmentL1sw/L2sw/Npu/L1swNpuRouting  (:61-64)
  3. new SuperNodeStore().init(); store.replace(sn)              (:67-69)
  4. new CoveragePlanEngine(store, 1, 0, 0, HashTuple.TWO)       (:71)
     new PathService(store, new PathEngine(),
        new RouteLookupEngine(), engine)                          (:72-73)
  5. pathService.planPathsCoverageEx(request)                    (:81)
  6. buildReport(...) → 写 target/coverage-rack4-report.md       (:86-90)
```

`CoverageDiagramGenerator` 流程类似，多一步 `buildTopoPortInfo` 抽端口映射（`CoverageDiagramGenerator.java:76`），最终产出 HTML。

---

## 6. 数据流概览图

```
┌───────────────────────────────────────────────────────────────────────┐
│  触发层                                                                │
│  ┌──────────────────────┐   ┌──────────────────────┐                  │
│  │ JUnit @BeforeAll     │   │ main() 离线工具       │                  │
│  │ 5 个测试类            │   │ 2 个 main 类          │                  │
│  └──────────┬───────────┘   └──────────┬───────────┘                  │
│             │ 直接调                     │ 直接调                       │
│             ▼                            ▼                              │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ RackTopologyLoader.loadRawTopology()  (薄包装)       │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          │ 委托                                       │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ FullRackTopologyGenerator.buildRawTopology()         │              │
│  │  生成 128 NPU + 16 L1SW + 4 L2SW + 路由表            │              │
│  │  副作用：触发 HashUtils 静态块 → DllLoader 加载 .so  │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          │ SuperNode                                  │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ （可选）拓扑裁剪 removeIf(...)                       │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ CoverageRouteAugmentor.augmentL1sw/L2sw/Npu/L1swNpu │              │
│  │  补 ECMP 默认路由 + 跨框 /32 路由                     │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ 装入仓库：SuperNodeStore.replace(sn)                  │              │
│  │  方式 A：SncService.setSuperNode(sn)                  │              │
│  │  方式 B：直接 new SuperNodeStore + PathService        │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ 引擎执行                                              │              │
│  │  SncService.planPathsCoverage(Ex)                    │              │
│  │   → PathService → CoveragePlanEngine                  │              │
│  │      → RouteLookupEngine (LPM)                       │              │
│  │      → HashUtils.nativeHash → libubswitch.so (JNA)   │              │
│  └───────────────────────┬─────────────────────────────┘              │
│                          ▼                                            │
│  ┌─────────────────────────────────────────────────────┐              │
│  │ CoveragePathsResult                                  │              │
│  │  → JUnit 断言 / 写 MD 报告 / 写 HTML 图               │              │
│  └─────────────────────────────────────────────────────┘              │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 7. 文件级路径速查表

| 触发方 | 加载入口 | 裁剪 | augment 全套 | 装配方式 | 北向接口 | 产物 |
|--------|---------|------|--------------|---------|---------|------|
| `FullRackTopologyJettyIdTest` | 直接 `buildRawTopology` | 否 | 否 | 否 | 否 | JUnit 断言 |
| `CoverageIntraChassisTest` | `RackTopologyLoader` | 单机框 | 4 个全调 | `PathService` 直接构造 | `planPathsCoverageEx` | `target/coverage-intra-chassis-report.md` |
| `PlanPathsCoverageIntegrationTest` | `RackTopologyLoader` | 双机框 | 仅 L1+L2 | `SncService` | `planPathsCoverage` | JUnit 断言 |
| `PlanPathsCoverageExIntegrationTest` | `RackTopologyLoader` | 单机框 | 4 个全调 | `SncService` | `planPathsCoverageEx` | JUnit 断言 |
| `service/CoverageMainSuccessTest` | `RackTopologyLoader` | 否（全 4 框） | 仅 L1+L2 | `PathService` 直接构造 | `planPathsCoverage` | JUnit 断言 + stdout 报告 |
| `service/CoverageExRackReport` | `RackTopologyLoader` | 否（全 4 框） | 4 个全调 | `PathService` 直接构造 | `planPathsCoverageEx` | `target/coverage-rack4-report.md` |
| `service/CoverageDiagramGenerator` | `RackTopologyLoader` | 否（全 4 框） | 仅 L1+L2 | `PathService` 直接构造 | `planPathsCoverage` | `coverage_diagram.html` |

---

## 8. 注意事项

1. **路径重复**：仓库里同时存在 `umdk/test/snc/...` 与 `umdk/umdk/test/snc/...` 两份 `FullRackTopologyGenerator.java`。`pom.xml:68` 的 `testSourceDirectory` 指向**前者**（`umdk/test/snc/java`），后者是过时副本且缺少 `CoverageIntraChassisTest`、`FullRackTopologyJettyIdTest`、`PlanPathsCoverageExIntegrationTest` 等新文件。维护时只改前者，建议清理后者以免误读。

2. **原生库强依赖**：`FullRackTopologyGenerator.JETTY_ID_BASE = HashUtils.JETTY_ID_MIN`（`FullRackTopologyGenerator.java:79`）造成**类加载即触发原生库加载**。若 `.so` 缺失或架构不匹配，`HashUtils.LIB` 为 `null`，跑 `FullRackTopologyJettyIdTest` 时类初始化就会失败（不一定要走到 `nativeHash`）。测试替换 `.so` 必须用 `-Djna.library.path` 指向外部目录，详见《SNC 原生库加载与测试替换方案》。

3. **裁剪副作用**：`SuperNode` 的 `npuDevices` / `swDevices` 是 `LinkedHashMap`，`removeIf` 直接改原 Map。由于 `buildRawTopology` 每次都 `new` 一个 `SuperNode`，裁剪不会污染下一次生成；但同一 `SuperNode` 实例被多个测试方法共用时（`@BeforeAll` 装配一次），测试方法间不要再次改动拓扑。

4. **augment 顺序**：`L1sw → L2sw → Npu → L1swNpu` 是约定顺序，不能调换。`augmentL1swNpuRouting` 依赖前三步建立的路由表结构。

5. **`PlanPathsCoverageIntegrationTest` 的双机框裁剪**：保留了 4 个 L2SW，专门跑老接口 `planPathsCoverage` 的框间 L1SW↔L2SW 链路覆盖；`PlanPathsCoverageExIntegrationTest` 反向操作——单机框去掉 L2SW，强制走框内 2 跳。两者是互补关系，不要合并。

6. **`FullRackTopologyJettyIdTest` 不依赖引擎**：它是夹具自检测试，不调 `CoverageRouteAugmentor`、不调 `SncService`，只验证生成器输出结构。若只想验证生成器是否正常，单独跑这个测试最快：
   ```bash
   mvn test -Dtest=FullRackTopologyJettyIdTest
   ```
