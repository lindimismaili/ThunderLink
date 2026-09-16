# ThunderLink Setup

This guide prepares the Mac sender, Windows receiver, and their direct network
link. Run platform commands only on the matching native OS.

Both applications must use protocol v4. Update them together: older peers
are rejected explicitly. Each connection/reconfiguration has a new stream
session ID so delayed video and input cannot contaminate a restarted stream.

For input sharing without video over Thunderbolt networking, Ethernet, or
Wi-Fi/LAN, use [Cross setup](CROSS.md). Cross discovers existing addresses and
uses a paired TLS connection on TCP 50004; the fixed-address instructions below
describe the Screen Share direct-link configuration.

## Portable desktop apps

End users unzip and open **ThunderLink.app** (Apple Silicon Mac) or
**ThunderLink.exe** (x64 Windows). No compiler, source checkout, runtime download,
or terminal is needed. System graphics/media support, permissions, and a working
network link are still required. Windows N editions need Media Foundation enabled;
HEVC requires an installed compatible decoder. Unsupported codecs show errors.

From the repository root, developers package on the matching native OS:

```sh
./scripts/package-mac.sh
```

```powershell
# Native Visual Studio developer PowerShell, with CMake and Windows SDK available
.\scripts\package-windows.ps1
```

Outputs are in ignored `dist/`: the launchable app/executable, a versioned ZIP,
and a SHA-256 checksum. The version comes from `packaging/VERSION`. Mac packaging
uses ad-hoc signing for personal use; this is not notarized public distribution.
Windows packaging builds and tests Release, checks runtime DLL imports, and uses
the static MSVC runtime. Its shaders, icon, manifest, and license notices are
embedded. Both artifacts use only system frameworks/DLLs at runtime.

### Desktop behavior and settings

Both open to Connection, Settings, and Diagnostics. Closing hides the window in
the Mac menu bar or Windows tray; the icon menu contains only Show and Quit.
Hiding preserves the session. Hidden Windows viewers release remote input and
stop presentation. Quit ends the session and restores resources normally.
Disconnect pauses reconnection until Connect or a new launch. Reopening an
already-running app shows its existing window.

Settings are saved to `~/Library/Application Support/ThunderLink/settings.json`
on Mac and `%LOCALAPPDATA%\ThunderLink\settings.json` on Windows. Load order is
built-in defaults, validated saved options, then explicit CLI overrides. Overrides
are temporary; the Settings editor starts from saved values. Save and Restart
saves validated settings, tears down the old session, and opens a new process
without the old CLI overrides. Invalid settings keep networking stopped and show
Reset to Defaults. Files contain JSON arrays of the same validated CLI options;
use the UI rather than editing them manually.

Packaged logs live at `~/Library/Logs/ThunderLink/mac.log` and
`%LOCALAPPDATA%\ThunderLink\Logs\windows.log`. Diagnostics → Open Logs reveals
them. Development executables retain repository logging; see `logs/README.md`.
Neither app needs to write next to its executable. ImGui ini/log files are disabled.

The Mac's Settings → Configure Mac Adapter action identifies the adapter and
shows its proposed address before requesting administrator authorization. Launch
never changes an adapter automatically. Permission buttons open the relevant
macOS settings; restart if requested after granting access. The Windows dashboard
links to Network and Firewall settings and explains the default adapter addresses.

## System Overview

ThunderLink uses these default endpoints on a `/24` direct link:

| Device | Address | Role |
| --- | --- | --- |
| Mac | `169.254.1.1` | Capture, encode, send video, receive input |
| Windows PC | `169.254.1.2` | Receive, decode, render video, send input |

The channels are UDP `50001` for video, TCP `50002` for input, and TCP `50003`
for control. Both applications accept endpoint and port overrides.

## macOS Prerequisites

- macOS 13 or newer.
- Xcode or Xcode Command Line Tools with Swift 5.9 or newer for builds,
  Swift 6 or newer with Swift Testing for the test suite.
- Screen Recording permission for display capture.
- Accessibility permission for injected mouse and keyboard events.
- A hardware encoder that supports the codec selected by the receiver.

Confirm the toolchain:

```sh
uname -s
swift --version
xcodebuild -version
```

Do not install or change system-level tools without the user's authorization.

### Build and run the sender

