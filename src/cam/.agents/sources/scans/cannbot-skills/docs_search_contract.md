# Docs-search contract

status: **accepted**

本文件只钉 **API / 官方资料怎么检索、检索结果算哪类证据**。不做 Knowledge 正文，不把 asc-devkit 手册搬进仓。

```text
accepted Knowledge / Case
  已冻结的事实

本文件
  事实还没冻结时，去哪查、查到了怎么标

accepted 之后
  很薄的 retrieval Skill 或 workflow
  不是第二套 API Card
```

消费方：写 kernel 时查 overload、debug 时合同未知、优化时不得用博客覆盖 Card。  
不消费：把 1022 篇 API md 入库。

---

## 先问什么

```text
1. 这次是 reuse（已有 Card 覆盖）还是 verification / promotion（新事实、冲突、要改 Card）？
2. target CANN version 是什么？本机 CANN 是否与它一致？
3. 要查的是哪条 overload？
4. 查到的东西是合同、示例，还是别人的推荐？
```

不要先问“先跑一遍在线搜索”。也不要把 docs-search 写成改代码前的门禁仪式（[operator-debug](../../../skills/operator-debug/SKILL.md) 已否）。

---

## Rule

```text
retrieval
  ≠
Knowledge fact

canonical retrieval
  ≠
source-of-truth verification

reuse mode
  accepted Knowledge / Case 已覆盖且 scope 匹配
  → 直接复用，不重复调查

verification / promotion mode
  新事实、合同不确定、发现冲突、准备改 accepted Card
  → 回到 target-version first-party
     header / official docs
  不得用 Card 证明 Card

先确定 target CANN version
  本机头文件仅在版本匹配时，才是该目标的一手证据
  不一致 → 本机头文件只作其他版本参考，不得覆盖 target 官方资料

先看 overload，再填数

same API name
  ≠
same overload / same contract

查到示例
  ≠
查到合同

数字后缀变体（Add.md / Add-25.md）
  必须列全再选
  后缀数字无语义
```

证据标法：

| 落到哪 | 可标 |
| --- | --- |
| 官方文档与头文件一致，且都对上 target version | `official-document` + `source-code` |
| 两者冲突 | **不要合并**；分别记录；保留 version / overload / platform conflict |
| 仅示例 / 模板 | `recommendation` 或 unknown |
| 仅 cannbot / 仅博客 | 不得升 fact |
| 本机 CANN ≠ target version | 本机头文件不得当该目标一手证据 |

```text
debug / perf heuristic
  cannot override
accepted API / hardware contract
```

检索结果也一样：新查到的“512B 更快”不能覆盖 copy Card。

```text
reuse mode 顺序
  accepted Knowledge / Case（scope 匹配）
    → 停

verification / promotion 顺序
  1. target-version 官方文档
  2. 仅当本机 CANN == target：本机头文件
  3. 可选：同版本 asc-devkit docs（examples 仍是 recommendation）
  4. 在线兜底，钉 target version
```

---

## 版本纪律，不要抄 cannbot 环境

cannbot docs-search 写死：A3、`ASC_DEVKIT_DIR`、asc-devkit **9.0.0**、本地 1022 篇 API。这些不是 Weaver 检索合同。

```text
检索始终先确定 target CANN version
  用户给定 / 任务声明 / 运行环境
  ≠
repo 默认永远是 8.5.0

local source is authoritative for target
  only when versions match
```

没有 `$ASC_DEVKIT_DIR` ≠ 不能查。有别的版本的 devkit ≠ 覆盖 target 头文件。  
在线搜钉 **target version**，不要默默用最新页。

本轮扫描环境（provenance，不是合同）：`cam_wangyibo_dev_85` / CANN 8.5.0。

---

## 和现有 Skill 怎么接

```text
operator-development
  API 合同未知 → 本检索；已有 Card → 先读 Card
  算子语义 / 参数协议未知 → operator-spec-reconstruction，不是本检索

operator-spec-reconstruction
  重建「这个算子做什么」
  不替代本 Skill 查 DataCopyPad / TQue / CANN overload

operator-debug
  API 合同未知才检索
  不是 classify 之前的第 0 步仪式

operator-optimization
  检索不能引入与 Card 冲突的“更快点”

tiling / copy / mask
  先 overload / 单位 / 容量，再看示例怎么写
```

---

## What NOT To Generalize

| 不要当成 contract | 为什么 |
| --- | --- |
| 把 api-index / 示例目录整份拷进 Weaver | 体积 + 易过时；CURATION_PLAN 禁止手册搬家 |
| 拷 Python 爬虫脚本当 Knowledge | 手段；本机未必能跑 |
| “必须先 docs-search 再改代码” | 已否的仪式 |
| 无后缀 = 基础且一定适用 950 | 变体功能可能完全不同；还要看平台 |
| 示例里的 double buffer 模板 = 设计合同 | 读 TQue Card |
| cannbot compatibility 的 A3 / 9.0.0 | 那是他们的环境表 |
| Weaver 默认 CANN = 8.5.0 | 那是本轮扫描环境 |
| 两份一手材料冲突就挑顺的 | 分别记录，不合并 |

---

## Sources

| Field | Value |
| --- | --- |
| 发现来源（非真源） | cannbot `ops/ascendc-docs-search` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 归位 | [docs_search_promotion_verdict.md](docs_search_promotion_verdict.md) |
| 已冻结 | mask overload-first；Batch 2 Cards；debug “检索不是门禁” |
| 本轮扫描环境（非合同） | `cam_wangyibo_dev_85` / CANN 8.5.0 |
| Last written | 2026-08-22 |
| status | accepted |
