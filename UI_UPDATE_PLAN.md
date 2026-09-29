# ThunderLink shared control UI migration

Status: implementation in progress. The shared frontend and build/resource
pipeline exist, but both desktop apps still use their native dashboards. The
native webview hosts and action adapters are required before rollout.

## Implementation checkpoint (2026-09-28, Windows host)

- Added `shared_ui/` with the versioned contract, Lit controls, Dark Studio
  CSS, arrangement preview, local fixture host, and deterministic asset build.
  Chromium and WebKit fixture suites pass (6 tests); unit tests pass (8 tests).
- Added SwiftPM resource packaging and Windows embedded resources generated
  from the same manifest. The Windows resource test verifies allowlisted paths
  and hashes from the packaged executable. Shared assets are 224,036 bytes,
  within the 2 MiB budget.
- Added native command admission gates and shared fixtures. The Windows
  portable suite passes 21/21, and native Debug/Release suites pass 23/23.
  The Swift gate and Mac package have not been compiled on a Mac.
- The Windows portable ZIP build completes and verifies its embedded UI
  resources. It still launches the existing ImGui dashboard. No WebView2 or
  WKWebView host, native command dispatch, Evergreen recovery, offline kit,
  two-device validation, or Mac native test has passed yet. Do not mark any
  migration acceptance checkbox complete on the basis of this checkpoint.

## 1. Outcome and fixed decisions

Replace the Mac SwiftUI desktop controls and Windows ImGui dashboard with one
shared web control UI, including Cross pairing and display arrangement. Keep
ThunderLink as two native desktop applications, with local UI assets embedded
inside their windows. Launching the app must not open a browser or start an HTTP
server. The native media/input engines remain responsible for streaming and
device control.

| Decision | Required implementation |
| --- | --- |
| Shared frontend | `shared_ui/`: TypeScript, Lit components, Vite, plain CSS; npm with a committed lockfile. Use Node 24 LTS for development/builds. End users need neither Node nor npm. |
| Native hosts | WKWebView in the existing Mac NSWindow; WebView2 in the existing Windows dashboard HWND. Preserve the existing app entrypoints and native service ownership. |
| Windows distribution | Use the installed Evergreen WebView2 Runtime. If missing or too old, show a small native setup/recovery window with explicit Install/Update, Retry, and Quit actions. |
| Offline setup | Supply a separate optional Windows offline setup kit containing Microsoft's Evergreen standalone installer. Do not bundle a private Fixed Version runtime with the default app. |
| Visual design | Preserve Dark Studio. One CSS token source, locally bundled Inter font with its license, shared SVG icons, and shared components control both platforms. Retain native title bars and OS permission dialogs. |
| Coverage | Migrate every existing dashboard control. Keep the Windows D3D12 viewer and its compact ImGui toolbar/stats overlay native; route their actions through the same native controller used by the web dashboard. |
| Platform roles | Windows still selects the session mode and owns Cross arrangement. Mac still owns capture, permissions, and power controls. Render capability-based differences in shared components; do not invent remote configuration APIs to make both sides show identical buttons. |
| Synchronization | Both packages contain the same frontend build and UI contract version. This means shared presentation and behavior, not replication of machine-local settings over the network. |
| Compatibility | Preserve existing settings, CLI precedence, pairing stores, protocol v4, the 48-byte video header, and Cross authentication/ownership semantics. |

Microsoft recommends Evergreen for most apps; it is shared and updated separately
from ThunderLink. Missing-runtime installation can use its small online
bootstrapper or an offline standalone installer. A private Fixed Version runtime
would add substantial size and app-owned servicing responsibilities.
[Microsoft distribution guidance](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution)

Identical source, fonts, spacing, and components are required. Pixel-perfect
equivalence of OS chrome and text antialiasing is not an acceptance requirement.
Both native platforms must still be built and tested.

## 2. Existing code and planned ownership

