#!/usr/bin/env python3
"""根据生成的 kernel URMA issue 草稿创建 GitCode issue。"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


DEFAULT_API_BASE = "https://api.gitcode.com"
DEFAULT_OWNER = "openeuler"
DEFAULT_REPO = "kernel"
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
        description="使用标题和 Markdown 正文文件创建 GitCode issue。"
    )
    parser.add_argument("--owner", default=DEFAULT_OWNER, help="GitCode owner 或组织名。")
    parser.add_argument("--repo", default=DEFAULT_REPO, help="GitCode 仓库名。")
    parser.add_argument("--title", required=True, help="Issue 标题。")
    parser.add_argument("--body-file", required=True, help="Issue Markdown 正文文件路径。")
    parser.add_argument("--token-env", default=DEFAULT_TOKEN_ENV, help="保存 token 的环境变量名。")
    parser.add_argument("--api-base", default=DEFAULT_API_BASE, help="GitCode API base URL。")
    parser.add_argument("--dry-run", action="store_true", help="只打印脱敏后的请求 payload。")
    parser.add_argument("--submit", action="store_true", help="提交 issue 到 GitCode。")
    args = parser.parse_args()

    if args.dry_run and args.submit:
        parser.error("--dry-run 和 --submit 不能同时使用")

    return args


def read_body(path):
    with open(path, "r", encoding="utf-8") as body_file:
        return body_file.read()


def require_text(name, value):
    if not value or not value.strip():
        raise ValueError(f"{name} 不能为空")


def sanitized_payload(args, body):
    return {
        "owner": args.owner,
        "repo": args.repo,
        "title": args.title,
        "body": body,
        "api_base": args.api_base.rstrip("/"),
        "token_env": args.token_env,
        "mode": "submit" if args.submit else "dry-run",
    }


def extract_issue_ref(response_obj):
    for key in ("html_url", "web_url", "url"):
        value = response_obj.get(key)
        if value:
            return value

    for key in ("number", "id", "iid"):
        value = response_obj.get(key)
        if value is not None:
            return f"{key}: {value}"

    return json.dumps(response_obj, ensure_ascii=False, sort_keys=True)


def submit_issue(args, body):
    token = os.environ.get(args.token_env)
    if not token:
        raise RuntimeError(f"未设置 {args.token_env}")

    api_base = args.api_base.rstrip("/")
    url = f"{api_base}/api/v5/repos/{urllib.parse.quote(args.owner)}/issues"
    form = urllib.parse.urlencode(
        {
            "access_token": token,
            "repo": args.repo,
            "title": args.title,
            "body": body,
        }
    ).encode("utf-8")

    request = urllib.request.Request(
        url,
        data=form,
        headers={
            "Accept": "application/json",
            "Authorization": f"Bearer {token}",
            "Content-Type": "application/x-www-form-urlencoded",
            "User-Agent": "codex-umdk-issue-skill",
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

        if not args.submit:
            print(json.dumps(sanitized_payload(args, body), ensure_ascii=False, indent=2))
            return 0

        result = submit_issue(args, body)
        print(extract_issue_ref(result))
        return 0
    except Exception as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
