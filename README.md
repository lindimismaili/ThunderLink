# ThunderLink

ThunderLink connects a Mac and Windows PC in two modes. **Screen Share** streams the Mac display to Windows with mouse and keyboard forwarding. **Cross** shares the Windows mouse and keyboard across the edges of the Mac's own displays, without transmitting video, over Thunderbolt networking, Ethernet, or Wi-Fi/LAN.

See [Cross setup and monitor arrangement](docs/CROSS.md) and [validation status](docs/CROSS_VALIDATION.md).

## Portable desktop apps

ThunderLink now has coordinated dark desktop windows with Connection, Settings,
and Diagnostics. Close hides to the Mac menu bar or Windows tray; its menu has
**Show** and **Quit**. Hidden sessions remain connected.

Build portable releases from the repository root on each native platform:

```sh
./scripts/package-mac.sh
```

```powershell
.\scripts\package-windows.ps1
```

Open `dist/ThunderLink.app` on Mac or `dist/ThunderLink.exe` on Windows. Versioned
ZIPs and checksums are also written to `dist/`. The Windows artifact must be built
on Windows. See [desktop setup](docs/SETUP.md#portable-desktop-apps) and the
[desktop validation record](docs/DESKTOP_VALIDATION.md) for remaining checks.

## Purpose

Screen Share provides a low-latency remote-display workflow with default direct-link addresses `169.254.1.1` (Mac) and `169.254.1.2` (Windows). Cross discovers and pairs computers using their existing network addresses, including DHCP and link-local addresses, and keeps input sharing independent of video.

## Project outline

- [mac_sender/](mac_sender/) — macOS 13+ Swift/AppKit application. It captures a display with ScreenCaptureKit, encodes H.264 or HEVC video, and injects forwarded input.
- [windows_receiver/](windows_receiver/) — Windows C++17 application built with CMake. It receives, decodes, renders, and captures local input.
- [common/](common/) — shared C headers defining ports, protocol constants, frame headers, input-event fields, and socket helpers.
- [logs/README.md](logs/README.md) — explains the tracked per-device diagnostic logs.

The control channel coordinates `HELLO`, `CONFIGURE`, `START`, `STOP`, and keepalive messages over TCP port 50003. Encoded video travels from Mac to Windows over UDP port 50001; input travels from Windows to Mac over TCP port 50002.

## Setup and installation

1. Connect the Mac and Windows PC with the direct Thunderbolt link and ensure the link uses the default addresses above, or choose matching overrides.
2. On the Mac, grant the sender Screen Recording permission and Accessibility permission for input injection.
3. For source builds only, install Xcode/Command Line Tools with Swift 5.9 or newer on the Mac.
4. For Windows source builds only, install Visual Studio with C++ desktop development, a Windows SDK, CMake 3.20+. CMake downloads `nlohmann/json` and Dear ImGui during configuration.

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

1. Start `build\\Release\\ThunderLink.exe` on Windows.
2. Start the sender on the Mac:

   ```sh
   cd mac_sender
   swift run ThunderLinkSender
   ```

3. The sender connects to the receiver, completes the control handshake, and begins streaming. Configure the direct-link address explicitly in Settings → Configure Mac Adapter when needed.

To use different endpoints or encoder settings, pass sender arguments such as:

```sh
swift run ThunderLinkSender --peer 169.254.1.2 --bind 169.254.1.1 --codec hevc --bitrate 25
```

## Testing

Run the portable C++ regression suite with `cmake -S tests -B /tmp/thunderlink-tests`, `cmake --build /tmp/thunderlink-tests`, and `ctest --test-dir /tmp/thunderlink-tests --output-on-failure`. Run sender tests with `scripts/test-mac.sh` (Swift 6 or newer). Native Windows builds also include Media Foundation, D3D12 texture, and window/input integration targets. Then run both applications over the direct link and confirm:

- the Windows receiver completes the control handshake and displays the Mac stream;
- mouse and keyboard input reaches the Mac correctly;
- changing the negotiated resolution or codec does not interrupt the stream; and
- [`logs/mac.log`](logs/mac.log) and [`logs/windows.log`](logs/windows.log) contain no unexpected errors.

Both applications use protocol v4 and reject incompatible peers. The 48-byte, session-scoped video header remains unchanged, so `common/fixtures/frame_header_v3.hex` is still checked by both test suites. Update shared headers in `common/`, the [Cross wire contract](common/CROSS_PROTOCOL.md), and the Swift mirrors together. See [the repair validation record](docs/REPAIR_VALIDATION.md) and [Cross validation](docs/CROSS_VALIDATION.md) for verified checks and outstanding native/physical gates.

## Conclusion

ThunderLink keeps the transport contract in `common/` while allowing each platform to use its native capture, graphics, and input APIs. Build and validate both sides together whenever networking, frame format, or input-event behavior changes.
