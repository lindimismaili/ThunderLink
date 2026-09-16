# Example invocations

## Active mode

> Explicitly invoke `cross-platform-testing` in Active mode to add the requested
> reconnect behavior on Windows, run available tests, and hand the Mac
> participant exact Swift and physical-link verification requests.

Expected behavior: announce the goal, detected OS, Active mode, and Git gate;
require an exactly synchronized clean branch; start the idle coder and monitor;
activate only the coder; review its work; write and push the mailbox handoff;
then activate the monitor and enter Waiting mode.

## Waiting mode with no mailbox

> Explicitly invoke `cross-platform-testing` in Waiting mode on this Mac. Wait
> for the Windows participant's next committed handoff.

Expected behavior: announce state and complete the strict Git gate; start the
idle coder and the read-only monitor; do not create `MEMORY.md`; poll for up to
one hour. After `UPDATE_AVAILABLE`, fast-forward or stop on failure, require the
mailbox to have arrived in the commit, then activate the coder only for a valid
Mac request.

## Ambiguous mode

> Explicitly invoke `cross-platform-testing` for the project.

Expected behavior: announce the goal and native OS, then ask the user to choose
Active or Waiting before spawning subagents, writing files, or polling.

## Unsafe initial checkout

> Invoke `cross-platform-testing` in Waiting mode, but this branch has one local
> commit that has not been pushed.

Expected behavior: report that the branch is ahead, enter Stopped mode, and ask
the user to reconcile Git first. Do not spawn agents, push, pull, or create the
mailbox.