Read `AGENTS.md`, `docs/SETUP.md`, `docs/ARCHITECTURE.md`, `docs/DESIGN.md`,
`docs/CROSS.md`, and both native testing guides before implementation. Identify
the host OS natively and preserve all unrelated working-tree changes, including
the existing story deletions. Do not commit, push, switch branches, install OS
packages, or change agent configuration as part of this task.

| Area | Existing entrypoints | Change |
| --- | --- | --- |
| Shared UI | New `shared_ui/` package | Components, CSS tokens, bridge types/schema, fixtures, frontend tests, preview and build tooling. |
| Mac shell | `mac_sender/Sources/UI/StatusController.swift`, `AppDelegate.swift` | Retain window/status menu and app lifecycle; extract presentation-independent actions/state and install WKWebView. Separate `AppState` from the old SwiftUI views. |
| Windows shell | `windows_receiver/src/App/Application.{h,cpp}`, `Window.{h,cpp}` | Extract dashboard actions/state, add WebView2 host, handle focus/visibility/resize, remove migrated ImGui dashboard/arrangement rendering. |
| Domain actions | Mac `DesktopSettings`, `CrossServer`, power controllers; Windows `DesktopSettings`, `CrossController`, `CrossSettings` | Reuse current validation and persistence; expose narrow UI adapters. Move mutations currently performed while drawing into native lifecycle/controller code. |
| Assets/build | `mac_sender/Package.swift`, `windows_receiver/CMakeLists.txt`, Windows resource template | Package one compiled frontend with both applications; pin WebView2 SDK and statically link its loader. |
| Delivery/tooling | `scripts/dev.*`, `scripts/package-*`, `scripts/test-mac.sh`, `.vscode/tasks.json`, `.gitignore` | Build and invalidate UI assets correctly, add preview/check commands, package runtime recovery and optional offline kit. |
| Tests/docs | Existing portable/native test suites and desktop preview tests; setup/design/architecture/testing docs | Add bridge and UI suites, replace SwiftUI preview coverage, document new build/runtime requirements and measured results. |

Use the existing `web-ui-design` workflow for visual implementation and browser
QA. Use installed tools where available; missing native machines or compilers
are explicit verification gates, never grounds to claim a passing result.

## 3. Shared UI and native bridge

### UI structure and state

- Organize components into shell/navigation, Connection, Settings, Diagnostics,
  Cross pairing, Cross controls, and an SVG display-arrangement editor. Shared
  buttons, fields, cards, status messages, and dialogs use one design system.
- Keep Connection, Settings, Diagnostics navigation and the current Dark Studio
  colors. Use a roughly 200px sidebar, 8px spacing increments, 24-32px content
  padding, and scrollable content. Collapse navigation on narrow previews.
- Use semantic HTML controls, visible focus, labels, live error/status regions,
  reduced-motion support, and keyboard equivalents for every canvas operation.
  Use CSS state classes/custom properties rather than platform-specific markup.
- The native host is authoritative for connection, trust, topology, settings,
  permissions, and operation results. Frontend state owns navigation, unsaved
  settings, arrangement drafts, selection, zoom, and pan only.
- Keep saved settings distinct from effective CLI-overridden settings. Incoming
  status updates must not overwrite edits, move focus/caret, reset scroll, or
  discard an arrangement draft. Successful saves update the saved baseline;
  validation failures retain the draft and show field/action errors.
- On incompatible topology changes, mark the draft stale and require an explicit
  refresh/review. Do not apply a draft against a different peer or topology.

### Local message contract

Define a versioned contract in `shared_ui/contract/` with JSON Schema, TypeScript
types, and valid/invalid fixtures consumed by frontend, Swift, and C++ tests.
Use JSON messages, not arbitrary native objects exposed to JavaScript.

| Message | Minimum fields and behavior |
| --- | --- |
| Ready/hello | UI contract version and asset build hash. Host replies with role, capabilities, effective/saved state, and a fresh UI instance ID. |
| Command | Contract version, UI instance ID, unique request ID, action, validated payload; include pairing/topology/settings revision where relevant. |
| Result | Matching request ID, success/data or stable error code with user-facing message and optional field errors. Long actions report accepted/pending then an explicit completion or failure. |
| State | Monotonic revision, capabilities, session/connection, settings, permissions/power, Cross state, and diagnostics. Ignore older revisions. Represent 64-bit IDs/revisions as strings where JavaScript precision would be lost. |

