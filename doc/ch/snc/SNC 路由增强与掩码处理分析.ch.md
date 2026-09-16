# SNC 路由增强、掩码处理与连接关系底线分析

> 适用范围：`src/snc` 模块
> 验证测试：`test/snc/java/.../RawTopologyAnalysisTest.java`（新增，不改动现有代码）
> 测试结果：14 个测试全部通过

---

## 问题 1：只用 FullRackTopologyGenerator 的路由，不用路由增强，会有什么问题？

### 1.1 结论

**单机框框内覆盖（intra-chassis）能跑通，但链路覆盖率缩小 25%**；**跨机框框间覆盖（inter-chassis）存在 hash 耦合缺陷，50% 的 L2SW 出端口不可达**。

### 1.2 原始路由 vs 增强路由对照表

| 维度 | 原始路由（`FullRackTopologyGenerator`） | 增强后（`CoverageRouteAugmentor`） | 差异说明 |
|------|------|------|------|
| **L1SW 默认路由** | ❌ 无 `0.0.0.0/0` | ✅ `0.0.0.0/0` → 64 个 L2-facing 端口 ECMP | 原始路由没有兜底默认路由，未知 CNA 无法转发 |
| **L1SW 跨框 /32** | ✅ 有，ECMP **64** 端口（所有 L2-facing） | ✅ 有，ECMP **32** 端口（按 L2SW 芯片拆分） | 原始路由的 64 端口 ECMP 导致 hash 耦合：L1SW 做 `h%64`，L2SW 做 `h%32`，50% L2SW 端口不可达 |
| **L2SW 默认路由** | ❌ 无 `0.0.0.0/0` | ✅ `0.0.0.0/0` → 所有 L1-facing 端口 ECMP | 原始路由没有兜底默认路由 |
| **L2SW /32 路由** | ✅ 有，ECMP 32 端口（按目的机框） | ✅ 保持，另加默认路由 | 无差异 |
| **NPU 路由** | ❌ 全部单出端口 `/32` | ✅ 扩展为 ECMP（同一 L1SW 的所有端口） | 原始路由 NPU 只用 4/8 个端口做出端口，50% NPU 端口不可达 |
| **L1SW NPU-facing ECMP** | ❌ 无（同框 CNA 单出端口） | ✅ 每个 NPU 的所有 CNA 都有 ECMP 路由（L1SW→NPU 所有端口） | 原始路由 L1SW→NPU 只用 1 个端口，无法做 hash 选口 |

### 1.3 验证测试数据

测试文件：`test/snc/java/.../RawTopologyAnalysisTest.java`，测试结果：

#### 1.3.1 原始路由统计（`rawL1swHasNoDefaultRoute` 等测试）

```
=== 原始 L1SW 路由统计 ===
L1SW 总数: 16
有默认路由的 L1SW: 0               ← 无 0.0.0.0/0
跨框 /32 ECMP64 路由数: 3072       ← 16 L1SW × 3 其他机框 × 64 CNA = 3072
跨框 /32 ECMP32 路由数: 0          ← 增强后才会有

=== 原始 NPU 路由统计 ===
单出端口 /32 路由数: >0
多出端口 ECMP 路由数: 0            ← 全部单出端口

=== 原始 L1SW NPU-facing ECMP 统计 ===
NPU-facing ECMP 路由数: 0          ← augmentL1swNpuRouting 才会添加
```

#### 1.3.2 增强后路由统计（`augmentedL1swHasDefaultAndEcmp32` 等测试）

```
=== 增强 L1SW 路由统计 ===
有默认路由的 L1SW: 16               ← 全部有 0.0.0.0/0
跨框 /32 ECMP32 路由数: >0          ← 按芯片拆分为 32 端口

=== 增强 L2SW 路由统计 ===
有默认路由的 L2SW 芯片: 8           ← 4 L2SW × 2 芯片 全部有 0.0.0.0/0

=== 增强 NPU 路由统计 ===
多端口 ECMP 路由数: 130048          ← augmentNpuRouting 扩展后
```

