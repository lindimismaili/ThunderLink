# ThunderLink Architecture

## Purpose and Boundaries

ThunderLink streams one Mac display to a Windows PC over a low-latency direct
network link and returns Windows mouse/keyboard input to the Mac. It is two
native applications joined by a versioned wire protocol, not a single
cross-platform process.

- `mac_sender/`: Swift/AppKit process using ScreenCaptureKit, VideoToolbox,
  CoreMedia/CoreVideo, and CoreGraphics.
- `windows_receiver/`: C++17 Win32 process using Winsock, Media Foundation,
  Direct3D 12, DXGI, and ImGui.
- `common/`: C headers for shared protocol values and socket helpers. Swift
  mirrors the wire constants in `NetworkConfig.swift` because it does not import
  these headers directly.

## Runtime Data Flow

```text
Mac ScreenCaptureKit -> VideoToolbox H.264/HEVC -> UDP packetization
       -> Windows reassembly -> Media Foundation decode -> D3D12 render

Windows raw mouse/keyboard -> JSON over TCP -> Mac event mapping -> CGEvent

Windows control server <-> Mac control client
```

The Windows receiver starts first and listens for control connections. The Mac
sender connects, negotiates stream settings, then begins capture and encoded
frame delivery. Either side may stop; the Mac client retries after disconnect.

## Network Channels

| Port | Transport | Direction | Purpose |
| --- | --- | --- | --- |
| `50001` | UDP | Mac to Windows | Chunked encoded video access units |
| `50002` | TCP | Windows to Mac | Length-prefixed JSON input events |
| `50003` | TCP | Bidirectional | Length-prefixed JSON control messages |

Defaults are Mac `169.254.1.1`, Windows `169.254.1.2`, subnet
`255.255.255.0`. Runtime flags may override endpoints and ports, but both sides
must agree.

## Control and Configuration

1. The Mac sends `HELLO` with protocol version 2.
2. Windows sends `CONFIGURE` with desired resolution, FPS, codec, and bitrate.
3. The Mac sends `START` with the configuration it will use.
4. Windows sends periodic `PING`; the Mac returns `PONG` with timing data.
5. Windows may send runtime `CONFIGURE` or `REQUEST_KEYFRAME` messages.
6. `STOP` or `BYE` ends the stream and triggers safe teardown/reconnect behavior.

The receiver owns the requested stream configuration. The sender owns capture
feasibility and encoded frame production. UI must display negotiated or decoded
values when they differ from defaults.

## Video Framing

Protocol version 2 normally carries Annex-B H.264 or HEVC access units. Each
encoded frame is split into UDP payloads no larger than `1400` bytes so the
40-byte packed `TlnkFrameHeader` remains within a standard 1500-byte Ethernet
MTU.

The header contains magic, frame ID, packet sequence/count, packet payload size,
total encoded-frame bytes, width, height, format, and flags. Windows reassembles
by frame ID, validates metadata, drops incomplete frames after the configured
timeout, and requests a new keyframe when recovery is needed. BGRA32 remains a
legacy/test format, not the normal product path.

## Component Ownership

### macOS

- `AppDelegate` coordinates lifecycle and the capture-to-network pipeline.
- `DisplayManager`, `DisplayController`, and `ScreenCapture` select, configure,
  capture, and restore displays.
- `VideoEncoder` owns hardware H.264/HEVC encoding.
- `FrameSender`, `ControlClient`, and `InputReceiver` own the three channels.
- `EventMapper` and `InputInjector` translate and post input events.
- `StatusController` presents menu-bar state without owning protocol behavior.

### Windows

- `Application` coordinates window, network, decode, render, input, and UI.
- `NetworkManager`, `FrameReceiver`, and `InputSender` own the three channels.
- `FrameBuffer` owns bounded UDP reassembly state.
- `VideoDecoderMF` owns Media Foundation H.264/HEVC decoding.
- `D3D12Renderer`, `TextureManager`, and `SwapChain` own GPU upload and present.
- `InputCapture` and `InputMapper` produce protocol input events.

## Cross-Platform Invariants

- Keep protocol values, enums, header layout, control fields, ports, and timing
  constants synchronized between `common/` and `NetworkConfig.swift`.
- Preserve the packed 40-byte little-endian frame header and validate all
  received sizes before allocation or copy.
- Keep UDP payloads compatible with a standard 1500-byte MTU unless both sides
  and the documented network requirement change together.
- Never block capture, encode, receive, decode, render, or input threads on UI
  work or unbounded queues.
- Bound in-flight frame memory, discard stale incomplete data, and recover with
  a keyframe rather than rendering corrupt frames.
- Keep display coordinates, capture dimensions, decoded dimensions, window
  aspect ratio, and normalized input mapping consistent through reconfigure.
- Teardown must restore Mac display state, release Windows cursor confinement,
  close sockets, and permit a clean reconnect.

## Change Rules

Protocol changes require coordinated source changes and native builds on both
platforms. Changes to capture, codec, packetization, decode, rendering, or input
require physical two-device validation before being described as end-to-end
verified. Follow [MAC_TESTING.md](MAC_TESTING.md) and
[WINDOWS_TESTING.md](WINDOWS_TESTING.md).
