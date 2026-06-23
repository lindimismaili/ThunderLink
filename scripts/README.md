# scripts/

## git-autosync.sh — continuous repo sync with AI conflict resolution

Keeps this repo continuously mirrored with its git remote across your two dev
machines (Mac + Windows). When the other machine pushes ("a sync event"), this
machine **pulls it immediately**; if the pull **conflicts**, the `claude` CLI is
invoked headless to resolve the conflict, then the merge is finished and pushed
so both sides converge.

### What one tick does
1. **Auto-commit** any local changes (so WIP also syncs and nothing is lost).
2. **Fetch** the remote branch.
3. If the remote **advanced**, merge it. On conflict → **Claude resolves** the
   markers, then the merge is committed. If Claude can't fully resolve, the merge
   is **aborted** (tree left clean) and you get a macOS notification.
4. **Push** so the peer receives your commits.

### Run it

```bash
# one-off sync now
scripts/git-autosync.sh once

# foreground poll-loop (Ctrl-C to stop)
scripts/git-autosync.sh watch

# install as a background launchd agent that starts at login and stays running
scripts/git-autosync.sh install
scripts/git-autosync.sh status
scripts/git-autosync.sh uninstall
```

Logs go to `~/Library/Logs/git-autosync.log` (kept **outside** the repo on
purpose, so the daemon never commits its own log in a loop). Tail it with:

```bash
tail -f ~/Library/Logs/git-autosync.log
```

### Configuration (environment variables)

| Variable | Default | Meaning |
|---|---|---|
| `AUTOSYNC_BRANCH` | `main` | branch to keep synced |
| `AUTOSYNC_REMOTE` | `origin` | git remote |
| `AUTOSYNC_INTERVAL` | `15` | seconds between ticks in `watch` mode |
| `AUTOSYNC_CLAUDE` | `claude` | path to the Claude CLI |
| `AUTOSYNC_EXCLUDE` | *(empty)* | space-separated pathspecs to **not** auto-commit, e.g. `"logs/"` |
| `AUTOSYNC_CLAUDE_TIMEOUT` | `300` | max seconds for a Claude resolution |

To change the interval for the installed agent, re-run with the env set:

```bash
AUTOSYNC_INTERVAL=30 scripts/git-autosync.sh install
```

### Notes & caveats
- **"Immediately"** here means *poll-driven* (default every 15s) — there's no
  long-lived process on GitHub to push events to you, so a short poll is the
  practical equivalent. Lower `AUTOSYNC_INTERVAL` for tighter latency.
- This repo currently **tracks `logs/`**. With auto-commit on, those runtime
  logs will produce a commit every time they change while the app runs. If you
  don't want that churn, either set `AUTOSYNC_EXCLUDE="logs/"` or untrack them:
  `git rm -r --cached logs && echo 'logs/' >> .gitignore`.
- Conflict resolution needs the `claude` CLI on `PATH`. The `install` command
  bakes the discovered `git`/`claude` directories into the launchd agent's PATH.
- **Windows side:** this script + the launchd agent are macOS-only. On Windows,
  run `once`/`watch` under Git Bash (e.g. via Task Scheduler) to get the same
  behavior; the `install`/`uninstall`/`status` subcommands don't apply there.
