# ThunderLink Setup

This guide prepares the Mac sender, Windows receiver, and their direct network
link. Run platform commands only on the matching native OS.

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
- Xcode or Xcode Command Line Tools with Swift 5.9 or newer.
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
swift run ThunderLinkSender
```

Debug and release binaries are under `mac_sender/.build/`. On first launch,
macOS may request Screen Recording and Accessibility access; restart the sender
after granting permissions if capture or input remains unavailable.

By default, the sender tries to assign `169.254.1.1/24` to a suitable direct-link
network service and may show an administrator prompt. For a local smoke test or
when the adapter is already managed manually, prevent that change with:

```sh
swift run ThunderLinkSender --no-auto-ip
```

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
.\build\Release\ThunderLinkReceiver.exe
```

CMake prefers `dxc`, falls back to `fxc`, and otherwise copies the HLSL sources
for runtime compilation with `D3DCompileFromFile`. The release executable is
normally `windows_receiver\build\Release\ThunderLinkReceiver.exe`.

## Direct-Link Configuration

1. Connect the dedicated Thunderbolt, USB-C Ethernet, or other direct network
   adapters.
2. Configure Windows IPv4 manually as `169.254.1.2` with subnet mask
   `255.255.255.0`; leave gateway and DNS empty for this isolated link.
3. Allow the receiver through Windows Firewall on the direct-link profile. It
   must accept UDP `50001` and TCP `50003`; it also opens an outbound TCP
   connection to the Mac on `50002`.
4. Let the Mac sender assign `169.254.1.1`, or configure that address manually
   and run with `--no-auto-ip`.
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
--window <WxH> --fullscreen --vsync --console
```

The receiver owns the negotiated resolution, frame rate, codec, and bitrate.
Use identical endpoint and port overrides on both sides.

## Common Setup Failures

| Symptom | Check |
| --- | --- |
| Mac bind error `EADDRNOTAVAIL`/errno 49 | Confirm `169.254.1.1` is assigned or use the intended `--bind` value. |
| Receiver waits for Mac | Check TCP `50003`, peer addresses, firewall, and that the sender is running. |
| Connected but black video | Check UDP `50001`, codec support, standard MTU, decoder logs, and keyframe requests. |
| Video works but input does not | Check TCP `50002`, macOS Accessibility permission, and cursor confinement state. |
| Windows shader load fails | Confirm build output contains `build\shaders`, or inspect runtime shader compilation errors. |
| CMake configure cannot fetch dependencies | Restore network access or use an already populated build cache; do not vendor new copies casually. |

See [MAC_TESTING.md](MAC_TESTING.md) or
[WINDOWS_TESTING.md](WINDOWS_TESTING.md) before declaring setup complete.