Implement native command groups for connection/mode, saved settings/reset/restart,
Mac permissions/power/adapter setup, Cross discovery/pairing/preferences/layout,
viewer navigation/configuration, and diagnostics/log access. Each adapter checks
capability and current native state; disabled or absent frontend buttons are not
the authorization boundary. Pairing approval binds to the current pending peer
and pairing attempt; layout apply binds to peer identity and topology revision.

- Validate message shape, types, numeric/string bounds, and command payloads
  before dispatch. Cap commands at 64 KiB and state/result messages at 1 MiB;
  bound log/diagnostic payloads and report truncation explicitly.
- Invoke existing asynchronous controller operations on their required thread
  or queue. Never perform capture, decode, network waits, administrator prompts,
  or blocking persistence inside the JavaScript/render callback.
- Send connection/error/pairing transitions promptly; coalesce presentation
  snapshots to at most 10 Hz and ordinary metrics/log refresh to 1 Hz. Pause
  dashboard telemetry while hidden and send a fresh snapshot when shown.
- A new UI instance invalidates old callbacks and pending request delivery.
  Do not replay commands after reload. Deduplicate request IDs for the current
  instance, retaining in-flight entries and a bounded recent-result cache;
  reject excess pending work visibly. A delayed response never triggers a second
  save, approval, restart, or installer launch.
- Keep authentication material in native secure storage. Only UI-required peer
  identifiers and the current verification code enter the view. Render device
  names, errors, and log text as text, never as HTML.

### Resource loading and recovery

- Bundle all HTML, JS, CSS, icons, and fonts locally. No CDN, remote UI updates,
  service worker, localhost server, or release-mode mock bridge.
- Windows serves an allowlisted asset manifest from embedded resources through
  WebView2 request interception at `https://thunderlink.invalid/`. Mac serves its
  resource bundle through a WKURLSchemeHandler at `thunderlink-ui://app/`.
  Reject traversal, unknown paths, unexpected schemes, and incorrect MIME types.
- Compile scripts into a local classic bundle (IIFE) with extracted CSS and
  relative asset URLs, avoiding runtime module fetching across custom schemes.
- Accept commands only from the trusted main document. Apply a restrictive CSP,
  deny external navigation/popups/downloads and unneeded web permissions, and
  disable developer tooling in release builds. Native handlers open only the
  existing allowlisted OS settings/log destinations.
- On bridge-version mismatch, missing assets, or renderer-process failure, show
  a native recovery view with Retry, Disconnect, and Quit. Preserve native
  cleanup paths even if JavaScript is unavailable. Do not resurrect the full
  legacy dashboard as a permanent fallback.
- Normal dashboard hide/reload does not own a session. Explicit webview crash
  recovery releases Screen Share forwarded input and pauses Cross ownership;
  reconnecting the UI must not silently resume remote control. Use existing
  native release/pause operations and document this recovery behavior.

## 4. Feature migration checklist

The implementer must map every current UI action to its native owner and a test
before deleting the old view. Preserve these behaviors in the shared UI:

