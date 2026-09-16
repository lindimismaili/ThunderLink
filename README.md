# ThunderLink

ThunderLink is a two-process screen-streaming and input-forwarding system for a Mac sender and a Windows receiver connected over a direct Thunderbolt link. The Mac captures and encodes its display, the Windows application decodes and renders it with Direct3D 12, and mouse and keyboard events travel back to the Mac.

## Purpose

The project provides a low-latency, direct-link remote-display workflow without relying on a routed network. Its default link-local addresses are `169.254.1.1` (Mac) and `169.254.1.2` (Windows).

## Project outline

- [mac_sender/](mac_sender/) — macOS 13+ Swift/AppKit application. It captures a display with ScreenCaptureKit, encodes H.264 or HEVC video, and injects forwarded input.
- [windows_receiver/](windows_receiver/) — Windows C++17 application built with CMake. It receives, decodes, renders, and captures local input.
- [common/](common/) — shared C headers defining ports, protocol constants, frame headers, input-event fields, and socket helpers.
- [logs/README.md](logs/README.md) — explains the tracked per-device diagnostic logs.

The control channel coordinates `HELLO`, `CONFIGURE`, `START`, `STOP`, and keepalive messages over TCP port 50003. Encoded video travels from Mac to Windows over UDP port 50001; input travels from Windows to Mac over TCP port 50002.

## Setup and installation

1. Connect the Mac and Windows PC with the direct Thunderbolt link and ensure the link uses the default addresses above, or choose matching overrides.
2. On the Mac, grant the sender Screen Recording permission and Accessibility permission for input injection.
3. Install Xcode/Command Line Tools with Swift 5.9 or newer on the Mac.
4. On Windows, install Visual Studio with C++ desktop development, a Windows SDK, CMake 3.20+, and a shader compiler (`dxc` is preferred; `fxc` is supported). CMake downloads `nlohmann/json` and Dear ImGui during configuration.

## Building

Build the sender on the Mac:

```sh
cd mac_sender
swift build
```

Build the receiver from a Visual Studio developer prompt on Windows:

```cmd
cd windows_receiver
cmake -B build -S .
cmake --build build --config Release
```

## Usage

1. Start `build\\Release\\ThunderLinkReceiver.exe` on Windows.
2. Start the sender on the Mac:

   ```sh
   cd mac_sender
   swift run ThunderLinkSender
   ```

3. The sender connects to the receiver, completes the control handshake, and begins streaming. By default it configures the direct-link address if needed; use `--no-auto-ip` to disable that behavior.

To use different endpoints or encoder settings, pass sender arguments such as:

```sh
swift run ThunderLinkSender --peer 169.254.1.2 --bind 169.254.1.1 --codec hevc --bitrate 25
```

## Testing

There is currently no automated test target. Verify a change with a clean build on its target platform, then run both applications over the direct link and confirm:

- the Windows receiver completes the control handshake and displays the Mac stream;
- mouse and keyboard input reaches the Mac correctly;
- changing the negotiated resolution or codec does not interrupt the stream; and
- [`logs/mac.log`](logs/mac.log) and [`logs/windows.log`](logs/windows.log) contain no unexpected errors.

For wire-protocol changes, update the shared headers in `common/` and the mirrored Swift definitions in `mac_sender/Sources/Network/NetworkConfig.swift` together; otherwise the two applications can silently desynchronize.

## Conclusion

ThunderLink keeps the transport contract in `common/` while allowing each platform to use its native capture, graphics, and input APIs. Build and validate both sides together whenever networking, frame format, or input-event behavior changes.
