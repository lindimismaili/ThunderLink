# Cross-Platform Coordination Protocol

This protocol is the authority for a `cross-platform-testing` session. It uses
the current branch's configured upstream as transport and root `MEMORY.md` as
the bounded, human-readable mailbox.

## Invariants

- One open session has at most one claimed Mac participant and one claimed
  Windows participant. Generate a UUID for each agent ID and session ID.
- Only the platform named by `TURN` may enter Active Mode and change product
  files. `TURN: NONE` means both participants wait unless the user gives one of
  them a new concrete task.
- Each agent replaces only its own reply and request slots. Preserve the peer's
  slots byte-for-byte unless resolving a user-authorized takeover.
- `REVISION` starts at `1` and increases by exactly one per committed mailbox
  update. Request IDs are unique within the session, such as `mac-0001` or
  `windows-0001`. A request is processed once, only by the addressed turn, and
  only when the local reply does not already name it in `REPLY-TO`.
- `STATUS` is `OPEN`, `BLOCKED`, or `COMPLETE`; `TURN` is `MAC`, `WINDOWS`, or
  `NONE`. Use uppercase values exactly.
- Keep timestamps in UTC ISO 8601 form (`YYYY-MM-DDTHH:MM:SSZ`). Keep the four
  message slots even when their content is `NONE`.
- An actionable peer request remains `OPEN` and assigns `TURN` to that peer.
  Use `BLOCKED` only when user input or an external state change is required;
  set `TURN: NONE` and stop polling.

## Git preflight

Before joining or advancing a session:

1. Confirm the worktree and index do not contain unrelated changes that a
   pull, task commit, or handoff could disturb. Preserve all unrelated work.
2. Resolve the non-empty current branch and its configured upstream. Stop on a
   detached HEAD, missing upstream, or an upstream that does not match
   `MEMORY.md`.
3. Fetch the configured upstream. Compare local and upstream ancestry. Update
   only with a fast-forward-only pull. Never rebase, create a merge commit,
   force push, switch branches, or discard changes.
4. If local and upstream have diverged, or a push is rejected, fetch and
   inspect the remote `MEMORY.md`. Do not resolve the race automatically. Mark
   the local attempt as stopped and tell the user what differs.
5. Follow the repository's commit-message instructions. Stage only the task's
   intended files, this agent's permitted log when needed, and `MEMORY.md`.
   Never rewrite or stage the other device's tracked log.

The current branch and upstream are a user-selected session boundary. A
participant on another branch or upstream must stop instead of switching.

## Creating or joining a session

Use [the template](../assets/MEMORY.template.md) when root `MEMORY.md` does not
exist or contains a `COMPLETE` session that may be replaced.

- Fill every placeholder. Claim only the native platform's agent ID and leave
  the other as `UNCLAIMED`.
- A first participant in Waiting Mode creates an `OPEN` session with
  `TURN: NONE`, commits and pushes it, then waits.
- A first participant in Active Mode creates the session, performs the user's
  work, and hands a request to the other platform as described below.
- To join, validate the schema, session ID, branch, upstream, revision, and
  existing claims before inserting the local agent ID. A joining waiter keeps
  `TURN: NONE`; a joining agent with a concrete user task may become Active.
- Refuse an `OPEN` session when the local platform is claimed by a different
  agent ID. A replacement agent may take over only after explicit user
  authorization. Never silently overwrite an open participant.

Commit and push the claim before polling or doing peer-requested work. If two
first participants race, the rejected pusher fetches, validates the winning
session, and joins it; it does not overwrite or force push.

## Active Mode and handoff

1. Confirm that either the user supplied the concrete task or `TURN` targets
   this platform with an unprocessed request.
2. Fast-forward before editing. Inspect the peer request, relevant source,
   repository instructions, and native testing guide.
3. Implement or test only the scoped request. Run portable tests plus all
   locally available native checks. Physical-device checks require actual
   evidence from both devices.
4. Replace this platform's reply slot with a concise but complete report. Set
   `REPLY-TO` to the processed peer request ID, or `NONE` for the originating
   user task. Include changed files, commands, outcomes, relevant log excerpts,
   failures, and unverified gates.
5. Replace this platform's request slot:
   - If peer work remains, create a new request ID and state exact actions and
     acceptance evidence. Set `TURN` to the peer platform.
   - If only final acknowledgement remains, request `verify-completion` from
     the originating platform and set `TURN` to it.
   - Use `ID NONE` and body `NONE` only when no peer work is pending.
6. For an existing committed mailbox, increment `REVISION`; for the first
   committed handoff of a newly created session, keep the template's
   `REVISION: 1`. Update `UPDATED_AT`, keep `STATUS: OPEN`, validate all four
   slots, commit the scoped work and `MEMORY.md`, then push normally.
7. After a successful push, enter Waiting Mode. Do not continue editing while
   the peer owns the turn.

## Waiting Mode and polling

One poll cycle is:

1. Wait approximately 30 seconds using the harness's bounded wait mechanism.
2. Fetch the configured upstream and compare its tip with the last observed
   tip. Do not mutate the worktree merely because time passed.
3. If the upstream advanced and the local worktree is safe, fast-forward only.
   Validate `MEMORY.md` before acting.
4. Continue waiting when the session is `OPEN` but `TURN` names the peer or is
   `NONE`. Enter Active Mode only for a new request addressed to this platform
   or a new concrete user task.
5. Stop on `BLOCKED`, `COMPLETE`, interruption, branch/upstream mismatch,
   divergence, invalid schema, conflicting participant claim, or unsafe local
   changes. Report the exact reason.

Do not implement polling as an infinite shell loop. Execute bounded cycles and
send a short progress update at least once per minute while the turn remains
active in the harness.

## Completion handshake

The responder to the last substantive request reports its evidence and sends a
`verify-completion` request back to the originating platform. The origin then:

1. Fast-forwards, reviews the reply and repository state, and runs any final
   locally available checks.
2. Replaces its reply slot with the acknowledgement and names the
   `verify-completion` request ID in `REPLY-TO`.
3. Sets its request to `ID NONE` with body `NONE`, `STATUS: COMPLETE`, and
   `TURN: NONE`; increments `REVISION` and updates `UPDATED_AT`.
4. Commits and pushes the final mailbox state, leaving all four latest slots in
   root `MEMORY.md` for the next invocation to inspect.

The peer may fast-forward the final commit and stop. A later explicit
invocation may replace a `COMPLETE` session with a new session; never append an
unbounded history to the file because Git already retains prior revisions.

## Failure and evidence rules

- Never put secrets, credentials, recovery material, private screen contents,
  or unredacted personal data in the mailbox or commit history.
- Summarize large logs and give paths plus timestamps. Preserve the full local
  evidence when needed, and obey `logs/README.md` for per-device tracked logs.
- On crashes, protocol mismatches, persistent black frames, input runaway, or
  display-restoration failures, stop unsafe testing, preserve evidence, and
  release input/display state according to the native guide.
- Report the native OS, tested commit, validation tier, commands and results,
  and every skipped device, permission, codec, network, or physical-link gate.