| Surface | Required controls and behavior |
| --- | --- |
| Connection | Truthful disconnected/waiting/preparing/streaming/error states; peer/role; Connect/Disconnect; actual resolution/FPS/codec/bitrate; Windows Back to Video; actionable setup guidance. |
| Mode | Screen Share/Cross selector on Windows; actual negotiated/active mode on Mac. Preserve mutual exclusion, mode-switch restart behavior, and cleanup before changing modes. |
| Settings | Both endpoints and ports; saved/effective distinction; Mac display selection and unavailable-display handling; Windows codec/resolution/FPS/bitrate/VSync; Save and Restart; Reset to Defaults; invalid/unwritable settings recovery. |
| Mac permissions/power | Separate Screen Recording and Accessibility states/actions, refresh on activation; keep-awake toggle; closed-lid authorization and truthful status; existing native adapter confirmation/admin flow. Preserve saved stream options during unrelated saves. |
| Cross pairing | Discovered peers, manual address, verification code and explicit approval on both devices, Mac decline, paired-device removal, changed-identity and expired-pairing errors. Preserve existing control ownership; do not add new wire commands. |
| Cross controls | Connect/Disconnect, Enable/Pause, peer/route/latency/input destination, automatic/pinned interface, modifier mapping, emergency shortcut validation, peer-specific preferences, visible failures. |
| Cross arrangement | Windows-owned editor: fixed Windows tiles; movable/scalable Mac tiles; identify local/remote displays; zoom/pan/reset; touching-edge highlights and snap; keyboard moves and X/Y entry; reset Mac tiles; remove missing tiles; Apply and validation errors. |
| Diagnostics | Actual protocol version from native constants; endpoints/ports; available stream/decoder/route/power metrics; bounded recent Windows logs, copy and Open Logs; Mac Open Logs/permission refresh. No invented Mac metrics or Cross video statistics. |
| Native viewer | Keep D3D12 video presentation and ImGui overlay, resolution changes, VSync, keyframes, stats/logs, F1/F11/Escape. Dashboard entry/exit uses the same native action/state adapter. |
| App lifecycle | Close-to-tray/menu, Show/Quit menu, single instance, focus/resize/minimize/fullscreen, safe Disconnect, restart with saved settings and no stale CLI overrides, quit cleanup, Explorer tray recreation. |

For arrangement, preview geometry/snapping is shared TypeScript; the existing
native `common/cross_layout.h` validation remains authoritative. Use common
geometry fixtures to detect drift between JS previews and native acceptance.
Only apply edits on explicit Apply. Do not change OS display layouts, remap
missing displays, transfer arrangement ownership to Mac, or restore stale input
ownership while updating the UI.

## 5. Implementation sequence and build integration

Complete these stages in order. Keep intermediate changes buildable. Retain old
dashboard code only until the replacement passes parity checks, then remove it
before final acceptance.

1. **Capture the baseline.** Inventory controls and controller effects; record
   current build/test results, UI at default/minimum sizes, package sizes, app
   startup, idle/hidden CPU and memory, and native streaming performance. Use
   synthetic fixtures for automated screenshots and protect real settings/logs.
2. **Extract native adapters.** Move Mac settings/display/adapter actions out of
   SwiftUI, and Windows saves/peer/topology mutations out of draw functions.
   Keep old views temporarily calling these adapters. Add command/state tests
   with fake services before connecting a webview.
3. **Build the shared UI.** Establish the contract, fixture/mock host, components,
   forms, arrangement editor, and all state/error cases. Browser preview is
   development-only and cannot connect to native services. Lock all dependencies.
4. **Integrate Mac.** Install a retained WKWebView in StatusController's existing
   window. Use main-thread state delivery and weak message-handler ownership;
   detach handlers/observers on teardown. Preserve AppDelegate's native session,
   power, permission, menu, and shutdown coordination. Replace SwiftUI preview
   tests with WKWebView snapshot/interaction tests. Remove obsolete SwiftUI UI
   code/imports/framework linkage only when no remaining code needs them.
5. **Integrate Windows.** Initialize WebView2 on the UI thread with correct COM
   lifetime, retain controller/environment safely, resize to client bounds, and
   use per-user WebView data under LocalAppData. Handle asynchronous creation,
   app shutdown during creation, renderer failure, and hidden/show transitions.
   While the dashboard is active, disable Screen Share forwarding and release
   held input; while viewing video, hide and suspend the webview where supported.
   Keep decoder draining and Cross input independent of dashboard rendering.
   Handle accelerator/focus routing explicitly: viewer shortcuts apply only in
   Screen Share viewer context; Cross keeps its existing key behavior.
