# 前缀负载感知路由 — 测试同事串讲材料

> 分支：`feat_prefix_lb2`　|　串讲对象：测试同事　|　目的：理解特性逻辑、找到测试切入点
> 配套代码：`src/aigw/internal/gs/prefix_cache_lb.go`、`internal/prefixcache/`、`internal/kvevents/`

## 一句话理解

**前缀负载感知**（Prefix Cache LB）：请求来了，看哪个 worker 的 KV cache 里已经存了相同前缀，就路由给谁，复用 KV、省 prefill。本质是"被动感知"——vLLM 推什么我记什么，请求来查表选 worker。

> 🆕 **本分支改进**：原算法只看前缀命中率，命中高的 worker 会被反复路由成热点。本分支引入**前缀匹配负载均衡感知调度算法**：负载不均衡时优先均衡负载、负载均衡时优先前缀命中，避免热点 Pod。详见下文专节。

---

## 它解决什么问题

LLM 推理时，**前缀（system prompt + few-shot + 历史）会被反复计算**。如果同一个 worker 上次已经算过这段前缀、KV cache 还在，下次相同前缀的请求再发给它，就能跳过这部分 prefill，TTFT 显著下降。

问题：aigw 是个网关，下面挂多个 vLLM worker。**aigw 怎么知道哪个 worker 上存了哪段前缀？** —— 这就是这套机制的核心。

## 三个角色

```
              ┌─────────────────────────┐
   请求 ──────▶│      aigw (网关)         │
              │  prefixCacheLB.schedule()│─── 算前缀 hash 链
              │  查 hash→worker 表       │─── 选命中%最高的 worker
              └──────────┬──────────────┘
                         │ 1) 哪个 worker 有这段前缀？
            ┌────────────┴───────────────┐
            ▼                             ▼
   ┌─────────────────┐          ┌──────────────────┐
   │  vLLM worker A   │          │  vLLM worker B    │
   │  (HTTP 8081)     │          │  (HTTP 8080)      │
   │  KV cache里有    │          │  KV cache 里没有   │
   │  [前缀A]         │          │                  │
   └────────┬─────────┘          └──────────────────┘
            │ 2) 调度时 vLLM 把存的块通过 ZMQ:5557 推事件
            ▼
   BlockStored {block_hashes, token_ids, ...} ──▶ aigw 维护 hash→worker 表
```

- **aigw**：维护一张 `hash → worker` 表；请求来时算前缀 hash 查表选 worker。
- **vLLM worker**：跑推理时把"我存了哪些 block"通过 ZMQ 推给 aigw。
- **render 服务**：aigw 调 vLLM 的 `/v1/chat/completions/render` 把请求文本变 `token_ids`（tokenize），因为 hash 是按 token 算的。

## 请求路由的完整链路（[prefix_cache_lb.go](src/aigw/internal/gs/prefix_cache_lb.go) `schedule()`）

