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
ctest --test-dir build -C Release --output-on-failure
```

Record the exact commands, generator, architecture, exit status, warnings, and
embedded shader initialization result. A successful Windows build
proves compilation only; it does not prove Mac capture/encode, packet delivery,
Media Foundation decode at runtime, rendering, or input injection.

Also build Debug and run its CTest configuration. Tests include portable
protocol/reassembly, loopback control/input, statistics, and forwarded-state
coverage; the native window/input test does not inject events into the Mac.
`DecoderContractTests` uses fake MFTs to force negotiation rejection, async
events, stream changes, and same-codec software fallback. `DecoderIntegrationTests`
checks installed codecs and worker shutdown. `TextureIntegrationTests` exercises
repeated NV12 texture changes on WARP with the D3D12 debug layer; exit 77 means
the layer is unavailable and the check remains pending, rather than passing.
During video tests use the D3D12 debug layer and inspect first-frame upload and
repeated texture resizing for resource-state/lifetime errors. Verify hardware
decode and same-codec software fallback separately; absent codec support must
produce a visible error without a render-thread initialization loop.

## 3. Local Smoke Test

1. Run `.\build\Release\ThunderLink.exe --console` when a console is
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
   Repeat with rapid reconfiguration and cable interruption. Hold a key or
   mouse button while switching focus/overlay, then confirm the Mac receives
   releases. Verify `--fullscreen`, F11 restoration, and idle bandwidth/FPS.
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

## Portable desktop acceptance

Run `scripts/package-windows.ps1` in a Visual Studio developer PowerShell. This
builds/tests Release and packages `dist/ThunderLink.exe`. Also run Debug native
tests using the commands above. Inspect imports with `dumpbin /DEPENDENTS`:
no VCRUNTIME/MSVCP runtime dependency is expected. Embedded shader source compilation
must work without the checkout or a `shaders` directory.

Copy only the EXE to a folder with spaces and Unicode and launch it from another
working directory on a Windows installation without developer tools. Confirm
Local AppData settings/logs and functioning resources. Test first launch, invalid
settings recovery, Save and Restart, and temporary CLI overrides. Missing Media
Foundation or codec support must be reported clearly.

Inspect Connection, Settings, Diagnostics, and the viewer at 100%, 150%, and 200%
DPI, including a live monitor change. Use keyboard navigation and test the minimum
window size. Verify Close hides to the tray, both click menus contain only Show
and Quit, Show focuses the retained window, and relaunch does not duplicate it.
Restart Explorer while hidden and verify the icon returns. OS shutdown must exit.

Hide while streaming and while holding keys/buttons: Mac input must be released,
the cursor must be free, network/decode queues must remain bounded, and hidden
frames must not be presented. Restore both windowed and fullscreen sessions and
verify current video resumes without stale input. Check idle CPU usage. Disconnect
must suspend reconnection until Connect; quitting must perform full cleanup.

## Cross acceptance

Build native Debug/Release including Cross Schannel, DNS-SD, display enumeration,
hooks, Raw Input, and arrangement UI. Run portable Cross geometry/settings tests
and native identity/transport tests. Verify the Cross dashboard remains usable
without starting Media Foundation or receiving video.

Exercise mouse movement and keyboard suppression while another desktop app has
focus and while ThunderLink is hidden. Check pointer parking/hiding/restoration,
high-rate mice, modifiers and repeats, the configurable emergency shortcut,
Escape/F1/F11 forwarding, physical-key release quarantine, and bounded queues.
Inspect the arrangement at 100/150/200% DPI with portrait/mixed-resolution monitors;
verify keyboard placement, snapping, Identify, scale, and invalid-layout errors.

Run the physical matrix and fault scenarios in the Mac Cross acceptance section
with the same builds. Verify no forwarded clicks/keys reach Windows applications,
no remote state survives lease expiry, and reconnection does not regain ownership
until a fresh edge crossing. Discovery failure must still allow manual reachable
addresses; changed certificates must never become trusted automatically. Confirm
Direct Ethernet, Thunderbolt networking, Wi-Fi, and mixed LAN results individually
in CROSS_VALIDATION.md rather than inferring them from a successful build.