6. **Unify assets and builds.** Produce one frontend `dist/` and a manifest of
   content hashes plus contract/build versions. Generate the Windows RC asset
   mapping from that manifest. Add a local SwiftPM resource package under
   `shared_ui/native/`, with generated resources copied from the same dist and
   a bundle accessor used by the sender. Mac packaging copies the resulting
   SwiftPM resource bundle into the app; test lookup outside the checkout.
   Derive the UI build ID from sorted relative paths and content hashes, excluding
   timestamps and machine paths. Release verification compares extracted asset
   manifests from both finished packages; a mismatch fails the release gate.
7. **Wire build commands.** Existing dev, native test, and packaging scripts run
   the shared UI preparation before native builds. Track UI sources, lockfile,
   schema, manifest, and resource-generation inputs in stale-build checks and
   CMake dependencies. Clean raw native builds without prepared assets fail with
   an actionable UI-build command. Never ship missing, stale, or preview assets.
8. **Implement delivery/recovery.** Add the Evergreen setup flow and offline kit
   described below; integrate native recovery screens on both platforms. Test
   missing runtime and broken assets before deleting the legacy dashboard.
9. **Remove duplication and finish.** Delete migrated SwiftUI and ImGui dashboard,
   settings, pairing, and arrangement views plus obsolete tests/helpers. Retain
   ImGui only where needed by the native viewer. Run the complete acceptance
   matrix, update documentation, and produce both packages and evidence.

### Shared development commands

Provide `npm ci`, `npm run check` (types/lint/contract checks), `npm test`
(Vitest), `npm run test:e2e` (Playwright Chromium/WebKit), and `npm run build`
(production assets, manifest, native resource preparation). Native adapters use
the same JSON fixtures as the frontend. Pin tool/package versions at initial
implementation and keep the lockfile reviewed.

The E2E command builds the production UI and a separate fixture host, starts its
loopback server, and shuts it down after testing. Inject the mock native bridge
from that host before the app loads; exclude all fixture hosts and mocks from
the production asset manifest. Install test browser binaries into the configured
tool cache when absent; do not silently install OS browser dependencies.

Add `shared_ui/deploy.sh` and `deploy.ps1` with matching `build-preview`, `preview`,
`clear-build`, and `reset` commands. Use one persisted, initially available
loopback preview port, strict port handling, and owned-process cleanup. Install
local npm dependencies only when absent/inconsistent; fail on build errors.
Clear/Reset target only known generated outputs/local dependencies.

Add the corresponding UI preview tasks without removing existing native Build
and Run tasks. Follow the web-ui-design editor reference for task order/icons,
restart behavior, cancellation, and port persistence. Do not add harness or Git
configuration. Ignore frontend dependencies/builds, generated native resources,
runtime downloads, test output, and `.tmp/`; retain sources, manifests, lockfile,
schemas, licenses, and synthetic fixtures.

### Packaging and missing-runtime setup

- Pin a stable WebView2 SDK in CMake with an immutable version and checksum;
  statically link its loader using the existing static MSVC runtime. Do not
  bundle Node, a web server, or a private browser runtime.
- Detect Evergreen availability and the minimum version needed by used APIs.
  A missing/incompatible runtime opens native recovery before starting new
  capture/input sessions. Install/Update runs only after the user clicks it;
  never install a system runtime silently on application launch.
- The online path downloads Microsoft's bootstrapper over HTTPS, validates the
  Microsoft signature, launches it without requesting elevation by default,
  reports cancellation/failure, and rechecks availability before Retry. Manage
  temporary downloads in an owned directory and remove them after use.
- Keep the default ZIP small. Offer an optional offline setup ZIP containing
  the same app plus the signed Evergreen standalone installer and instructions.
  Use that installer when present; no network is required for that setup path.
  Offline without a runtime/installer shows clear recovery rather than hanging.
- Update packaging dependency checks to account for the static loader's actual
  OS imports without broadly bypassing DLL validation. Include UI/font/library
  notices, UI manifest, app version, and artifact SHA-256 files. Diagnose app,
  contract, UI build, and runtime versions in Diagnostics.
- Assets are packaged and work offline after runtime setup. Keep settings/logs
  in existing per-user locations; WebView profile/cache uses separate app-owned
  per-user storage. No writes beside a packaged executable are required.
