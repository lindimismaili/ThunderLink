# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project: ThunderLink

A two-process screen streaming + input forwarding system between a Mac (sender) and a Windows PC (receiver), intended to run over a direct Thunderbolt link on the `169.254.1.x/24` link-local subnet. The Mac captures its display with ScreenCaptureKit and streams raw BGRA frames; the Windows side renders them via D3D12 and forwards mouse/keyboard input back.

Two independent build systems, one shared C header directory:

- [mac_sender/](mac_sender/) — Swift Package (SwiftPM), macOS 13+
- [windows_receiver/](windows_receiver/) — CMake / C++17, MSVC + Windows SDK
- [common/](common/) — header-only C definitions consumed by **both** sides (protocol constants, frame header struct, input event JSON field names, socket helpers)

## Commit & sync policy (cross-device project)

This project is developed across **two machines** — the Mac sender and the Windows
receiver — that share code through git. So, after completing a full update, bugfix,
or new feature that **builds and is ready for testing**, **commit and sync
automatically, without asking first**:

1. Stage only the intended changes. Do **not** commit secrets/`.env` or build output
   (`windows_receiver/build/`, `mac_sender/.build/`). The `logs/` files **are**
   tracked on purpose for cross-device debugging — see [logs/README.md](logs/README.md).
2. Commit on the **current branch** with a concise, descriptive message.
3. Sync with the remote: `git fetch`, `git pull --ff-only` if behind, then `git push`,
   so the other machine can pull and test.

Only do this for a coherent, testable checkpoint — never for half-finished work, and
never switch branches or open PRs as part of it. A change that touches the wire
protocol must update **both** `common/` and the Swift mirror in
[NetworkConfig.swift](mac_sender/Sources/Network/NetworkConfig.swift) in the same
commit (see the wire-protocol note below), or the two machines will desync.

## Build & Run

### Mac sender
```bash
cd mac_sender
swift build              # debug
swift build -c release   # release
swift run ThunderLinkSender
```
Requires Screen Recording and Accessibility permissions (for `CGEvent` input injection in [InputInjector.swift](mac_sender/Sources/Input/InputInjector.swift)).

### Windows receiver
```cmd
cd windows_receiver
cmake -B build -S .
cmake --build build --config Release
build\Release\ThunderLinkReceiver.exe
```
Shaders are compiled at build time with `dxc` (preferred) or `fxc` from the Windows 10 SDK; the CMakeLists searches the standard `Windows Kits/10/bin/...` paths. If neither is found, the app falls back to `D3DCompileFromFile` at runtime, and `SHADER_DIR` is baked in as a compile definition pointing at the build's `shaders/` output.

`nlohmann/json` is fetched via `FetchContent` — no manual install needed.

There is no test target on either side.

## Architecture

### Wire protocol — single source of truth in `common/`
All three ports, the 32-byte frame header layout, magic number `0x544C4E4B` ("TLNK"), pixel/flag/control enums, and JSON field name constants live in [common/protocol.h](common/protocol.h), [common/frame_header.h](common/frame_header.h), and [common/input_event.h](common/input_event.h). Both static_assert (C++) and _Static_assert (C) enforce `sizeof(TlnkFrameHeader) == 32`. **The Swift side does not include these headers** — it duplicates the constants in [NetworkConfig.swift](mac_sender/Sources/Network/NetworkConfig.swift). Any protocol change must be made in **both** places or the wire format will silently desync.

Three channels:
- **UDP 50001** — video frames, Mac → Windows. Frames are chunked into ≤8192-byte payloads, each prefixed with a `TlnkFrameHeader` (`frame_id`, `packet_seq`, `total_packets`, ...). Receiver reassembles by `frame_id` and drops incomplete frames after `TLNK_FRAME_TIMEOUT_MS` (50ms).
- **TCP 50002** — input events, Windows → Mac. Newline-delimited JSON (`mouse_move`, `mouse_button`, `key`, `scroll`); field/value names are `TLNK_JSON_*` defines so the C++ encoder and Swift decoder stay in lockstep.
- **TCP 50003** — control / handshake (`HELLO`, `START`, `STOP`, `CONFIGURE`, `PING`/`PONG`, `BYE`).

UDP sockets get 4 MB SO_RCVBUF/SO_SNDBUF; TCP sockets have `TCP_NODELAY` set — see [common/network_utils.h](common/network_utils.h), which is the cross-platform Winsock2-vs-POSIX shim.

### Mac sender (Swift, AppKit)
Entry point is [main.swift](mac_sender/Sources/main.swift) which runs an `NSApplication` with an `AppDelegate`. **Note:** `AppDelegate` is referenced but not yet defined in the tree — adding it is a prerequisite to running. The pieces it wires together already exist:

- `Capture/` — [DisplayManager](mac_sender/Sources/Capture/DisplayManager.swift) enumerates `SCDisplay`s; [ScreenCapture](mac_sender/Sources/Capture/ScreenCapture.swift) configures an `SCStream` for BGRA32 at the requested resolution/FPS and delivers `CVPixelBuffer`s via `ScreenCaptureDelegate`.
- `Network/` — `FrameSender` chunks pixel buffers into UDP packets; `InputReceiver` accepts the TCP input connection from Windows.
- `Input/` — `EventMapper` translates the JSON event into a Mac key/button; `InputInjector` posts the synthesized `CGEvent` (requires Accessibility permission).
- `Utils/PixelBuffer.swift` — helpers around `CVPixelBuffer` locking / row stride.

### Windows receiver (C++17, namespace `thunderlink`)
Entry point [main.cpp](windows_receiver/src/main.cpp) → [Application](windows_receiver/src/App/Application.h) owns all subsystems via `unique_ptr`:
- `App/Window` — Win32 message loop; forwards raw input to `InputCapture`.
- `Network/NetworkManager` — drives the control-channel handshake and starts/stops the `FrameReceiver` (UDP) and `InputSender` (TCP) based on `OnConnected` / `OnDisconnected` callbacks fired into `Application`.
- `Renderer/D3D12Renderer` + `SwapChain` + `TextureManager` — uploads BGRA frames as D3D12 textures and presents them through the swap chain. Vertex/pixel shaders live in [windows_receiver/shaders/](windows_receiver/shaders/) and are loaded as `.cso` from `SHADER_DIR` (or compiled at runtime as fallback).
- `Utils/FrameBuffer` — receive-side reassembly pool (`TLNK_FRAME_POOL_SIZE = 4` in-flight frames, max 3840×2160).
- `Input/InputCapture` + `InputMapper` — captures Win32 input and converts to the shared JSON event types in `input_event.h`.

### Coupling points to watch
- Mac uses BGRA32 (`kCVPixelFormatType_32BGRA`), which is also what the D3D12 textures expect — keep `TLNK_FORMAT_BGRA32` (= 0) as the only format until both sides handle alternatives.
- The frame header is `#pragma pack(push, 1)`; any field addition must update `TLNK_HEADER_SIZE` and the Swift packing code in `FrameSender` simultaneously.
- Modifier flag bit values (`TLNK_MOD_SHIFT=0x01` ... `TLNK_MOD_META=0x08`) are duplicated as string keys in JSON — both the bitmask and the string form must match across sides.
