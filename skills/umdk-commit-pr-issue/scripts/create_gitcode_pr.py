#!/usr/bin/env python3
"""推送 UMDK 分支并创建 GitCode pull request。"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_API_BASE = "https://api.gitcode.com"
DEFAULT_REPO_PATH = "."
DEFAULT_TOKEN_ENV = "GITCODE_TOKEN"


def localize_argparse_message(message):
    return (
        message.replace("the following arguments are required:", "缺少必填参数：")
        .replace("unrecognized arguments:", "无法识别的参数：")
        .replace("expected one argument", "需要一个参数值")
        .replace("invalid choice:", "无效选项：")
    )


class ChineseArgumentParser(argparse.ArgumentParser):
    def format_usage(self):
        return super().format_usage().replace("usage:", "用法:")

    def format_help(self):
        return (
            super()
            .format_help()
            .replace("usage:", "用法:")
            .replace("options:", "选项:")
            .replace("show this help message and exit", "显示帮助信息并退出")
        )

    def error(self, message):
        self.print_usage(sys.stderr)
        self.exit(2, f"{self.prog}: 错误：{localize_argparse_message(message)}\n")


def parse_args():
    parser = ChineseArgumentParser(
        description="推送源分支并创建 GitCode PR。"
    )
    parser.add_argument("--repo-path", default=DEFAULT_REPO_PATH, help="本地 git 仓库路径。")
    parser.add_argument("--source-remote", required=True, help="用于推送源分支的 remote。")
    parser.add_argument("--source-branch", required=True, help="要推送并作为 PR head 的本地源分支。")
    parser.add_argument("--target-remote", required=True, help="PR 目标仓库所在 remote。")
    parser.add_argument("--target-branch", required=True, help="PR base 使用的目标分支名。")
    parser.add_argument("--title", required=True, help="PR 标题。")
    parser.add_argument("--body-file", required=True, help="PR Markdown 正文文件路径。")
    parser.add_argument("--token-env", default=DEFAULT_TOKEN_ENV, help="保存 token 的环境变量名。")
    parser.add_argument("--api-base", default=DEFAULT_API_BASE, help="GitCode API base URL。")
    parser.add_argument("--dry-run", action="store_true", help="只打印脱敏后的 push 和 PR 计划。")
    parser.add_argument("--submit", action="store_true", help="推送分支并提交 PR。")
    args = parser.parse_args()

    if args.dry_run and args.submit:
        parser.error("--dry-run 和 --submit 不能同时使用")

    return args


def run_git(repo_path, args, check=True):
    result = subprocess.run(
        ["git", "-C", repo_path] + args,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if check and result.returncode != 0:
        stderr = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"git {' '.join(args)} 失败：{stderr}")
    return result


def read_body(path):
    with open(path, "r", encoding="utf-8") as body_file:
        return body_file.read()


def require_text(name, value):
    if not value or not value.strip():
        raise ValueError(f"{name} 不能为空")


def parse_gitcode_remote(url):
    value = url.strip()
    patterns = (
        r"^git@gitcode\.com:(?P<path>.+?)(?:\.git)?$",
        r"^ssh://git@gitcode\.com(?::\d+)?/(?P<path>.+?)(?:\.git)?$",
        r"^https://gitcode\.com/(?P<path>.+?)(?:\.git)?$",
        r"^http://gitcode\.com/(?P<path>.+?)(?:\.git)?$",
    )
    for pattern in patterns:
        match = re.match(pattern, value)
        if not match:
            continue
        path = match.group("path").strip("/")
        if path.endswith(".git"):
            path = path[:-4]
        parts = [part for part in path.split("/") if part]
        if len(parts) < 2:
            break
        owner = "/".join(parts[:-1])
        repo = parts[-1]
        return owner, repo, f"{owner}/{repo}"
    raise ValueError(f"不支持的 GitCode remote URL：{url}")


def remote_url(repo_path, remote):
    result = run_git(repo_path, ["remote", "get-url", remote])
    return result.stdout.strip()


def ensure_clean_worktree(repo_path):
    result = run_git(repo_path, ["status", "--porcelain"])
    if result.stdout.strip():
        raise RuntimeError("工作区存在未提交改动")


def ensure_local_branch(repo_path, branch):
    run_git(repo_path, ["rev-parse", "--verify", f"refs/heads/{branch}"])


def ensure_remote_branch(repo_path, remote, branch):
    result = run_git(repo_path, ["ls-remote", "--heads", remote, branch], check=False)
    if result.returncode != 0:
        stderr = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"无法查询目标分支 {remote}/{branch}：{stderr}")
    if not result.stdout.strip():
        raise RuntimeError(f"找不到目标分支：{remote}/{branch}")


def build_plan(args, body):
    source_url = remote_url(args.repo_path, args.source_remote)
    target_url = remote_url(args.repo_path, args.target_remote)
    source_owner, source_repo, source_path = parse_gitcode_remote(source_url)
    target_owner, target_repo, target_path = parse_gitcode_remote(target_url)
    cross_repo = source_path != target_path
    head = f"{source_owner}:{args.source_branch}" if cross_repo else args.source_branch
    payload = {
        "title": args.title,
        "head": head,
        "base": args.target_branch,
        "body": body,
    }
    if cross_repo:
        payload["fork_path"] = source_path

    return {
        "repo_path": args.repo_path,
        "push": ["git", "-C", args.repo_path, "push", args.source_remote, f"{args.source_branch}:{args.source_branch}"],
        "source": {
            "remote": args.source_remote,
            "branch": args.source_branch,
            "owner": source_owner,
            "repo": source_repo,
            "path": source_path,
        },
        "target": {
            "remote": args.target_remote,
            "branch": args.target_branch,
            "owner": target_owner,
            "repo": target_repo,
            "path": target_path,
        },
        "api_base": args.api_base.rstrip("/"),
        "endpoint": f"/api/v5/repos/{target_owner}/{target_repo}/pulls",
        "token_env": args.token_env,
        "mode": "submit" if args.submit else "dry-run",
        "payload": payload,
    }


def extract_pr_ref(response_obj):
    for key in ("html_url", "web_url", "url"):
        value = response_obj.get(key)
        if value:
            return value

    for key in ("number", "id", "iid"):
        value = response_obj.get(key)
        if value is not None:
            return f"{key}: {value}"

    return json.dumps(response_obj, ensure_ascii=False, sort_keys=True)


def push_source_branch(args):
    run_git(args.repo_path, ["push", args.source_remote, f"{args.source_branch}:{args.source_branch}"])


def submit_pr(args, plan):
    token = os.environ.get(args.token_env)
    if not token:
        raise RuntimeError(f"未设置 {args.token_env}")

    api_base = args.api_base.rstrip("/")
    owner = urllib.parse.quote(plan["target"]["owner"], safe="")
    repo = urllib.parse.quote(plan["target"]["repo"], safe="")
    url = f"{api_base}/api/v5/repos/{owner}/{repo}/pulls"
    form_values = {"access_token": token}
    form_values.update(plan["payload"])
    form = urllib.parse.urlencode(form_values).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=form,
        headers={
            "Accept": "application/json",
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/x-www-form-urlencoded",
            "User-Agent": "codex-umdk-pr-skill",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            raw = response.read().decode("utf-8", errors="replace")
            if not raw.strip():
                return {"status": response.status}
            try:
                return json.loads(raw)
            except json.JSONDecodeError:
                return {"status": response.status, "response": raw}
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        sanitized = raw.replace(token, "<redacted>")
        raise RuntimeError(f"HTTP {exc.code}: {sanitized}") from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"请求失败：{exc.reason}") from exc


def main():
    args = parse_args()
    try:
        body = read_body(args.body_file)
        require_text("title", args.title)
        require_text("body", body)
        ensure_clean_worktree(args.repo_path)
        ensure_local_branch(args.repo_path, args.source_branch)
        plan = build_plan(args, body)

        if not args.submit:
            print(json.dumps(plan, ensure_ascii=False, indent=2))
            return 0

        if not os.environ.get(args.token_env):
            raise RuntimeError(f"未设置 {args.token_env}")
        ensure_remote_branch(args.repo_path, args.target_remote, args.target_branch)
        push_source_branch(args)
        result = submit_pr(args, plan)
        print(extract_pr_ref(result))
        return 0
    except Exception as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
