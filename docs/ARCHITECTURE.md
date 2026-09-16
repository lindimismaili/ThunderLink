# ThunderLink Architecture

## Session power ownership

`SessionPowerController` observes actual Screen Share streaming and Cross
configured-session state, gated by the saved, default-on `keepAwakeOnPower`
preference and the app's disconnected/paused state. It owns a process-scoped
IOKit idle-display assertion while on AC. Power-source notifications and a
retry timer release/reacquire it without restarting the app. This is local
policy; no v4 wire contract changes are involved.

Lid sleep is a separate privilege boundary. Apple's public idle assertions
[explicitly allow lid-close sleep](https://developer.apple.com/documentation/iokit/kiopmassertiontypepreventuseridlesystemsleep),
and the private `AppliesOnLidClose` property requires an Apple entitlement in
[powerd](https://github.com/apple-oss-distributions/PowerManagement/blob/main/pmconfigd/PMAssertions.c).
`LidSleepController` instead launches the bundled `ThunderLinkPowerHelper`
through macOS administrator authorization, once per app launch on an explicit
Settings action. It renews a four-second monotonic lease on the main run loop.

The helper receives only a constrained, random temporary directory and owner
UID. It opens the directory and bounded regular files with no symlink
following, checks owner/mode/link count, and holds a system-wide lock. No client
command, executable, or arbitrary mutation path is accepted. The sole privileged
mutation is fixed `/usr/bin/pmset -a disablesleep 0|1`; successful exit status
alone is insufficient, so the helper verifies `IOPMrootDomain.SleepDisabled`.
Existing external overrides are rejected, preserving their configuration.

The helper polls at 500 ms, independently gates on AC and thermal state, and
releases on stale/malformed leases or termination signals. Failed/partial writes
retain cleanup responsibility and restoration is retried until verified. The
app revokes the lease on quit instead of killing the watchdog. Nothing is
installed permanently. This handles app failure; SIGKILL/helper failure or an
abrupt reboot during an override still requires administrator recovery as
documented in SETUP.md. There is no boot-time recovery daemon.

ThunderLink has two native processes: a macOS ScreenCaptureKit/VideoToolbox
sender and a Windows Media Foundation/Direct3D 12 receiver. Both applications
must use protocol **v4**; older versions are deliberately incompatible.

## Channels and negotiation

| Channel | Default endpoint | Direction |
| --- | --- | --- |
| Encoded video | UDP 50001 | Mac to Windows |
| Input | TCP 50002 | Windows to Mac |
| Control | TCP 50003 | Bidirectional; Windows listens |

TCP messages are UTF-8 JSON with a four-byte big-endian length prefix, bounded
to 65,536 bytes. Channel workers own socket writes and closure; UI and video
callbacks enqueue work. Partial messages survive readiness polls. EOF, invalid
framing, write failures, and deadlines end the connection instead of being
mistaken for idle traffic.

Windows receives HELLO with version 4 and issues CONFIGURE with resolution,
FPS, codec, bitrate, and a fresh nonzero 64-bit stream session ID. JSON encodes
`session_id` as exactly 16 lowercase hexadecimal characters. Mac validates and
starts its encoder and capture before sending START with the actual parameters
and matching session ID. Video delivery opens afterward. Windows activates
reassembly only on matching START and requests a fresh keyframe, covering early
UDP packets that race the acknowledgement.

Each runtime CONFIGURE creates a new session. STOP and keyframe requests carry
the session ID; stale messages and video/input from prior sessions are ignored.
STOP includes an actionable `reason`. BYE ends a control connection. Periodic
PING/PONG provides liveness and round-trip timing. Hardware/software decode
fallback preserves the negotiated codec; an unsupported codec is an error.

## Video header and reassembly

The little-endian video header is 48 bytes. Bytes 0–39 retain the former field
layout; bytes 40–47 contain the session ID:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | magic |
| 4 | u64 | frame ID |
| 12 | u32 | packet sequence |
| 16 | u32 | total packets |
| 20 | u32 | packet payload bytes |
| 24 | u32 | total frame bytes |
| 28, 32 | u32, u32 | width, height |
| 36, 38 | u16, u16 | format, flags |
| 40 | u64 | session ID |

Frames carry Annex-B H.264 or HEVC access units. The legacy BGRA representation
is retained for isolated tests, not selected by the product. Chunks remain at
most 1,400 bytes: 48 + 1,400 + 20 IPv4 + 8 UDP = 1,476 bytes, within a standard
1,500-byte MTU.

Decode bytes into aligned metadata; never bind C++ references to packed fields.
Validate the active session, dimensions, format/flags, exact chunk count and
length, total allocation bound, and matching metadata before indexing or copying.
All offsets use wide arithmetic. Duplicate and stale frames cannot advance the
completion counter. The four-slot pool, queues, and held-slot ownership share
one mutex. Held frames are released using session plus frame ID, including after
session replacement. Loss callbacks execute outside the lock.

The UDP worker expires incomplete frames by clock every 10 ms, including idle
periods; a 50 ms incomplete-frame timeout triggers keyframe recovery. A new
session resets reassembly counters and invalidates pending/completed old work.

## Capture and display lifecycle

A retained Mac lifecycle coordinator serializes configuring, streaming, stopping,
and failure cleanup. Generation checks prevent cancelled asynchronous startup
from publishing success or restarting capture. Encoder creation, submission,
and invalidation share serial ownership. Teardown closes frame delivery, drains
capture/encoding, releases input state, and restores the original display mode.
Quit awaits this cleanup. Capture/encoder failures update visible state and
notify Windows instead of leaving a false streaming indication.

Configuration limits are shared: dimensions at most 3840×2160, FPS 1–240,
bitrate 1,000–200,000 kbps, ports 1–65,535. Resolution requests may be native
0×0, aspect-derived 0×H, or positive even WxH. Resolved NV12 dimensions must be
even, at least 2×2, and within the bound. Unsupported native sizes require a
smaller preset. Invalid CLI values fail before opening channels or changing a
display. Actual values appear in START and the interface.

## Decode and GPU ownership

A dedicated COM-initialized decoder worker negotiates hardware MFT candidates,
unlocks asynchronous transforms, and follows their need-input/have-output
events. Candidate negotiation and same-codec synchronous software fallback do
not run on the UI thread. Encoded input is bounded to four queued access units;
decoded output to two owned frames. Overload flushes dependent encoded work and
requests a keyframe. Already-decoded frames may be skipped for lower latency.
Every queued result is session-tagged. The renderer polls owned decoded output
without borrowing mutable worker memory.

Texture resizing happens before recording frame commands, after prior GPU uses
finish. Replacement resources/descriptors are committed together. Uploads use
COPY_DEST before copying and PIXEL_SHADER_RESOURCE afterward. Upload storage is
reused only after its GPU fence completes. Failed uploads preserve the last
valid displayed frame and are reported instead of counted as rendered.

## Input and observability

Input events include `session_id`; `reset` releases all injected keys/buttons.
Windows tracks accepted forwarded downs, releases on focus/overlay/session
transitions, and does not suppress corresponding releases merely because the
UI now owns input. Mac injection uses serial ownership and also releases on
input EOF/control teardown. Queued old input never replays on reconnect.
Movement is coalesced; queue saturation resets the channel rather than losing
release events silently.

F11 and `--fullscreen` use the same window-owned fullscreen state; Escape releases
cursor confinement. The Stats panel and logs share cached one-second bandwidth
samples. Idle rates decay and session transitions reset counters.

## Validation boundary

Portable CTest suites cover protocol, transport, reassembly, input state, and
statistics, decoder queues, and loopback channels. Swift tests cover sender
framing, validation, input sinks, and capture lifecycle using injected services.
Neither proves native Windows codec/GPU behavior or physical-link success.
Follow [MAC_TESTING.md](MAC_TESTING.md) and [WINDOWS_TESTING.md](WINDOWS_TESTING.md)
for native builds, UI checks, and physical-device acceptance. The shared wire
contract lives in `common/` and its Swift mirrors; change and test both together.

The Media Foundation implementation follows Microsoft's [asynchronous MFT
contract](https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts)
and uses [contiguous copies from 2D buffers](https://learn.microsoft.com/en-us/windows/win32/api/mfobjects/nf-mfobjects-imf2dbuffer-contiguouscopyto)
to remove hardware surface padding before publishing owned NV12 output.

## Portable desktop shell

The native video/input engines remain platform-specific. SwiftUI/AppKit owns the
Mac desktop window and status menu; Win32/ImGui owns the Windows dashboard,
viewer, and tray icon. The shared wire protocol is unchanged.

Both entrypoints establish a single-instance boundary before opening channels:
Mac holds an advisory file lock in Application Support and signals Show through
a per-user distributed notification; Windows holds a session-local named mutex
and sends a Show message to the existing window. These are lifetime mechanisms,
not peer authentication or transport security.

Saved options reuse CLI validation. Defaults load first, saved options next,
and launch overrides last. The UI edits saved values separately. Writes replace
the settings file atomically; failures remain visible without restarting.
Save and Restart drains channels/capture/decoder state, releases the instance
boundary, and relaunches without the old arguments. Invalid configuration leaves
a recoverable desktop available without opening networking.

Visibility is independent of connection lifetime. Mac window close never invokes
stream shutdown. Windows close releases input and hides the retained HWND; its
loop continues consuming bounded encoded/decoded queues without presenting
hidden frames. Show resumes normal rendering. User Disconnect stops the network
workers and pauses retries; final Quit also destroys rendering and restores
display/input state. macOS Disconnect settles reversible lifecycle cleanup rather
than calling the terminal shutdown operation.

Windows shader strings are generated from canonical HLSL by CMake and compiled
from memory by the system D3D compiler. Resources and static C++ runtime travel
inside the executable. The Mac uses a standard bundle and OS-provided frameworks.
Packaged logs/settings use writable per-user storage, never source/build paths.

## Cross sessions

Cross is an independent input-only mode. Its Mac listener advertises
`_thunderlink-cross._tcp`; Windows connects over a single authenticated TLS
stream on TCP 50004. The wire contract is [common/CROSS_PROTOCOL.md](../common/CROSS_PROTOCOL.md).
Screen Share keeps its separate UDP video and TCP input/control channels. Both
sides negotiate protocol v4; the video header layout remains 48 bytes.

Native discovery treats DNS-SD records as connection hints. Certificate identity,
explicit approval on both devices, and persisted pins establish trust. Network
addresses are routes, never device identities. Pins/private identity material
use protected OS storage. Layouts/preferences live in atomic versioned
`cross.json` files without private key material.

The Windows Cross input thread owns background capture and local recovery,
independently of visible windows and render cadence. Hooks only queue work;
network workers own TLS writes. A virtual pointer traverses a validated canvas;
per-display transforms map it to real Mac global coordinates. Edge geometry in
`common/cross_layout.h` has no OS dependencies. Mac injection is serialized,
checks session/generation/topology, and releases held state on every ownership
termination. Consecutive pointer movements may coalesce; transitions and
key/button events remain ordered.

Ownership is acknowledged before forwarding. A 250ms renewal cadence and a
1000ms expiry bound the lifetime of active remote ownership. Connection-only
heartbeats use `renew_ownership: false`; a healthy network worker cannot keep
remote input held after the input worker stops renewing. A 1500ms connection
heartbeat deadline closes an unresponsive path so address failover can begin
without waiting for the operating system's TCP timeout. Disconnect,
permission changes, sleep/lock, queue faults, and topology changes return control
locally. Reconnection gets a fresh session; old ownership never resumes. An
approved Cross configure drains the Screen Share lifecycle before ready, and
Screen Share cannot restart while Cross owns the active mode. Cross does not
start screen capture, codecs, or video channels and does not change display modes.

Automatic route choice prefers available Thunderbolt networking, Ethernet, then
Wi-Fi, while retaining a healthy path. Fallback first returns local control and
then connects to another address for the same authenticated peer. DHCP/link-local
addresses are consumed as configured by the OS; discovery does not mutate network
configuration. IPv4/IPv6 and manual addresses support networks where discovery is
not available. Cross's TLS protections do not imply that the legacy Screen Share
video channel is encrypted.
