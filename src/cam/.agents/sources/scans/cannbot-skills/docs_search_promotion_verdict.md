# Docs-search promotion verdict

status: **accepted**

对照：cannbot `ops/ascendc-docs-search/` @ `c4c425641d11127945d22e5a752fcad77646be97`  
模型：[docs_search_contract.md](docs_search_contract.md)

只决定 **检索纪律进哪**。不进 `knowledge/` 正文。

---

## 总判

cannbot 这份 Skill 是 **检索器 + 本地索引**，不是新的 API 事实源。

```text
值得留
  本地 / 同版本官方 优先于在线
  同名 API 先列变体再选
  示例 ≠ 合同
  查阅钉 target version；本机头文件仅版本匹配时才是一手
  reuse ≠ verification

必须改写
  搜索根从 $ASC_DEVKIT_DIR / “默认 8.5.0” 改成 target-version discipline
  “当前环境 A3 / devkit 9.0.0” 丢掉
  Explore Agent / 强制 find 命令 不写进 Knowledge

不要搬
  references/api-index.md（1022 路）
  example-catalog、compatibility 全表
  scripts/*.py 爬虫
  “高性能模板就是那条 Add 示例”
```

---

## 按文件

| 内容 | 处理 | 去哪 |
| --- | --- | --- |
| 本地优先、在线兜底 | **keep** | 薄 Skill / workflow |
| 变体 `Name.md` / `Name-数字.md` | **keep** | 同上；与 mask overload-first 合流 |
| `$ASC_DEVKIT_DIR/docs/zh/api` 作为唯一根 | **rewrite** | Weaver 不假设 devkit 在仓里 |
| 在线脚本 + 版本过滤 | **keep 钉 target version** | 不拷脚本；不把 8.5.0 写成仓默认 |
| api-index / example-catalog | **none** | 检索缓存，不是 Knowledge |
| compatibility A3/9.0.0 | **none** | |
| Explore subagent 提示词 | **none** | orchestration |

---

## 归位

| 产物 | 本轮 | 下一动作 |
| --- | --- | --- |
| [docs_search_contract.md](docs_search_contract.md) | 检索模型 | **`accepted`** |
| 本文件 | 压缩裁决 | **`accepted`** |
| `skills/docs-search/SKILL.md` | 薄检索 | 写；**禁止**进 knowledge/ |
| 新 API Card | **none** | 有缺口再开，不靠索引膨胀 |

不要和 operator-debug 抢“第 0 步”。

---

## Review 清单

- [ ] 把检索结果直接写成 Knowledge fact  
- [ ] 假设人人有 `$ASC_DEVKIT_DIR`  
- [ ] 用 9.0.0 文档覆盖 8.5.0 头文件  
- [ ] 把 8.5.0 写成 Weaver 默认检索合同  
- [ ] 用 accepted Card 证明同一张 Card  
- [ ] 两份一手冲突时自行合并  
- [ ] 拷 api-index 进仓  
- [ ] 写成“改代码前必须检索”  

---

## Sources

| Field | Value |
| --- | --- |
| CANNBot | `ops/ascendc-docs-search` @ `c4c425641d11127945d22e5a752fcad77646be97` |
| 模型 | [docs_search_contract.md](docs_search_contract.md) |
| Last written | 2026-08-22 |
| status | accepted |
