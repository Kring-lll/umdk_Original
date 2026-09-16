# SNC NPU-L1 Path-Selection Hash Coverage Planning Design Document

> This document defines the design **and the finalized implementation** of the new interface
> `planPathsCoverageEx`, which extends the existing `planPathsCoverage`. The existing interface
> only covers **L1SW↔L2SW** out-port path selection (hash); the new interface **adds NPU↔L1SW
> path-selection hash and link coverage** to the coverage link universe, so that the coverage
> ratio accounts for all out-ports along NPU→L1→L2→L1→NPU.
>
> **NPU→L1SW port selection uses the standard two-tuple `(DstCNA, jettyId)`** (one jettyId per
> physical port, range `[32, 1023]`), reusing the existing `libubswitch` native hash (dll/so).
> **The CNA of the selected NPU port becomes the SCNA** for the downstream L1SW/L2SW hash
> selections, and the **ACK direction is symmetric** (destination CNA = source CNA, jettyId of
> the destination NPU port). The tests emit a Markdown coverage report
> (`target/coverage-intra-chassis-report.md`).

---

## Table of Contents

1. [Overview](#1-overview)
2. [Current-State Analysis](#2-current-state-analysis)
3. [Requirements and Interface Definition](#3-requirements-and-interface-definition)
4. [Overall Design](#4-overall-design)
5. [Data Structure Design](#5-data-structure-design)
6. [Algorithm Design](#6-algorithm-design)
7. [Detailed Flow](#7-detailed-flow)
8. [Result Output and Statistics](#8-result-output-and-statistics)
9. [Compatibility and Impact Analysis](#9-compatibility-and-impact-analysis)
10. [Test Design](#10-test-design)
11. [File List and Implementation Steps](#11-file-list-and-implementation-steps)
12. [Open Questions](#12-open-questions)
13. [Inter-/Intra-Chassis Two-Phase Coverage and Examples](#13-inter-intra-chassis-two-phase-coverage-and-examples)

---

## 1. Overview

### 1.1 Business Background

The SNC (Supernode Network Controller) coverage-planning capability is provided by
`planPathsCoverage`: given a super-node topology (including routing tables), it selects a set of
EID pairs (src/dst NPU ports) whose **forward/reverse hash path selection** traverses the target
out-port set, and outputs the EID-pair → covered-link mapping plus coverage ratio, repeat ratio,
EID uniformity and other statistics.

### 1.2 Problem Statement

The **coverage link universe** of the current implementation (`CoveragePlanEngine`) is built by
`collectBidirectionalLinks()`, which only contains:

| Direction | Device | Filter | Note |
|:---|:---|:---|:---|
| L1SW → L2SW | L1SW | Route out-port `remoteDevice` ∈ L2SW set | All out-ports (including single-port routes) |
| L2SW → L1SW | L2SW | Non-default route (maskLength != 0) and out-port count > 1, out-port `remoteDevice` is an L1SW | ECMP routes only |

For the NPU↔L1SW segment:

1. **NPU→L1SW**: the source NPU port is **fixed** by the candidate (`NpuCandidate`), and its L1SW
   ingress port is uniquely determined by the physical connection `npuPortConns`; it **does not
   participate in hash path selection**, and the NPU uplink out-port is not part of the coverage
   link universe, so it is not counted in the coverage ratio.
2. **L1SW→NPU**: the last hop of `traceForwardPath()` / `traceReversePath()` directly takes
   `dstL1swOutPorts.get(0)` / `srcL1swRevOutPorts.get(0)` with **no hash**, and that out-port is
   also absent from the coverage link universe.

When an NPU has **multiple physical uplinks to L1SW** (ECMP), the real hardware uses a hash to
select among the NPU out-port set. Without modeling it:

- the coverage statistics are incomplete (NPU↔L1SW out-ports are missing);
- the generated EID pairs cannot guarantee that all NPU↔L1SW out-ports are covered;
- the last-hop `get(0)` disagrees with hardware hash behavior, making coverage conclusions wrong.

### 1.3 Design Goals

| ID | Goal |
|:---:|:---|
| G1 | Add the northbound interface `planPathsCoverageEx` (`SncService` → `PathService` → `CoveragePlanEngine`); **the existing `planPathsCoverage` keeps its signature and behavior unchanged** |
| G2 | **Hash-based NPU→L1SW out-port selection**: take the out-port set of the NPU routing table (LPM by destination CNA) as the ECMP member set, and select the actual out-port using the same hash tuple as L1↔L2 |
| G3 | **Hash-based L1SW→NPU out-port selection**: replace the deterministic `get(0)`; the member set is the L1SW route's out-port set towards the destination NPU |
| G4 | Add NPU→L1SW and L1SW→NPU out-ports to the **coverage link universe**, so `totalLinks` / `coveredCount` / `coverageRate` cover the whole path |
| G5 | Output **per-layer statistics** (`layerStats`) so NPU↔L1SW and L1SW↔L2SW coverage can be inspected separately |
| G6 | Reuse the existing configuration (`hashFunc` / `fixedDataUdpPort` / `fixedAckUdpPort` / `HashTuple`); **no new configuration item** |

### 1.4 Non-Goals

- Do not modify `planPathsCoverage` (its link universe, hash points, or result semantics).
- Do not modify the NPU-internal forwarding model, the intra-chassis topology model, or any
  `planPath`-related path.
- Do not introduce a new hash algorithm; `HashUtils.nativeHash` (`libubswitch`) is still used.
- Do not change the ECMP member set or selection rule of the L1SW↔L2SW segment.

### 1.5 Terminology

| Term | Meaning |
|:---|:---|
| Coverage link universe | The full set of out-ports that must be "hit" by EID-pair hash selection (`linkMap`) |
| Out-port | An egress interface on some forwarding chip of a device (`LinkInfo` / `CoveredLinkDetail`) |
| Member set (ECMP member set) | The out-port set returned by one route lookup; the hash picks one modulo `size()` |
| NPU uplink | An NPU forwarding-chip out-port facing an L1SW |
| Layer | `NPU_L1` (NPU↔L1SW), `L1_L2` (L1SW↔L2SW) |
| Hop | One device-to-device forwarding segment: NPU→L1SW, L1SW→L2SW, L2SW→L1SW, L1SW→NPU |

### 1.6 Finalized Implementation Rules (2026-09-13)

| Item | Final rule | Where it lives |
|:---|:---|:---|
| NPU→L1SW selection hash | Standard two-tuple **`(DstCNA, jettyId)`** via the new `HashUtils.nativeHashDstCnaJetty(dstCna, jettyId, ecmpCnt, hashFunc)`, reusing `libubswitch` (`ubswitch_Hash_ecmp`, dll/so) mapped as `dip←DstCNA`, `sport←jettyId` (the industry-standard entropy slot for a QP/jetty id), `sip/dport/protocol←0` | `util/HashUtils.java` |
| jettyId range | **`[32, 1023]`**, one per physical port; out-of-range values throw `IllegalArgumentException` | `HashUtils.JETTY_ID_MIN/MAX`, `isValidJettyId` |
| Topology input field | SuperNode JSON `jettyId` (`TestDataLoader`); template JSON `jetty_id` (`128_npu_rack.json`) + `PortLoader`/`SncPort`; `FullRackTopologyGenerator` uses a **fixed allocation** `JETTY_ID_BASE + portIndex` (32..39, identical on every invocation, pinned by `FullRackTopologyJettyIdTest`); when absent it falls back to `32 + portId` and is counted in `jettyIdFallback` | `entity/NpuPortEntity.jettyId`, `TestDataLoader`, `TopoTemplateService`, `FullRackTopologyGenerator.JETTY_ID_BASE` |
| NPU egress member set | Out-ports of the route entry hit by LPM on the NPU's **forwarding chip** for `DstCNA`, filtered to peers that are L1SW (`ecmpCnt = size`) | `CoveragePlanEngine.selectNpuEgress` |
| SCNA chaining | **The CNA of the selected NPU port becomes the SCNA** used by the subsequent L1SW→L2SW, L2SW→L1SW and L1SW→NPU hash selections | `traceForwardPathEx` |
| ACK direction | Symmetric: **`DstCNA = source CNA`**, **`jettyId = jettyId of the destination NPU port`**; the selected port's CNA becomes the reverse SCNA driving the reverse L1SW selections | `traceReversePathEx` |
| Coverage link universe | NPU→L1SW and L1SW→NPU (`layer = NPU_L1`) plus L1SW→L2SW and L2SW→L1SW (`layer = L1_L2`); single-out-port routes are included | `collectNpuL1Links` + existing `collectBidirectionalLinks` |
| Links per EID pair | **8** covered links (4 forward + 4 ACK); order in §8.1 | `PathService.planPathsCoverageEx` |
| Northbound interface | `SncService.planPathsCoverageEx` → `PathService.planPathsCoverageEx` → `CoveragePlanEngine.findCoverageEx`; the old interface is unchanged | see §3.1 |
| Coverage test report | The integration test `CoverageIntraChassisTest` (single-chassis fixture) emits a Markdown report (per-layer coverage, CRC8/SCNA evidence, the 2+2 intra-chassis links, diagnostics) | `target/coverage-intra-chassis-report.md` |

---

## 2. Current-State Analysis

### 2.1 Existing Call Chain

```
SncService.planPathsCoverage(CoveragePathsRequest)      // DATAREADY + null checks
  └─ PathService.planPathsCoverage(request)             // parameter validation, DTO conversion
       └─ CoveragePlanEngine.findCoverage(superNode, dataUdpSrcPort, ackUdpSrcPort,
                                           requirement)
            ├─ collectNpuCandidates(devices)            // NPU port candidates (eid + cna)
            ├─ collectL2swNames(devices)
            ├─ collectBidirectionalLinks(...)           // ★ link universe: L1→L2 / L2→L1 only
            ├─ precomputeTopology(devices, l2swNames)   // npuPortConns / l2swChipRemotePorts
            └─ findCoverageDiffPath(...)                // per (src,dst) forward/reverse tracing
                 ├─ traceForwardPath(...)               // 4 hops; hash at L1→L2, L2→L1
                 ├─ traceReversePath(...)               // 4 hops, symmetric
                 ├─ runGreedyCoverage(...)              // MIN_COVERAGE
                 └─ runGreedyCoverageDualDisjoint(...)  // REDUNDANT
```

### 2.2 Existing Hash Usage Points

All hashing is wrapped in `CoveragePlanEngine.nativePortIdx(scna, dcna, sport, dport, ecmpCnt)`:

| # | Location | Direction | scna / dcna | sport / dport | Member set |
|:---:|:---|:---|:---|:---|:---|
| H1 | `traceForwardPath` | L1SW→L2SW | `src.cna` / `dst.cna` | `dataUdpSrcPort` / `ackUdpSrcPort` | L1SW route out-ports whose remote is an L2SW |
| H2 | `traceForwardPath` | L2SW→L1SW | `src.cna` / `dst.cna` | `dataUdpSrcPort` / `ackUdpSrcPort` | Ports on the L2SW ingress port's chip leading to `dst.remoteL1sw` |
| H3 | `traceReversePath` | L1SW→L2SW (rev) | `dst.cna` / `src.cna` | `ackUdpSrcPort` / `dataUdpSrcPort` | Destination L1SW route out-ports whose remote is an L2SW |
| H4 | `traceReversePath` | L2SW→L1SW (rev) | `dst.cna` / `src.cna` | `ackUdpSrcPort` / `dataUdpSrcPort` | Ports on the reverse L2SW ingress chip leading to `src.remoteL1sw` |
| — | `traceForwardPath` | **NPU→L1SW** | none (fixed by `npuPortConns`) | none | **no hash** |
| — | `traceForwardPath` | **L1SW→NPU** | none (`get(0)`) | none | **no hash** |
| — | `traceReversePath` | **L1SW→NPU (rev)** | none (`get(0)`) | none | **no hash** |

### 2.3 Existing Link Universe and Path Artifacts

- `LinkInfo.getKey()` = `switchDevice + ":" + outPortName`; `linkMap` de-duplicates by this key
  (on conflict, the entry with the larger `totalOutPorts` wins).
- `CoveredPair` carries `forwardLinkDetails` (2 entries: L1→L2, L2→L1) and `reverseLinkDetails`
  (2 entries) — **exactly 4 covered links per EID pair**.
- `PathService.planPathsCoverage()` concatenates `forwardLinkDetails` + `reverseLinkDetails` into
  `CoveredEidPair.coveredLinks` (4 entries). `CoverageDiagramGenerator` depends on that fixed
  order (`li == 0/1/2/3` → diagram type 1/2/3/4).

### 2.4 Gap Analysis

| Dimension | Current (`planPathsCoverage`) | Target (`planPathsCoverageEx`) |
|:---|:---|:---|
| Coverage link universe | L1→L2, L2→L1 | L1→L2, L2→L1 **+ NPU→L1, L1→NPU** |
| NPU out-port selection | Fixed by the candidate (physical connection) | NPU route LPM + **hash** |
| L1SW last-hop out-port | Deterministic `get(0)` | **Hash**-based |
| Links per EID pair | 4 (2 forward + 2 reverse) | 8 (4 forward + 4 reverse) |
| Statistics | Single layer (aggregate) | Aggregate + **per-layer (NPU_L1 / L1_L2)** |
| Failure diagnostics | `fwdFail1..12` / `revFailA..H` | Plus NPU-segment failure counters |
| Northbound interface | `planPathsCoverage` | **New `planPathsCoverageEx`** (old one unchanged) |

---

## 3. Requirements and Interface Definition

### 3.1 New Interface Signatures

```java
// com.huawei.umdk.snc.SncService
/**
 * Coverage planning (extended): besides L1SW↔L2SW coverage, adds NPU↔L1SW hash path
 * selection and link coverage.
 *
 * <p>Differences from {@link #planPathsCoverage(CoveragePathsRequest)}:
 * <ul>
 *   <li>the coverage link universe additionally contains NPU→L1SW and L1SW→NPU out-ports;</li>
 *   <li>NPU uplink out-ports are selected by hash (the out-port set hit by the NPU route is
 *       the ECMP member set);</li>
 *   <li>the L1SW→NPU last-hop out-port is selected by hash (no longer the first out-port);</li>
 *   <li>the result additionally carries {@code scope} and {@code layerStats}.</li>
 * </ul>
 *
 * @param request coverage request; {@code superNodeName} is required and
 *                {@code coverageRequirement} defaults to
 *                {@link CoverageRequirement#MIN_COVERAGE} when null
 * @return result with status {@code SUCCESS} / {@code COVERAGE_INCOMPLETE} /
 *         {@code TOPO_NOT_FOUND}
 * @throws SNCStateException if the current state is not DATAREADY
 * @throws IllegalArgumentException if request is null
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

### 3.2 Layer Responsibilities

| Layer | Responsibility |
|:---|:---|
| `SncService` | State-machine check (`DATAREADY`), null check, logging, delegate to `PathService` |
| `PathService` | Request validation, engine invocation, conversion of `CoverageSearchResult` into `CoveragePathsResult` (incl. layer stats and the `coveredPairs` reverse index) |
| `CoveragePlanEngine` | Link-universe construction, NPU/L1/L2 hash selection, forward/reverse tracing, greedy pair selection, statistics |
| `CoverageRouteAugmentor` (test) | Augment multi-out-port routes for NPU and L1SW to build NPU↔L1SW ECMP scenarios |

### 3.3 Status Codes and Exceptions

| Scenario | Behavior |
|:---|:---|
| State is not `DATAREADY` | `SncService` throws `SNCStateException` |
| `request == null` | `SncService` throws `IllegalArgumentException` |
| `superNodeName` empty | Returns `PlanStatus.TOPO_NOT_FOUND` (same as the old interface; no exception) |
| SuperNode not found | Returns `PlanStatus.TOPO_NOT_FOUND` |
| Full coverage | `PlanStatus.SUCCESS` |
| Not full coverage | `PlanStatus.COVERAGE_INCOMPLETE` + `errorMessage` (with per-layer details) |

> **Note:** the new interface reuses `PlanStatus` and adds no new status code, avoiding
> northbound contract expansion; "which layer is incomplete" is expressed by `layerStats`
> and `errorMessage`.

### 3.4 Relationship with the Existing Interface

| Interface | Coverage link universe | Links per EID pair | Hash NPU↔L1? |
|:---|:---|:---:|:---:|
| `planPathsCoverage` | L1↔L2 | 4 | No |
| `planPathsCoverageEx` | L1↔L2 + NPU↔L1 | 8 | Yes |

Both interfaces coexist; `planPathsCoverageEx` changes no behavior of the old interface
(see §9.1), which facilitates canary rollout and regression comparison.

---

## 4. Overall Design

### 4.1 Change Overview by Layer

| Module | File | Type | Description |
|:---|:---|:---|:---|
| DTO | `dto/CoverageLinkScope.java` | New | Coverage scope enum `L1_L2` / `NPU_L1_L2` |
| DTO | `dto/CoverageLinkLayer.java` | New | Link layer enum `NPU_L1` / `L1_L2` |
| DTO | `dto/CoverageLayerStats.java` | New | Per-layer statistics |
| DTO | `dto/CoverageLink.java` | Extend | Add `deviceType`, `layer` |
| DTO | `dto/CoveragePathsResult.java` | Extend | Add `scope`, `layerStats` |
| Engine | `engine/CoveragePlanEngine.java` | Extend | `LinkInfo.layer`, `NpuCandidate.chipIndex`, NPU-uplink precompute, `findCoverageEx`, NPU-segment hash, last-hop hash, layer statistics |
| Service | `service/PathService.java` | Extend | Add `planPathsCoverageEx` (share conversion via a common private method) |
| Service | `SncService.java` | Extend | Add northbound method + update `History` comment |
| Test | `CoverageRouteAugmentor.java` | Extend | `augmentNpuRouting`: multiple NPU uplinks, multiple L1→NPU out-ports |
| Test | New `CoverageNpuL1MainSuccessTest` etc. | New | See §10 |

> **Unchanged:** `CoveragePathsRequest`, `CoverageStats`, `CoveredEidPair`, `CoveredEidPairRef`,
> `CoverageRequirement`, `HashTuple`, `SNCConfig`, `HashUtils`.

### 4.2 Coverage Universe Extension (`CoverageLinkScope`)

```
CoverageLinkScope
 ├─ L1_L2       // old interface: L1SW→L2SW, L2SW→L1SW
 └─ NPU_L1_L2   // new interface: NPU→L1SW, L1SW→L2SW, L2SW→L1SW, L1SW→NPU
```

- The enum value is both returned in the result (`CoveragePathsResult.scope`) and used as an
  internal branch switch, avoiding scattered `boolean` flags in the service layer.
- The engine passes `CoverageLinkScope scope` into `collectBidirectionalLinks` and
  `traceForwardPath/traceReversePath`, so the old and new paths share one greedy/statistics
  implementation.

### 4.3 Layer Statistics

```
CoveragePathsResult
 ├─ scope         : CoverageLinkScope
 ├─ layerStats    : Map<CoverageLinkLayer, CoverageLayerStats>
 └─ stats         : CoverageStats (aggregate; semantics identical to the old interface)
```

- Both the aggregate `stats` and `layerStats` are computed from the same `linkMap`, guaranteeing
  `sum(layerStats[*].totalLinks) == stats.totalLinks`.
- `fullCoverage = ∀ layer: layerStats[layer].coveredCount == layerStats[layer].totalLinks`.
- Under `NPU_L1_L2`, `totalLinks = NPU↔L1 out-ports + L1↔L2 out-ports`.

### 4.4 Design Decisions

| Decision | Chosen | Rationale / Alternatives |
|:---|:---|:---|
| Interface form | **New standalone method** `planPathsCoverageEx` | The requirement is "add a new interface"; the old interface stays unchanged, enabling regression comparison. Alternative: add a `scope` field to the request and reuse `planPathsCoverage` (changes the old interface's behavior boundary; rejected) |
| Interface naming | `planPathsCoverageEx` (extended) | Keeps the `planPathsCoverage` prefix for discoverability; `Ex` can host further layers (e.g. intra-chassis) later. Alternatives: `planPathsCoverageWithNpuL1` (more explicit), `planPathsFullCoverage`. **Renaming touches only the three signatures in §3.1 plus the file list in §11** |
| Request DTO | **Reuse `CoveragePathsRequest`** | Parameters are identical (`superNodeName` + `coverageRequirement`), avoiding a new DTO and duplicated validation; no `scope` field is added — the scope is implied by the method name |
| Result DTO | **Reuse `CoveragePathsResult`, add `scope` / `layerStats`** | Callers can reuse existing rendering logic (`eidPairs` / `coverageLinks` / `stats`); the cost is a changed `@AllArgsConstructor` arity (see §5.7) |
| Layer statistics field | New `CoverageLayerStats`; **`CoverageStats` untouched** | Confines the `@AllArgsConstructor` change to `CoveragePathsResult`, leaving `CoverageStats` and its tests intact |
| NPU uplink member set | **Out-port set of the LPM-hit NPU route entry** (filtered to remotes that are L1SW) | Matches hardware behavior ("NPU route lookup → multi-out-port ECMP"); does not rely on the single-port assumption of `npuPortConns` |
| Does the old interface reuse the new hash? | No | `planPathsCoverage` keeps the existing `get(0)` and fixed-connection semantics so historical results stay reproducible |
| Configuration | Reuse `hashFunc` / `fixedDataUdpPort` / `fixedAckUdpPort` / `HashTuple` | No new configuration; `HashTuple` controls the tuple width and both interfaces share the same engine instance configuration |

### 4.5 End-to-End Data Flow (New Interface)

```
CoveragePathsRequest(superNodeName, coverageRequirement)
        │
        ▼
SncService.planPathsCoverageEx        [DATAREADY check]
        │
        ▼
PathService.planPathsCoverageEx       [validation + fixed ports from SNCConfig]
        │
        ▼
CoveragePlanEngine.findCoverageEx(superNode, data, ack, requirement)
        │
        ├─ collectNpuCandidates(devices)              # candidates are still NPU ports (EID/CNA)
        ├─ collectL2swNames(devices)
        ├─ collectBidirectionalLinks(..., NPU_L1_L2)   # ★ universe now contains NPU↔L1
        ├─ precomputeTopology(devices, l2swNames)      # NPU uplink view / L2 remote-port view
        └─ findCoverageDiffPath(..., NPU_L1_L2)
              ├─ traceForwardPath  → 4 CoveredLinkDetail (NPU→L1, L1→L2, L2→L1, L1→NPU)
              ├─ traceReversePath  → 4 CoveredLinkDetail (symmetric)
              └─ runGreedyCoverage / runGreedyCoverageDualDisjoint   # reused, layer-agnostic
        │
        ▼
CoverageSearchResult (with layerTotalLinks/layerCoveredCount/fullCoverage)
        │
        ▼
PathService builds CoveragePathsResult(scope, eidPairs, coverageLinks, stats, layerStats)
```

---

## 5. Data Structure Design

> As in §4.1, all new DTOs use Lombok annotations (`@Getter`, `@Setter`, `@NoArgsConstructor`,
> `@AllArgsConstructor`, `@EqualsAndHashCode`, `@ToString`), consistent with the existing DTO style.

### 5.1 Enum `CoverageLinkScope`

```java
package com.huawei.umdk.snc.dto;

/** Coverage link scope. */
public enum CoverageLinkScope {
    /** L1SW↔L2SW out-ports only (the scope of the old interface planPathsCoverage). */
    L1_L2,

    /** NPU↔L1SW + L1SW↔L2SW out-ports (the scope of the new interface planPathsCoverageEx). */
    NPU_L1_L2
}
```

### 5.2 Enum `CoverageLinkLayer`

```java
package com.huawei.umdk.snc.dto;

/**
 * Link layer, used to group out-ports into NPU↔L1SW or L1SW↔L2SW.
 *
 * <p>Direction is not expressed here; it is derived from the device types at both ends:
 * the owner is an NPU ⇒ NPU→L1SW; the owner is an L1SW and the peer is an NPU ⇒ L1SW→NPU.
 */
public enum CoverageLinkLayer {
    /** NPU↔L1SW (NPU uplink out-port / L1SW out-port facing an NPU). */
    NPU_L1,

    /** L1SW↔L2SW. */
    L1_L2
}
```

### 5.3 Per-Layer Statistics `CoverageLayerStats`

```java
package com.huawei.umdk.snc.dto;

/** Coverage statistics of one layer ({@link CoverageLinkLayer}); fields mirror {@link CoverageStats}. */
public class CoverageLayerStats {

    private Integer totalLinks;      // total out-ports in this layer
    private Integer coveredCount;    // covered out-ports in this layer
    private Double coverageRate;     // coveredCount / totalLinks
    private Integer minRepeatCount;  // min cover count over covered links in this layer
    private Integer maxRepeatCount;  // max cover count over covered links in this layer
    private Double avgRepeatCount;   // average cover count over covered links in this layer
    private Double repeatRate;       // (ΣcoverCount - coveredCount) / totalLinks
}
```

**Field semantics and formulas** (identical to `CoverageStats`, to stay comparable):

| Field | Formula |
|:---|:---|
| `totalLinks` | Number of links of this layer in `linkMap` |
| `coveredCount` | Number of links of this layer with `coverCount > 0` |
| `coverageRate` | `totalLinks > 0 ? coveredCount / totalLinks : 0` |
| `minRepeatCount` | Minimum `coverCount` among covered links; 0 when none is covered |
| `maxRepeatCount` | Maximum `coverCount` among covered links; 0 when none is covered |
| `avgRepeatCount` | `ΣcoverCount / coveredCount` over covered links; 0 when `coveredCount == 0` |
| `repeatRate` | `(ΣcoverCount - coveredCount) / totalLinks` |

### 5.4 `CoverageLink` Extension

```java
public class CoverageLink {

    /** Owner device of the out-port. **Extended semantics:** may be a SW (L1SW/L2SW) or an NPU. */
    private String switchDevice;

    private Integer chipIndex;
    private String outPort;
    private String remoteSwitch;
    private String remotePort;
    private Integer outPortIndex;
    private Integer totalOutPorts;
    private List<CoveredEidPairRef> coveredPairs;
    private Integer coverCount;

    /** NEW: owner device type, "NPU" / "SW" (converted from DeviceType.name(); dto must not depend on entity). */
    private String deviceType;

    /** NEW: link layer (NPU_L1 / L1_L2); the old interface leaves it null. */
    private CoverageLinkLayer layer;
}
```

> **Architecture constraint:** the `dto` package must not depend on the `entity` package (see
> *SNC Framework Architecture Design* §3.3). `deviceType` is a `String` rather than `DeviceType`,
> converted by the service layer from `DeviceType.name()`, matching the existing
> `HopInfo.deviceType` convention.
>
> **Compatibility note:** the field name `switchDevice` is kept (to avoid breaking the JSON
> contract and existing parsers); its meaning is relaxed from "switch device name" to
> "owner device name of the out-port".

### 5.5 `CoveragePathsResult` Extension

```java
public class CoveragePathsResult {

    private PlanStatus status;
    private String errorMessage;
    private List<CoveredEidPair> eidPairs;
    private List<CoverageLink> coverageLinks;
    private CoverageStats stats;

    /** NEW: the coverage scope used by this call; always L1_L2 for the old interface. */
    private CoverageLinkScope scope;

    /** NEW: per-layer statistics (NPU_L1 / L1_L2); set by the new interface only, null for the old one. */
    private Map<CoverageLinkLayer, CoverageLayerStats> layerStats;
}
```

**Field semantics:**

| Field | Description |
|:---|:---|
| `status` | `SUCCESS` / `COVERAGE_INCOMPLETE` / `TOPO_NOT_FOUND` |
| `errorMessage` | On incomplete coverage, carries per-layer details, e.g.:<br>`物理遍历覆盖率未达100%: 96.88% (2016/2080 出端口已覆盖) [NPU_L1: 98.4% 1008/1024; L1_L2: 95.3% 1008/1056]` |
| `eidPairs` | Each EID pair's `coveredLinks` has 8 entries (4 forward + 4 reverse; order in §8.1) |
| `coverageLinks` | All out-ports with `coverCount` / `coveredPairs` / `layer` / `deviceType` |
| `stats` | Aggregate statistics (unchanged formula) |
| `scope` | `NPU_L1_L2` |
| `layerStats` | One `CoverageLayerStats` per layer |

### 5.6 Engine-Internal Structure Extensions

#### 5.6.1 `LinkInfo` gains a layer

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

    /** NEW: link layer. */
    public final CoverageLinkLayer layer;

    /** Existing constructors are kept and delegate to the new one. */
    public LinkInfo(String switchDevice, Integer chipIndex, String outPortName,
                    String remoteSwitch, String remotePort,
                    int outPortIndex, int totalOutPorts) {
        this(switchDevice, chipIndex, outPortName, remoteSwitch, remotePort,
             outPortIndex, totalOutPorts, null, CoverageLinkLayer.L1_L2);
    }

    /** NEW overload: with dstChassis and layer. */
    public LinkInfo(String switchDevice, Integer chipIndex, String outPortName,
                    String remoteSwitch, String remotePort,
                    int outPortIndex, int totalOutPorts,
                    String dstChassis, CoverageLinkLayer layer) {
        // ... field assignment
    }

    /** Key unchanged: deviceName + ":" + outPortName. */
    public String getKey() {
        return switchDevice + ":" + outPortName;
    }
}
```

> `getKey()` is unchanged, so NPU links and SW links never collide (device names are globally
> unique). The `linkMap` `merge` de-duplication rule is unchanged.

#### 5.6.2 `CoveredLinkDetail` gains a layer

Same as `LinkInfo`: add a `CoverageLinkLayer layer` field (new constructor; old constructor kept)
so the service layer can copy it directly into `CoverageLink.layer`.

#### 5.6.3 NPU uplink connection precompute

The existing `NpuPortConn` (NPU port → unique L1SW/port/chip/chassis) is kept unchanged (still
used by the old interface and by candidate resolution). A **device-level** uplink view is added
for the hash member set:

```java
static class NpuUplink {
    String l1swName;      // peer L1SW device name
    String l1swPortName;  // peer L1SW port name (remotePort of the NPU out-port)
    int l1swChipIdx;      // chip of the peer L1SW port
    String l1swChassis;   // peer L1SW chassis
    int npuChipIdx;       // NPU-side forwarding chip index
}

static class PrecomputedTopo {
    Map<String, NpuPortConn> npuPortConns = new HashMap<>();          // existing: npuDevice:port -> conn
    Map<String, List<String>> npuL1FacingPorts = new HashMap<>();     // NEW: npuDevice -> NPU uplink port names
    Map<String, NpuUplink> npuUplinkByPort = new HashMap<>();         // NEW: npuDevice:port -> NpuUplink
    Map<String, List<String>> l1NpuFacingPorts = new HashMap<>();     // NEW: l1swDevice -> NPU-facing port names
    // existing L2 maps unchanged
    Map<String, Map<Integer, Map<String, List<String>>>> l2swChipRemotePorts = new HashMap<>();
    Map<String, DeviceEntity> devices;
    Set<String> l2swNames;
}
```

- `npuL1FacingPorts` / `npuUplinkByPort`: built by scanning NPU device ports (`NpuPortEntity`
  whose `remoteDevice` is an L1SW and whose peer port is resolvable).
- `l1NpuFacingPorts`: built by scanning L1SW device ports whose `remoteDevice` is an NPU.
- These maps are only used for candidate/default ordering; the set that actually participates in
  hashing comes from the **LPM-hit route entry** (§6.2), intersected by port name.

#### 5.6.4 `CoverageSearchResult` gains layer statistics

```java
public static class CoverageSearchResult {
    // ... existing fields unchanged
    /** NEW: total links per layer (layer -> count). */
    public final Map<CoverageLinkLayer, Integer> layerTotalLinks;
    /** NEW: covered count per layer (layer -> count). */
    public final Map<CoverageLinkLayer, Integer> layerCoveredCount;
}
```

> `CoverageSearchResult` is engine-internal with a package-visible constructor, so adding fields
> does not affect the northbound contract.

#### 5.6.5 Chip resolution and jettyId lookup (`NpuCandidate` is left unchanged)

The implementation deliberately **does not modify** `NpuCandidate`: the NPU chip owning a
candidate port is located by the new private helper
`CoveragePlanEngine.chipIndexOfPort(dev, portName)` (scan `forwardingChips`, match the port name,
return its `chipIndex`), and the candidate port itself is obtained via the existing
`findPortInDevice`.

`jettyId` is read from `NpuPortEntity.getJettyId()`; when it is missing or out of range the code
falls back to `HashUtils.JETTY_ID_MIN + portId` and increments the `exJettyFallback` diagnostic,
so topologies without the field still work and the gap is visible in the coverage report.

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
    exJettyFallback++;                     // count the missing topology input
    Integer id = port == null ? null : port.getId();
    return HashUtils.JETTY_ID_MIN + (id == null ? 0 : id);   // 32 + portId
}
```

### 5.7 DTO Compatibility (`@AllArgsConstructor` Impact)

Existing DTOs use Lombok `@AllArgsConstructor`; adding fields **changes the all-args constructor
signature** (compile error) at the following places (the fix is mechanical: append the new
arguments):

| DTO | Current all-args arity | Affected location |
|:---|:---|:---|
| `CoveragePathsResult` | 5 | `test/.../dto/CoveragePathsResultTest.java` (`allArgsConstructor`) |
| `CoverageLink` | 9 | `test/.../dto/CoverageLinkTest.java` (`allArgsConstructor`, `equalsHashCodeToString`) |
| `CoverageStats` | 20 | **unchanged** (not modified here) |
| `CoveragePathsRequest` | 2 | **unchanged** (not modified here) |
| `CoveredEidPair` / `CoveredEidPairRef` | 9 / 2 | **unchanged** |

> **Zero-compile-impact alternative:** to avoid touching existing tests at all, a wrapper result
> could be introduced: `CoveragePathsExResult { CoveragePathsResult coverage; CoverageLinkScope scope;
> Map<CoverageLinkLayer, CoverageLayerStats> layerStats; }`. However this adds a nesting level to
> the northbound contract and splits the result type between the two interfaces. This design
> **recommends extending `CoveragePathsResult` directly** (lowest caller migration cost; only two
> test files change).

---

## 6. Algorithm Design

### 6.1 Coverage Link Universe Construction (`collectBidirectionalLinks` extension)

A new `CoverageLinkScope scope` parameter is added. Under `L1_L2` the behavior is exactly as
today; under `NPU_L1_L2` two additional segments are appended:

```java
private void collectBidirectionalLinks(SuperNode superNode,
                                       Map<String, DeviceEntity> devices,
                                       Set<String> l2swNames,
                                       CoverageLinkScope scope,
                                       List<LinkInfo> links) {
    // (1) [existing] L1SW out-ports, remote ∈ L2SW  → layer = L1_L2
    // (2) [existing] L2SW out-ports, non-default route with > 1 out-ports, remote is L1SW → L1_L2

    if (scope == CoverageLinkScope.NPU_L1_L2) {
        // (3) [NEW] NPU out-ports, remote is L1SW → layer = NPU_L1
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

        // (4) [NEW] L1SW out-ports, remote is NPU → layer = NPU_L1
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

    // Existing linkMap de-duplication is unchanged:
    // linkMap.merge(link.getKey(), link,
    //               (a, b) -> a.totalOutPorts >= b.totalOutPorts ? a : b);
}
```

**Inclusion criteria:**

| Segment | Condition | Require outPorts > 1 |
|:---|:---|:---:|
| L1SW→L2SW (existing) | remote ∈ L2SW | No |
| L2SW→L1SW (existing) | non-default route AND outPorts > 1 AND remote is L1SW | Yes |
| NPU→L1SW (new) | remote is L1SW | No |
| L1SW→NPU (new) | remote is NPU | No |

A single-out-port route (`totalOutPorts == 1`) has no ECMP, but that out-port is a
**mandatory link** and belongs to the coverage denominator, so it must be included.
L2SW→L1SW keeps the existing "ECMP routes only" criterion so that old results stay reproducible.

### 6.2 NPU→L1SW Path-Selection Hash (new H5 / H7a)

**Member-set definition:** among the out-ports of the route entry hit by LPM (by destination CNA)
on the source NPU's **forwarding chip** (the chip owning the candidate port), the subset whose
`remoteDevice` is an L1SW.

```java
/** Forward: source NPU → source L1SW (new H5). */
private List<String> npuUplinkMembers(SuperNode superNode, NpuCandidate src,
                                      String dstCna, PrecomputedTopo topo) {
    RoutingEntry entry = lookupRoute(superNode, src.deviceName, src.chipIndex, dstCna);
    if (entry == null || entry.getOutPortInfos() == null) return null;   // route missing
    DeviceEntity npu = topo.devices.get(src.deviceName);
    ForwardingChip chip = npu.getForwardingChips().get(src.chipIndex);
    List<String> members = new ArrayList<>();
    for (OutPortInfo opi : entry.getOutPortInfos().values()) {
        PortEntity p = chip.getPorts().get(opi.getPortName());
        if (p == null || p.getRemoteDevice() == null) continue;
        if (isL1Sw(topo.devices.get(p.getRemoteDevice()))) members.add(p.getPortName());
    }
    return members;                                                       // empty => no uplink
}
```

Selection (implementation: `CoveragePlanEngine.selectNpuEgress`):

```
chipIdx  = chipIndexOfPort(srcNpu, src.portName)
srcPort  = srcNpu.getForwardingChips().get(chipIdx).getPorts().get(src.portName)
jettyId  = jettyIdOf(srcPort)                     // NpuPortEntity.jettyId; falls back to 32+portId
M        = npuUplinkMembers(srcNpu, chipIdx, dst.cna)   // L1SW-facing out-ports of the LPM hit
if M.isEmpty()  → fail NPU_ROUTE_NOT_FOUND / NPU_UPLINK_NOT_FOUND
idx      = HashUtils.nativeHashDstCnaJetty(dst.cna, jettyId, M.size(), hashFunc)   // ★ H5
outPort  = M.get(idx)
scna     = port(srcNpu, chipIdx, outPort).getCna()      // ★ requirement 3: selected CNA -> SCNA
l1Dev    = port(...).getRemoteDevice()                  // source L1SW
l1InPort = port(...).getRemotePort()                    // source L1SW in-port
l1Chip   = findPortInDevice(l1Dev, l1InPort).getChipIndex()
detail#1 = CoveredLinkDetail(src.deviceName, chipIdx, outPort, l1Dev, l1InPort,
                             idx, M.size(), null,
                             CoverageLinkLayer.NPU_L1, DeviceType.NPU)
```

> **Key difference from the current implementation:** the old code uses
> `npuPortConns.get(src.deviceName + ":" + src.portName)` to **physically bind** the source L1SW
> via the candidate port; the new code derives the NPU out-port and its peer L1SW from
> **route LPM + hash**, matching hardware behavior ("NPU route lookup → multi-out-port ECMP").
> The candidate port (`src.portName` / `src.cna` / `src.eid`) still carries the **flow's source
> endpoint identity** used to build EID pairs and hash inputs.

**Degradation:** when `M.size() == 1`, `floorMod(hash, 1) == 0`, so the selection is
deterministic — equivalent to the single-uplink case.

### 6.3 L1SW→NPU Path-Selection Hash (new H6 / H7d)

**Member-set definition:** among the out-ports of the route entry hit by LPM (by destination CNA)
on the destination L1SW's ingress-port chip, the subset with
`remoteDevice == dst.deviceName`.

```
entry = lookupRoute(superNode, dstL1swName, dstL1swChipIdx, dst.cna)
if entry == null   → fail NPU_DST_ROUTE_NOT_FOUND
M = [opi.portName | opi ∈ entry.outPortInfos.values(),
       port(dstL1sw, dstL1swChipIdx, opi.portName).getRemoteDevice() == dst.deviceName]
if M.isEmpty()     → fail NPU_DST_PORT_NOT_FOUND
idx = nativePortIdx(scna, dst.cna, dataUdpSrcPort, ackUdpSrcPort, M.size())   // ★ H6 (scna = selected NPU port CNA)
outPort = M.get(idx)                                                        // replaces the existing get(0)
covered = LinkInfo(dstL1swName, dstL1swChipIdx, outPort, dst.deviceName,
                   port(dstL1sw, dstL1swChipIdx, outPort).getRemotePort(),
                   idx, M.size(), null, CoverageLinkLayer.NPU_L1)
```

> **Replacing the existing `dstL1swOutPorts.get(0)`:** the old code takes the first out-port, so
> the remaining out-ports can never be covered and the behavior disagrees with hardware hashing.
> The new code selects by hash so that every out-port in `M` can be hit by some EID pair.

### 6.4 Reverse-Path Symmetry (H7 / H8)

The reverse trace (destination NPU → … → source NPU) is strictly symmetric with the forward one:

| Reverse hop | Device | Member-set source | Hash input (scna, dcna, sport, dport) | Layer |
|:---|:---|:---|:---|:---|
| H7a | **Destination NPU → destination L1SW** | Destination NPU route LPM(`src.cna`), L1SW-facing out-ports | **`(DstCNA=src.cna, jettyId=jettyId of the destination NPU port)`** (`nativeHashDstCnaJetty`) | `NPU_L1` |
| H7b | Destination L1SW → L2SW | existing H3 member set, SCNA = **CNA of the H7a selected port** (`scnaRev`) | `nativePortIdx(scnaRev, src.cna, ack, data)` | `L1_L2` |
| H7c | L2SW → source L1SW | existing H4 member set, SCNA as H7b | same as H7b | `L1_L2` |
| H7d | **Source L1SW → source NPU** | Source L1SW route LPM(`src.cna`), NPU-facing out-ports | same as H7b (`scnaRev` driven; replaces the existing `get(0)`) | `NPU_L1` |

**Symmetry invariant:** the devices and out-port sets traversed by the forward and reverse paths of
the same EID pair correspond one-to-one; the forward/reverse hash inputs differ only by
**swapping SCNA/DCNA and sport/dport**, so the two directions select independently (consistent with
the existing design §9.5).

### 6.5 Coverage Decision and `fullCoverage`

The greedy algorithms (`runGreedyCoverage` / `runGreedyCoverageDualDisjoint` /
`trimRedundantPairs` / `rebalanceEidUsage`) are **fully reused and layer-agnostic** — once
`CoveredPair.forwardCoveredKeys` / `reverseCoveredKeys` are extended to 4 keys each, the greedy
loop naturally includes NPU links in `uncovered`.

```java
// in buildResult:
boolean fullCoverage = true;
for (Map.Entry<CoverageLinkLayer, Integer> e : layerTotalLinks.entrySet()) {
    if (layerCoveredCount.getOrDefault(e.getKey(), 0) < e.getValue()) {
        fullCoverage = false;
        break;
    }
}
```

- **MIN_COVERAGE:** every link in every layer has `coverCount ≥ 1`.
- **REDUNDANT:** every link in every layer has `coverCount ≥ 2` (the existing `min = 2` decision in
  `runGreedyCoverageDualDisjoint` becomes per-layer).

### 6.6 Failure Branches and Diagnostics

In addition to the existing `fwdFail1..12` / `revFailA..H`, new counters are added (incremented
only under `NPU_L1_L2`; printing follows the existing `logFwdFail()` policy: first occurrence plus
every 500 failures):

| Counter | Trigger |
|:---|:---|
| `npuFwdFail1` | Source NPU route LPM miss (`NPU_ROUTE_NOT_FOUND`) |
| `npuFwdFail2` | No L1SW-facing out-port in the source NPU route (`NPU_UPLINK_NOT_FOUND`) |
| `npuFwdFail3` | Peer L1SW port of the selected NPU out-port cannot be resolved |
| `npuFwdFail4` | Destination L1SW route LPM miss (`NPU_DST_ROUTE_NOT_FOUND`) |
| `npuFwdFail5` | No out-port pointing to `dst.deviceName` in the destination L1SW route (`NPU_DST_PORT_NOT_FOUND`) |
| `npuRevFail1..5` | Corresponding symmetric reverse failures |

> These failures only mean "this EID pair is unusable" (same semantics as the existing `fwdFail*`:
> `continue` to the next pair) and do not change the interface status; if no EID pair is usable at
> all, `linkMap` cannot be covered and `COVERAGE_INCOMPLETE` is returned.

### 6.7 Key Invariants

| ID | Invariant |
|:---:|:---|
| I1 | NPU↔L1 member sets come only from **LPM-hit route entries**, not from blanket physical-connection enumeration (consistent with hardware forwarding) |
| I2 | When the member-set `size == 1`, hashing degrades to deterministic selection, matching the old interface for the same scenario |
| I3 | The `linkMap` key remains `deviceName:outPortName`, so there is no cross-layer collision; de-duplication still keeps the larger `totalOutPorts` |
| I4 | Under `L1_L2`, the link universe, hash points, links per `CoveredPair`, and statistics are bit-for-bit identical to the old implementation |
| I5 | `sum(layerStats[*].totalLinks) == stats.totalLinks` and `sum(layerStats[*].coveredCount) == stats.coveredCount` |
| I6 | Each EID pair has the same number of forward and reverse covered links (4 each), and the device-level paths mirror each other |
| I7 | Greedy/statistics logic contains no layer branches; layering appears only in `collectBidirectionalLinks` (universe) and `buildResult` (statistics) |

---

## 7. Detailed Flow

### 7.1 Service-Layer Flow

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
// PathService.planPathsCoverageEx -- shares validation/conversion with the existing method
public CoveragePathsResult planPathsCoverageEx(CoveragePathsRequest request) {
    return planPathsCoverageInternal(request, CoverageLinkScope.NPU_L1_L2);
}

public CoveragePathsResult planPathsCoverage(CoveragePathsRequest request) {
    return planPathsCoverageInternal(request, CoverageLinkScope.L1_L2);   // behavior unchanged
}
```

`planPathsCoverageInternal(request, scope)` is extracted from the current
`planPathsCoverage` body and branches on `scope` in only these places:

| Location | Branched behavior |
|:---|:---|
| Engine invocation | `L1_L2` → `coveragePlanEngine.findCoverage(...)`; `NPU_L1_L2` → `findCoverageEx(...)` |
| Result filling | Set `result.setScope(scope)`; under `NPU_L1_L2` also call `buildLayerStats(searchResult)` to set `layerStats` |
| `errorMessage` | Under `NPU_L1_L2`, append per-layer details (§5.5) |
| `CoverageLink` conversion | Fill `deviceType` (mapped from `link.deviceType`: NPU/SW) and `layer` |

Everything else (`eidPairs` construction, `coverageLinks` construction, `coveredPairs` reverse
index, `stats` filling) is fully shared, avoiding duplicate implementations.

### 7.2 Engine Flow `findCoverageEx`

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
                              CoverageLinkScope.NPU_L1_L2, links);      // ★ universe extended
    if (links.isEmpty()) return emptyResult();

    Map<String, LinkInfo> linkMap = new HashMap<>();
    for (LinkInfo link : links) {
        linkMap.merge(link.getKey(), link,
                      (a, b) -> a.totalOutPorts >= b.totalOutPorts ? a : b);
    }
    int totalLinks = linkMap.size();

    PrecomputedTopo topo = precomputeTopology(devices, l2swNames);      // ★ incl. NPU uplink view

    boolean dual = (requirement == CoverageRequirement.REDUNDANT);
    return findCoverageDiffPath(superNode, linkMap, npuCandidates, totalLinks,
                                fixedDataUdpSrcPort, fixedAckUdpSrcPort, topo,
                                CoverageLinkScope.NPU_L1_L2, dual);     // ★ scope passthrough
}
```

> The only difference between `findCoverageEx` and `findCoverage` is `scope`; all other steps are
> identical. To reduce duplication, the body of `findCoverage(...)` can first be refactored into
> `findCoverageInternal(..., scope)` (see §11 implementation steps).

### 7.3 Forward Path Tracing (8 Steps)

`traceForwardPath(superNode, src, dst, dataUdpSrcPort, ackUdpSrcPort, topo, scope)` returns a
`PathTrace` whose `forwardLinkDetails` grows from 2 to 4 entries.

| Step | Operation | Artifact / failure branch |
|:---:|:---|:---|
| 1 | Source NPU route LPM (`src.chipIndex`, destination `dst.cna`) → filter L1SW-facing out-ports = `M_npu` | `npuFwdFail1/2` |
| 2 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_npu.size())` → `npuOutPort`; resolve peer L1SW port/chip | `npuFwdFail3`; **covered link (1) `NPU_L1`** |
| 3 | Source L1SW route LPM (chip from Step 2, `dst.cna`) → filter L2SW-facing out-ports = `M_l1l2` | existing `fwdFail2` |
| 4 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l1l2.size())` → `l1OutPort`; resolve L2SW ingress port/chip | existing `fwdFail3a/3b`; **covered link (2) `L1_L2`** |
| 5 | L2SW route towards `dst.remoteL1sw` → `M_l2l1` (existing `l2swChipRemotePorts`) | existing `fwdFail8` |
| 6 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l2l1.size())` → `l2OutPort`; resolve destination L1SW ingress port/chip | existing `fwdFail9/10/11`; **covered link (3) `L1_L2`** |
| 7 | Destination L1SW route LPM (chip, `dst.cna`) → filter out-ports with `remoteDevice == dst.deviceName` = `M_l1npu` | `npuFwdFail4/5` |
| 8 | `idx = nativePortIdx(src.cna, dst.cna, data, ack, M_l1npu.size())` → `dstOutPort`; verify peer is `dst.deviceName` | existing `fwdFail3c`; **covered link (4) `NPU_L1`** |

> Steps 2 and 7 are new; steps 3–6 are the existing logic, except that the L1SW/chip obtained in
> Step 2 replaces the L1SW/chip previously supplied by `npuPortConns` (when
> `scope == NPU_L1_L2`).

### 7.4 Reverse Path Tracing (8 Steps, Symmetric)

`traceReversePath(superNode, src, dst, ackUdpSrcPort, dataUdpSrcPort, fwdTrace, topo, scope)`
likewise extends `reverseLinkDetails` to 4 entries:

| Step | Operation | Area |
|:---:|:---|:---|
| R1 | Destination NPU route LPM (destination chip, `src.cna`) → filter L1SW-facing out-ports; hash(`dst.cna, src.cna, ack, data`) | **new**, covered link (1) `NPU_L1` |
| R2 | Destination L1SW → L2SW (existing H3) | existing `revFailA/B/C` |
| R3 | L2SW → source L1SW (existing H4) | existing `revFailD/E` |
| R4 | Source L1SW route LPM (`src.cna`) → filter out-ports with `remoteDevice == src.deviceName`; hash(`dst.cna, src.cna, ack, data`) | **replaces `get(0)`**, covered link (4) `NPU_L1` |

**Symmetric combination of forward/reverse covered links:**

```
Forward: (1) srcNPU→srcL1   (2) srcL1→L2   (3) L2→dstL1   (4) dstL1→dstNPU
Reverse: (1) dstNPU→dstL1   (2) dstL1→L2   (3) L2→srcL1   (4) srcL1→srcNPU
```

### 7.5 Greedy Selection (Reused, Unchanged)

In `findCoverageDiffPath`, for each `(src, dst)` combination:

```java
PathTrace fwd = traceForwardPath(superNode, src, dst, data, ack, topo, scope);
if (!fwd.viable) continue;
ReversePathTrace rev = traceReversePath(superNode, src, dst, ack, data, fwd, topo, scope);
if (!rev.viable) continue;

List<String> fwdKeys = fwd.forwardLinkDetails.stream()
                           .map(CoveredLinkDetail::getLinkKey).collect(toList());   // 4 entries
List<String> revKeys = rev.reverseLinkDetails.stream()
                           .map(CoveredLinkDetail::getLinkKey).collect(toList());   // 4 entries
allPairs.add(new PairCoverage(src, dst, fwdKeys, revKeys,
    fwd.forwardLinkDetails, rev.reverseLinkDetails, fwd.srcChassis, fwd.dstChassis));
```

The subsequent `runGreedyCoverage` (MIN) / `runGreedyCoverageDualDisjoint` (REDUNDANT) and
`trimRedundantPairs` / `rebalanceEidUsage` are fully reused; only the `fullCoverage` decision
becomes per-layer (§6.5).

### 7.6 Result Construction

1. `buildResult(...)` computes `allLinkInfos`, per-link `coverCount`, aggregate metrics, and
   **per-layer metrics** (group by `LinkInfo.layer`, then apply the same formulas).
2. `CoverageSearchResult` returns `layerTotalLinks` / `layerCoveredCount`.
3. `PathService`:
   - `eidPairs[*].coveredLinks` = 4 forward + 4 reverse (order per §8.1);
   - `coverageLinks[*].layer` / `deviceType` filled;
   - `result.layerStats` = per-layer `CoverageLayerStats`;
   - `result.scope = NPU_L1_L2`;
   - `errorMessage` (when not fully covered) appended with per-layer details.

---

## 8. Result Output and Statistics

### 8.1 `coveredLinks` Order Convention

Under `NPU_L1_L2`, `CoveredEidPair.coveredLinks` has exactly **8 entries** in the following order
(identical to the concatenation order of `forwardLinkDetails` + `reverseLinkDetails`):

| Index | Direction | Device (out-port owner) | Peer | Layer |
|:---:|:---|:---|:---|:---|
| 0 | Forward | source NPU | source L1SW | `NPU_L1` |
| 1 | Forward | source L1SW | L2SW | `L1_L2` |
| 2 | Forward | L2SW | destination L1SW | `L1_L2` |
| 3 | Forward | destination L1SW | destination NPU | `NPU_L1` |
| 4 | Reverse | destination NPU | destination L1SW | `NPU_L1` |
| 5 | Reverse | destination L1SW | L2SW | `L1_L2` |
| 6 | Reverse | L2SW | source L1SW | `L1_L2` |
| 7 | Reverse | source L1SW | source NPU | `NPU_L1` |

> Under `L1_L2` there are still **4 entries** (indices 0..3 = 2 forward + 2 reverse) with
> **unchanged order and semantics**.
>
> **Rendering adaptation:** `CoverageDiagramGenerator` currently hardcodes
> `li == 0/1/2/3 → type 1/2/3/4`; it must instead classify by `CoverageLink.layer` plus the
> owner/peer device types so that both interfaces are supported (see §9.2).

### 8.2 Filling the New `CoverageLink` Fields

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
    cl.setLayer(li.layer);                       // NEW
    cl.setDeviceType(li.deviceType.name());      // NEW: NPU / SW
    return cl;
}
```

`LinkInfo` also gains a `DeviceType deviceType` field (`LinkInfo` lives in the `engine` package,
so it may reference `DeviceType` from the `entity` package directly).

### 8.3 Statistics and Layers

```java
// Aggregate (existing formula): covers all layers
stats.totalLinks    = Σ linkMap.size()
stats.coveredCount  = Σ link.coverCount > 0 ? 1 : 0
stats.coverageRate  = coveredCount / totalLinks
// ... the remaining repeat-rate / EID-uniformity fields are unchanged

// Per-layer (new):
layerStats[NPU_L1].totalLinks   = out-ports of this layer
layerStats[NPU_L1].coveredCount = covered out-ports of this layer
layerStats[L1_L2].totalLinks    = out-ports of this layer
layerStats[L1_L2].coveredCount  = covered out-ports of this layer
```

Typical relations under `NPU_L1_L2`:

```
totalLinks(NPU_L1) ≈ deduplicated NPU uplink out-ports + deduplicated L1SW NPU-facing out-ports
totalLinks(L1_L2)  = totalLinks of the existing planPathsCoverage (equal when the topology is unchanged)
```

### 8.4 Output Example

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

> The `errorMessage` text is currently hard-coded in Chinese inside `PathService`; the new
> interface reuses that prefix and appends the per-layer segment. If the project localizes
> messages later, both interfaces should be updated together.

### 8.5 Quick Difference Table vs. the Old Interface

| Output | `planPathsCoverage` | `planPathsCoverageEx` |
|:---|:---|:---|
| `scope` | `L1_L2` | `"NPU_L1_L2"` |
| `eidPairs[*].type` | `INTER_CHASSIS` | `INTER_CHASSIS` / `INTRA_CHASSIS` |
| `layerStats` | `null` | two layers |
| `coverageLinks[*].layer` | `null` | `NPU_L1` / `L1_L2` |
| `coverageLinks[*].deviceType` | `null` | `"NPU"` / `"SW"` |
| `totalLinks` | L1↔L2 only | NPU↔L1 + L1↔L2 |
| `coveredLinks` count | 4 per pair | 8 (inter-chassis) / 4 (intra-chassis) |

---

## 9. Compatibility and Impact Analysis

### 9.1 Zero Behavioral Change for the Existing Interface

| Change | Effect on `planPathsCoverage` |
|:---|:---|
| `collectBidirectionalLinks` gains a `scope` parameter | Under `L1_L2` the executed branch is line-for-line identical; no NPU segment is appended |
| `traceForwardPath` / `traceReversePath` gain a `scope` parameter | Under `L1_L2` the source L1SW is still fixed via `npuPortConns` and the last hop still uses `get(0)` |
| `LinkInfo` / `CoveredLinkDetail` gain fields | Old constructors are kept and delegate to the new ones; `L1_L2` links get `layer = L1_L2` |
| `CoveragePathsResult` / `CoverageLink` gain fields | All pre-existing fields (incl. the old `eidPairs`/`coverageLinks` members and `stats`) keep exactly the same values; among the new fields `scope = L1_L2` and `eidPairs[*].type = INTER_CHASSIS` are populated (purely additive, useful to tell the origin), while `layerStats` and `coverageLinks[*].layer/deviceType` stay `null` |
| `runGreedyCoverage*` / `buildResult` | Under `L1_L2` the `fullCoverage` decision is equivalent to the single-layer one; the statistics formulas are unchanged |
| `SncService` / `PathService` public methods | The old methods keep their signatures; the body is extracted into `planPathsCoverageInternal(..., L1_L2)` with unchanged semantics |

> **Regression baseline:** the existing coverage cases in `PlanPathsCoverageIntegrationTest`,
> `CoverageMainSuccessTest`, and `PathServiceTest` must **pass unchanged** as the acceptance
> criterion for "zero behavioral change".

### 9.2 Existing Tests and Tools That Must Be Updated

| File | Reason | Change |
|:---|:---|:---|
| `test/.../dto/CoveragePathsResultTest.java` | `CoveragePathsResult` all-args arity 5 → 7 | Append `null, null` to the arguments; add assertions for `scope` / `layerStats` |
| `test/.../dto/CoverageLinkTest.java` | `CoverageLink` all-args arity 9 → 11 | Append `null, null` to the arguments; add assertions |
| `test/.../service/CoverageDiagramGenerator.java` | Hardcoded `li == 0/1/2/3 → type 1/2/3/4` | Classify by `link.getLayer()` + `deviceType`; render NPU links as the NPU↔L1SW segment |
| `test/.../dto/CoverageStatsTest` (if added later) | `CoverageStats` is unchanged | No change needed |

> Other tests (`CoveragePathsRequestTest`, `CoveredEidPairTest`, `CoverageRequirementTest`,
> `SncServiceIntegrationTest`, the non-coverage cases of `PathServiceTest`, etc.) are unaffected.

### 9.3 Performance Impact

| Dimension | Change | Note |
|:---|:---|:---|
| EID-pair candidate enumeration | **Unchanged** (O(n²)) | The candidate set is still NPU ports; layering does not expand it |
| Per path trace | +2 route LPM lookups (one NPU segment each in the forward/reverse direction) | `lookupRoute` is O(number of mask lengths), typically ≤ 5 |
| Per path trace | +2 `nativeHash` calls (NPU segments) | Same order of magnitude as the existing L1/L2 hashes |
| `linkMap` size | Roughly +1× (NPU↔L1 out-ports) | The universe grows from "L1↔L2" to "NPU↔L1 ∪ L1↔L2" |
| Greedy selection | Grows with the `uncovered` set | `greedySelect`'s inner scan is driven by `linkToPairIdx`; more links mean more `uncovered` iterations |
| Memory | `CoveredPair` link lists 2 → 4 (forward/reverse each); `CoveredLinkDetail` doubles | Roughly +2× link details for the full EID-pair set |

> **Recommendation:** run a baseline measurement on the full rack topology
> (128 NPU / 16 L1SW / 4 L2SW) first (`CoverageDiagramGenerator` already prints elapsed time) and
> decide whether candidate pruning (e.g. cross-chassis pairs only) is needed.

### 9.4 Thread Safety

- `CoveragePlanEngine` itself is stateless; `hashFunc`, the fixed ports, and `hashTuple` are final.
- **Existing hazard:** the failure counters `fwdFail1..12` / `revFailA..H` are `private static int`,
  so concurrent calls pollute each other. When adding `npuFwdFail*` / `npuRevFail*`, it is
  **recommended to convert them to instance fields** (or reset them in `uninit`) to avoid
  introducing new shared mutable state.
- `SuperNode` / `SuperNodeStore` concurrency safety remains the caller's responsibility (unchanged).

### 9.5 Configuration Compatibility

| Configuration | New? | Note |
|:---|:---:|:---|
| `SNCConfig.hashFunc` | No | The NPU segment reuses the same function selector |
| `SNCConfig.fixedDataUdpPort` / `fixedAckUdpPort` | No | The NPU segment reuses the fixed ports (effective for 4-/5-tuple hashing) |
| `SNCConfig.hashTuple` | No | The NPU segment reuses the same tuple width |
| NPU-segment switch | **Not added** | Whether the NPU segment is active is decided by which interface is called (`planPathsCoverage` vs `planPathsCoverageEx`) |

---

## 10. Test Design

### 10.1 Test Fixture Enhancements

In the current rack topology each NPU route has **exactly one out-port per destination CNA**
(`FullRackTopologyGenerator`: "each 1 outPort"), which cannot drive NPU-segment hashing. The
following should be added to `CoverageRouteAugmentor`:

```java
/**
 * Augment NPU↔L1SW multi-out-port routes to drive NPU-segment hash selection.
 *
 * <p>For every NPU, expand the route to the "destination CNA via the peer L1SW" into the
 * multiple ports that the NPU has towards that L1SW (ECMP), maintaining maskLengths as well.
 * Also expand the L1SW NPU-facing /32 route into multiple out-ports when the L1SW has
 * multiple ports towards the same NPU.
 */
public static void augmentNpuRouting(SuperNode superNode);

/** Counts NPU→L1SW out-ports (same style as countL1swToL2swLinks). */
public static int countNpuToL1swLinks(SuperNode superNode);

/** Counts L1SW→NPU out-ports. */
public static int countL1swToNpuLinks(SuperNode superNode);
```

> If the real topology has no multi-uplink NPU, the NPU member set is always 1 and hashing
> degrades; the test focus then shifts to **including NPU↔L1 links in the coverage denominator**
> (`totalLinks = countNpuToL1swLinks + countL1swToNpuLinks + countL1swToL2swLinks +
> countL2swToL1swLinks`).

### 10.2 Unit Tests (`CoveragePlanEngineNpuL1Test`, new)

| Case | Scenario | Assertion |
|:---|:---|:---|
| T1 | NPU single uplink (member set size = 1) | NPU out-port = the only uplink; `coveredLinks` contains an `NPU_L1` link; the EID pair matches the old interface |
| T2 | NPU multiple uplinks (size ≥ 2), fixed ports `(0,0)` two-tuple | Repeated calls are stable; different EID pairs can land on different NPU out-ports |
| T3 | `HashTuple.FIVE`, different sport/dport | NPU out-port selection varies with the ports (proves the NPU segment participates in hashing) |
| T4 | Source NPU route missing | `traceForwardPath` not viable, `npuFwdFail1` increments, the pair is not selected |
| T5 | Source NPU route has no L1SW-facing out-port | `npuFwdFail2` increments |
| T6 | L1SW last hop with multiple out-ports | Different EID pairs can cover different L1SW→NPU out-ports (proves `get(0)` was replaced) |
| T7 | L1SW last-hop route missing / no NPU-facing out-port | `npuFwdFail4/5` increment |
| T8 | Old-interface regression | `findCoverage(..., L1_L2)` yields the same `totalLinks` / `coveredCount` / 4 links per pair as before the change |
| T9 | Layer-statistics consistency | `sum(layerStats[*].totalLinks) == stats.totalLinks`; same for `coveredCount` |
| T10 | Per-layer `fullCoverage` decision | Build a fixture where only the NPU_L1 layer is uncovered → must return `COVERAGE_INCOMPLETE` |

### 10.3 Northbound Integration Test (`PlanPathsCoverageExIntegrationTest`, delivered)

Fixture: the full-rack topology produced by `FullRackTopologyGenerator` → pruned to a
**single chassis** (rack1 NPU + rack1 L1SW, no L2SW), augmented by `augmentL1swRouting` /
`augmentL2swRouting` / `augmentNpuRouting` / `augmentL1swNpuRouting`, then wired through
`SncService.init + setSuperNode` (the 2-chassis scenario is reserved for the old interface, see §13.1).

| Case | Assertion |
|:---|:---|
| `minCoverage` | `status ∈ {SUCCESS, COVERAGE_INCOMPLETE}`; `scope == NPU_L1_L2`; `eidPairs` non-empty; each pair `type == INTRA_CHASSIS` with `coveredLinks.size() == 4`; `L1_L2.totalLinks == 0`; `sum(layerStats) == stats`; NPU_L1 `coveredCount > 0` |
| `redundant` | as above, plus every `layer == NPU_L1` link with `coverCount > 0` has `coverCount ≥ 2` |
| `notDataReady` | throws `SNCStateException` when `setSuperNode` was not called (not DATAREADY) |
| `nullRequest` | `request == null` throws `IllegalArgumentException` |
| `missingSuperNode` | unknown `superNodeName` → `TOPO_NOT_FOUND` |
| `afterUninit` | calling after `uninit` throws `SNCStateException` |

**Full-rack (4-chassis) coverage report tool:** `test/.../service/CoverageExRackReport` (a `main`
entry point, deliberately **not** a CI case so that regression stays fast):

```bash
mvn -o -DskipTests test-compile
java -Xmx6g -Djna.library.path=<resources> \
     -cp "target/classes;target/test-classes;<jna.jar>;<fastjson2.jar>" \
     com.huawei.umdk.snc.service.CoverageExRackReport MIN_COVERAGE target/coverage-rack4-report.md
```

Measured (see §13.7): the full rack reaches **100%** (4096/4096 out-ports), `SUCCESS`, 598
inter-chassis EID pairs, ≈189 s.

### 10.4 Assertion Matrix (New Fields)

| Assertion | Expression |
|:---|:---|
| Layer totals | `layerStats.values().stream().mapToInt(CoverageLayerStats::getTotalLinks).sum() == stats.getTotalLinks()` |
| Links per pair | `eidPairs.stream().allMatch(p -> p.getCoveredLinks().size() == 8)` |
| NPU link device type | For entries with `layer == NPU_L1 && "NPU".equals(deviceType)`, `remoteSwitch` must be an L1SW device name |
| L1→NPU link device type | For entries with `layer == NPU_L1 && "SW".equals(deviceType)`, `remoteSwitch` must be an NPU device name |
| `coveredPairs` consistency | The existing assertion (`coveredPairs.size() == coverCount`) must also hold for the new layer |
| Coverage-ratio relation | `stats.coverageRate` is consistent with the weighted combination of the two layer rates |

### 10.5 Regression Comparison Test

Keep the existing `planPathsCoverage` integration cases untouched. Additionally, a **side-by-side
case** is recommended: for the same topology and configuration, `planPathsCoverageEx`'s
`layerStats[L1_L2]` (totalLinks / coveredCount) should be comparable with the corresponding
values of `planPathsCoverage` (and exactly equal if `augmentNpuRouting` does not modify the
L1SW↔L2SW routes).

---

## 11. File List and Implementation Steps

### 11.0 Delivered (2026-09-13; all 477 tests pass)

| File | Status | Description |
|:---|:---|:---|
| `src/snc/.../util/HashUtils.java` | Modified | `nativeHashDstCnaJetty` + `JETTY_ID_MIN/MAX` + `isValidJettyId` |
| `src/snc/.../entity/NpuPortEntity.java` | Modified | `jettyId` + explicit 2-arg/3-arg constructors |
| `src/snc/.../dto/CoverageLinkScope.java`, `CoverageLinkLayer.java`, `CoverageLayerStats.java` | New | Scope / layer / per-layer statistics |
| `src/snc/.../dto/CoverageLink.java`, `CoveragePathsResult.java` | Modified | `+deviceType`/`+layer`, `+scope`/`+layerStats` |
| `src/snc/.../engine/CoveragePlanEngine.java` | Modified | Extended path (`findCoverageEx`/`traceForwardPathEx`/`traceReversePathEx`/`selectNpuEgress`/`collectNpuL1Links`/`getExDiagnostics`); the old path is untouched |
| `src/snc/.../service/PathService.java` | Modified | `planPathsCoverageInternal` + `planPathsCoverageEx` + `buildLayerStats`/`formatLayerDetail` |
| `src/snc/.../SncService.java` | Modified | Northbound `planPathsCoverageEx` |
| `src/snc/.../route/topo/template/loader/PortLoader.java`, `model/SncPort.java`, `service/TopoTemplateService.java` | Modified | Template input `jetty_id` → `SncPort.jettyId` (falls back to `32 + portId`) |
| `src/snc/src/main/resources/128_npu_rack.json` | Modified | `"jetty_id": 32 + port_id` written for 256 NPU ports |
| `test/snc/resources/topo_data_2npu_1port.json`, `topo_data_4npu_8port.json` | Modified | `"jettyId"` written for NPU ports |
| `test/.../TestDataLoader.java` | Modified | Parses `jettyId` |
| `test/.../FullRackTopologyGenerator.java` | Modified | **Fixed** per-port `jettyId = JETTY_ID_BASE + portIndex` (32..39, identical across generator invocations) |
| `test/.../FullRackTopologyJettyIdTest.java` | New | Pins the fixed jetty allocation: range / uniqueness per NPU / exact mapping / identical across two invocations (3 cases) |
| `test/.../service/CoverageExRackReport.java` | New | Full 4-chassis rack coverage report tool (`main`, writes `target/coverage-rack4-report.md`, not a CI case) |
| `test/.../CoverageRouteAugmentor.java` | Modified | `augmentNpuRouting` (NPU multi-uplink ECMP) + `countNpuToL1swLinks` / `countL1swToNpuLinks` |
| `test/.../util/HashUtilsJettyTest.java` | New | 6 cases: range / out-of-range / modulo range / determinism / distribution (128 samples over 2 ECMP members hit both, 64:64) / **CRC8 bit-for-bit vs. a Java reference** |
| `test/.../CoverageIntraChassisTest.java` | New | **PathService-level test of the new interface (single-chassis fixture)**: 0 inter pairs, intra 2-hop coverage (SUCCESS, 64/64, 18 pairs) + end-to-end CRC8/SCNA chaining verification + layer-statistic consistency + route-scope check, emitting the report |
| `test/.../PlanPathsCoverageExIntegrationTest.java` | New | **Northbound integration test of the new interface (`SncService` + generator single-chassis subset, counterpart of the old interface's IT)**: MIN_COVERAGE / REDUNDANT (incl. "covered link ≥ 2") + not-DATAREADY / null / unknown-supernode / after-uninit contracts, 6 cases |
| `test/.../dto/CoverageLinkTest.java`, `CoveragePathsResultTest.java` | Modified | All-args constructors updated + assertions for the new fields |
| `ubswitch.c` (repository root) | Modified | New exported symbol `ubswitch_Hash_dieEcmp` (CRC-8/ATM over the `(DstCNA, jettyId)` two-tuple) |
| `build_ubswitch.ps1` (repository root) | New | Rebuilds `libubswitch.dll` and both `.so` files (MinGW gcc / clang `-fuse-ld=lld --target=…-linux-gnu -nostdlib`) |
| `umdk/src/snc/src/main/resources/libubswitch*.{dll,so}` | Updated | Rebuilt with the new symbol: dll 66048 B, x86_64 so 3152 B, aarch64 so 3144 B |
| `src/snc/.../dto/CoveragePathType.java` | New | Inter-/intra-chassis marker (INTER_CHASSIS / INTRA_CHASSIS) |
| `src/snc/.../dto/CoveredEidPair.java` | Modified | `+type` |
| `test/.../CoverageIntraChassisTest.java` | New | Single-chassis topology: 0 inter pairs, intra 2-hop coverage (SUCCESS, 64/64, 18 pairs) + `target/coverage-intra-chassis-report.md` |
| `test/.../dto/CoveredEidPairTest.java` | Modified | All-args constructor updated with `type` |
| `src/snc/.../engine/CoveragePlanEngine.java` | Modified (2nd) | Two-phase coverage (inter → intra), `npuL1Peers`/`l2PortsTowardsL1Peers`/`l1PortsTowardsDevice`, intra 2-hop tracing, path-type tagging |
| `test/.../CoverageRouteAugmentor.java` | Modified (2nd) | `augmentL1swNpuRouting` (L1SW routes expanded to NPU-device scope) + `checkL1swNpuRouteScope`/`npuCnasOf` |

**Measured result:** see §13.7. Single chassis (the only automated integration test of the new
interface): `SUCCESS`, 18 intra pairs, NPU_L1 **100% (64/64)**; the 2-chassis scenario only runs
old-interface regression (`PlanPathsCoverageIntegrationTest` / `CoverageMainSuccessTest`). All
NPU-segment failure counters are 0 and `jettyIdFallback` is 0, confirming that the topology input
carries the jetty ids.

### 11.1 File List

| File | Type | Description |
|:---|:---|:---|
| `doc/ch/snc/SNC NPU-L1 Coverage Path Planning Design.ch.md` | New | This document (Chinese) |
| `doc/en/snc/SNC NPU-L1 Coverage Path Planning Design.md` | New | English counterpart (per the repo's ch/en one-to-one convention) |
| `src/snc/.../dto/CoverageLinkScope.java` | New | Coverage scope enum |
| `src/snc/.../dto/CoverageLinkLayer.java` | New | Link layer enum |
| `src/snc/.../dto/CoverageLayerStats.java` | New | Per-layer statistics DTO |
| `src/snc/.../dto/CoverageLink.java` | Modify | +`deviceType`, +`layer` |
| `src/snc/.../dto/CoveragePathsResult.java` | Modify | +`scope`, +`layerStats` |
| `src/snc/.../engine/CoveragePlanEngine.java` | Modify | `LinkInfo`/`CoveredLinkDetail` layer, NPU uplink view, `findCoverageEx`, H5/H6/H7a/H7d, per-layer `buildResult`, failure counters |
| `src/snc/.../service/PathService.java` | Modify | `planPathsCoverageInternal`, `planPathsCoverageEx`, `layerStats`/`deviceType` filling |
| `src/snc/.../SncService.java` | Modify | `planPathsCoverageEx` + `History` comment |
| `test/snc/java/.../CoverageRouteAugmentor.java` | Modify | `augmentNpuRouting` / `countNpuToL1swLinks` / `countL1swToNpuLinks` |
| `test/snc/java/.../engine/CoveragePlanEngineNpuL1Test.java` | New | Unit tests (§10.2) |
| `test/snc/java/.../PlanPathsCoverageExIntegrationTest.java` | New | Integration tests (§10.3) |
| `test/snc/java/.../dto/CoverageLayerStatsTest.java` | New | DTO test for layer statistics (following the existing DTO test template) |
| `test/snc/java/.../dto/CoveragePathsResultTest.java` | Modify | Add the new fields to the all-args constructor call |
| `test/snc/java/.../dto/CoverageLinkTest.java` | Modify | Add the new fields to the all-args constructor call |
| `test/snc/java/.../service/CoverageDiagramGenerator.java` | Modify | Classify link types by `layer`/`deviceType` |
| `doc/ch/snc/SNC API Guide.ch.md` / `doc/en/snc/SNC API Guide.md` | Modify | Document `planPathsCoverageEx` and the new result fields |
| `doc/ch/snc/SNC Test Specification.ch.md` / `doc/en/snc/SNC Test Specification.md` | Modify | Add the new interface's test-case matrix |

### 11.2 Suggested Implementation Order

1. **DTOs first:** add `CoverageLinkScope` / `CoverageLinkLayer` / `CoverageLayerStats`, extend
   `CoverageLink` / `CoveragePathsResult`, and fix the two DTO tests → compiles cleanly.
2. **Engine refactor (no behavior change):** add the `scope` parameter to `findCoverage` /
   `findCoverageDiffPath` / `collectBidirectionalLinks` / `traceForwardPath` /
   `traceReversePath`, keeping the `L1_L2` branch line-for-line identical; make `buildResult`
   compute layer statistics (single layer under `L1_L2`).
3. **Run the old regression:** all existing coverage tests green proves step 2 changed no behavior.
4. **Add the NPU segment:** add the NPU uplink view to `precomputeTopology`; add segments (3)(4) to
   `collectBidirectionalLinks`; wire H5/H6/H7a/H7d and the failure counters into
   `traceForwardPath`/`traceReversePath`.
5. **Service wiring:** extract `planPathsCoverageInternal`, add `planPathsCoverageEx`
   (`PathService` + `SncService`), and fill `scope` / `layerStats` / `deviceType` / `layer`.
6. **Fixtures and cases:** `augmentNpuRouting` + unit/integration tests; update
   `CoverageDiagramGenerator`.
7. **Documentation:** update the API Guide and Test Specification (both languages) and finalize
   this document's English version.

---

## 12. Open Questions

| ID | Question | Impact | Suggestion |
|:---:|:---|:---|:---|
| Q1 | Final interface name (`planPathsCoverageEx` vs `planPathsCoverageWithNpuL1` vs `planPathsFullCoverage`) | Affects the §3.1 signatures and all documents | Decide during review; renaming costs only three signatures plus a doc replace |
| Q2 | For NPU out-port hashing, should the SCNA be the **NPU port-level CNA** (the uplink port's own CNA) instead of the flow's source CNA? | Affects hash-input semantics and results | This design keeps the existing convention (source CNA); adjust if the hardware actually hashes with the egress port's CNA |
| Q3 | Should the L1SW→NPU last hop also be included in `REDUNDANT`'s "≥ 2 per link"? | Affects the number of EID pairs and redundancy | This design applies ≥ 2 uniformly per layer; make it configurable per layer if finer granularity is needed |
| Q4 | Is an intermediate scope ("NPU↔L1 only, without L1↔L2") needed? | Affects the `CoverageLinkScope` values | Currently only `L1_L2` / `NPU_L1_L2`; `NPU_L1` can be added if required |
| Q5 | Should the failure counters be converted from `static` to instance fields in this change? | Affects concurrency semantics | Recommended (§9.4) |
| Q6 | Should `CoverageLink.switchDevice` be renamed to `deviceName` along with the semantic extension? | Affects the JSON contract | Recommend keeping the original name and documenting the semantics (to preserve compatibility) |

---

## 13. Inter-/Intra-Chassis Two-Phase Coverage and Examples

> This chapter is the 2026-09-13 addendum: the two coverage scopes (框间/框内), the route-scope
> expansion, the relation between the EID and the physical egress port, the CRC8 NPU→L1
> selection algorithm, and worked examples of both flows.

### 13.1 The Two Coverage Scopes and the Output Shape

| Interface | Coverage | Output |
|:---|:---|:---|
| `planPathsCoverage` (old) | **Inter-chassis only** (L1SW↔L2SW) | 4 links per EID pair, `CoveredEidPair.type = null` |
| `planPathsCoverageEx` (new) | **Inter-chassis + intra-chassis** | both kinds appear in `eidPairs`, distinguished by `type` |

| Phase | Path | Links per EID pair | Marker |
|:---|:---|:---:|:---|
| 1 **inter-chassis** | `NPU → L1SW → L2SW → L1SW → NPU` | 8 (4 forward + 4 ACK) | `CoveredEidPair.type = INTER_CHASSIS`; `CoverageLink.layer ∈ {NPU_L1, L1_L2}` |
| 2 **intra-chassis** | `NPU → L1SW → NPU` (same chassis, no L2SW) | 4 (2 forward + 2 ACK) | `CoveredEidPair.type = INTRA_CHASSIS`; `CoverageLink.layer = NPU_L1` |

Execution order (`CoveragePlanEngine.findCoverageEx`):

1. **Inter-chassis first**: enumerate **cross-chassis** EID pairs, trace the 4-hop forward/ACK
   paths and greedily select the pairs that cover the L1SW↔L2SW out-ports;
2. **Then look for gaps**: keep the out-ports with
   `layer == NPU_L1 && coverCount < required` (`required` = 1 for MIN_COVERAGE, 2 for REDUNDANT);
3. **Close the gaps with intra-chassis pairs**: only when gaps exist, enumerate **same-chassis**
   EID pairs, trace the 2-hop forward/ACK paths and greedily select against the *gap set*;
4. **Merge**: the phase-1 and phase-2 EID pairs are merged and the final statistics are recomputed
   over the complete link universe.

> Consequently phase 2 may legitimately produce 0 pairs when phase 1 already covered every
> NPU↔L1SW out-port; in a single-chassis topology (or when no cross-chassis path exists) the whole
> coverage comes from phase 2.
>
> **Test convention (2026-09-13):** the **2-chassis scenario is used only for the old interface
> `planPathsCoverage` regression** (`PlanPathsCoverageIntegrationTest`, `CoverageMainSuccessTest`).
> All automated tests of the new `planPathsCoverageEx` use the topology produced by
> `FullRackTopologyGenerator`, pruned to a **single chassis**:
> - `PlanPathsCoverageExIntegrationTest` (**northbound `SncService`**, the counterpart of the old
>   interface's integration test): MIN/REDUNDANT coverage plus state-machine / null-argument /
>   unknown-supernode / after-uninit contract cases;
> - `CoverageIntraChassisTest` (`PathService` level): intra 2-hop, CRC8 selection, SCNA chaining,
>   layer-statistic consistency and route-scope verification, emitting the intra coverage report.

### 13.2 Example A: inter-chassis flow (4 hops + ACK)

> The sample data is taken from a planning run on the 2-chassis topology. Per the test convention
> above the 2-chassis scenario only runs old-interface regression, so this example serves as a
> **flow walkthrough**; the automated assertions of the new interface live in §13.3 (single chassis).

Topology: 2-chassis subset (8 NPU + 8 L1SW + 4 L2SW). EID pair:
`rack2#board1#npu1:400GUB 1/2/1` ↔ `rack1#board1#npu2:400GUB 1/4/1`.

| # | Direction | Device | Out-port | Peer | Peer port | layer |
|--:|:---|:---|:---|:---|:---|:---|
| 0 | forward | rack2#board1#npu1 | 400GUB 1/2/1 | rack2#l1sw1 | 400GUB 1/0/1 | NPU_L1 |
| 1 | forward | rack2#l1sw1 | 400GUB 1/0/78 | l2sw1 | 400GUB 1/0/7:2 | L1_L2 |
| 2 | forward | l2sw1 | 400GUB 1/0/7:2 | rack1#l1sw1 | 400GUB 1/0/78 | L1_L2 |
| 3 | forward | rack1#l1sw1 | 400GUB 1/0/3 | rack1#board1#npu2 | 400GUB 1/4/1 | NPU_L1 |
| 4 | ACK | rack1#board1#npu2 | 400GUB 1/4/2 | rack1#l1sw1 | 400GUB 1/0/4 | NPU_L1 |
| 5 | ACK | rack1#l1sw1 | 400GUB 1/0/94 | l2sw1 | 400GUB 1/0/15:2 | L1_L2 |
| 6 | ACK | l2sw1 | 400GUB 1/0/47:2 | rack2#l1sw1 | 400GUB 1/0/94 | L1_L2 |
| 7 | ACK | rack2#l1sw1 | 400GUB 1/0/1 | rack2#board1#npu1 | 400GUB 1/2/1 | NPU_L1 |

Key points:
1. the source NPU selects its egress with **CRC8 `(DstCNA, jettyId)`** among the uplink ports
   towards the L1SW that serves the destination (hop 0);
2. the **CNA of that selected port becomes the SCNA** used by the L1SW→L2SW (hop 1) and
   L2SW→L1SW (hop 2) selections;
3. the destination L1SW looks up `DstCNA` and hash-selects among **all** of its ports towards the
   destination NPU (hop 3);
4. the ACK direction is symmetric (hops 4–7), originating from the destination NPU port.

### 13.3 Example B: intra-chassis flow (2 hops + ACK, real test output)

Topology: single chassis (rack1: 4 NPU + 4 L1SW, no L2SW) → phase 1 has no usable pair, so the
whole coverage comes from phase 2. EID pair:
`rack1#board1#npu2:400GUB 1/4/1` ↔ `rack1#board1#npu1:400GUB 1/2/1`.

| # | Direction | Device | Out-port | Peer | Peer port | layer |
|--:|:---|:---|:---|:---|:---|:---|
| 0 | forward | rack1#board1#npu2 | 400GUB 1/4/1 | rack1#l1sw1 | 400GUB 1/0/3 | NPU_L1 |
| 1 | forward | rack1#l1sw1 | 400GUB 1/0/2 | rack1#board1#npu1 | 400GUB 1/2/2 | NPU_L1 |
| 2 | ACK | rack1#board1#npu1 | 400GUB 1/2/1 | rack1#l1sw1 | 400GUB 1/0/1 | NPU_L1 |
| 3 | ACK | rack1#l1sw1 | 400GUB 1/0/4 | rack1#board1#npu2 | 400GUB 1/4/2 | NPU_L1 |

### 13.4 Route-Scope Expansion and Receiver Semantics

**Receiver semantics (key premise):** a packet whose **DstCNA belongs to an NPU device** is
accepted by that NPU **even when it arrives on a port that is not the CNA's own port**. The routing
constraint therefore relaxes from "CNA ↔ port is one-to-one" to "**the NPU owning the CNA is
reachable**".

| Location | Previous scope | Expanded scope | Implementation |
|:---|:---|:---|:---|
| L1SW routes | /32 route only for CNAs of physically attached ports | for **every NPU this L1SW has a port to**, a /32 route for **every CNA** of that NPU, with **all** of the L1SW's ports towards that NPU as out-ports | `CoverageRouteAugmentor.augmentL1swNpuRouting`, checked by `checkL1swNpuRouteScope` |
| L2SW→L1SW selection | fixed to `dst.remoteL1sw` (the L1SW of the destination port) | any L1SW that can reach the destination NPU device | `PrecomputedTopo.npuL1Peers` + `l2PortsTowardsL1Peers` |
| L1SW→NPU selection | route had a single out-port (no ECMP) | all L1SW ports towards the destination NPU (≥2 ⇒ hash-selectable) | `l1PortsTowardsDevice` + `nativePortIdx(scna, dstCna, …)` |

Beneficial side effect: the ECMP member sets of the L1SW→NPU and L2SW→L1SW hops no longer
degenerate, so the planner can actually hit those out-ports — the NPU↔L1SW layer can reach 100%.

### 13.5 The EID vs. the Physical Egress Port (the "awkward" point)

- `srcPort / srcCna / srcEid` identify the **endpoint** (which logical port of which device
  originates/receives the flow); they do **not** constrain the physical egress port;
- the source NPU selects its real egress among its uplink ports towards the destination's L1SW
  using **CRC8 `(DstCNA, jettyId)`**; the **CNA of the selected port is the SCNA** used by the
  downstream L1/L2 selections;
- therefore `CoveredEidPair.srcPort` (the chosen endpoint identity) and `coveredLinks[0].outPort`
  (the real egress) **may differ** — this is intended, and
  `CoverageIntraChassisTest.verifyForwardIntraChain` / `verifyAckIntraChain` assert it explicitly;
- the ACK direction behaves the same way: `destPort` is the ACK's sending endpoint identity while
  the real egress is decided by `(srcCna, jettyId of the destination port)`.

### 13.6 Native Library: the CRC8 NPU→L1 Selection Algorithm

`ubswitch.c` (repository root) exports a new symbol:

```c
int ubswitch_Hash_dieEcmp(const char *dst_cna, int jetty_id, int ecmp_cnt);
/* CRC-8/ATM: poly 0x07, init 0x00, no reflection, no final XOR
   byte stream = ASCII of DstCNA (NUL excluded) + low byte + high byte of jettyId
   ecmp_cnt == 0 -> raw CRC (0..255); ecmp_cnt > 0 -> CRC % ecmp_cnt */
```

| Purpose | Native symbol | Java entry point |
|:---|:---|:---|
| Inter-chassis L1SW↔L2SW selection, L1SW→NPU selection | `ubswitch_Hash_ecmp` | `HashUtils.nativeHash(...)` / `nativePortIdx(...)` |
| **NPU→L1SW selection (CRC8)** | **`ubswitch_Hash_dieEcmp`** | **`HashUtils.nativeHashDstCnaJetty(dstCna, jettyId, ecmpCnt, hashFunc)`** |

Binary rebuild: `build_ubswitch.ps1` (MinGW `gcc -O2 -shared` for `libubswitch.dll`; `clang
--target={x86_64,aarch64}-unknown-linux-gnu -fuse-ld=lld -nostdlib -shared` for the two `.so`
files); artifacts land in `umdk/src/snc/src/main/resources/`. CRC8 correctness is verified
bit-for-bit against a Java reference implementation (including the `ecmpCnt` modulo) by
`HashUtilsJettyTest.crc8MatchesReference`.

### 13.7 Measured Results

| Scenario | Interface | status | inter pairs | intra pairs | NPU_L1 coverage | L1_L2 coverage | Report |
|:---|:---|:---|--:|--:|--:|--:|:---|
| **full rack, 4 chassis** (148 devices) | **new** `planPathsCoverageEx` | **`SUCCESS`** | **598** | 0 | **100.00% (2048/2048)** | **100.00% (2048/2048)** | `target/coverage-rack4-report.md` (≈189 s) |
| 2-chassis subset | **old** `planPathsCoverage` (inter-chassis only) | see `PlanPathsCoverageIntegrationTest` | — | — | — | — | console (`CoverageMainSuccessTest`) |
| single chassis (no L2SW) | **new** `planPathsCoverageEx` | `SUCCESS` | 0 | 18 | **100% (64/64)** | 0/0 | `target/coverage-intra-chassis-report.md` |

> The full rack (4 chassis) is exactly what `FullRackTopologyGenerator` produces (128 NPU × 8 ports
> + 16 L1SW + 4 L2SW). The coverage universe holds 4096 out-ports (NPU_L1 2048 = NPU→L1 1024 +
> L1→NPU 1024; L1_L2 2048 = L1→L2 1024 + L2→L1 1024) and **all 4096 are covered by 598
> inter-chassis EID pairs**; all ten engine diagnostics are 0 and `jettyIdFallback` is 0.
> How to run it is in §10.3 (the `CoverageExRackReport` tool, kept out of CI to keep regression fast).

> For reference only, a 2-chassis run of the new interface measured 161 inter pairs, NPU_L1 100%
> (128/128), L1_L2 55.27% (566/1024) and 0 intra pairs (phase 1 had already covered all NPU_L1);
> this is **not** an automated test case.
