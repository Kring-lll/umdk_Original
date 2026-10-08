# Operator-debug contract

status: **accepted**

本文件只钉 **这个 Skill 遇到症状时怎么思考**。不写工具手册，不把 CANNBot 三套 debug 流程搬进来。

```text
CANNBot precision / runtime / crash
    ↓
本文件：Weaver 诊断模型
    ↓
skills/operator-debug/SKILL.md      ← 短路由器
workflows/correctness-debug/        ← 一页闭环（蓝区 ↔ 内网）
```

Batch 2 已经冻结开发链。operator-debug **复用**这些 Knowledge，不重写一套“调试常识”。

证据种类：`official-document` | `source-code` | `recommendation` | `unknown`。  
调试排序、优先假说一律是 `recommendation`，不得升成 Knowledge fact。

---

## 先问什么

```text
1. 可观察症状是什么？进程有没有跑完？有没有非 0 返回码？输出长什么样？
2. 这是哪一类：compile / load / runtime error，wrong result，hang，还是 crash？
3. 现在手里有什么证据？缺的最小证据是什么？
4. 按已冻结 Knowledge 排出假说，而不是按症状表写死根因。
5. 下一个动作能不能否定当前最高假说？不能 → 还不是检查，只是再试一次。
```

不要先问“加 `PipeBarrier<PIPE_ALL>` 能不能好”，也不要先问“507035 是不是没对齐”。那是假说，不是第 1 问。

---

## Rule

```text
operator-debug 是路由器，不是排障百科

每轮选择一个 primary route
  不要并行跑多套流程
  新证据改变症状定义时，允许重新 classify
  one primary route at a time
    ≠
  classification is immutable

  compile / load / runtime error  → runtime-error
  wrong result                    → wrong-result
  hang                            → hang
  crash                           → crash

wrong result
  ≠
precision problem

诊断循环
  symptom
    → classify
    → collect evidence
    → form ranked hypotheses
    → smallest discriminating check
    → fix
    → retest

五条边界
  symptom        ≠ root cause
  error code     ≠ root cause
  hypothesis     ≠ Knowledge fact
  fix            ≠ validation
  blind retry    ≠ debugging

same number / same unit
  ≠
same API contract
  （调试里同样成立：507035 里的“32B”
    不能反推 DataCopyPad.blockLen 必须 32B 对齐）

debug heuristic
  cannot override
accepted API contract
```

---

## 诊断循环

```text
symptom
  用户原话、返回码、输出形态、是否跑完
        ↓
classify          ← 只分桶，不下根因；选定本轮 primary route
  ├─ compile / load / runtime error
  ├─ wrong result
  ├─ hang
  └─ crash
  新证据改变“跑完了没有 / 返回码 / 是否还在跑”
    → 重新 classify，不要并行开第二套流程
        ↓
collect evidence  ← 先读已有日志 / 最小复现 / 对照 Knowledge
        ↓
form ranked hypotheses
  每条假说必须能链到一张 accepted Knowledge
  或一条可检验的工程观察
  排序是 recommendation，不是 fact
        ↓
smallest discriminating check
  一个动作，两种结果应能抬高或丢掉最高假说
        ↓
fix               ← 只改当前被证据支持的那一处
        ↓
retest            ← 原最小复现 + 至少一条原先失败用例
```

跳步规则：

```text
还不能 classify          → 先问“跑完了没有 / 返回码是什么 / 输出是什么”
还没有最小复现           → 先冻结 shape / dtype / 固定输入，不要改 kernel
假说链不到 Knowledge
  也链不到可检验观察     → 不要开改；去查 overload / 读 Card
检查不能区分两条假说     → 换更小的检查，不要同时改两处
```

---

## 五条边界

### 1. symptom ≠ root cause

全 0、随机、偶发错、`sum=0` **只是症状**。

已冻结说法见 [pipeline-events-and-ordering](../../../knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md)：

```text
全 0 / 随机
  → 优先假说：CopyIn→Compute 或 Compute→CopyOut 缺 readiness
  → 再查 32B / effective vs aligned
  → 再查 GetValue/SetValue 当了生产搬运

优先假说
  ≠
唯一病因
  ≠
已经定位
```

`PipeBarrier<PIPE_ALL>` 加上之后变对，只说明 **跨流水 readiness 假说升权**。还要回到显式 ordering 合同：普通 Vector 优先 TQue；手写 `SetFlag`/`WaitFlag` 是另一条合法机制。不要把 `PIPE_ALL` 留在生产路径上当修复。

### 2. error code ≠ root cause

返回码只决定 **进哪棵假说树**，不决定改哪一行。