- Measure app executable/bundle and compressed ZIP growth, distinguishing UI
  assets, loader, optional installer, and WebView cache/runtime disk usage.
  Set an initial shared-asset budget of 2 MiB uncompressed including fonts;
  fail the size check above that budget rather than silently increasing it.

## 6. Tests and acceptance gates

### Automated checks

| Layer | Required cases |
| --- | --- |
| Contract/adapters | Both native hosts serialize accepted fixtures; malformed/oversized/unknown actions fail safely; role/capability checks; stale UI instance/request/revision; expired pairing; stale topology; asynchronous success/failure; duplicate commands; teardown with pending callbacks. |
| State/forms | Saved versus effective CLI values; status changes preserve drafts/focus; invalid ports/resolution/FPS/bitrate; unavailable display; failed persistence; restart/reset; external saved-state change produces a conflict instead of overwriting edits. |
| Cross | Pair/decline/forget/re-pair; changed identity; manual/interface preferences; enable/pause/disconnect; invalid emergency shortcut; valid touching edges versus corners/overlaps/islands; negative coordinates, scaling, missing/rotated/Retina displays, stale peer/topology Apply. |
| Shared UI | Every feature-table action, waiting/loading/empty/error/busy states, bridge absent/mismatch/reconnect, keyboard navigation, focus and live errors, responsive layout, malicious text displayed inertly, no unexpected network requests. |
| Resource/host | Asset hash match, missing asset, path traversal, rejected navigation/subframe messages, renderer termination, reload/show/hide, early quit during initialization, WebView profile creation failure, bounded telemetry/logs. |
| Installer/build | Runtime present/missing/too old; install cancelled/failed/offline/invalid signature; Retry; repeated clicks; clean/incremental builds; changed UI/lockfile invalidates outputs; preview failure/restart/port conflict; source-preserving Clear/Reset. |

Use mocked controllers and temporary profiles/settings for automated UI tests.
Do not inject test input into the user's applications, grant trust, authorize
power changes, modify adapters, or install/remove system runtimes automatically.
Use a disposable Windows VM for runtime absence and installation scenarios.

Run these commands after updating the wrappers to prepare shared assets:

```text
# Shared UI, from shared_ui/
npm ci
npx playwright install chromium webkit
npm run check
npm test
npm run test:e2e
npm run build

# Portable C++ regression suites, from repository root
cmake -S tests -B .tmp/ui-portable-tests
cmake --build .tmp/ui-portable-tests --config Release
ctest --test-dir .tmp/ui-portable-tests -C Release --output-on-failure

# Native Windows, Visual Studio developer shell
cmake -S windows_receiver -B windows_receiver/build -A x64 -DBUILD_TESTING=ON
cmake --build windows_receiver/build --config Debug
ctest --test-dir windows_receiver/build -C Debug --output-on-failure
cmake --build windows_receiver/build --config Release
ctest --test-dir windows_receiver/build -C Release --output-on-failure
powershell -NoProfile -File scripts/package-windows.ps1

# Native macOS
swift build --package-path mac_sender
swift build --package-path mac_sender -c release
./scripts/test-mac.sh
./scripts/package-mac.sh
```

Add native UI integration tests to their actual platform suites. Playwright's
Chromium/WebKit runs complement tests inside real WebView2/WKWebView; they do not
prove native focus, permissions, packaging, or input lifecycle behavior. Preserve
all existing protocol, framing, reassembly, input, decode/texture, Cross, settings,
and power regression suites. Record unavailable debug-layer tests as skipped.

### Visual, native, and physical validation

- Compare shared fixture screens across both hosts at the existing 940x660
  default and 780x560 minimum outer-window sizes, accounting for native chrome;
  also check 390, 768, 1024, and 1440 CSS-pixel preview widths, Windows
  100/150/200% scaling, and Mac Retina. No clipped fields/buttons or lost canvas
  access. Assert shared geometry/style expectations and inspect temporary
  screenshots; tolerate text rasterization differences between engines.
