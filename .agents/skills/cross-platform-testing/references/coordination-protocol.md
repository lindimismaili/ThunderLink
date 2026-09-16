# Cross-Platform Coordination Protocol

This protocol is the authority for a `cross-platform-testing` session. The main
agent is the coordinator; the coder changes product files; the monitor only
observes the configured upstream. The current branch's upstream is transport,
and root `MEMORY.md` is the bounded human-readable mailbox.

## State machine

The coordinator owns exactly one current state:

- `ACTIVE`: the local participant has a concrete user task or a valid mailbox
  request addressed to its native platform.
- `WAITING`: the peer owns the turn or no request is currently actionable.
- `SYNCING`: a monitor-reported upstream change is being fetched and
  fast-forwarded.
- `STOPPED`: a safety, schema, synchronization, or user-input gate ended the
  workflow. No automatic restart is allowed in the same turn.

Say the state when it is first selected and whenever it changes. When state is
unclear, ask the user whether this participant is Active or Waiting. Do not
infer a state from the existence or absence of `MEMORY.md`.

## Initial Git gate

Complete this gate before spawning the coder or monitor:

1. Confirm the worktree and index contain no changes that could be disturbed.
   Preserve all unrelated work.
2. Resolve a non-empty current branch and its configured upstream. Stop on a
   detached HEAD or missing upstream.
3. Check Git administrative state and stop if a merge, rebase, cherry-pick,
   revert, or bisect is unfinished.
4. Fetch the configured upstream, then compute ahead and behind counts. Both
   must be zero. Do not repair, pull, commit, push, switch, or merge during this
   first gate. Tell the user exactly what must be reconciled and stop.
5. Record the branch, upstream, and 40-character synchronized commit as the
   session baseline.

This strict first gate prevents an agent from mistaking a stale checkout,
unpushed local commit, or unresolved operation for a valid coordination state.

## Participants and mailbox invariants

- One open session has at most one claimed Mac participant and one claimed
  Windows participant. Generate a UUID for each agent ID and session ID.
- Only the platform named by `TURN` may perform product work. `TURN: NONE`
  means both participants wait unless the user supplies a new concrete task.
- Each platform replaces only its own reply and request slots. Preserve the
  peer's slots byte-for-byte unless the user explicitly authorizes takeover.
- `REVISION` starts at `1` and increases by exactly one per committed mailbox
  update. Request IDs are unique within the session, such as `mac-0001` or
  `windows-0001`. Process a request once and only when the local reply does not
  already name it in `REPLY-TO`.
- `STATUS` is `OPEN`, `BLOCKED`, or `COMPLETE`; `TURN` is `MAC`, `WINDOWS`, or
  `NONE`. Use uppercase values exactly.
- Keep UTC timestamps in `YYYY-MM-DDTHH:MM:SSZ` form and retain all four
  message slots, even when their content is `NONE`.
- An actionable peer request remains `OPEN` and assigns `TURN` to that peer.
  Use `BLOCKED` only when user input or an external state change is required;
  set `TURN: NONE` and stop polling.

## Creating or joining a session

Only an `ACTIVE` coordinator may create root `MEMORY.md`, using
[the template](MEMORY.template.md). A `WAITING` coordinator must not
create, initialize, claim, or commit the mailbox when it is absent. It waits for
the active peer's committed mailbox to arrive through Git.

When Active creates a session:

1. Fill every placeholder. Claim only the native platform's agent ID and leave
   the peer `UNCLAIMED`.
2. Set `TURN` from the concrete task and planned handoff. Perform the task
   before committing the first handoff.
3. Stage only intended task files, the permitted native log when necessary,
   and `MEMORY.md`. Follow repository commit-message rules and push normally.

To join an arrived session, validate schema, session ID, branch, upstream,
revision, status, turn, and existing claims before inserting the local agent
ID during the next Active handoff. Refuse an `OPEN` session when the local
platform is claimed by a different agent ID. Takeover requires explicit user
authorization.

## Coder lifecycle

The coordinator starts the coder with a readiness-only task. The coder returns
exactly:

```text
CODER_READY
```

When activated, the coder receives the current commit, native platform,
concrete request, file ownership, applicable instructions, and acceptance
checks. It must not touch Git history or `MEMORY.md`. It returns one of:

```text
CODER_COMPLETE
<changed files, commands, results, failures, and unverified gates>
```

```text
CODER_BLOCKED reason=<concise reason>
```

The coordinator inspects the actual worktree and validation evidence before
accepting either response or changing state.