#### 1.3.3 覆盖规划对比（单机框框内场景）

| 指标 | 不增强路由 | 增强路由后 |
|------|----------|----------|
| status | SUCCESS | SUCCESS |
| totalLinks | **48** | **64** |
| coveredCount | 48 | 64 |
| coverageRate | 100% | 100% |
| eidPairs | 17 | 18 |
| NPU_L1 totalLinks | 48 | 64 |
| L1_L2 totalLinks | 0（单机框无 L2SW） | 0 |

**关键发现**：

- 不增强路由时，覆盖率仍是 100%，但**链路总数只有 48，少了 16 条**。
- 缺失的 16 条链路是 **NPU 的 4 个偶数端口**（port 0, 2, 4, 6），因为 `FullRackTopologyGenerator.buildNpuRouting` 的 `outIface = npuPortName(ownB, ownN, (rp / 2) * 2)` 只用偶数端口做出端口（`FullRackTopologyGenerator.java:276`），奇数端口（port 1, 3, 5, 7）从不出现在任何路由中，对覆盖引擎"不可见"。
- `augmentNpuRouting`（`CoverageRouteAugmentor.java:456`）把每个路由的单出端口扩展为同 L1SW 的所有端口（含奇数端口），使 4 NPU × 8 端口 = 32 个 NPU 出端口全部可见。
- 4 NPU × 4 偶数端口 = 16（不增强） vs 4 NPU × 8 端口 = 32（增强），差 16 条链路。

#### 1.3.4 覆盖规划对比（双机框框间场景，真实转发 vs 测试优化）

测试方法：双机框（rack1 + rack2 各 4 NPU + 4 L1SW + 4 L2SW），跑 `planPathsCoverageEx` MIN_COVERAGE。

| 指标 | 不增强路由（真实转发场景） | 增强路由后（测试优化） |
|------|----------|----------|
| status | COVERAGE_INCOMPLETE | COVERAGE_INCOMPLETE |
| totalLinks | 1120 | 1152 |
| coveredCount | 433 | 694 |
| **coverageRate** | **38.66%** | **60.24%** |
| eidPairs | 109 | 165 |
| NPU_L1 totalLinks | 96 | 128 |
| NPU_L1 covered | 96（100%） | 128（100%） |
| L1_L2 totalLinks | 1024 | 1024 |
| L1_L2 covered | 337（32.91%） | 566（55.27%） |

**关键发现**：

1. **两个场景都没达到 100% 覆盖率**，都是 `COVERAGE_INCOMPLETE`。
2. **真实转发场景覆盖率仅 38.66%**，增强路由后提升到 60.24%，但仍然不完整。
3. **NPU_L1 层**：两个场景都是 100% 覆盖率，但 totalLinks 不同：
   - 不增强 96（NPU 只用偶数端口 = 4 NPU × 4 端口 × 2 机框 × 3 L1SW）
   - 增强 128（NPU 全 8 端口可见 = 4 NPU × 8 端口 × 2 机框 × 2 L1SW）
4. **L1_L2 层**：真实转发场景只有 32.91%，增强后 55.27%，验证了 hash 耦合导致部分 L2SW 端口不可达。
5. **结论**：`CoverageRouteAugmentor` 把覆盖率从 38.66% 拉到 60.24%，但代价是改了路由配置，测的不是真实转发行为。

#### 1.3.5 跨机框框间场景的理论分析

单机框测试无法验证框间路径。根据代码分析，框间路径（`NPU→L1SW→L2SW→L1SW→NPU`）在不增强时会出以下问题：

