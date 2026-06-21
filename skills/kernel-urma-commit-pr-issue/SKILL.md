---
name: kernel-urma-commit-pr-issue
description: 为 openEuler kernel URMA 工作生成或审阅提交说明、配套中文 issue 草稿，并在用户明确要求时提交 GitCode issue 或 PR，尤其适用于 openEuler kernel 仓中的 drivers/ub/urma、ubcore、ubagg、uburma、ipourma 和 udma 改动。用户要求编写、润色、校验、推断、拆分、修订 kernel URMA 提交说明，生成提交或 diff 对应 issue，或在 GitCode 上提交/创建对应 issue/PR 时使用。不要用于单独生成泛化 PR 描述、merge commit、reviewer 元数据或发布说明。
---

# Kernel URMA Commit、Issue 和 PR

本 skill 只用于 kernel 侧 URMA/UDMA/IPoURMA/UBCORE/UBAGG 提交说明、提交或 diff 对应的中文 issue 草稿、该 issue 的显式 GitCode 提交，以及显式 GitCode PR 提交。不要生成 merge commit 文本、单独的泛化 PR 描述、reviewer 行、`!<pr>` 标题或 `See merge request` 行。只有在 PR 提交流程需要时，才允许生成必要的 PR 标题和正文。

实际创建或 amend commit 时，先读取 `references/openeuler-kernel-commit.md`，并遵循其中更严格的 openEuler Kernel SIG 提交说明规则。

## 必查上下文

写 commit、issue 或 PR 前先检查：

```bash
git -C <kernel-repo> status --short --branch
git -C <kernel-repo> diff --stat
git -C <kernel-repo> diff -- drivers/ub/urma
git -C <kernel-repo> log --format='%H%n%B%n---END---' -40 -- drivers/ub/urma
```

对 bugfix，需要检查可能的引入提交：

```bash
git -C <kernel-repo> log --oneline -- drivers/ub/urma/<path>
git -C <kernel-repo> blame -- drivers/ub/urma/<path>
```

## 提交说明形式

kernel URMA 非 merge commit 通常使用 openEuler inclusion metadata：

```text
<subsystem>: <concise subject>

<inclusion-type> inclusion
category: <bugfix|feature|performance|cleanup|doc|other>
bugzilla: https://atomgit.com/openeuler/kernel/issues/<id>
CVE: NA

-----------------------------------------------------------

说明问题和行为变化。

Fixes: <12+ char sha> ("introducing subject")
Signed-off-by: <real name> <email>
```

根据邻近历史选择 `inclusion-type`：

- ubcore/ubagg/uburma/ipourma 等 URMA 栈改动使用 `urma inclusion`。
- 底层 `ub: udma` 驱动改动使用 `driver inclusion`。
- 只有邻近历史或用户要求时，才使用其它 inclusion type。

## 提交规则

- `category:` 必填；缺陷修复使用 `bugfix`。
- 关联 issue 时 `bugzilla:` 必填。优先按邻近历史使用 `https://atomgit.com/openeuler/kernel/issues/<id>` 或 `https://atomgit.com/src-openeuler/kernel/issues/<id>`。
- 写提交说明时，将 GitCode issue URL 规范为 AtomGit URL。
- 同区域邻近提交大量使用 `CVE: NA` 时，或用户要求时，非 CVE 改动也保留 `CVE: NA`；否则可省略。
- 能识别引入提交时，bugfix 添加 `Fixes:`。
- kernel subject 要简洁，遵循 Linux 风格。除非用户明确要求，否则避免内部工单措辞。
- 禁止包含 merge request 元数据，例如 `From: @...`、`Reviewed-by:`、`!<pr>` 或 `See merge request`。

## 从提交生成 Issue

只有用户明确要求为 kernel URMA 提交、当前 diff 或用户提供的提交说明生成对应 issue 时，才使用此模式。

issue 输出必须全部使用中文，包括标题。固定使用以下格式：

```markdown
# <中文标题>

## 问题描述
<基于 diff 和 commit 信息描述问题现象、影响范围、触发条件或缺陷背景。证据不足时写“待补充”，不要编造复现环境、测试结果或 issue 编号。>

## 修复建议
<基于 diff 描述建议的修复方向或已采取的修复方式。证据不足时写“待补充”。>
```

issue 必须有代码依据：

