# Example invocations

## Active Mode

> Explicitly invoke the `cross-platform-testing` skill in Active Mode to add the requested reconnect
> behavior, run all tests available on this Windows machine, and hand the Mac
> agent the exact Swift and physical-link checks it must perform.

Expected behavior: detect native Windows, validate and fast-forward the current
upstream, join or create the session, perform the scoped work, report evidence
without claiming Mac verification, push one turn handoff, and wait.

## Waiting Mode

> Explicitly invoke the `cross-platform-testing` skill in Waiting Mode. Claim this Mac participant,
> poll the current upstream, and execute only a new request addressed to Mac.

Expected behavior: detect native macOS, create or join one open session, push
the claim when needed, poll in bounded cycles, and remain read-only until the
turn and an unprocessed request target Mac.

## Ambiguous request

> Explicitly invoke the `cross-platform-testing` skill for the project.

Expected behavior: ask whether to begin in Active Mode or Waiting Mode before
creating a session or performing Git mutations.