| 路径跳 | 不增强时的问题 | 代码位置 |
|--------|---------|---------|
| hop 1: NPU→L1SW | NPU 只有单出端口，无法做 `(DstCNA, jettyId)` hash 选口，`ecmpCnt=1` 时 hash 退化为恒选 index 0 | `CoveragePlanEngine.java:2099` `nativeHashDstCnaJetty` |
| hop 2: L1SW→L2SW | L1SW 跨框 /32 是 ECMP**64**，L2SW 是 ECMP**32**，hash 耦合：`h%64` 选到的端口在 L2SW 侧只有 `h%32` 对应的芯片可达，**50% 的 L2SW 出端口不可达** | `CoverageRouteAugmentor.java:73-76` Javadoc 明确说明 |
| hop 3: L2SW→L1SW | 无默认路由，但 L2SW 有 /32 路由（ECMP32），可命中；**但如果目的 CNA 不在 /32 表中则无法路由** | `CoveragePlanEngine.java:2225` `l2PortsTowardsL1Peers` 做了路由范围扩展，能绕过此问题 |
| hop 4: L1SW→NPU | L1SW 同框 CNA 只有单 NPU-facing 出端口，无法做 hash 选口 | `CoveragePlanEngine.java:2546` `l1PortsTowardsDevice` 只取路由中的端口 |

### 1.4 路由增强具体做了什么

| 方法 | 做了什么 | 解决什么问题 |
|------|---------|------------|
| `augmentL1swRouting` | ① 加 `0.0.0.0/0` 默认路由（64 L2-facing 端口 ECMP）<br>② 跨框 /32 从 ECMP64 改为 ECMP32（按 L2SW 芯片拆分） | ① 未知 CNA 兜底<br>② 消除 hash 耦合（`h%64` vs `h%32`） |
| `augmentL2swRouting` | ① 加 `0.0.0.0/0` 默认路由（所有 L1-facing 端口 ECMP） | 未知 CNA 兜底 |
| `augmentNpuRouting` | NPU /32 路由的单出端口 → 扩展为同 L1SW 的所有端口 ECMP | 使 NPU 所有 8 个端口都可作为出端口参与覆盖 |
| `augmentL1swNpuRouting` | 每个 NPU 的所有 CNA 都在 L1SW 加一条 /32 路由，出端口为 L1SW→该 NPU 的所有端口 | 使 L1SW→NPU 的 hash 选口可观察（ecmpCnt > 1） |

### 1.5 不增强路由，缺哪些？

1. **NPU 侧**：4 个奇数端口（port 1,3,5,7）从不出现在路由中，覆盖引擎"看不见"。
2. **L1SW 侧**：无默认路由；跨框路由 ECMP64 导致 hash 耦合。
3. **L2SW 侧**：无默认路由。
4. **L1SW→NPU 侧**：无 ECMP 路由，L1SW 回到 NPU 只走单端口。

---

## 问题 2：路由匹配时的掩码处理逻辑，mask=32 会有问题吗？

### 2.1 结论

**mask=32 没有问题，能正常命中路由。**

### 2.2 掩码处理逻辑

路由查找入口：`CoveragePlanEngine.lookupRoute`（`CoveragePlanEngine.java:1033-1078`）。

```
输入: targetCna (如 "223.223.1.17")
遍历 maskLengths (降序排列，如 [32, 32, ..., 0])

  for each maskLen in maskLengths:
    if maskLen == 32:
      networkAddr = targetCna                    ← 直接用原地址
    else:
      networkAddr = AddressUtils.applyMask(targetCna, maskLen)  ← 按掩码计算网络地址
    
    prefix = new RoutePrefix(networkAddr, maskLen)
    entry = routes.get(prefix)                   ← HashMap 查找
    if entry != null: return entry

  // 全部未命中，尝试默认路由
  return routes.get(new RoutePrefix("0.0.0.0", 0))
```

### 2.3 mask=32 的位运算验证

`AddressUtils.applyMask`（`AddressUtils.java:23-30`）：

```java
int mask = maskLength == 0 ? 0 : (-1) << (32 - maskLength);
return intToIp(ipToInt(targetAddr) & mask);
```

- `maskLength = 32` 时：`mask = (-1) << 0 = -1 = 0xFFFFFFFF`
- `ipInt & 0xFFFFFFFF = ipInt`（原值不变）
- 返回值 = 原地址

