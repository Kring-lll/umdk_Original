# Tolerance-source promotion verdict

status: **accepted**

对照：cannbot `ops/ops-precision-standard/` @ `c4c425641d11127945d22e5a752fcad77646be97`  
模型：[tolerance_source_contract.md](tolerance_source_contract.md)

只决定 **尺子怎么选、哪些数字能进 Knowledge**。不扩 debug Skill，不拷检查脚本。

```text
debug / perf heuristic
  cannot override
accepted API / hardware contract

cannbot 阈值表
  cannot become
official-document
  除非核到对应版本官方精度文
```

---

## 总判

cannbot 这份 Skill 有用的是 **分类树 + 混合容差比法**，不是那张 dtype 阈值表。

```text
值得留
  先问算子类，再选比法
  浮点：|a−g| ≤ atol + rtol×|g|
  整数：bitwise 或 abs=0
  量化：看出 dtype 再选上面两种
  Plan 覆盖任务验收；官方符合性必须真满足官方标准

必须改写
  float 表（2^-9 / 0.99 / 32 ULP）→ 未核官方前不得标 official-document
  “生态开源 experimental_standard”→ cannbot-secondary（仓内无该文件）
  非计算 = 一律 bitwise → Cast/舍入要分开

不要搬
  mixed_tolerance_check.py 等脚本
  测试用例生成长文、KS 教程
  precision-debug 缺省 1e-3 / 1e-5
```

不要做成 `skills/precision-debug/`。最多：

```text
knowledge/engineering-experience/tolerance-source.md   ← 短 Card：优先级 + 比法 + 分类
operator-debug SKILL                                   ← 一行：尺子读这篇，不要抄缺省表
```

---

## 按文件

| 内容 | 处理 | 去哪 |
| --- | --- | --- |
| SKILL 分类树 | **keep** | 短 Card |
| 混合容差公式 + matched_ratio 思想 | **keep 作 method** | 短 Card；阈值另证 |
| float 阈值表 | **none 作 fact** | 核到官方再补表；否则 unknown |
| integer bitwise / abs=0 | **keep** | 短 Card |
| 量化看出 dtype | **keep** | 短 Card |
| 非计算 bitwise | **rewrite** | 纯搬运可 bitwise；float Cast 不自动套 |
| 随机数 KS | **none 本轮** | 普通 Vector 默认树不收 |
| golden：CPU > 拼接 > 自写 | **recommendation** | 一句 |
| 测试用例生成 | **none** | |
| 脚本 / evals | **none** | |

---

## 归位

| 产物 | 本轮 | 下一动作 |
| --- | --- | --- |
| [tolerance_source_contract.md](tolerance_source_contract.md) | 真源模型 | **`accepted`** |
| 本文件 | 压缩裁决 | **`accepted`** |
| `knowledge/engineering-experience/tolerance-source.md` | 短 Card | 写 |
| 改 operator-debug 容差那一行 | 指向短 Card | 写 |
| 新 debug Skill / 脚本仓 | **none** | |

---

## Review 清单

写短 Card 时出现任一条就打回：

- [ ] 把 2^-9 或 1e-3 标成 official-document  
- [ ] 写成 precision-debug 第二本  
- [ ] 用 Case rtol 当 950 通例  
- [ ] 拷 Python 检查脚本  

---

## Sources

| Field | Value |
| --- | --- |
| CANNBot | `ops/ops-precision-standard` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 脚本自称 | `experimental_standard.md` §2（仓内未见） |
| 模型 | [tolerance_source_contract.md](tolerance_source_contract.md) |
| Last written | 2026-08-22 |
| status | accepted |
