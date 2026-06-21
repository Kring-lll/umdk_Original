# openEuler Kernel Commit Messages

Use this skill before every commit or commit-message amend in the openEuler
kernel repo. The source rule is the Kernel SIG patch submission guide:
https://gitcode.com/openeuler/community/blob/master/sig/Kernel/补丁提交规范.md

## Required workflow

1. Inspect current repo state and nearby history:
   - `git status --short --branch`
   - `git log --format='%H%n%B%n---END---' -20`
   - For bugfixes, use `git blame`/`git log -- <file>` to find likely
     introducer commits for `Fixes:`.
2. Choose the inclusion type:
   - `mainline inclusion`: backport from upstream Linux mainline.
   - `stable inclusion`: backport from Linux stable.
   - `community inclusion`: open source patch not accepted by mainline.
   - `kunpeng inclusion`, `ascend inclusion`, `dist inclusion`,
     `driver inclusion`, or a local subsystem-specific inclusion already used
     by nearby commits, such as `urma inclusion`.
3. Build the commit message using the template below. Do not omit mandatory
   fields unless the user explicitly asks to violate the convention.
   - Validate `bugzilla:` with `bugzilla: .*?atomgit\.com/(src-)?open_?euler/.*`.
   - If the user provides a GitCode openEuler issue URL, convert it to the
     matching AtomGit URL before writing `bugzilla:`.
4. Verify after commit/amend with `git log -1 --format=fuller` and
   `git diff --check <base>..HEAD` when possible.
5. If the rewritten commit was already pushed, update the remote only with
   `git push --force-with-lease <remote> <branch>`.

## Commit message template

```text
<subsystem>: <concise subject>

<inclusion-type> inclusion
from <version>              # only for mainline/stable/dist backports
commit <full upstream sha>  # only for mainline/stable backports
category: <cleanup|bugfix|performance|feature|doc|other>
bugzilla: <issue-or-bug-url>
CVE: <CVE-id>               # only for CVE fixes; otherwise omit
Reference: <url>            # required for community inclusion

--------------------------------

<original commit log for backports, otherwise a clear changelog>

<additional changelog when useful: why, real product impact,
reproducer/test method, or other context useful for future backports>

Fixes: <12+ char sha> ("original subject")  # for bugfixes when known
Signed-off-by: <real name> <email>
```

## Local conventions

- Keep the subject concise and Linux-style. Honor stricter user constraints
  such as "subject under 60 chars" or "subject starts with `urma:`".
- Do not include internal ticket wording such as "DTS" in the subject/body
  unless the user explicitly requests it.
- `category:` is mandatory. Use `bugfix` for defect fixes.
- `bugzilla:` is mandatory when the patch is tied to an issue. It must use an
  AtomGit openEuler URL matching `bugzilla: .*?atomgit\.com/(src-)?open_?euler/.*`.
  Do not write GitCode issue URLs in this field; normalize them first.
- `CVE:` is optional and should be omitted for non-CVE patches.
- `Reference:` is mandatory for `community inclusion`; omit it for local
  subsystem inclusions unless relevant history shows otherwise.
- `Fixes:` is not in the SIG template, but is standard for bugfix commits in
  this kernel history. Add one or more lines when the introducing commits are
  reasonably identifiable.
- Use the configured author identity for `Signed-off-by` unless the user
  provides a different real name/email.
