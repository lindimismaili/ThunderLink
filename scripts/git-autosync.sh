#!/usr/bin/env bash
#
# git-autosync.sh — continuously mirror this repo with its git remote, with
# AI-assisted conflict resolution.
#
# Built for sharing one repo across two dev machines (this project is developed
# on both Mac and Windows). When the *other* machine pushes ("a sync event"),
# this machine pulls it immediately. If the pull conflicts, the `claude` CLI is
# invoked headless to resolve the conflict, then the merge is finished and
# pushed so both sides converge.
#
# Each tick:
#   1. Auto-commit any local changes (so nothing is lost and WIP syncs too).
#   2. Fetch the remote branch.
#   3. If the remote advanced, merge it. On conflict → resolve with Claude.
#   4. Push so the peer receives our commits.
#
# Usage:
#   scripts/git-autosync.sh once       # run a single sync tick now
#   scripts/git-autosync.sh watch      # poll-loop forever (used by launchd)
#   scripts/git-autosync.sh install    # install + start the launchd agent (macOS)
#   scripts/git-autosync.sh uninstall  # stop + remove the launchd agent
#   scripts/git-autosync.sh status     # show agent state + recent log
#
# Config (environment overrides):
#   AUTOSYNC_BRANCH    branch to sync                      (default: main)
#   AUTOSYNC_REMOTE    remote name                         (default: origin)
#   AUTOSYNC_INTERVAL  seconds between ticks in watch mode (default: 15)
#   AUTOSYNC_CLAUDE    path to the claude CLI              (default: claude)
#   AUTOSYNC_EXCLUDE   space-separated pathspecs to NOT auto-commit
#                      (default: empty = commit everything; e.g. "logs/")
#   AUTOSYNC_LOG       daemon log file (kept OUTSIDE the repo on purpose)
#
set -uo pipefail

# ── Resolve paths ───────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${AUTOSYNC_REPO:-$(cd "$SCRIPT_DIR/.." && pwd)}"

BRANCH="${AUTOSYNC_BRANCH:-main}"
REMOTE="${AUTOSYNC_REMOTE:-origin}"
INTERVAL="${AUTOSYNC_INTERVAL:-15}"
CLAUDE_BIN="${AUTOSYNC_CLAUDE:-claude}"
EXCLUDE="${AUTOSYNC_EXCLUDE:-}"
CLAUDE_TIMEOUT="${AUTOSYNC_CLAUDE_TIMEOUT:-300}"

LABEL="com.thunderlink.autosync"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"

# Keep the daemon's own log OUTSIDE the repo so it never triggers a self-commit
# loop (this repo tracks logs/, which would otherwise feed the sync endlessly).
DEFAULT_LOG="$HOME/Library/Logs/git-autosync.log"
LOG_FILE="${AUTOSYNC_LOG:-$DEFAULT_LOG}"
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true

# Lock so a manual `once` and the watch loop never run a tick concurrently.
_lock_key="$(printf '%s' "$REPO_DIR" | shasum 2>/dev/null | cut -c1-12)"
LOCK_DIR="${TMPDIR:-/tmp}/git-autosync-${_lock_key:-default}.lock"

HOSTNAME_SHORT="$(hostname -s 2>/dev/null || hostname)"

# ── Helpers ─────────────────────────────────────────────────────────────────
log() {
    # Timestamped line to both the log file and stdout (launchd captures stdout).
    local line
    line="$(date '+%Y-%m-%d %H:%M:%S') [$HOSTNAME_SHORT] $*"
    printf '%s\n' "$line" >>"$LOG_FILE" 2>/dev/null
    printf '%s\n' "$line"
}

notify() {
    # Best-effort macOS notification; silent elsewhere.
    osascript -e "display notification \"$1\" with title \"git-autosync\"" >/dev/null 2>&1 || true
}

git_q() { git -C "$REPO_DIR" "$@"; }

acquire_lock() {
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        printf '%s' "$$" >"$LOCK_DIR/pid"
        return 0
    fi
    # Steal a stale lock whose owner is gone.
    local oldpid
    oldpid="$(cat "$LOCK_DIR/pid" 2>/dev/null)"
    if [ -n "$oldpid" ] && ! kill -0 "$oldpid" 2>/dev/null; then
        rm -rf "$LOCK_DIR" 2>/dev/null
        if mkdir "$LOCK_DIR" 2>/dev/null; then
            printf '%s' "$$" >"$LOCK_DIR/pid"
            return 0
        fi
    fi
    return 1
}

release_lock() { rm -rf "$LOCK_DIR" 2>/dev/null || true; }

