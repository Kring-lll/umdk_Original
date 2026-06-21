# UMDK Commit / Issue / PR Skill 使用说明

## 适用范围

本 skill 用于 openEuler UMDK 仓的提交说明、中文 issue 草稿、GitCode issue 创建和 GitCode PR 创建，适合以下目录或组件的改动：

- `<umdk-repo>`
- URMA / UMQ / UMS / UDMA 用户态代码
- `urma_admin`、`urma_perftest`、`uvs`、URMA library、测试和文档改动

当用户要求 agent 执行“生成提交说明”“检查 commit message”“提交 issue”“提交 PR”等任务时，优先使用本 skill。

## 包内容

```text
umdk-commit-pr-issue/
├── SKILL.md
├── README.md
├── agents/openai.yaml
└── scripts/
    ├── create_gitcode_issue.py
    └── create_gitcode_pr.py
```

`SKILL.md` 是 agent 执行流程的权威说明；`README.md` 面向使用者，说明如何复用和准备环境。

## 使用前准备

1. 确认工作区：

```bash
cd <umdk-repo>
git status --short --branch
```

2. 配置 GitCode token：

```bash
export GITCODE_TOKEN=<your-token>
```

不要把 token 写入仓库文件、git remote URL、命令历史或 PR 正文。

3. PR 提交前明确路由：

- 源 remote，例如用户 fork 对应的 remote
- 源分支，例如 `fix-xxx`
- 目标 remote，例如 `origin`
- 目标分支，例如 `master`
- PR 标题和正文，或要求 agent 根据 commit/diff 生成

PR 是远端副作用，agent 不应自动猜测源/目标分支。

## 常见流程

### 1. 生成或重写 commit message

让 agent 基于当前 diff 或指定 commit 执行：

```text
按 UMDK 提交 skill 检查并重写当前 commit message。
```

agent 会检查：

- `git status --short --branch`
- `git diff --stat`
- 相关路径历史
- bugfix 的可能 `Fixes:` 提交

UMDK 普通提交通常使用：

```text
urma: <简洁动作>

说明问题或修改动机。

说明行为变化和用户可见影响。

Signed-off-by: <name> <email>
```

偏驱动或历史已使用 inclusion 格式的提交，可以使用 `driver inclusion` / `category:` / `bugzilla:` / `CVE: NA` 等字段。

### 2. 创建中文 issue

用户明确要求“提交 issue”时，agent 会先生成中文 issue 草稿，格式为：

```markdown
# <中文标题>

## 问题描述
...

## 修复建议
...
```

确认后使用脚本提交：

```bash
python3 skills/umdk-commit-pr-issue/scripts/create_gitcode_issue.py \
  --title '<中文标题>' \
  --body-file <issue-body-file> \
  --submit
```

默认目标为 `openeuler/umdk`。

### 3. 创建 GitCode PR

用户明确要求“提交 PR”且路由完整时，agent 使用：

```bash
python3 skills/umdk-commit-pr-issue/scripts/create_gitcode_pr.py \
  --repo-path <umdk-repo> \
  --source-remote <source-remote> \
  --source-branch <source-branch> \
  --target-remote <target-remote> \
  --target-branch <target-branch> \
  --title '<PR title>' \
  --body-file <pr-body-file> \
  --submit
```

脚本会先 push 源分支，再创建 PR。

## 安全约束

- 工作区有未提交改动时，不提交 PR。
- token 未设置时，不提交 issue/PR。
- 不要用同一仓同一分支作为 source 和 target。
- 不要把 token 打印到日志或写入文件。
- 失败后清理临时正文文件和 dry-run payload。

## 失败排查

- DNS 或 SSH 失败：先确认 `git ls-remote <remote>` 能访问。
- push 失败：确认源 remote 有写权限，源分支名正确。
- API 失败：确认 `GITCODE_TOKEN` 有创建 issue/PR 权限。
- 目标分支不存在：确认 `git ls-remote --heads <target-remote> <target-branch>`。