1. **取 ready 实例**：`getReadyInstances()` 拿候选 worker URL 列表。
2. **URL→instanceName 映射**：表里 key 是 instanceName（如 `worker-19000`），但 metric 里是 URL（`127.0.0.1:19000`），要建一张映射表 `urlToInstanceName`（[manager.go:285](src/aigw/internal/prefixcache/manager.go#L285) `GetInstanceNameByUrl`）。
3. **tokenize**：若有 renderClient，调 `RenderChat` 把 prompt 变 `token_ids`（chat-template 域）。失败则走文本兜底 `MatchPrefixByText`。
4. **查表匹配**：`MatchPrefix(modelName, -1, tokenIDs, readyInstances)` → 返回 `{instanceName: matchPercent}` 和 matchLength（命中多少 token）。
5. **选 worker**：`selectFromMatched()` —— 这里是改进后的**前缀匹配负载均衡感知调度算法**（详见下一节）。核心两分支：
   - **负载不均衡**（各 ready worker 的 `TokenNum` 最大-最小差 > `loadImbalanceThreshold`）→ 选**最低负载** worker，避免热点；
   - **负载均衡**（差 ≤ 阈值）→ 在命中% ≥ `matchThreshold` 的 worker 里选**命中%最高**的，同分取**更低 TokenNum**；无达标命中 → 返回 nil 走 fallback。
6. **命中**：返回该 worker 的 `PrefillUrl`；并 `AddPrefix` 把本次前缀也登记到表（乐观登记，下次直接命中）。
7. **未命中**：`fallback()` → 走 `leastConnLB`（最小连接数）兜底，不报错。

> ⚠️ 测试重点：`matchThreshold` 是命中与 fallback 的分水岭（均衡态下）。`loadImbalanceThreshold` 是"均衡负载"与"优先命中"的分水岭。构造"负载明显倾斜"和"负载均衡+部分命中"两组场景验证两条分支。

## 前缀匹配负载均衡感知调度算法（[selectFromMatched](src/aigw/internal/gs/prefix_cache_lb.go)）

> 这是当前分支相对上游做的**改进点**。原来的 `selectFromMatched` 只看前缀命中%、选命中最高的 worker，**不看负载**——结果前缀命中高的 worker 被反复路由，成为热点 Pod，负载打爆。新算法把"负载均衡"和"前缀命中"两条目标联合决策。

### 一句话概括

**负载不均衡时优先均衡负载，负载均衡时优先 Prefix Cache 命中**，实现"以存代算"（用已存的 KV cache 顶 prefill 计算）的同时避免热点 Pod。

### 算法步骤

1. **分词**：对请求 tokenize（render 域 token_ids，同上节）。
2. **查匹配**：查询所有 ready Pod 的前缀缓存匹配，得到每个 Pod 的**匹配长度 + 匹配率**（`matchPercent`）。
3. **取负载**：取每个 Pod 当前负载 = `TokenNum`（已占 token 数）。
4. **算跨度**：在**所有 ready Pod** 上算 `max(TokenNum) - min(TokenNum)`。
5. **分支决策**：

```
                max_load - min_load
                       │
          ┌────────────┴────────────┐
          ▼                         ▼
   > thresh (不均衡)           ≤ thresh (均衡)
          │                         │
   选最低负载 Pod          在命中%≥matchThreshold的Pod里
   (不管有没有前缀命中)     选命中%最高的；同分取更低TokenNum
          │                  无达标命中 → nil → fallback
          ▼                         ▼
   均衡负载、避免热点         最大化复用KV、省prefill
```

- **不均衡分支**：返回的 Pod **不一定有前缀匹配**——这是刻意的。负载已经倾斜时，继续往热 Pod 上堆前缀命中只会让热点更热，所以先均衡负载。
- **均衡分支**：负载差不多时，优先选前缀命中最高的 Pod，最大化"以存代算"收益；命中率相同时选负载更低的，避免制造新热点。
- **门禁**：均衡分支下只有命中% ≥ `matchThreshold` 才算"有效命中"，否则返回 nil，由 `schedule()` 走 leastConn 兜底（leastConn 本身就选低负载 Pod）。

### 两个阈值的角色

| 阈值 | 配置 | 作用 | 默认 |
|---|---|---|---|
| `matchThreshold` | env `AIGW_PREFIX_CACHE_MATCH_THRESHOLD` | 命中%门禁：均衡态下低于此值视为"没命中"，走 fallback | 50（联调建议 10） |
| `loadImbalanceThreshold` | JSON `loadBalancer.loadImbalanceThreshold` | 负载跨度门禁：`max-min` 超过此值判为不均衡，切到均衡分支 | 0（=禁用均衡分支，退化为只看命中率的旧行为） |

> `loadImbalanceThreshold = 0` 时算法退化为原始前缀选择逻辑，可作为对照基线或回退开关。

### 示例（3 个 worker，`loadImbalanceThreshold=1000`，`matchThreshold=50`）

| 场景 | A | B | C | 决策 |
|---|---|---|---|---|
| 负载不均衡 | load 5000 / 80% | load 1000 / 90% | load 3000 / 70% | max-min=4000>1000 → 选 B（最低负载，哪怕不是最高命中） |
| 负载均衡 | load 1000 / 80% | load 1100 / 90% | load 1200 / 70% | max-min=200≤1000 → 选 B（命中90%最高） |
| 均衡+同命中 | load 1000 / 80% | load 800 / 80% | load 1200 / 70% | max-min=400≤1000 → A、B 同 80%，选 B（load 800 更低） |
| 均衡+低命中 | load 1000 / 30% | load 1100 / 20% | — | max-min=100≤1000，但最高命中 30%<50 门禁 → nil → leastConn 兜底 |



- token 按 `uint32 big-endian`（4字节）编码；
- 每 `blockSize` 个 token 为一个 block，算一次 `xxhash`；
- **链式**：`block[i] 的 hash = xxhash(parent=block[i-1]的hash, block[i]的tokens)`，第一个 block 的 parent 是 `seed`；
- 只对完整 block 算，末尾不足一个 block 的尾巴丢弃。

**最关键的对齐约束**：aigw 的 `AIGW_PREFIX_CACHE_BLOCK_SIZE` 必须 **等于** vLLM 的 `--block-size`。两边不一样 → aigw 切 block 的边界和 vLLM 不一致 → 永远 0 命中。设计文档明确写这是联调第一坑。

## `hash→worker` 表怎么维护（vLLM 主动推）

vLLM 0.18 自带 KV 事件 publisher，配置 `enable_kv_cache_events=true` 即起 ZMQ PUB（默认 `tcp://*:5557`），用 msgpack 推三类事件：

| 事件 | 含义 | aigw 处理（[sync_table.go](src/aigw/internal/prefixcache/sync_table.go)） |
|---|---|---|
| `BlockStored` | worker 存了新 block | `ProcessBlockStored`：用 token_ids 重算 aigw xxhash，存 `hash→instance` |
| `BlockRemoved` | worker 淘汰了 block | `ProcessBlockRemoved`：从表里删 |
| `AllBlocksCleared` | worker 清空（模型重载等） | `RemoveInstanceFromAllContexts`：删该 worker 全部记录 |

aigw 侧接收链路：`kvevents/manager.go` 的 `SubscribeInstance` → 建 `ZMQClient` → `receiveLoop` 收消息 → `decodeBatch`（剥 msgpack）→ `eventHandler.handleEvent` → 分发给 `prefixCacheManager`（`OnBlockStored` 等）。

> **注意一个设计细节**：vLLM 推过来的 block hash 是 **vLLM 自己算的**，和 aigw 算的 xxhash **不一样**。所以 aigw 收到事件后，用事件里带的 `token_ids` **自己重算一遍 aigw 侧 hash**存表（[sync_table.go:142](src/aigw/internal/prefixcache/sync_table.go#L142) `ProcessBlockStored`）。查表匹配时 aigw 也用自己的 hash，两边才能对上。`HashMapping.engineToAigw` 是 vLLM hash→aigw hash 的缓存，避免重复算。

## worker 什么时候才被订阅

不是配置了就订阅。**worker 注册时**才建 ZMQ 订阅：

- `POST /aigw/v1/register-instance`（body: ip/port/name/model/role）→ `gs_manager.go:441` `registerInstance` → `pcMgr.SubscribeInstance(name, ip, port, model)`；
- 反注册时 `UnsubscribeInstance` 拆 ZMQ + 清表。
- **ZMQ 端口 5557 在 aigw 侧硬编码**（`kvevents/manager.go:193-198`），若宿主端口冲突要改代码。

## 配置项速查

| 配置 | 默认 | 说明 |
|---|---|---|
| `loadBalancer.mixed` | - | 设 `"prefixCache"` 启用（[gs_manager_opts.go:129](src/aigw/internal/gs/gs_manager_opts.go#L129)） |
| `AIGW_PREFIX_CACHE_ENABLED` | false | 开 prefix cache 模块 |
| `AIGW_KV_EVENTS_ENABLED` | false | 开 ZMQ 订阅 |
| `AIGW_PREFIX_CACHE_BLOCK_SIZE` | 16 | **必须=vLLM `--block-size`** |
| `AIGW_PREFIX_CACHE_MATCH_THRESHOLD` | 50 | 命中%阈值，联调建议先 10 |
| `loadBalancer.loadImbalanceThreshold` | 0 | 负载跨度阈值（token 数）：max-min 超过则优先均衡负载；0=禁用退化旧行为 |
| `renderClient.baseURL` | - | 指向 vLLM render 端口（tokenize，无状态，单值即可） |
| `model` | - | **必须=vLLM `--served-model-name`** |

示例配置见 [configs/aigw-prefix-cache.example.json](src/aigw/configs/aigw-prefix-cache.example.json)。

---

## 测试切入点（给测试同事的重点）

### 已有的测试脚手架

| 位置 | 是什么 | 怎么用 |
|---|---|---|
| [test/e2e/mock_prefix_cache_server.py](src/aigw/test/e2e/mock_prefix_cache_server.py) | mock：k8s API(8000)+4个 worker(19000-19003)+render(18080)+ZMQ publisher | 起 mock 环境，模拟整条链路 |
| [verify_prefix_cache.sh](src/aigw/test/e2e/verify_prefix_cache.sh) | 环境自检脚本（端口/健康/配置/日志） | 排查环境是否就绪 |
| [benchmark_prefix_cache.sh](src/aigw/test/e2e/benchmark_prefix_cache.sh) | 跑前缀命中 benchmark | 性能验证 |
| `internal/gs/*_test.go` | Go 单测（LB 选择、命中排序、fallback） | 单元层验证 |

### 建议的测试场景清单

1. **正常命中**：相同前缀两次请求 → 第二次应路由到同一 worker，vLLM `prefix_cache_hits_total` 增长。
2. **阈值边界**：构造命中% 刚好 = / 略低于 / 略高于 `matchThreshold`，验证选前缀 vs 走 leastConn 的分界。
3. **完全不同前缀**：走 leastConn fallback，不报错。
4. **block_size 不一致**：aigw 和 vLLM block_size 设不一样 → 应永远 0 命中（负面用例，验证配置校验或至少行为符合预期）。
5. **worker 动态上下线**：注册/反注册 worker → ZMQ 订阅建立/断开，表项增删正确。
6. **KV 事件缺失**：vLLM 不发 BlockStored（或 ZMQ 断连）→ 表空 → 全走 fallback，不崩。ZMQ 重连后恢复。
7. **matchThreshold 调参**：10 vs 50，观察命中率/误命中率变化。
8. **负载不均衡→均衡分支切换（新算法重点）**：构造一个 worker `TokenNum` 显著高于其他（max-min > `loadImbalanceThreshold`），即使它前缀命中%最高，请求也应路由到**最低负载** worker，而非命中最高的。
9. **负载均衡→优先命中（新算法重点）**：各 worker `TokenNum` 接近（max-min ≤ 阈值）时，请求应路由到前缀**命中%最高**的 worker。
10. **同命中率的 tiebreak（新算法）**：两个 worker 命中%相同、负载均衡时，应选 `TokenNum` 更低的。
11. **`loadImbalanceThreshold=0` 退化**：设为 0，验证算法退化为只看命中率的旧行为（基线对照）。

### 排查时的日志关键字

- `[prefixCache]` — 前缀路由分支选择、命中%、fallback。新算法关键日志：
  - `load imbalanced (spread=X > threshold=Y)` — 命中不均衡分支，选最低负载 Pod
  - `load balanced (spread=X <= threshold=Y)` — 命中均衡分支，按前缀命中选
  - `top match X% below threshold Y` — 均衡态但无达标命中，defer 到 fallback
- `[kvevents]` — ZMQ 连接、订阅、decode、sequence gap、重连
- `[prefixcache]` — 表的存删、context 创建、eviction
- `[render]` — tokenize 调用

---

## 附：数据流关系图

```
                    ┌──────────────── KV 事件总线 (ZMQ:5557) ────────────────┐
                    │                                                      │
vLLM BlockStored ──▶│──▶ prefixcache.SyncPrefixTable  (hash→worker 表)      │
                    └──────────────────────────────────────────────────────┘
                                      ▲
请求 ──▶ prefixCacheLB.schedule() ───┘  (查表选 worker)
         │ 不命中
         ▼
       leastConn fallback
```

aigw 被动接收 vLLM 推送的 KV 块事件维护 `hash→worker` 表，请求来时查表选 KV 复用最优的 worker，不命中则走 leastConn 兜底。
