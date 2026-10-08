# Tolerance-source contract

status: **accepted**

本文件只钉 **正确性判定用哪套容差、数字从哪来**。不写第二本 precision-debug，不把 CANNBot 阈值表升成官方标准。

```text
operator-debug
  输出错了怎么查

本文件
  “错了”按哪条尺子量

accepted 之后
  短 Knowledge 或很薄的路由
  链回 operator-debug 的 numerical 族
```

消费方：`operator-debug`（wrong-result / numerical）、ST / 测试脚本、correctness-debug 的 observation。  
不消费：性能收益、Cast RoundMode 教程、DumpTensor。

证据种类：`official-document` | `source-code` | `recommendation` | `unknown`。

---

## 先问什么

```text
1. 任务 Plan / 用户有没有写下 rtol、atol、bitwise、matched_ratio？
2. 这是计算、搬运/Cast、整数、量化，还是随机数？
3. 有没有对得上 CANN 版本的官方精度标准？
4. 没有 1 和 3 时，能不能诚实标 unknown，而不是抄一张缺省表？
```

不要先问“FP16 是不是 1e-3”。那是 cannbot precision-debug 的兜底，不是真源。

---

## Rule

```text
task acceptance threshold
  Plan / 用户合同优先
  没有再找 applicable 官方标准
  再没有 → unknown
  不要发明第四档“Skill 缺省表”

official-conformance claim
  必须实际满足并引用对应官方标准
  Plan 不能覆盖官方标准

Plan overrides test acceptance
  ≠
Plan overrides official standard

comparison method
  ≠
threshold numbers

|actual − golden| ≤ atol + rtol × |golden|
  是浮点常用比法
  不是任何 dtype 的官方阈值

integer compute
  bitwise match 或 abs error = 0

non-compute copy
  通常 bitwise
  float Cast 的舍入不是“搬运 bitwise”

rtol 挂了
  ≠
公式错了
  （那是 wrong-result 症状，先走 operator-debug）
```

---

## 真源优先级

```text
这次任务验收（task acceptance）
  Plan / 用户合同
      ↓ 没有
  官方标准（须 applicability 匹配）
      ↓ 对不上或找不到
  unknown 临时尺子
    记录：比了什么、为什么不是官方
    不得写成“已按官方标准通过”

能否声称符合官方标准（official conformance）
  仅当实际满足并引用了 applicable 官方标准
  Plan 写 1e-2 而官方更严 → 任务可以按 Plan 收
    ≠ 可以说“符合该官方标准”
```

官方标准存在 ≠ 适用于当前调用。匹配时确认（有则查，不是每项必填）：

```text
CANN / version
operator / operator class
input / output dtype
mode / quantization / relevant attributes
platform / SOC（若标准有区分）
```

CANNBot `float_compute.md` 的 `2^-9` / `0.99` 表、precision-debug 的 FP16 `1e-3` / FP32 `1e-5`，本轮都 **不是** `official-document`。脚本自称对齐 `experimental_standard.md`，仓库里未见该文件 → `cannbot-secondary`。

Case 里的 rtol（如 43 的 `1e-3`、60 的 `1e-2`）是 **那条测试的合同**，不是 950 通用标准。

---

## 分类（只选尺子，不诊断）

和 cannbot SKILL 树同构，但只决定 **比法家族**：

| 类 | 判定（够用就停） | 比法 | 数字从哪来 |
| --- | --- | --- | --- |
| 随机 / 随机输出 | 按分布生成 | **不**用普通 elementwise 容差；路由到专门 statistical validation | 本轮不定义 KS / p-value |
| 非计算 | 搬移；无数值计算 | 通常 bitwise | Plan；Cast 另看 |
| 整数计算 | 输入输出都是整数算术 | bitwise 或 abs=0 | 一般不需要 rtol |
| 量化 | 整型 ↔ 浮点 | 看出 dtype：浮点走混合容差，整型走出整数 | 同上 |
| 浮点计算 | 浮点算术 | 混合容差公式 | Plan 或官方表 |

```text
选尺子
  ≠
定位根因

FP16 rtol 挂
  可能是 Cast / 中间精度 / 同步 / 公式 / 尺子本身过严
  回到 operator-debug 的 wrong-result 五族
```

---

## 和 debug / 闭环怎么接

```text
failure contract.observation
  可以写：rtol/atol 下 matched_ratio、max_abs_error
  必须写：用的是 Plan、官方哪一版，还是 unknown 临时尺子

蓝区
  可以选定尺子、设计对照
  不上板不得报“精度已过”

内网 retest
  同一 failure contract + 同一 tolerance source
```

标杆（golden）：比被测实现 **更高精度或已约定** 的参考。cannbot 的 CPU > 小算子拼接 > 自写，是 `recommendation`。不要把“必须用 CPU float64”写成唯一合法 golden。

---

## What NOT To Generalize

| 不要当成 contract | 为什么 |
| --- | --- |
| 再写一本 precision-debug | Batch 3 已有路由 |
| 把 float 阈值表标 official-document | 未核到版本化官方文 |
| FP16=1e-3 / FP32=1e-5 | precision-debug 兜底，已否 |
| 所有 Cast 必须 bitwise | 舍入存在时过严 |
| Case rtol 当平台标准 | 那是单测合同 |
| 拷脚本进 Knowledge | 手段 |
| Plan 宽松 ⇒ 符合官方标准 | 只覆盖任务验收 |
| 本轮定义 KS / p-value | 只路由，不背统计标准 |

---

## Sources

| Field | Value |
| --- | --- |
| 发现来源（非真源） | cannbot `ops/ops-precision-standard` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 归位 | [tolerance_promotion_verdict.md](tolerance_promotion_verdict.md) |
| 已冻结 | [operator-debug](../../../skills/operator-debug/SKILL.md)；wrong-result ≠ precision |
| Last written | 2026-08-22 |
| status | accepted |
