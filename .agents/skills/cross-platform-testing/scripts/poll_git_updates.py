#!/usr/bin/env python3
"""Poll a configured Git upstream without changing the worktree."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


FINAL_UPDATE = "UPDATE_AVAILABLE old={old} new={new}"
FINAL_TIMEOUT = "MONITOR_TIMEOUT baseline={baseline}"
FINAL_STOP = "MONITOR_STOP reason={reason}"


class MonitorError(RuntimeError):
    """A deterministic monitor safety failure."""


def git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        ["git", *args],
        cwd=repo,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
        check=False,
    )
    if check and completed.returncode != 0:
        raise MonitorError(f"git-{args[0]}-failed")
    return completed


def git_text(repo: Path, *args: str) -> str:
    return git(repo, *args).stdout.strip()


def git_path(repo: Path, name: str) -> Path:
    value = git_text(repo, "rev-parse", "--git-path", name)
    path = Path(value)
    return path if path.is_absolute() else repo / path


def ensure_safe(
    repo: Path,
    expected_branch: str | None,
    expected_upstream: str | None,
) -> tuple[str, str]:
    branch = git_text(repo, "symbolic-ref", "--quiet", "--short", "HEAD")
    if not branch:
        raise MonitorError("detached-head")
    upstream = git_text(
        repo,
        "rev-parse",
        "--abbrev-ref",
        "--symbolic-full-name",
        "@{upstream}",
    )
    if not upstream:
        raise MonitorError("missing-upstream")
    if expected_branch and branch != expected_branch:
        raise MonitorError("branch-changed")
    if expected_upstream and upstream != expected_upstream:
        raise MonitorError("upstream-changed")

    operations = (
        "MERGE_HEAD",
        "CHERRY_PICK_HEAD",
        "REVERT_HEAD",
        "BISECT_LOG",
        "rebase-merge",
        "rebase-apply",
    )
    if any(git_path(repo, name).exists() for name in operations):
        raise MonitorError("git-operation-in-progress")
    if git_text(repo, "status", "--porcelain=v1", "--untracked-files=all"):
        raise MonitorError("worktree-not-clean")
    return branch, upstream


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--interval-seconds", type=float, default=10.0)
    parser.add_argument("--heartbeat-seconds", type=float, default=60.0)
    parser.add_argument("--timeout-seconds", type=float, default=3600.0)
    parser.add_argument("--expected-branch")
    parser.add_argument("--expected-upstream")
    parser.add_argument("--baseline")
    args = parser.parse_args()
    if args.interval_seconds <= 0 or args.heartbeat_seconds <= 0 or args.timeout_seconds <= 0:
        parser.error("interval, heartbeat, and timeout must be positive")
    return args


def main() -> int:
    args = parse_args()
    try:
        requested_repo = args.repo.resolve()
        repo = Path(git_text(requested_repo, "rev-parse", "--show-toplevel")).resolve()
        branch, upstream = ensure_safe(repo, args.expected_branch, args.expected_upstream)
        baseline = args.baseline or git_text(repo, "rev-parse", upstream)
        if len(baseline) != 40:
            raise MonitorError("invalid-baseline")
        if git_text(repo, "rev-parse", "HEAD") != baseline:
            raise MonitorError("head-does-not-match-baseline")

        remote = git_text(repo, "config", f"branch.{branch}.remote")
        if not remote:
            raise MonitorError("missing-upstream-remote")

        started = time.monotonic()
        deadline = started + args.timeout_seconds
        next_heartbeat = started + args.heartbeat_seconds

        while True:
            ensure_safe(repo, branch, upstream)
            git(repo, "fetch", "--quiet", remote)
            ensure_safe(repo, branch, upstream)
            current = git_text(repo, "rev-parse", upstream)

            if current != baseline:
                ancestor = git(
                    repo,
                    "merge-base",
                    "--is-ancestor",
                    baseline,
                    current,
                    check=False,
                )
                if ancestor.returncode != 0:
                    raise MonitorError("upstream-history-rewritten")
                print(FINAL_UPDATE.format(old=baseline, new=current), flush=True)
                return 0

            if git_text(repo, "rev-parse", "HEAD") != baseline:
                raise MonitorError("local-head-changed")

            now = time.monotonic()
            if now >= deadline:
                print(FINAL_TIMEOUT.format(baseline=baseline), flush=True)
                return 2
            if now >= next_heartbeat:
                elapsed = int(now - started)
                print(
                    f"MONITOR_WAITING baseline={baseline} elapsed_seconds={elapsed}",
                    flush=True,
                )
                next_heartbeat = now + args.heartbeat_seconds

            time.sleep(min(args.interval_seconds, max(0.0, deadline - now)))
    except (
        MonitorError,
        FileNotFoundError,
        NotADirectoryError,
        subprocess.TimeoutExpired,
    ) as exc:
        reason = str(exc) if isinstance(exc, MonitorError) else type(exc).__name__.lower()
        print(FINAL_STOP.format(reason=reason), flush=True)
        return 3
    except KeyboardInterrupt:
        print(FINAL_STOP.format(reason="interrupted"), flush=True)
        return 130


if __name__ == "__main__":
    sys.exit(main())
