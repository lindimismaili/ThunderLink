# Cross-device logs

These files let you read what happened on the *other* machine after a `git` sync,
which is handy while testing the Mac sender and Windows receiver on separate boxes.

- `windows.log` — appended by the Windows receiver (`thunderlink::Logger`).
- `mac.log` — appended by the Mac sender (`TLLogger`).

Each run appends a `===== session start =====` marker and then timestamped lines
(INFO/WARN/ERROR + the same text printed to the terminal).

## Workflow

1. Run an app — it appends to its own file.
2. `git add logs/ && git commit && git push` from that device.
3. `git pull` on the other device to read it.

Per-device files (not one shared file) are used on purpose: both machines only
ever write their *own* file, so syncing never produces a merge conflict.

These accumulate forever. To reset, just empty the file (e.g. `: > logs/mac.log`)
and commit.
