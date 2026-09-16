---
name: cross-platform-testing
description: Explicitly coordinate Mac and Windows work through a main coordinator, a coding agent, a read-only Git monitor, and root MEMORY.md handoffs.
---

```toml
id = "ac5cfc2c00a94dc3febfd2051c278579"
```

# Cross-Platform Testing

Coordinate one native macOS participant and one native Windows participant
through the current branch's configured Git upstream. Use this skill only when
the user explicitly invokes it.

## Skill self-test

When the user asks to test, smoke-test, or verify this skill itself, read
[the test procedure](references/TEST.md) completely and execute it instead of
the normal cross-platform coordination workflow. After the test subagent
returns or fails to start, report success or failure to the user and end the
turn immediately. Do not continue into Git preflight, Active mode, Waiting
mode, product work, or handoff polling during a skill self-test.

## First invocation

1. Read the nearest `AGENTS.md` and
   [the coordination protocol](references/coordination-protocol.md) completely.
2. Detect the native host with `uname -s` on macOS or
   `[System.Environment]::OSVersion.Platform` in Windows PowerShell. WSL is not
   a Windows participant.
3. Determine mode from the request:
   - Concrete plans, implementation, diagnosis, or testing mean **Active**.
   - Waiting, watching, polling, or being the other device mean **Waiting**.
   - If the request does not make the mode unambiguous, report the mode as
     undetermined and explicitly ask the user to choose Active or Waiting
     before creating files, polling, or delegating.
4. State to the user:
   - the cross-device goal;
   - the detected native host OS;
   - the proposed current mode: **Active** or **Waiting**;
   - that Git synchronization and unfinished-operation checks are in progress.
5. Run the protocol's initial Git gate. Continue only when the worktree and
   index are safe, the branch has an upstream, local and upstream are exactly
   synchronized, and no merge, rebase, cherry-pick, revert, or bisect remains.
   If any check fails, report the exact condition and stop so the user can fix
   it first.

## Coordinator and subagents

The invoking main agent is the coordinator and owns the current state, Git
synchronization, `MEMORY.md`, commits, pushes, user updates, and final decision
making. It does not delegate that accountability.

After mode and the initial Git gate are confirmed, spawn exactly these two
project subagents when the harness supports them:

- `cross-platform-coder`: use the project definition in
  this skill's `agents/cross-platform-coder.toml`. It is GPT-5.6 Sol with high
  reasoning. Start it with a readiness task; it must return `CODER_READY` and
  become idle until the coordinator supplies the current scoped task.
- `cross-platform-monitor`: use the project definition in
  this skill's `agents/cross-platform-monitor.toml`. It is GPT-5.6 Luna with low
  reasoning and must remain read-only. In Waiting mode, tell it to run
  `scripts/poll_git_updates.py` from this skill with a 10-second check interval,
  60-second heartbeat, and 3600-second timeout. In Active mode, have it return
  `MONITOR_READY` and leave it idle until the handoff has been pushed.

If the named definitions are not available in the current session, use the
closest supported subagent configurations with those exact model and reasoning
settings and report the fallback. Do not silently change either role. If the
harness cannot create both agents, stop and tell the user.

## State ownership

At every transition, the coordinator must know and state one of these modes:
`ACTIVE`, `WAITING`, `SYNCING`, or `STOPPED`. The coder never decides the mode.
The monitor only reports repository observations. A subagent completion does
not change mode until the coordinator validates it.

- In `ACTIVE`, send the coder the concrete request, applicable repository
  instructions, the current commit, the native host, and required validation.
  The coder edits and tests but never pulls, commits, pushes, switches branches,
  or edits `MEMORY.md`.
- In `WAITING`, the coder remains idle and the monitor polls. A waiting
  participant must not create `MEMORY.md` when it is absent; the file must
  arrive through an upstream commit from the active participant.
- On `UPDATE_AVAILABLE`, enter `SYNCING`, stop product work, fetch, and
  fast-forward only. If synchronization fails for any reason, enter `STOPPED`,
  end the coordination workflow, and tell the user the exact failure. Never
  rebase, merge, force-push, or auto-resolve a divergence.
- After a successful fast-forward, require root `MEMORY.md`, validate it, read
  the message addressed to this platform, enter `ACTIVE`, and then activate the
  coder with that request. An update without a valid addressed mailbox message
  is a stopped invalid handoff.
- Accept coder results only as `CODER_COMPLETE` with its evidence or
  `CODER_BLOCKED` with a reason. Review its changes and validation before
  updating the mailbox.
- The coordinator updates `MEMORY.md`, stages only scoped files, commits, and
  pushes according to repository rules. After a successful push, enter
  `WAITING`, reactivate the monitor for a new one-hour polling window, and keep
  the coder idle.

For ThunderLink product work, read the applicable native testing guide:
[Windows](../../../docs/WINDOWS_TESTING.md) or
[macOS](../../../docs/MAC_TESTING.md). Read
[architecture](../../../docs/ARCHITECTURE.md) and the coupled protocol files
when work crosses system or wire boundaries. Never claim cross-device success
from one machine.

Never include credentials, secrets, private screen contents, or unredacted
personal data in `MEMORY.md`, commits, logs, or subagent messages.

Explicit invocation authorizes only the session-scoped fetches,
fast-forward-only pulls, task commits, and pushes described by the protocol. It
does not authorize force pushes, rebases, merge commits, branch creation or
switching, pull requests, publication, or unrelated changes.