| 码 | 允许写成 | 禁止写成 |
| --- | --- | --- |
| 161xxx | 参数 / dtype / shape / nullptr **假说入口** | “参数写错了，改某个字段” |
| 361xxx | runtime / 设备 / 环境 **假说入口** | “环境坏了” |
| 507035 | 向量核异常信号；访存 / buffer / 对齐 / copy shape **假说树** | “一定是 DataCopyPad 没 32B 对齐或 UB overflow” |
| 561002 | TilingKey / TilingFunc / 不支持的参数组合 **假说入口** | “tiling 公式错了” |
| 561003 | 安装 / vendor / SEL / SOC / 环境 **假说入口** | “Python 选了另一条 kernel”（那是 [delivery](../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md)） |
| 507046 | stream sync timeout → 先按 **hang** 想 | 和 507035 混成同一个病 |

507035 经 `aclrtStreamSynchronize` 回到 Host 时，**先 classify 为 runtime error**，不要只因为名字带 exception 就进 crash。

表面是 timeout error、设备其实一直不结束 → 重新 classify 为 **hang**。  
用户说“精度错”、后来看到返回码非 0 → 重新 classify 为 **runtime-error**，不要继续走 wrong-result。

错误码语义门（给以后 references，不是本文件要冻结的 taxonomy）：

```text
只从 CANNBot 二手资料来的码
  → debug routing hint

核到对应 CANN 版本官方 error-code 文档之后
  → 才可标 official error-family meaning

不要因为数字看起来稳定
  就写成跨 CANN 版本的永久分类学
```

### 3. debug hypothesis ≠ Knowledge fact

```text
Knowledge
  DataCopy 要 32B 长度 + UB 起址
  DataCopyPad 吃非对齐；Ext.blockLen 单位是字节
  跨 pipe producer-consumer dependency 必须被显式表达
  普通 Vector 优先 TQue EnQue/DeQue；手写 HardEvent 是另一条合法机制
  repeatTime 常为 uint8_t 且官方约束 ≤ 255
  TQue.num = backing blocks；depth = queue protocol

Skill 可以说
  全 0 时先查 readiness（recommendation）
  507035 时先把 copy 参数和 UB 预算排在前面（recommendation）

Skill 不可以说
  全 0 = 没同步
  507035 = 没对齐
  “优先查同步”是硬件事实
```

排序可以变，Card 不能为了排障被改写。

### 4. fix ≠ validation

```text
改完编译过
  ≠
修好了

原最小复现变对
  ≠
这个症状类都解决了
```

retest 最低要求：

1. 同一条最小复现再跑一遍  
2. 至少再跑一条原先失败的用例（若有）  
3. 若改的是 tiling / buffer / 对齐：走完 [tiling-from-shape](../../../knowledge/engineering-experience/tiling-from-shape.md) 的 constraint check，不要只改 kernel 常数  

没上板就不要报“精度已过”。蓝区无 NPU：可以完成 classify / 假说 / 代码级检查，Runtime 标 **未测**。

### 5. blind retry ≠ debugging

```text
有信息增益的一步
  新观察：返回码、plog 行、dump 片段、二进制是否更新、
          对齐/不对齐、FP32/FP16 分叉

无信息增益的一步
  同一类改动再试一次
  同时改同步、对齐、公式、tile
  没确认二进制更新就继续改
```

多次无信息增益 → 停试错，切 **二分 / dump / instrumentation**。  
不要保留 CANNBot 的“>7 次”神奇数字。切策略的判据是 **信息增益**，不是计数器。

输出完全不变：把 **stale binary / cache** 当作一条假说去检验（清 `build/`、`kernel_cache`，核 sha256）。不要把清 cache 写成每次调试的第 0 步仪式。

---

## Classify

只分桶。用户说“精度不对”但进程没跑完或返回码非 0，**不要进 wrong-result**。

| 桶 | 判定（够用就停） | 本轮 primary route |
| --- | --- | --- |
| **compile / load / runtime error** | 编译失败；找不到 kernel / `.so`；aclnn / aclrt 非 0；507035 经 stream sync 回来 | `runtime-error` |
| **wrong result** | kernel **跑完**，输出全 0 / 随机 / NaN / rtol 不达标 / 个别值错 / tail 少 / 看起来像算错了 | `wrong-result` |
| **hang** | 进程或 kernel 不结束、超时、507046、核无响应 | `hang` |
| **crash** | Segfault / Abort / 进程被杀；有 core 或明确 abort | `crash` |

`wrong-result` 里面再排假说族，**不要**把整条路径叫 precision：