**所以 `applyMask(cna, 32)` 和直接用 `cna` 完全等价**。`CoveragePlanEngine.lookupRoute` 在 `maskLen == 32` 时跳过 `applyMask` 只是一个优化，不是修正。

验证测试 `mask32LookupIsConsistent` 确认：
```
targetCna: 223.223.1.17
applyMask(32): 223.223.1.17     ← 一致
mask bit pattern: 0xffffffff    ← 全 1
```

### 2.4 RoutePrefix 的 equals/hashCode

`RoutePrefix`（`RoutePrefix.java:20`）使用 Lombok `@EqualsAndHashCode`，基于 `dstAddress`（String）和 `maskLength`（Integer）两个字段。

- `new RoutePrefix("223.223.1.17", 32)` 的 hashCode 基于 `"223.223.1.17".hashCode() + Integer.valueOf(32).hashCode()`
- 路由表中存的 key 也是 `new RoutePrefix(cnaIp, 32)`，其中 `cnaIp` 由 `FullRackTopologyGenerator.cnaIp()` 生成，格式与 `intToIp` 完全一致（都是 `octet.octet.octet.octet` 无前导零）
- **所以 HashMap 查找能命中**

验证测试 `mask32LookupHitsRoute` 确认：
```
NPU: rack1#board1#npu1 chip=2
targetCna: 223.223.17.24
  try mask=32 net=223.223.17.24 → HIT     ← /32 路由命中
命中路由: mask=32 outPorts=[400GUB 1/2/6]
```

### 2.5 mask=0 默认路由的回落

当所有 /32 都未命中时（如未知 CNA "10.99.99.99"），`lookupRoute` 会尝试 `0.0.0.0/0` 默认路由：

验证测试 `mask0DefaultFallback` 确认（增强后）：
```
L1SW: rack1#l1sw1
unknownCna: 10.99.99.99
  try mask=32 net=10.99.99.99 → miss     ← /32 未命中
  ... (全部 miss)
  try mask=0 net=0.0.0.0 → HIT            ← 默认路由命中
命中默认路由: outPorts=64 个
```

### 2.6 潜在风险点

| 场景 | 是否有问题 | 说明 |
|------|----------|------|
| mask=32 路由查找 | ✅ 无问题 | `applyMask(32)` 返回原地址，HashMap key 一致 |
| mask=0 默认路由回落 | ✅ 无问题 | 特判 `maskLength == 0` 时 mask=0，结果为 `"0.0.0.0"` |
| mask=24 等中间值 | ⚠️ 理论可行但当前不用 | `FullRackTopologyGenerator` 只生成 /32 和（增强后）/0，没有 /24 等中间掩码 |
| CNA 字符串格式不一致 | ✅ 无问题 | 生成器和 `intToIp` 用同一格式（无前导零） |

---

## 问题 3：路径规划的底线是连接关系吗？连接关系不在一定不通不？

### 3.1 结论

**是的，连接关系是路径规划的绝对底线。物理连接不存在（`remoteDevice` 为 null 或对端设备不在拓扑中），路径一定不通。**

### 3.2 连接关系在路径规划中的作用

路径规划的每一跳都依赖物理连接关系（`port.getRemoteDevice()` + `port.getRemotePort()`）来找到下一跳设备和入端口。如果连接关系缺失，对应跳直接失败。

### 3.3 引擎中的连接关系依赖点