## Monitor lifecycle and polling

The monitor is read-only with respect to tracked and untracked files. It may
run only Git inspection and fetch operations. It never pulls, commits, pushes,
switches branches, edits product files, or creates/reads/edits `MEMORY.md`.

From the skill directory, the monitor runs:

```text
<python-interpreter> scripts/poll_git_updates.py --repo <project-root> --interval-seconds 10 --heartbeat-seconds 60 --timeout-seconds 3600 --expected-branch <branch> --expected-upstream <upstream> --baseline <40-char-commit>
```

Use the repository's selected Python interpreter. The helper performs a
bounded one-hour loop, fetches every 10 seconds, prints a heartbeat at least
once per minute, and leaves the worktree unchanged. The monitor relays its
terminal line exactly, then becomes idle:

```text
UPDATE_AVAILABLE old=<40-char-commit> new=<40-char-commit>
MONITOR_TIMEOUT baseline=<40-char-commit>
MONITOR_STOP reason=<reason>
```

On `MONITOR_TIMEOUT`, the coordinator may start another bounded window only
while the user still wants waiting to continue. On `MONITOR_STOP`, enter
`STOPPED` and report the reason.

## Synchronizing an update

On `UPDATE_AVAILABLE`, the coordinator enters `SYNCING`:

1. Confirm the branch and upstream still match the baseline, no Git operation
   is in progress, and the worktree and index are safe.
2. Fetch the upstream again. Require the previous local commit to be an
   ancestor of the new upstream and require local ahead count to be zero.
3. Fast-forward only. Do not create a merge commit, rebase, force, stash,
   discard changes, or resolve divergence automatically.
4. If any command fails or any invariant differs, enter `STOPPED`, terminate
   both subagent workflows, and tell the user the exact command and condition.
5. After the fast-forward, require root `MEMORY.md`. Validate all fields and
   require a new unprocessed request addressed to the local platform. If it is
   absent or invalid, enter `STOPPED` as an invalid handoff.
6. Read the mailbox message, enter `ACTIVE`, and give the scoped request to the
   idle coder.

## Active handoff

After accepting `CODER_COMPLETE`:

1. Run any coordinator-level inspection and locally available validation.
2. Replace this platform's reply slot. Set `REPLY-TO` to the processed peer
   request ID, or `NONE` for the originating user task. Include changed files,
   commands, results, failures, relevant log excerpts, and unverified gates.
3. Replace this platform's request slot:
   - If peer work remains, create a new request ID with exact actions and
     acceptance evidence, then set `TURN` to the peer.
   - If only final acknowledgement remains, request `verify-completion` from
     the originating platform and set `TURN` to it.
   - Use `ID NONE` and body `NONE` only when no peer work remains.
4. For an existing committed mailbox, increment `REVISION`. For the first
   handoff of a newly created session, retain `REVISION: 1`. Update
   `UPDATED_AT`, validate all slots, stage only scoped files, commit, and push.
5. A rejected push or any other synchronization failure immediately enters
   `STOPPED`; do not resolve it automatically.
6. After a successful push, enter `WAITING`, keep the coder idle, and reactivate
   the monitor with the new commit as its baseline.

## Completion handshake

The responder to the last substantive request reports its evidence and sends a
`verify-completion` request to the originating platform. The origin then:

1. Synchronizes, reviews the reply and repository state, and runs final local
   checks.
2. Replaces its reply slot with acknowledgement and names the
   `verify-completion` request ID in `REPLY-TO`.
3. Sets its request to `ID NONE` with body `NONE`, `STATUS: COMPLETE`, and
   `TURN: NONE`; increments `REVISION` and updates `UPDATED_AT`.
4. Commits and pushes the final mailbox state, leaving the latest four slots in
   root `MEMORY.md` for the next invocation.

The peer may fast-forward the final commit and stop. A later explicit Active
invocation may replace a `COMPLETE` session; never append unbounded history
because Git retains prior revisions.

## Failure and evidence rules

- Never put secrets, credentials, recovery material, private screen contents,
  or unredacted personal data in the mailbox, Git history, logs, or prompts.
- Summarize large logs and provide paths plus timestamps. Obey
  `logs/README.md` for per-device tracked logs.
- On crashes, protocol mismatches, persistent black frames, input runaway, or
  display-restoration failures, stop unsafe testing and preserve evidence.
- Report native OS, tested commit, validation tier, commands and results, and
  every skipped device, permission, codec, network, or physical-link gate.
