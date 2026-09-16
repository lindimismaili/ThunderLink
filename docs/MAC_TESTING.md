# macOS Testing Protocol

Use this protocol on a native Mac. A Mac-only result does not prove Windows
decode/render behavior or complete end-to-end operation.

## 1. Preflight

- Confirm `uname -s` reports `Darwin` and record the macOS version.
- Inspect `git status --short`; preserve unrelated files and existing logs.
- Read [SETUP.md](SETUP.md) and identify the change's affected subsystems.
- Confirm the Swift toolchain, Screen Recording permission, Accessibility
  permission, intended display, peer address, and physical link state.
- For a local-only launch, use `--no-auto-ip` unless the user intends the sender
  to reconfigure the network service.

## 2. Build Verification

From `mac_sender/`, run:

```sh
swift build
swift build -c release
../scripts/test-mac.sh
```

Record the exact commands, exit status, and warnings. A successful Swift build
proves compilation only; it does not prove capture permission, hardware encode,
network transport, Windows decode, or input injection.

From the repository root, also run the portable C++ suites (CMake and a C++17
compiler must already be installed):

```sh
cmake -S tests -B /tmp/thunderlink-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/thunderlink-tests --parallel
ctest --test-dir /tmp/thunderlink-tests --output-on-failure
```

These tests use fake clocks/sinks and loopback sockets, without display changes
or input injection. The network tests fetch the same pinned JSON dependency as
the receiver build. Use separate build directories with
`-DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` and
`-DCMAKE_CXX_FLAGS='-fsanitize=thread -fno-omit-frame-pointer'` for sanitizer runs.
Do not combine ThreadSanitizer with AddressSanitizer. No full Xcode installation
is required when a working Command Line Tools Swift toolchain is available.

`scripts/test-mac.sh` runs `swift test --disable-xctest` with Swift Testing.
On Swift 6.4 Command Line Tools it explicitly loads the bundled Testing macro
plugin, which that toolchain's build backend does not discover automatically.
It neither installs packages nor changes the selected developer directory.

## 3. Local Smoke Test

1. Run `swift run ThunderLinkSender --no-auto-ip` when no network change is
   intended.
2. Confirm the desktop window and ThunderLink status icon appear. Close hides
   the window; the icon menu contains exactly Show and Quit.
3. Open the status panel and verify waiting/connected state, peer address,
   permission state, and Quit behavior are readable and accurate.
4. When a receiver is absent, confirm retries do not crash or freeze the UI.
5. Quit normally and confirm the sender stops cleanly and does not leave the
   main display in a changed mode.

## 4. Cross-Device Integration

With a natively built Windows receiver on the direct link:

1. Start Windows first, then start the Mac sender with the intended addresses.
2. Confirm `HELLO -> CONFIGURE -> START`, connected status, and continuous
   encoded video without persistent corruption or black frames.
3. Test H.264 and HEVC separately when both machines support them; do not infer
   one codec from the other.
4. Exercise source/native, 1440p, 1080p, 720p, and 480p choices as applicable.
   Confirm aspect ratio, even dimensions, cursor mapping, and stable recovery.
5. From Windows, test pointer movement, left/right buttons, scrolling, common
   keys, modifiers, shortcuts, and cursor release/reacquisition.
6. Stop and restart either application. Confirm disconnect state, Mac display
   restoration, reconnect, a fresh keyframe, resumed video, and resumed input.
   Repeat during startup and rapid resolution changes; verify old session
   packets cannot restore stale video/input. Disconnect the cable and confirm
   peer timeout, cleanup, and recovery when it is reconnected.
7. Check `logs/mac.log`, `logs/windows.log`, and the visible Windows diagnostics
   for unexpected errors, repeated frame loss, or decoder initialization loops.

## 5. Change-Specific Checks