```sh
cd mac_sender
swift build
swift build -c release
../scripts/test-mac.sh
swift run ThunderLinkSender
```

Debug and release binaries are under `mac_sender/.build/`. On first launch,
macOS may request Screen Recording and Accessibility access; restart the sender
after granting permissions if capture or input remains unavailable.

For a local smoke test without a peer, use loopback endpoints:

```sh
swift run ThunderLinkSender --bind 127.0.0.1 --peer 127.0.0.2 --no-auto-ip
```

`--no-auto-ip` remains accepted for compatibility. Network setup is now an
explicit desktop action regardless of that legacy flag.

## Windows Prerequisites

- Windows 10 or 11 with a Direct3D 12-capable GPU and current driver.
- Visual Studio 2022 with **Desktop development with C++** and a Windows 10 or
  11 SDK.
- CMake 3.20 or newer.
- Media Foundation support for the codec under test.
- Internet access during the first CMake configure so `FetchContent` can obtain
  `nlohmann/json` and Dear ImGui.

Use a native Visual Studio Developer PowerShell or Developer Command Prompt.
WSL cannot validate the receiver's Win32, D3D12, or Media Foundation paths.

Confirm the environment in PowerShell:

```powershell
[System.Environment]::OSVersion.Platform
cmake --version
cl
```

### Build and run the receiver

```powershell
cd windows_receiver
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\ThunderLink.exe
```

CMake embeds the canonical HLSL sources; the receiver compiles them from memory
using the Windows system D3D compiler. No loose shader folder or separate shader
compiler is required. The development executable is
`windows_receiver\build\Release\ThunderLink.exe`.

## Direct-Link Configuration

1. Connect the dedicated Thunderbolt, USB-C Ethernet, or other direct network
   adapters.
2. Configure Windows IPv4 manually as `169.254.1.2` with subnet mask
   `255.255.255.0`; leave gateway and DNS empty for this isolated link.
3. Allow the receiver through Windows Firewall on the direct-link profile. It
   must accept UDP `50001` and TCP `50003`; it also opens an outbound TCP
   connection to the Mac on `50002`.
4. Use Settings → Configure Mac Adapter to assign `169.254.1.1`, or configure
   that address manually in System Settings.
5. Confirm each device can reach the other address before diagnosing the
   application protocol.

Start the Windows receiver first, then the Mac sender. The sender reconnects to
the receiver automatically after a disconnect.

## Runtime Overrides

Common flags on both applications include:

```text
--bind <ip> --peer <ip>
--video-port <n> --input-port <n> --control-port <n>
--codec h264|hevc --bitrate <Mbps> --fps <n> --res <WxH>
```

Mac-only flags:

```text
--no-auto-ip --display <index>
```

Windows-only flags:

```text
--window <WxH> --fullscreen --vsync --no-vsync --console
```

The receiver owns the negotiated resolution, frame rate, codec, and bitrate.
Use identical endpoint and port overrides on both sides.

FPS must be 1–240 and bitrate 1–200 Mbps. Ports must be 1–65535. Resolutions
must be native `0x0`, aspect-derived `0xH`, or positive even `WxH`, with resolved
dimensions no larger than 3840×2160. Choose a smaller preset when the native
display exceeds this bound. Invalid arguments are rejected before startup.
Decoder fallback uses software for the same codec; it does not silently switch
HEVC to H.264. The Stats panel reports the active decoder backend.

## Common Setup Failures

| Symptom | Check |
| --- | --- |
| Mac bind error `EADDRNOTAVAIL`/errno 49 | Confirm `169.254.1.1` is assigned or use the intended `--bind` value. |
| Receiver waits for Mac | Check TCP `50003`, peer addresses, firewall, and that the sender is running. |
| Connected but black video | Check UDP `50001`, codec support, standard MTU, decoder logs, and keyframe requests. |
| Video works but input does not | Check TCP `50002`, macOS Accessibility permission, and cursor confinement state. |
| Windows shader initialization fails | Inspect the embedded shader compilation error and system graphics support in the log. |
| CMake configure cannot fetch dependencies | Restore network access or use an already populated build cache; do not vendor new copies casually. |

See [MAC_TESTING.md](MAC_TESTING.md) or
[WINDOWS_TESTING.md](WINDOWS_TESTING.md) before declaring setup complete.