```text
wrong-result
  ├─ data movement / ordering
  ├─ indexing / layout / tail
  ├─ tiling / bounds
  ├─ formula / logic
  └─ numerical / dtype / precision
```

只有最后一族才是数值精度。tail 少算、offset 错、Broadcast stride、effective/aligned 混用、错误 tile 映射、公式写错，都先走前面几族。

性能差 **不是** 本 Skill。交给 `operator-optimization` / profiling。  
多卡通信 hang：先读 [MC2 coupling](../../../knowledge/engineering-experience/mc2-compute-collective-coupling.md)，不要在普通 Vector 路径里展开 CrossCore。  
SIMT / DCache：本轮不进默认树。

hang ≠ crash。CANNBot 把两者塞进同一个 Skill；Weaver 必须拆开。

compile / load 本轮不单开 env Skill。它们是 `runtime-error` 的前两支：

```text
runtime-error
  ├─ compile          编译器 / 头文件 / Host 代码
  ├─ load             安装、vendor、SEL、SOC、OPP 路径
  └─ execute          161 / 361 / 507035 / 561002 / 其它执行期码
```

---

## 复用 Batch 2，不要重写

假说必须能指回这些文。Skill 只负责 **排序和最小检查**。

| 检查什么 | 读哪张 Card | 不要在 debug 里再发明 |
| --- | --- | --- |
| 平台 UB / 核数是否抄死 | [core-and-memory-hierarchy](../../../knowledge/hardware/ascend950/core-and-memory-hierarchy.md) | “950 UB = 248KB” |
| tile / buffer 超预算、要对齐回传 | [tiling-from-shape](../../../knowledge/engineering-experience/tiling-from-shape.md) | 新的切分手册 |
| DataCopy vs Pad、effective vs aligned | [memory-copy-and-alignment](../../../knowledge/ascendc-api/data-movement/memory-copy-and-alignment.md) | “Pad 的 blockLen 必须 32B” |
| 全 0、缺跨流水 readiness、PIPE_ALL 假说 | [pipeline-events-and-ordering](../../../knowledge/ascendc-api/synchronization/pipeline-events-and-ordering.md) | “全 0 = 同步”；“必须 EnQue，没有别的机制” |
| Alloc/Free vs EnQue/DeQue、`num`≠`depth` | [ub-tque-tbuf-and-double-buffer](../../../knowledge/ascendc-api/data-movement/ub-tque-tbuf-and-double-buffer.md) | hang = 没 Free |
| 结果 silently 错、repeat 截断 | [mask-repeat-and-kernel-limits](../../../knowledge/ascendc-api/vector/mask-repeat-and-kernel-limits.md) | “Level-0 一拍一定 256B” |
| 561003 vs Python 选路 | [delivery](../../../knowledge/engineering-experience/custom-op-delivery-call-and-selected-path.md) | kernel not found = 选错 op |

constraint 失败时的修复方向已经写在 tiling 文里：

```text
超限 / 不对齐 / API 不合法
  → revise split / tile / buffer
  或 kernel 内加 outer loop，拆成多次合法调用
```

不要在 debug Skill 里另写一套“减小 tile”教程。

---

## 最小可判别检查（例，不是清单）

一条检查只回答一个分叉。

```text
CopyIn → CopyOut，不做 Compute
  这个实验把
    MTE2 → Vector → MTE3
  收成
    MTE2 → MTE3
  因此它覆盖不了 MTE2→V / V→MTE3 readiness

  对 → GM/UB 地址、CopyIn/CopyOut 参数、
        effective/aligned、基本搬运路径降权
        Vector Compute / 公式 / dtype 升权
        MTE2→V / V→MTE3 readiness 不降权
  错 → 先留在 copy / 对齐 / CopyOut 有效长，不要先改公式

同一 shape，FP32 过、FP16/BF16 挂
  算法整体错 降权
  中间精度 / Cast 路径 / Cast 后额外同步 / 额外 castBuf 升权
  （仍是假说排序，不是“半精度一定要升 FP32”）

改代码后输出字节级不变 + sha256 也不变
  逻辑假说全部挂起
  stale binary / 没链到正在跑的 kernel 升权

Alloc/Free 或 EnQue/DeQue 明显不成对，且症状是 hang
  pairing 假说升权
  还不是 crash，也不要先上 GDB
```

`PIPE_ALL` 只当判别实验，用完拆掉。  
DumpTensor / PRINTF / GDB / mssanitizer / plog 都是 **手段**，按路径需要再打开，见下节。

---

## Skill 长什么样

最终形态：

