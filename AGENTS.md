# ThunderLink Agent Guide

ThunderLink is a two-process, low-latency remote-display system for a directly
connected Mac and Windows PC. The macOS sender captures and hardware-encodes a
display; the Windows receiver decodes and renders it with Direct3D 12, then
forwards mouse and keyboard input to the Mac.

## Before You Work

1. Identify the host OS with a native command; do not infer it from the path.
   Use `uname -s` on macOS or
   `[System.Environment]::OSVersion.Platform` in PowerShell on Windows. WSL is
   not a substitute for native Windows GUI, D3D12, or Media Foundation testing.
2. Inspect `git status --short` and preserve unrelated work.
3. Read [docs/SETUP.md](docs/SETUP.md) before installing, building, or running.
4. Follow the testing guide for the machine you are operating:
   - macOS: [docs/MAC_TESTING.md](docs/MAC_TESTING.md)
   - Windows: [docs/WINDOWS_TESTING.md](docs/WINDOWS_TESTING.md)
5. For product or UI changes, follow [docs/DESIGN.md](docs/DESIGN.md). For
   system boundaries and data flow, read
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Repository Map

- `mac_sender/` — macOS 13+ Swift/AppKit sender built with SwiftPM.
- `windows_receiver/` — Windows C++17 receiver built with CMake/MSVC.
- `common/` — shared C protocol and socket definitions used by Windows and
  mirrored in Swift.
- `logs/` — deliberately tracked, per-device diagnostic logs; see
  [logs/README.md](logs/README.md).

## Non-Negotiable Rules

- Treat `common/` and
  `mac_sender/Sources/Network/NetworkConfig.swift` as a coupled wire contract.
  Update and validate both sides in the same change whenever a protocol
  constant, port, enum, packet layout, timeout, or format changes.
- Do not claim cross-device success from a build or single-machine run. Report
  the host OS, checks actually run, and every unverified native or physical
  device gate.
- Keep the normal video path encoded H.264/HEVC. Legacy BGRA is not the product
  path and must not be made the default without compatible changes on both
  sides.
- Do not read, expose, or commit secrets. Do not commit build output from
  `mac_sender/.build/` or `windows_receiver/build/`.
- Do not commit, push, pull, sync, switch branches, or open a pull request
  unless the user explicitly asks. When authorized, stage only intended files
  and stay on the current branch.
- Do not rewrite or discard another device's tracked log. Follow
  [logs/README.md](logs/README.md) for cross-device log handling.

## Validation Standard

There is no automated test target. At minimum, build on the affected native
platform and perform the applicable local checks. Networking, protocol,
capture, decode, rendering, or input changes require the two-device protocol in
both testing guides before they can be called fully verified.
