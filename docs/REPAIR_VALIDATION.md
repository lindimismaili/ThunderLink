# Protocol v3 repair validation

This records the local implementation and its verification boundary. Native
Windows and the two physical-device protocols remain required completion gates.

## Implemented changes

- Protocol v3: shared 48-byte golden fixture, fixed hexadecimal session IDs,
  strict packet/configuration validation, aligned wire reads, and session resets.
- One synchronized frame pool with session-scoped releases, bounded allocation,
  exact chunk lengths, and clock-driven idle eviction.
- Worker-owned TCP sockets, incremental framing, complete bounded writes,
  handshake/peer deadlines, reconnects, and queued keyframe/input commands.
- Serialized Mac capture lifecycle with generation checks, injectable services,
  truthful START/failure state, serial encoding, retained-frame keyframe recovery,
  session-gated input injection, and asynchronous termination cleanup.
- Dedicated Media Foundation worker, async MFT events, complete candidate type
  negotiation, same-codec software fallback, bounded owned queues, stale-output
  invalidation, and checked NV12 copies.
- Transactional texture replacement before command recording, fence-coupled
  upload reuse, input reset/release handling, window-owned fullscreen state,
  and cached bandwidth/FPS statistics.

## Verified on this machine

Host: Darwin/macOS, arm64; Swift 6.4 Command Line Tools. No native Windows
compiler, Media Foundation runtime, or D3D12 runtime was used on this host.

| Check | Result |
| --- | --- |
| `swift build --package-path mac_sender` | Passed |
| `swift build --package-path mac_sender -c release` | Passed |
| `scripts/test-mac.sh` | 11 Swift tests passed |
| Portable Debug CMake/CTest | 10 targets passed |
| Portable CTest with AddressSanitizer + UndefinedBehaviorSanitizer | 10 targets passed; no sanitizer diagnostics |
| Separate portable CTest with ThreadSanitizer | 10 targets passed; no sanitizer diagnostics |
| `git diff --check` | Passed |
| Oversized FPS argument against the built Mac executable | Rejected with exit 1 before application startup |

The Swift toolchain emits linker warnings for two absent CLT search directories;
builds still succeed. Its Testing macro plugin needs explicit discovery, handled
by `scripts/test-mac.sh`; no toolchain installation or system changes are needed.
Build trees and diagnostic command output are under `.build/` and `/tmp/` and
are not tracked. Existing per-device logs are preserved.

Portable tests cover malformed/short/oversized/reordered/duplicate video,
maximum frame bounds, pool exhaustion and concurrency, session replacement,
stale releases, fake-clock expiry, actual idle UDP polling, fragmented TCP,
partial writes/EOF, peer silence, reconnects, version rejection, input queue
sessions, bandwidth snapshots, decoder overload/output ownership, and NV12 pitch.
Swift tests cover the shared wire fixture, framing deadlines, unsafe numeric
values, input reset/session transitions, rapid configuration, permission/encoder/
capture/START failures, startup cancellation, and exactly-once fake restoration.
Tests do not capture a display or inject real input.

## Required checks on native Windows

Build Debug and Release and run CTest in both configurations. In addition to
the portable targets, run:

- `DecoderContractTests`: injected MFT negotiation failures, hardware runtime
  failure, HEVC-preserving software fallback, async events, output stream IDs,
  stream changes, and owned output surviving transform shutdown.
- `DecoderIntegrationTests`: installed codec initialization, invalid input,
  stale sessions, backend diagnostics, and worker shutdown.
- `TextureIntegrationTests`: WARP/debug-layer initial uploads, repeated sizes,
  rejected upload, and resize rollback. A skipped result (77) leaves this gate open.
- `InputWindowTests`: fake input releases across focus/overlay/disable changes,
  fullscreen geometry restoration, and repeat-key suppression.

These Windows targets have been added but have **not been compiled or run here**.
Inspect D3D12 debug-layer output with the actual GPU, including initial frames
and repeated resolution changes. Test the real hardware and software codec
backends separately; a fake MFT cannot establish driver compatibility.

## Required physical-device acceptance

Execute both [Mac](MAC_TESTING.md) and [Windows](WINDOWS_TESTING.md) protocols:
H.264, supported HEVC, all supported resolution presets, static-screen recovery,
keyboard/mouse releases, VSync/fullscreen, restarting either process, cable
interruption, and recovery. Verify truthful state, responsive shutdown/reconnect,
restored display/cursor state, and no persistent rendering corruption.

No physical-link or live capture/input acceptance result is claimed by this record.