```text
skills/operator-debug/SKILL.md
    诊断循环 + 五条边界 + classify 表
        ↓
    runtime-error
    wrong-result
      ├─ data movement / ordering
      ├─ indexing / layout / tail
      ├─ tiling / bounds
      ├─ formula / logic
      └─ numerical / dtype / precision
    hang
    crash
        ↓
    skills/operator-debug/references/
      runtime-error.md
      wrong-result.md
      hang.md
      crash.md
    workflows/correctness-debug/          一页闭环（蓝区 ↔ 内网）
```

### 主体只许有

- 诊断循环  
- 五条边界  
- classify → 四条路径  
- “先读哪张 Knowledge”  
- 何时切二分 / dump / instrumentation  

### 主体不许有

| 手段 | 挂在哪 | 不要塞进 SKILL.md |
| --- | --- | --- |
| `aclGetRecentErrMsg` / plog / `parse_plog` | runtime-error、hang | 脚本与命令百科 |
| 清 cache / 对 sha256 | 各路径的 stale-binary 假说 | 每次调试第 0 步仪式 |
| DumpTensor / PRINTF / 误差分布 | wrong-result（尤其 numerical 族） | “7 步法” |
| 二分 / 最小工作路径 | 无信息增益之后 | 计数器 |
| GDB / coredump | crash | gdb 教程 |
| mssanitizer | hang / crash 的偶发、踩踏假说 | 三步编译安装 |
| docs-search | API 合同未知时 | “改代码前必须先检索” |

容差数字本轮 **不写进 Skill**。真源留给 Batch 4 的 `ops-precision-standard`。任务 Plan 若给了 rtol/atol，就用 Plan；否则标 unknown，不要抄 CANNBot 缺省表当标准。

---

## Decision Rules（给未来 Skill 用）

```text
MUST     先 classify，再收集证据，再排假说
MUST     先冻结最小复现，再改 kernel
MUST     一次只做一个可判别检查
MUST     改完走 retest
MUST NOT 把症状表或错误码写成根因
MUST NOT 为了排障改写 accepted Knowledge
MUST NOT 无信息增益时继续同质修改
MUST NOT 无 NPU 时要求 npu-smi 通过才能做 classify / 假说 / 代码检查

每轮 MUST 只走一个 primary route；新证据允许重新 classify

IF 进程没跑完或返回码非 0
THEN 不要进 wrong-result

IF 全 0 / 随机
THEN readiness → 对齐 → Get/SetValue 作为排序，不是诊断

IF 507035
THEN 进入访存/buffer/对齐/copy-shape 假说树；
     blockLen 单位和 Pad vs Copy 只读 copy Card

IF 561003
THEN 先安装/SEL/vendor/SOC；不要跳到 vLLM 选路

IF 改完输出完全不变
THEN 先检验二进制是否更新

IF 连续步骤没有新观察
THEN 停试错，切二分 / dump / instrumentation
```

---

## What NOT To Generalize

| 不要当成 contract | 为什么 |
| --- | --- |
| 一个 Debug Skill 包打天下 | 路由；细节在路径下 |
| 症状 → 固定修复补丁 | 症状只分桶 |
| 错误码 → 固定根因 | 只进假说树 |
| 清 cache / docs-search 作为门禁仪式 | 它们是假说或查合同 |
| hang 与 crash 共用一条树 | 证据和手段不同 |
| wrong result = precision | 数值/dtype 只是 wrong-result 里的一族 |
| classify 一次定终身 | 每轮一个 primary route；新证据可重分 |
| CopyIn→CopyOut 对 ⇒ readiness 没问题 | 该实验没覆盖 MTE2→V / V→MTE3 |
| 把 CANNBot 的 7、缺省 rtol、DumpTensor 7 步写进主体 | heuristic / 手段 / 未读标准 |
| 蓝区无 NPU 就不能思考 | 只是 Runtime 未测 |
| 为本 Skill 再开精度陷阱 Knowledge | Batch 2 已覆盖事实层 |
| 性能、SIMT、MC2 通信 hang 的完整树 | 出本路由 |

---

## Sources

| Field | Value |
| --- | --- |
| 发现来源（非真源） | cannbot `ops/ascendc-precision-debug` / `ascendc-runtime-debug` / `ascendc-crash-debug` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 已冻结复用 | Batch 2 六张 Card + tiling-from-shape + delivery |
| 归位 | [operator_debug_promotion_verdict.md](operator_debug_promotion_verdict.md) |
| 计划 | [knowledge_promotion_plan.md](knowledge_promotion_plan.md) §Batch 3 |
| Last written | 2026-08-22 |
| status | accepted |
