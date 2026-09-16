# Windows Testing Protocol

Use this protocol on native Windows. A Windows-only result does not prove Mac
capture/encode behavior or complete end-to-end operation.

## 1. Preflight

- Confirm `[System.Environment]::OSVersion.Platform` reports `Win32NT` and
  record the Windows version. Do not count WSL as native Windows testing.
- Inspect `git status --short`; preserve unrelated files and existing logs.
- Read [SETUP.md](SETUP.md) and identify the change's affected subsystems.
- Use a Visual Studio developer shell and confirm MSVC, CMake, Windows SDK,
  Direct3D 12, Media Foundation, intended peer address, and physical link state.
- Confirm the direct-link firewall profile allows the required receiver ports.

## 2. Build Verification

From `windows_receiver\`, run:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Record the exact commands, generator, architecture, exit status, warnings, and
shader compiler path or runtime-fallback message. A successful Windows build
proves compilation only; it does not prove Mac capture/encode, packet delivery,
Media Foundation decode at runtime, rendering, or input injection.

## 3. Local Smoke Test

1. Run `.\build\Release\ThunderLinkReceiver.exe --console` when a console is
   useful for diagnostics.
2. Confirm the window opens, remains responsive, and shows the waiting state
   when the Mac is absent.
3. Verify the toolbar, Logs panel, Stats overlay, close control, and resize
   behavior do not crash or obscure the video area.
4. Exercise F1 overlay visibility, F11 fullscreen/windowed restoration, Escape
   cursor release, and VSync toggling.
5. Exit normally and confirm the process, sockets, and optional console close.

## 4. Cross-Device Integration

With a natively built Mac sender on the direct link:

1. Start Windows first, then start the Mac sender with the intended addresses.
2. Confirm `HELLO -> CONFIGURE -> START`, connected status, and continuous
   encoded video without persistent corruption or black frames.
3. Test H.264 and HEVC separately when the receiver supports them; do not assume
   unsupported HEVC will silently pass or downgrade.
4. Exercise source/native, 1440p, 1080p, 720p, and 480p choices as applicable.
   Confirm aspect ratio, correct decoded dimensions, resize behavior, fullscreen,
   and useful image quality.
5. Test pointer movement, left/right buttons, scrolling, common keys, modifiers,
   shortcuts, focus transitions, cursor confinement, and Escape release.
6. Compare VSync off/on for presentation stability and latency; verify F1/F11
   behavior while connected.
7. Stop and restart either application. Confirm disconnect state, reconnect, a
   fresh keyframe, resumed video, and resumed input.
8. Check `logs/windows.log`, `logs/mac.log`, the Logs panel, and Stats overlay
   for unexpected errors, repeated frame loss, or decoder initialization loops.

## 5. Change-Specific Checks

| Change area | Required additional coverage |
| --- | --- |
| Decoder/rendering | H.264 and HEVC as supported; first-frame initialization; resolution changes; device/present errors; aspect ratio; fullscreen and resize. |
| Video reassembly | Missing, duplicate, late, and out-of-order packets; timeout/drop behavior; keyframe recovery; valid frame dimensions and sizes. |
| Control protocol | Matching version and fields on both sides; startup, runtime configure, ping/pong, stop, bye, and malformed/closed connection behavior. |
| Input | Coordinate edges and corners, buttons, wheel directions, keyboard mappings, modifiers, focus changes, confinement, and release. |
| Receiver UI | Waiting/connected status, toolbar, logs, stats, resolution selector, VSync, keyframe request, DPI scaling, keyboard access, and readable contrast. |
| Networking | Default and overridden endpoints/ports, firewall behavior, reconnect, unavailable peer, and standard 1500-byte MTU behavior. |

Any wire-contract change must be checked against `common/` and
`mac_sender/Sources/Network/NetworkConfig.swift`, then built and exercised on
both platforms.

## 6. Evidence and Reporting

Report:

- PC/GPU, Windows version, MSVC/CMake/SDK versions, display scaling, and codec.
- Commit or working-tree state tested, commands run, and their results.
- Whether testing was build-only, local smoke, or physical two-device end to
  end.
- Observed resolution/FPS, renderer/decoder state, relevant log timestamps, and
  any screenshots requested by the user.
- Every skipped check and the concrete device, permission, or environment gate.

Do not publish secrets, credentials, private screen contents, or unredacted
personal data as evidence.

## 7. Failure Handling

- Stop and preserve logs when a crash, device removal, protocol mismatch,
  persistent black frame, input runaway, or cursor-confinement failure occurs.
- Press Escape to release confinement before continuing input diagnosis.
- Restore window/fullscreen, VSync, firewall, and adapter settings changed for
  the test.
- Separate compile failures, shader failures, network failures, decoder
  failures, renderer failures, and cross-device failures; do not mask one by
  changing unrelated configuration.
- Do not erase or overwrite the other device's tracked log.

## 8. Completion Criteria

- **Build verified:** the affected Windows configurations compile successfully.
- **Local behavior verified:** the window, controls, renderer startup, and
  locally observable paths pass on native Windows.
- **End to end verified:** both native applications pass every applicable step
  in Section 4 over the physical link.

State the highest tier actually reached. Work requiring macOS or physical-link
evidence remains pending until that evidence exists.
