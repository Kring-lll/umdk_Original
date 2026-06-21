# Kernel URMA Commit / Issue / PR Skill 使用说明

## 适用范围

本 skill 用于 openEuler kernel 仓中 URMA 相关改动的提交说明、中文 issue 草稿、GitCode issue 创建和 GitCode PR 创建，适合以下路径或模块：

- `<kernel-repo>`
- `drivers/ub/urma`
- `ubcore`、`ubagg`、`uburma`、`ipourma`
- `ub: udma` 相关驱动改动

当用户要求 agent 执行 kernel URMA 的“commit message 检查”“提交 issue”“提交 PR”等任务时，使用本 skill。实际创建或 amend kernel commit message 时，应先读取本 skill 内置的 `references/openeuler-kernel-commit.md`。

## 包内容

```text
kernel-urma-commit-pr-issue/
├── SKILL.md
├── README.md
├── agents/openai.yaml
├── references/openeuler-kernel-commit.md
└── scripts/
    ├── create_gitcode_issue.py
    └── create_gitcode_pr.py
```

`SKILL.md` 是 agent 执行流程的权威说明；`README.md` 面向使用者，说明如何复用和准备环境。

## 使用前准备

1. 确认工作区：

```bash
cd <kernel-repo>
git status --short --branch
```

2. 配置 GitCode token：

```bash
export GITCODE_TOKEN=<your-token>
```

不要把 token 写入仓库文件、git remote URL、命令历史或 PR 正文。

3. PR 提交前明确路由：

- 源 remote，例如用户 fork 对应的 remote
- 源分支，例如 `OLK-6.6`
- 目标 remote，例如 `origin`
- 目标分支，例如 `OLK-6.6`
- PR 标题和正文，或要求 agent 根据 commit/diff 生成

PR 是远端副作用，agent 不应自动猜测源/目标分支。

## 常见流程

### 1. 检查或重写 kernel commit message

让 agent 先读取 `references/openeuler-kernel-commit.md`，再使用本 skill 的 URMA 约束：

```text
拿当前 commit 走一遍 kernel 提交 skill 流程，重写 commit message。
```

kernel URMA bugfix 通常使用：

```text
urma: <concise subject>

urma inclusion
category: bugfix
bugzilla: https://atomgit.com/openeuler/kernel/issues/<id>
CVE: NA

-----------------------------------------------------------

问题现象、根因和解决方案。

Fixes: <12+ char sha> ("introducing subject")
Signed-off-by: <name> <email>
```

底层 UDMA 驱动改动通常使用 `driver inclusion`。

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
python3 skills/kernel-urma-commit-pr-issue/scripts/create_gitcode_issue.py \
  --title '<中文标题>' \
  --body-file <issue-body-file> \
  --submit
```

默认目标为 `openeuler/kernel`。

### 3. 创建 GitCode PR

用户明确要求“提交 PR”且路由完整时，agent 使用：

```bash
python3 skills/kernel-urma-commit-pr-issue/scripts/create_gitcode_pr.py \
  --repo-path <kernel-repo> \
  --source-remote <source-remote> \
  --source-branch <source-branch> \
  --target-remote <target-remote> \
  --target-branch <target-branch> \
  --title '<PR title>' \
  --body-file <pr-body-file> \
  --submit
```

脚本会先 push 源分支，再创建 PR。

## 推荐提交前检查

```bash
git -C <kernel-repo> status --short --branch
git -C <kernel-repo> diff --check <base>..HEAD
git -C <kernel-repo> log --oneline <base>..HEAD
```

如果改动影响 URMA 模块，尽量运行对应构建，例如：

```bash
make M=drivers/ub/urma CONFIG_UB_URMA=m WERROR=1 modules -j$(nproc)
```

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