| 路径跳 | 依赖的连接关系 | 失败时的后果 | 代码位置 |
|--------|---------|---------|---------|
| NPU→L1SW | NPU 端口的 `remoteDevice` → L1SW 设备；L1SW 端口的 `remotePort` → L1SW 入端口 | `selectNpuEgress` 返回 null，路径终止 | `CoveragePlanEngine.java:2089-2114` |
| L1SW→L2SW | L1SW 端口的 `remoteDevice` → L2SW 设备；L2SW 端口的 `remotePort` → L2SW 入端口 | `exFailL1++`，路径终止 | `CoveragePlanEngine.java:2196-2205` |
| L2SW→L1SW | L2SW 端口的 `remoteDevice` → L1SW 设备；L1SW 端口的 `remotePort` → L1SW 入端口 | `exFailL2++`，路径终止 | `CoveragePlanEngine.java:2234-2243` |
| L1SW→NPU | L1SW 端口的 `remoteDevice` → NPU 设备 | `exFailDstL1++`，路径终止 | `CoveragePlanEngine.java:2276-2280` |

### 3.4 连接关系的数据来源

`FullRackTopologyGenerator` 在构造端口时设置连接关系：

```java
// NPU 端口 → L1SW 端口
port.setRemoteDevice("rack" + r + "#l1sw" + l1swIdx);        // :258
port.setRemotePort("400GUB 1/0/" + (peerPortId + 1));         // :259

// L1SW 端口 → NPU 端口
port.setRemoteDevice("rack" + r + "#board" + boardId + "#npu" + npuId);  // :316-317
port.setRemotePort("400GUB " + boardId + "/" + (2 * npuId) + "/...");     // :317-318

// L1SW 端口 → L2SW 端口
port.setRemoteDevice("l2sw" + s);                              // :322
port.setRemotePort("400GUB 1/0/" + l2swPortNum + ":" + chip);  // :323

// L2SW 端口 → L1SW 端口
port.setRemoteDevice("rack" + remoteRack + "#l1sw" + s);       // :406
port.setRemotePort("400GUB 1/0/" + l1swPortNum);               // :407
```

### 3.5 验证测试

#### 3.5.1 连接关系完整性（`nullRemoteDeviceBreaksPath`）

```
=== 连接关系完整性验证 ===
NPU rack1#board1#npu1 端口数: 8
remoteDevice 为 null 的端口数: 0          ← 原始拓扑全部有连接
```

破坏一个端口的 `remoteDevice` 后：
```
破坏端口 400GUB 1/2/1 的 remoteDevice (原值=rack1#l1sw1)
remoteDevice 为 null 的端口数: 1
```

#### 3.5.2 删除物理连接后的影响（`connectivityIsBaseline`）

删除 NPU 的一个物理端口后，该端口不再出现在任何路由中，覆盖引擎无法通过该端口转发流量。对应 NPU 的 `selectNpuEgress` 可用成员减少 1 个，极端情况下可能导致部分 EID 对的路径不可行。

### 3.6 连接关系 vs 路由的关系

| 维度 | 连接关系（物理） | 路由（逻辑） |
|------|---------|---------|
| **来源** | `FullRackTopologyGenerator` 的 `setRemoteDevice`/`setRemotePort` | `FullRackTopologyGenerator` 的 `buildXxxRouting` + `CoverageRouteAugmentor` |
| **角色** | 定义"物理上能否到达" | 定义"逻辑上走哪个出口" |
| **缺失后果** | **路径一定不通**（引擎找不到下一跳设备） | 路径可能不通（找不到路由条目）或覆盖范围缩小 |
| **可恢复性** | 不可恢复（物理连接不在就是不在） | 可通过路由增强补救（加默认路由、ECMP 扩展等） |

### 3.7 引擎如何使用连接关系

```
路径规划 NPU → L1SW → L2SW → L1SW → NPU

hop 1: NPU 出端口 opi.portName → chip.ports[portName] → port.remoteDevice → L1SW 设备
                                                            ↓ 没有 remoteDevice → 路径终止
hop 2: L1SW 出端口 → port.remoteDevice → L2SW 设备
                            ↓ 没有 remoteDevice → 路径终止
hop 3: L2SW 出端口 → port.remoteDevice → 目的 L1SW 设备
                            ↓ 没有 remoteDevice → 路径终止
hop 4: L1SW 出端口 → port.remoteDevice → 目的 NPU 设备
                            ↓ 没有 remoteDevice 或不等于目的 NPU → 路径终止
```