- Exercise real keyboard editing, tab order, clipboard/IME, focus restoration,
  VoiceOver and Windows Narrator, contrast, 200% zoom/text scaling, and reduced
  motion. All arrangement operations need a keyboard path.
- Test installed packages outside the repo in spaces/Unicode paths and from an
  unrelated working directory. Check Mac resources/signing; Windows launch with
  and without Evergreen; both apps offline after setup; repeat launches,
  minimize/close/show, hidden quit, restart, and corrupt/unwritable settings.
- Follow both native guides for Screen Share: H.264/HEVC, resolution changes,
  keyframes, VSync, resize/fullscreen, F1/F11/Escape, reconnect/cable loss, and
  dashboard/viewer transitions while keys/buttons are held. Verify release and
  restoration, and no extra video buffering or frame transfer through JS.
- Follow `docs/CROSS.md` and each guide's Cross acceptance on the separate
  Thunderbolt-IP, direct Ethernet, switched Ethernet, Wi-Fi, and mixed paths.
  Include denied Screen Recording with granted Accessibility, route fallback,
  manual addressing, paired identity changes, held-key mode switches, emergency
  return, lock/sleep, lost permissions, peer/UI crashes, and display removal.
  Record unavailable physical paths as pending in `docs/CROSS_VALIDATION.md`.
- Compare three equivalent baseline/new runs on each device: startup-to-controls,
  idle CPU, whole-app process-tree memory, hidden/video-mode usage, FPS, dropped
  frames, and available latency measurements. After a 5-second settling period,
  hidden controls must stop periodic JS/native telemetry. Investigate repeatable
  streaming degradation above 5% or beyond measured baseline variability; do
  not call it verified until explained or corrected. Report memory/size changes
  even when streaming performance is unchanged.

Keep task-owned screenshots/traces/test output under `.tmp/`, inspect failures,
then delete those temporary artifacts. Retain concise textual test evidence;
preserve the other device's tracked diagnostic log.

## 7. Documentation, rollout, and completion

Update design guidance to name the shared token/component source and permitted
role differences. Update architecture with the bridge/native-thread ownership,
resource loading, crash recovery, and dashboard/viewer boundary. Update setup,
README, development commands, package instructions, both native testing guides,
and Cross validation guidance. Replace the current claim that Windows never
needs a runtime download with the Evergreen prerequisite and explicit setup
flow. Correct stale UI protocol labels using native constants.

Use a Windows/macOS CI matrix if repository CI exists; otherwise add a workflow
for shared checks, contract fixtures, portable/native builds/tests, and artifact
manifest/size checks. Keep interactive native and physical-device gates explicit
and manual where runners cannot exercise them. CI must never publish releases
or change paired-device/system settings.

Completion requires all of the following:

- [ ] All controls in the feature table are migrated or explicitly retained in
  the native viewer as specified; every action has a native owner and coverage.
- [ ] Both apps use identical frontend asset hashes and the same UI contract;
  no duplicated Mac/Windows control-page implementation remains.
- [ ] Native services survive ordinary UI hide/reload and clean up correctly on
  disconnect, restart, quit, and renderer failure; Cross safety checks still pass.
- [ ] Shared, bridge, portable, native, packaging, accessibility, and applicable
  physical tests pass; pending device gates are individually recorded.
- [ ] Default and optional offline Windows packages work as documented; Mac
  bundles all local assets; both operate without internet after runtime setup.
- [ ] Package-size/performance evidence and updated docs are complete; temporary
  test artifacts are removed; unrelated changes and logs remain intact.

Report implementation, build verification, local behavior, and physical
end-to-end verification separately. Missing Mac or physical-device evidence
means the relevant gate is pending, even when the implementation is complete.
Do not treat a mock UI, successful compilation, or one-platform run as completion
of the entire migration.

Implementation references: [Lit](https://lit.dev/docs/),
[Vite](https://vite.dev/guide/),
[WebView2 security](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/security),
[WebView2 distribution](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution),
and [WKURLSchemeHandler](https://developer.apple.com/documentation/webkit/wkurlschemehandler).
