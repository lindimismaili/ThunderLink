---
name: cross-platform-testing
description: Explicitly coordinate one macOS agent and one Windows agent to implement or test ThunderLink through turn-based Git handoffs and root MEMORY.md messages.
---

```toml
id = "ac5cfc2c00a94dc3febfd2051c278579"
```

# Cross-Platform Testing

Coordinate one native macOS agent and one native Windows agent through the
current branch's Git upstream. Use this skill only when the user explicitly
invokes it.

## Start

1. Read the nearest `AGENTS.md` and inspect `git status --short`.
2. Detect the native host. On macOS run `uname -s`; on Windows use
   `[System.Environment]::OSVersion.Platform` in PowerShell. WSL is not a
   Windows participant.
3. Read [the coordination protocol](references/coordination-protocol.md)
   completely before changing files, pulling, committing, pushing, or polling.
   This is a bundled part of the invoked skill: resolve it from this skill
   directory and read it even when the target worktree is elsewhere or access
   to other external paths is restricted.
4. Select the mode from the user's request:
   - A concrete implementation, diagnosis, or testing request starts in
     **Active Mode**.
   - A request to wait, watch, or poll starts in **Waiting Mode**.
   - If neither is clear, ask the user to choose Active Mode or Waiting Mode.

## Work

- In Active Mode, perform only the user's requested work and the validation
  required by the target repository. When the target is ThunderLink, read the applicable native
  guide: [Windows testing](../../../docs/WINDOWS_TESTING.md) or
  [macOS testing](../../../docs/MAC_TESTING.md). Read
  [architecture](../../../docs/ARCHITECTURE.md) and the coupled protocol files
  when the change crosses system or wire boundaries. In an isolated fixture
  without those project documents, follow the fixture's own instructions and
  tests and report that ThunderLink-native validation does not apply.
- At a handoff, report the exact commit and working-tree state, commands and
  results, highest verification tier reached, and every unverified native or
  physical-device gate. Never claim cross-device success from one machine.
- In Waiting Mode, run one bounded poll cycle at a time. Do not start an
  unbounded shell loop or leave an unmanaged background process.
- Never include credentials, secrets, private screen contents, or unredacted
  personal data in `MEMORY.md`, commits, or logs.

The explicit invocation authorizes only the session-scoped fetches,
fast-forward pulls, commits, and pushes described by the protocol. It does not
authorize force pushes, rebases, merge commits, branch creation or switching,
pull requests, publication, or unrelated changes.