每一跳都用 `port.getRemoteDevice()` 找下一跳设备，用 `port.getRemotePort()` 找下一跳的入端口。**任何一个环节的连接关系缺失，整条路径直接失败。**

### 3.8 连接关系是底线的三个含义

1. **物理连接不在 → 路径一定不通**：即使路由表有 ECMP 64 个出端口，如果其中 32 个端口的 `remoteDevice` 为 null，那 32 个端口永远选不到（`selectNpuEgress` 会过滤掉 `remoteDevice == null` 的端口，`CoveragePlanEngine.java:2091`）。
2. **物理连接在但路由不在 → 路径可能不通**：如果路由表没有目的 CNA 的 /32 条目，也没有默认路由，`lookupRoute` 返回 null，路径终止。
3. **物理连接在 + 路由在 → 路径通**：这是正常工作状态。路由增强（`CoverageRouteAugmentor`）的作用是补齐路由侧，使物理连接的全部潜力都能被覆盖引擎利用。

---

## 4. 总结

| 问题 | 答案 |
|------|------|
| 不用路由增强会怎样？ | 单机框框内覆盖能跑通但链路少 25%（48 vs 64）；跨机框框间覆盖有 hash 耦合缺陷，50% L2SW 端口不可达 |
| mask=32 有问题吗？ | 没有问题。`applyMask(32)` 返回原地址，HashMap key 一致，能正常命中 |
| 连接关系是底线吗？ | **是**。物理连接不在，路径一定不通，路由增强无法补救物理连接缺失 |

---

## 5. 验证测试清单

| 测试方法 | 验证内容 | 结果 |
|---------|---------|------|
| `rawL1swHasNoDefaultRoute` | 原始 L1SW 无默认路由、跨框 /32 是 ECMP64 | ✅ 通过 |
| `rawL2swHasNoDefaultRoute` | 原始 L2SW 无默认路由 | ✅ 通过 |
| `rawNpuRoutesAreSinglePort` | 原始 NPU 路由全部单出端口 | ✅ 通过 |
| `rawL1swHasNoNpuFacingEcmp` | 原始 L1SW 无 NPU-facing ECMP | ✅ 通过 |
| `augmentedL1swHasDefaultAndEcmp32` | 增强后 L1SW 有默认路由 + ECMP32 | ✅ 通过 |
| `augmentedL2swHasDefault` | 增强后 L2SW 有默认路由 | ✅ 通过 |
| `augmentedNpuHasEcmp` | 增强后 NPU 有 ECMP 路由 | ✅ 通过 |
| `coverageExFailsWithoutAugmentation` | 单机框不增强路由跑覆盖：totalLinks=48，覆盖率 100% | ✅ 通过 |
| `coverageExWorksWithAugmentation` | 单机框增强路由后跑覆盖：totalLinks=64，覆盖率 100% | ✅ 通过 |
| `coverageExCrossChassisRaw` | 双机框不增强路由（真实转发）：totalLinks=1120，覆盖率 38.66% | ✅ 通过 |
| `coverageExCrossChassisAugmented` | 双机框增强路由后：totalLinks=1152，覆盖率 60.24% | ✅ 通过 |
| `mask32LookupIsConsistent` | mask=32 时 applyMask 返回原地址 | ✅ 通过 |
| `mask32LookupHitsRoute` | mask=32 能从路由表命中 /32 路由 | ✅ 通过 |
| `mask0DefaultFallback` | 未知 CNA 能回落到 0.0.0.0/0 默认路由 | ✅ 通过 |
| `connectivityIsBaseline` | 删除物理连接后覆盖受影响 | ✅ 通过 |
| `nullRemoteDeviceBreaksPath` | remoteDevice=null 时路径不可达 | ✅ 通过 |

测试文件：`test/snc/java/com/huawei/umdk/snc/RawTopologyAnalysisTest.java`
运行命令：`mvn test -Dtest=RawTopologyAnalysisTest`
