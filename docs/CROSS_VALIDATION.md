# Cross validation record

Host: native macOS (`uname -s`: Darwin). Work is in the existing dirty working
tree; no commit, push, branch switch, or reset was performed for Cross. Existing
Screen Share/desktop changes and per-device logs were preserved.

## Executed checks

| Check | Result |
| --- | --- |
| Portable CTest in `/tmp/thunderlink-cross-tests` | 16/16 passed (geometry, settings, framing, heartbeat, shortcut/modifier state, existing transport/protocol/input/frame regressions). |
| Cross AddressSanitizer + UndefinedBehaviorSanitizer | 5/5 passed (CrossLayoutTests, CrossSettingsTests, CrossFramingTests, CrossInputStateTests, CrossHeartbeatTests). |
| Shared protocol | C and Swift set to v4; unchanged 48-byte video layout; v2/v3 peers rejected by portable control test. |
| Mac debug/release build and Swift tests | Passed; `./scripts/test-mac.sh` ran 28 tests across 6 suites, including existing lifecycle/input/protocol regressions. |
| Mac rendered Cross UI | Cross and pairing states inspected at the 780-point window size; offscreen synthetic previews, not a live paired session. |
| Mac package | `./scripts/package-mac.sh` passed, including plist lint and ad-hoc signature validation. |
| Native Windows Debug/Release and tests | Not run: no native Windows execution environment in this session. |
| Two-device operation | Not run: requires the native Windows peer and physical devices. |

Portable commands used:

```sh
cmake -S tests -B /tmp/thunderlink-cross-tests -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_JSON=/tmp/thunderlink-desktop-tests/_deps/json-src
cmake --build /tmp/thunderlink-cross-tests --parallel
ctest --test-dir /tmp/thunderlink-cross-tests --output-on-failure
cmake -S tests -B /tmp/thunderlink-cross-sanitizers -DCMAKE_BUILD_TYPE=Debug \
  -DFETCHCONTENT_SOURCE_DIR_JSON=/tmp/thunderlink-desktop-tests/_deps/json-src \
  '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build /tmp/thunderlink-cross-sanitizers --target CrossLayoutTests CrossSettingsTests CrossFramingTests CrossInputStateTests CrossHeartbeatTests --parallel
ctest --test-dir /tmp/thunderlink-cross-sanitizers -R '^Cross' --output-on-failure
```

The JSON source override reused the existing pinned dependency cache on this
machine. On another machine, omit that override to let CMake fetch the pinned
version. Tests use fakes/temporary files and do not inject mouse/keyboard events
into desktop applications or read pre-existing credentials.

Mac commands used: `./scripts/test-mac.sh`,
`swift build --package-path mac_sender -c release`, and
`./scripts/package-mac.sh`. The arm64 artifact is
`dist/ThunderLink-0.1.0-macos-arm64.zip`, SHA-256
`b92c62ac8825f6d189b269a0df83ea77819a6bf60a86d2d4d03c9c2ef49f5d24`.
Existing Swift Sendable and CommandLineTools linker-path warnings were non-fatal.
Certificate tests used ephemeral in-memory keys; actual production Keychain
identity persistence and live TLS interoperability remain unverified.

## Physical gates — all pending

Run the Cross sections in [MAC_TESTING.md](MAC_TESTING.md) and
[WINDOWS_TESTING.md](WINDOWS_TESTING.md) against the same source version.

- Windows hook/Raw Input coexistence, suppression, pointer hiding, release
  quarantine, emergency chord, and high-rate input.
- Native Mac/Windows TLS interoperability and pairing approval on both screens;
  changed-identity rejection, Forget Device, re-pairing, and silent reconnect only
  for approved identities.
- Multiple monitors, negative origins, Retina/mixed DPI, portrait displays,
  edge offsets, arrangement changes, and background/hidden operation.
- Thunderbolt networking: actual Windows adapter/driver support and direct cable.
- Direct Ethernet, switched Ethernet, Wi-Fi/Wi-Fi, and mixed Ethernet/Wi-Fi.
- Adapter loss/failover, DHCP address changes, multicast blocked/manual entry,
  firewall/client-isolation errors, latency/loss, queue saturation, and peer crash.
- Lock/sleep/wake, permission changes, disconnect/quit, monitor unplugging, and
  repeated Screen Share/Cross transitions with held keys/buttons.
- Packaged apps launched outside the checkout, Mac local-network permission,
  Windows firewall/DNS-SD availability, and Cross on Windows N without media.

These are unverified runtime gates, not successful checks inferred from source,
portable tests, or a Mac build. Cross uses OS IP adapters; a USB-C/Thunderbolt
connector by itself does not establish a network path.