| Change area | Required additional coverage |
| --- | --- |
| Capture/display | Main and selected secondary display; native and scaled capture; restoration after reconfigure, disconnect, error, and quit. |
| Encoder/video packets | H.264 and HEVC as supported; keyframes; packet count/size; resolution metadata; recovery after loss. |
| Control protocol | Matching version and fields on both sides; startup, runtime configure, ping/pong, stop, bye, and malformed/closed connection behavior. |
| Input | Coordinate edges and corners, buttons, wheel directions, keyboard mappings, modifiers, focus changes, and Accessibility-denied behavior. |
| Menu UI | Waiting, connected, streaming, permission warning, status icon, dynamic values, keyboard access, and VoiceOver labels. |
| Networking | Default and overridden endpoints/ports, reconnect, unavailable peer, and standard 1500-byte MTU behavior. |

Any wire-contract change must be checked against `common/` and
`mac_sender/Sources/Network/NetworkConfig.swift`, then built and exercised on
both platforms.

## 6. Evidence and Reporting

Report:

- Mac model, macOS version, Swift/Xcode version, display arrangement, and codec.
- Commit or working-tree state tested, commands run, and their results.
- Whether testing was build-only, local smoke, or physical two-device end to
  end.
- Observed resolution/FPS, connection and permission states, relevant log
  timestamps, and any screenshots requested by the user.
- Every skipped check and the concrete device, permission, or environment gate.

Do not publish secrets, credentials, private screen contents, or unredacted
personal data as evidence.

## 7. Failure Handling

- Stop and preserve logs when a crash, protocol mismatch, persistent black
  frame, input runaway, or display-restoration failure occurs.
- Release cursor confinement from Windows before continuing input diagnosis.
- Restore the Mac display and network settings if the test changed them.
- Separate compile failures, permission failures, network failures, codec
  failures, and cross-device failures; do not mask one by changing unrelated
  configuration.
- Do not erase or overwrite the other device's tracked log.

## 8. Completion Criteria

- **Build verified:** the affected Mac configurations compile successfully.
- **Local behavior verified:** the menu UI, lifecycle, and locally observable
  paths pass on macOS.
- **End to end verified:** both native applications pass every applicable step
  in Section 4 over the physical link.

State the highest tier actually reached. Work requiring Windows or physical
link evidence remains pending until that evidence exists.

## Portable desktop acceptance

Run `scripts/package-mac.sh`. Copy/unzip the artifact outside the checkout into
a path containing spaces and Unicode; launch from Finder and an unrelated cwd.
Verify the bundle signature and deployment target, resource availability, and
per-user logs. No adapter changes or administrator prompt should occur at launch.

Inspect all three pages at default and minimum sizes. Exercise keyboard editing,
validation errors, Reset to Defaults, Save and Restart, CLI override precedence,
and failed/unwritable settings storage. Verify Screen Recording and Accessibility
states independently, with permission changes refreshed on app activation.

Close/show repeatedly while waiting and streaming. Check that the Dock entry hides
with the window, the status menu has exactly Show and Quit, and a second launch
shows the existing process. Disconnect must stay disconnected until Connect or a
new launch. Quit while hidden and during startup; verify cleanup and display
restoration. `DesktopPreviewTests` creates offscreen AppKit render artifacts in
`mac_sender/.build/desktop-previews`; these supplement native interaction checks.

## Cross acceptance

Run the Swift Cross protocol, topology, pairing-code, lease, and input tests,
including the shared `common/fixtures/cross_v4.json` fixture. Verify Cross can
become ready with Screen Recording denied and Accessibility granted, with no
capture/encoder/video socket or display-mode change. Inspect pairing and Cross
states at default/minimum desktop sizes. Use test doubles for injected input in
automated tests; do not send test keystrokes into the user's applications.

With native Windows, follow [CROSS.md](CROSS.md) and record separate results for
Thunderbolt IP networking, direct Ethernet, switched Ethernet, Wi-Fi/Wi-Fi, and
Ethernet/Wi-Fi. Check external/Retina/negative-origin/portrait displays, all shared
edges, modifier mapping, dragging within Mac, return to Windows, and hidden apps.
Compare pairing codes, deny pairing, forget/re-pair, and reject changed identities.
Test cable removal with Wi-Fi fallback, blocked discovery/manual addressing,
DHCP changes, sleep/lock, permission loss, peer crash, display unplugging, and
mode switching while keys/buttons are held. Confirm all state is released, fresh
sessions are required, and no characters appear in logs. Native Windows and each
physical transport remain separate evidence gates; record them in CROSS_VALIDATION.md.
