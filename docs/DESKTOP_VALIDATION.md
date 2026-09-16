# Portable Desktop Validation — 2026-09-16

## Implemented

- Native dark desktop pages on Mac and Windows: Connection, Settings, Diagnostics.
- Close-to-menu-bar/tray behavior, exactly Show/Quit in the icon menu, single
  instance, saved settings with CLI overrides, and explicit adapter setup on Mac.
- Windows hidden input release, bounded decode draining without presentation,
  Explorer tray restoration, embedded resources, and static runtime packaging.
- Native packaging scripts, versioned archives, icons, licenses, per-user logs,
  settings recovery, and orderly Save and Restart.

The pre-existing working tree contained substantial protocol/engine repairs.
Those changes were retained; no commit, branch operation, or sync was performed.
No protocol constants/layouts were changed for desktop packaging.

## Verified on this Mac

Host: Darwin / Apple Silicon, macOS 27.0 (26A428), Swift 6.4. The packaged Mach-O
reports minimum macOS 13.0. This is not a test on macOS 13 itself.

| Check | Result |
| --- | --- |
| `swift build --package-path mac_sender` | Passed; debug build |
| `scripts/test-mac.sh` | 15 tests in 5 suites passed |
| Portable CMake configure/build and CTest | 11/11 passed |
| `scripts/package-mac.sh` | Release bundle, ZIP, and SHA-256 produced |
| `codesign --verify --strict` | Ad-hoc signature valid |
| `otool -L` packaging audit | Only system frameworks/libraries required |
| `vtool -show-build` | arm64, minimum OS 13.0 |
| Relocated app | Started outside the checkout from a path with spaces and Unicode, using loopback CLI overrides |
| Per-user logging | Packaged process wrote to Library/Logs/ThunderLink |
| Second executable launch | Exited successfully; existing PID retained and Show notification handled |
| Missing bind address | Process remained alive, reported failure, continued retries, retained visible window state |
| AppKit layout snapshots | All 3 pages inspected at 940×660 and 780×538 content sizes |
| `git diff --check` | Passed |

The compiler reports pre-existing concurrency warnings in the timer/encoder code
and Command Line Tools linker search-path warnings. Builds and tests complete.

Offscreen snapshots come from the real NSHostingView/AppKit rendering path and
are generated under `mac_sender/.build/desktop-previews`. They prove rendered
layout, not live mouse/keyboard/menu behavior. Initial ImageRenderer snapshots
omitted AppKit-backed scroll views; those were replaced by NSHostingView snapshots.

Live computer-use inspection repeatedly returned `cgWindowNotFound`, despite the
app reporting a visible NSWindow and normal event-loop operation. Consequently,
live Mac menu close/show/quit, keyboard editing, settings-triggered relaunch, and
permission actions are **not claimed as interaction-verified**. Test processes
were stopped after smoke testing; no adapter/permission changes were made.

## Pending native Windows and physical checks

The user will run `scripts/package-windows.ps1` in native Visual Studio developer
PowerShell. No Windows EXE was built on this Mac. The script builds/tests Release,
audits DLL imports, and writes `dist/ThunderLink.exe` plus its ZIP/checksum.
Windows native compile, Debug tests, actual tray/menu behavior, Explorer restart,
DPI changes, fullscreen restoration, Media Foundation/D3D12, and clean-machine
portability are pending. PowerShell execution was not available on this host.

Both platform testing guides include the new desktop acceptance steps. Physical
Mac-to-Windows streaming, hidden-session input release, reconnect, codec support,
permission changes, display restoration, and Quit during streaming require the
two-device protocol before claiming end-to-end verification.

The Windows ImGui UI supports keyboard navigation and DPI scaling but lacks a
full native screen-reader accessibility tree. Public signing/notarization,
installers, automatic updates, and other CPU architectures are outside this build.