# Run a command with a wall-clock timeout (no coreutils `timeout` dependency).
run_with_timeout() {
    local secs="$1"; shift
    "$@" &
    local pid=$!
    (
        sleep "$secs"
        kill -0 "$pid" 2>/dev/null && kill -TERM "$pid" 2>/dev/null
        sleep 5
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    ) &
    local watchdog=$!
    wait "$pid" 2>/dev/null
    local rc=$?
    kill "$watchdog" 2>/dev/null
    wait "$watchdog" 2>/dev/null
    return $rc
}

# Files still carrying conflict markers among the unmerged set.
unresolved_conflicts() {
    local f
    while IFS= read -r f; do
        [ -z "$f" ] && continue
        if grep -nE '^(<<<<<<< |>>>>>>> )' "$REPO_DIR/$f" >/dev/null 2>&1; then
            printf '%s\n' "$f"
        fi
    done < <(git_q diff --name-only --diff-filter=U)
}

# ── Claude conflict resolution ──────────────────────────────────────────────
resolve_with_claude() {
    local conflicted
    conflicted="$(git_q diff --name-only --diff-filter=U)"
    [ -z "$conflicted" ] && return 0

    if ! command -v "$CLAUDE_BIN" >/dev/null 2>&1 && [ ! -x "$CLAUDE_BIN" ]; then
        log "ERROR: claude CLI not found ('$CLAUDE_BIN'); cannot auto-resolve"
        return 1
    fi

    log "conflict in: $(echo "$conflicted" | tr '\n' ' ')— invoking Claude"

    local prompt
    prompt="You are resolving git merge conflicts in the repository at $REPO_DIR.
These files contain conflict markers (<<<<<<<, =======, >>>>>>>):
$conflicted

For EACH file, resolve every conflict so the final content is correct and, for
source code, still compiles. Combine both sides' intent where they touch
different things; do not blindly discard either side. Remove ALL conflict
markers. Only edit the conflicted files — do NOT run git, do NOT commit, do NOT
touch unrelated files. When finished, no conflict markers may remain."

    # acceptEdits auto-applies file edits; no Bash is allowed, so the daemon
    # stays in control of all git operations.
    ( cd "$REPO_DIR" && run_with_timeout "$CLAUDE_TIMEOUT" \
        "$CLAUDE_BIN" -p "$prompt" --permission-mode acceptEdits ) >>"$LOG_FILE" 2>&1
    local rc=$?
    [ "$rc" -ne 0 ] && log "Claude exited non-zero ($rc)"

    local remaining
    remaining="$(unresolved_conflicts)"
    if [ -n "$remaining" ]; then
        log "ERROR: markers remain after Claude in: $(echo "$remaining" | tr '\n' ' ')"
        return 1
    fi
    return 0
}

# ── One sync tick ───────────────────────────────────────────────────────────
tick() {
    cd "$REPO_DIR" || { log "repo not found: $REPO_DIR"; return 1; }

    local cur
    cur="$(git_q rev-parse --abbrev-ref HEAD 2>/dev/null)"
    if [ "$cur" != "$BRANCH" ]; then
        log "on '$cur', not '$BRANCH' — skipping"
        return 0
    fi
    # Don't fight an in-progress merge/rebase started by hand.
    if git_q rev-parse -q --verify MERGE_HEAD >/dev/null 2>&1; then
        log "a merge is already in progress — skipping"
        return 0
    fi

    # 1. Auto-commit local changes (honor optional excludes).
    if [ -n "$(git_q status --porcelain)" ]; then
        if [ -n "$EXCLUDE" ]; then
            local specs=() p
            for p in $EXCLUDE; do specs+=( ":(exclude)$p" ); done
            git_q add -A -- . "${specs[@]}"
        else
            git_q add -A
        fi
        if ! git_q diff --cached --quiet; then
            git_q commit -m "autosync: $HOSTNAME_SHORT $(date '+%Y-%m-%d %H:%M:%S')" >>"$LOG_FILE" 2>&1
            log "auto-committed local changes"
        fi
    fi

    # 2. Fetch.
    if ! git_q fetch "$REMOTE" "$BRANCH" >>"$LOG_FILE" 2>&1; then
        log "fetch failed (offline?) — will retry"
        return 0
    fi

    local local_sha remote_sha base
    local_sha="$(git_q rev-parse HEAD)"
    remote_sha="$(git_q rev-parse "$REMOTE/$BRANCH" 2>/dev/null)" || return 0
    base="$(git_q merge-base HEAD "$REMOTE/$BRANCH" 2>/dev/null)"

    [ "$local_sha" = "$remote_sha" ] && return 0  # already in sync

    # 3. Integrate remote if it moved past our merge base.
    if [ "$remote_sha" != "$base" ]; then
        log "remote advanced — merging $REMOTE/$BRANCH"
        if ! git_q merge --no-edit "$REMOTE/$BRANCH" >>"$LOG_FILE" 2>&1; then
            if [ -n "$(git_q diff --name-only --diff-filter=U)" ]; then
                if resolve_with_claude; then
                    git_q add -A
                    git_q commit --no-edit >>"$LOG_FILE" 2>&1
                    log "conflicts resolved by Claude; merge committed"
                    notify "Merge conflict auto-resolved by Claude"
                else
                    log "ERROR: could not auto-resolve — aborting merge, leaving tree clean"
                    git_q merge --abort 2>/dev/null
                    notify "Autosync: unresolved conflict — manual fix needed"
                    return 1
                fi
            else
                log "merge failed without conflict files — aborting"
                git_q merge --abort 2>/dev/null
                return 1
            fi
        fi
    fi

    # 4. Push if we now have commits the remote lacks.
    if [ "$(git_q rev-parse HEAD)" != "$(git_q rev-parse "$REMOTE/$BRANCH" 2>/dev/null)" ]; then
        if git_q push "$REMOTE" "$BRANCH" >>"$LOG_FILE" 2>&1; then
            log "pushed to $REMOTE/$BRANCH"
        else
            log "push rejected (remote moved) — will merge + retry next tick"
        fi
    fi
    return 0
}