- 标题根据实际缺陷或行为变化提炼，不要逐词翻译英文 commit subject。
- 如果 diff 能识别影响模块，正文中写出 `ubcore`、`ubagg`、`uburma`、`ipourma`、`ub: udma` 等 kernel URMA 模块名。
- 不要包含 PR 元数据、reviewer 元数据、merge request 链接、提交尾注或发布说明表述。

## 提交 Issue

只有用户明确要求在 GitCode 上提交或创建生成的 issue 时，才使用此模式。生成 issue 草稿不等于获得提交权限。

默认提交目标：

- owner: `openeuler`
- repo: `kernel`
- token 环境变量：`GITCODE_TOKEN`
- API base: `https://api.gitcode.com`

脚本路径：

- 优先使用当前 skill 目录下的 `scripts/`。
- 如果需要显式指定 skill 目录，可设置：

```bash
SKILL_DIR=<path-to-kernel-urma-commit-pr-issue-skill>
```

提交前：

1. 按“从提交生成 Issue”的固定格式生成并展示中文 issue 标题和正文。
2. 确认用户在当前轮次或紧邻的上一轮批准中明确要求提交/创建 issue。
3. 检查 `GITCODE_TOKEN` 是否已设置。未设置时停止，并要求用户设置环境变量；不要在聊天中索要 token。
4. 使用内置脚本，不要手写 curl 命令：

```bash
python3 "$SKILL_DIR/scripts/create_gitcode_issue.py" \
  --title '<中文标题>' \
  --body-file <issue-body-file> \
  --submit
```

安全默认行为：

- 不带 `--submit`，或带 `--dry-run` 时，只预览。
- 禁止打印 token，禁止把 token 放入 URL、git config、仓库文件、shell history 或最终回复。
- 成功时只报告创建出的 issue URL/id。失败时报告 HTTP 状态和脱敏响应。
- 只有用户明确选择其它仓库时，才覆盖 `--owner` 和 `--repo`。
- 提交流程结束后按“收尾清理”删除本轮创建的临时正文、dry-run payload、临时辅助脚本等中间产物。

## 提交 PR

只有用户明确要求提交或创建 GitCode PR 时，才使用此模式。创建 PR 是远程副作用，并且必须包含 push。

用户必须选择全部路由输入；不要推断默认值：

- 源 remote
- 源分支
- 目标 remote
- 目标分支
- PR 标题和正文，除非用户要求你基于提交或 diff 生成最小可用标题和正文

提交前：

1. 检查 status、remote、源分支，以及将要提交的 commits。
2. 展示准确的 push 计划和 PR 目标计划。
3. 检查 `GITCODE_TOKEN` 是否已设置。未设置时停止，并要求用户设置环境变量；不要在聊天中索要 token。
4. 使用内置脚本：

```bash
python3 "$SKILL_DIR/scripts/create_gitcode_pr.py" \
  --repo-path <kernel-repo> \
  --source-remote <source-remote> \
  --source-branch <source-branch> \
  --target-remote <target-remote> \
  --target-branch <target-branch> \
  --title '<PR title>' \
  --body-file <pr-body-file> \
  --submit
```

安全默认行为：

- 不带 `--submit`，或带 `--dry-run` 时，只预览。
- 禁止自动选择源/目标 remote 或分支。
- 工作区存在未提交改动、remote 缺失、源分支缺失、目标分支找不到、push 失败或未设置 `GITCODE_TOKEN` 时停止。
- 禁止打印 token，禁止把 token 放入 URL、git config、仓库文件、shell history 或最终回复。
- 成功时只报告创建出的 PR URL/id。失败时报告失败步骤和脱敏错误。
- 提交流程结束后按“收尾清理”删除本轮创建的临时正文、dry-run payload、临时辅助脚本等中间产物。

## 收尾清理

Issue 或 PR 流程完成、失败停止或用户中止后，都必须清理本轮创建的中间产物：

- 删除用于提交的临时 issue/PR 正文文件、dry-run 输出文件、临时 API 辅助脚本和临时 payload 文件，例如 `/tmp/kernel-*.md`、`/tmp/*gitcode*pr*.py` 这类由本轮创建且只服务于提交流程的文件。
- 如果中间产物创建在仓库内，优先删除；删除后用 `git -C <kernel-repo> status --short` 确认没有遗留未跟踪文件或无关改动。
- 不要自动删除本地工作分支、已推送远端分支、构建目录、测试日志、用户提供的文件或用户已有文件；这些只在用户明确要求清理时处理。
- 最终回复中简要说明已清理哪些中间产物；如果有不能清理或故意保留的内容，也要说明原因。