run_once() {
    if acquire_lock; then
        trap 'release_lock' EXIT
        tick
        local rc=$?
        release_lock
        trap - EXIT
        return $rc
    else
        log "another autosync tick holds the lock — skipping"
        return 0
    fi
}

watch_loop() {
    log "autosync watching '$REPO_DIR' ($REMOTE/$BRANCH) every ${INTERVAL}s"
    trap 'release_lock; exit 0' INT TERM
    while true; do
        run_once
        sleep "$INTERVAL"
    done
}

# ── launchd integration (macOS) ─────────────────────────────────────────────
write_plist() {
    local git_dir claude_dir extra_path
    git_dir="$(dirname "$(command -v git)")"
    claude_dir=""
    if command -v "$CLAUDE_BIN" >/dev/null 2>&1; then
        claude_dir="$(dirname "$(command -v "$CLAUDE_BIN")")"
    fi
    # Bake in the dirs holding git + claude so launchd's bare PATH can find them.
    extra_path="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$git_dir:$claude_dir"

    mkdir -p "$(dirname "$PLIST")"
    cat >"$PLIST" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$SCRIPT_DIR/git-autosync.sh</string>
        <string>watch</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>$extra_path</string>
        <key>AUTOSYNC_INTERVAL</key>
        <string>$INTERVAL</string>
        <key>AUTOSYNC_BRANCH</key>
        <string>$BRANCH</string>
        <key>AUTOSYNC_REMOTE</key>
        <string>$REMOTE</string>
    </dict>
    <key>WorkingDirectory</key>
    <string>$REPO_DIR</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$HOME/Library/Logs/git-autosync.out.log</string>
    <key>StandardErrorPath</key>
    <string>$HOME/Library/Logs/git-autosync.err.log</string>
</dict>
</plist>
PLIST_EOF
    log "wrote launchd plist: $PLIST"
}

launchctl_load() {
    local domain
    domain="gui/$(id -u)"
    launchctl bootout "$domain/$LABEL" >/dev/null 2>&1 || true
    if launchctl bootstrap "$domain" "$PLIST" 2>/dev/null; then
        launchctl enable "$domain/$LABEL" 2>/dev/null || true
    else
        # Fall back to the legacy interface on older macOS.
        launchctl unload -w "$PLIST" >/dev/null 2>&1 || true
        launchctl load -w "$PLIST"
    fi
}

cmd_install() {
    write_plist
    launchctl_load
    log "installed + started '$LABEL' (interval ${INTERVAL}s, branch $BRANCH)"
    echo "Autosync is running. Tail the log with:"
    echo "  tail -f \"$LOG_FILE\""
}

cmd_uninstall() {
    local domain
    domain="gui/$(id -u)"
    launchctl bootout "$domain/$LABEL" >/dev/null 2>&1 \
        || launchctl unload -w "$PLIST" >/dev/null 2>&1 || true
    rm -f "$PLIST"
    log "uninstalled '$LABEL'"
}

cmd_status() {
    local domain
    domain="gui/$(id -u)"
    echo "== launchctl =="
    launchctl print "$domain/$LABEL" 2>/dev/null | grep -E "state|pid|program|last exit" \
        || echo "  not loaded"
    echo ""
    echo "== recent log ($LOG_FILE) =="
    tail -n 20 "$LOG_FILE" 2>/dev/null || echo "  (no log yet)"
}

# ── Dispatch ────────────────────────────────────────────────────────────────
case "${1:-once}" in
    once|--once)         run_once ;;
    watch|--watch)       watch_loop ;;
    install|--install)   cmd_install ;;
    uninstall|--uninstall) cmd_uninstall ;;
    status|--status)     cmd_status ;;
    *)
        echo "usage: $0 {once|watch|install|uninstall|status}" >&2
        exit 2
        ;;
esac
