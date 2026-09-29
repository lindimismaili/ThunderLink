# ThunderLink Product and Interface Design

## Product Principles

- **Video first:** the remote Mac display is the primary content. Controls and
  diagnostics must remain compact, optional, and clear of the viewing area.
- **Low latency by default:** avoid UI or processing choices that add buffering,
  block capture/render loops, or hide performance regressions. Offer stability
  controls such as VSync without making them mandatory.
- **Native on each platform:** use macOS windows and SwiftUI conventions for
  the sender, and Win32/ImGui conventions for the receiver. Shared behavior
  should be consistent without forcing identical visuals.
- **State must be obvious:** waiting, connected, streaming, permission, codec,
  resolution, and recoverable error states should be understandable without
  reading a log.
- **Failures must recover:** a disconnect, dropped frame, or closed peer should
  lead to a safe state and retry path. Never leave the Mac display changed or
  the Windows cursor trapped.
- **Minimal setup:** defaults serve the direct `169.254.1.x/24` link, while
  explicit overrides remain available for other controlled networks.

## Shared Interaction Rules

- Use concise user-facing labels and reserve protocol terminology for
  diagnostics.
- Show actual negotiated or measured values, not requested values presented as
  fact.
- Keep destructive or session-ending actions visually distinct and easy to
  confirm through their labels.
- Preserve keyboard operation, readable contrast, platform text scaling, and
  descriptive accessibility labels.
- Do not use color as the only indicator of connection, warning, or failure.
- Keep advanced diagnostics available but out of the default viewing path.

## macOS Sender

- Open a full desktop window. Closing hides it and switches to a menu-bar accessory.
- Use system symbols, semantic colors, native typography, standard spacing, and
  SwiftUI controls rather than bespoke decoration.
- Use Connection, Settings, and Diagnostics navigation. Keep status, peer, actual
  stream details, permissions, and session controls visible on Connection.
- Reflect Screen Recording and Accessibility requirements separately because
  capture can work while input injection does not.
- Quit and disconnect paths must stop capture/encoding and restore any display
  mode changed by the session.
- Settings → Power defaults to keeping an AC-powered Mac awake for either
  active session mode, including when the window is hidden. The saved toggle
  applies live; connection settings still require restart. Show a separate
  closed-lid authorization action and truthful waiting/active/error status.
  Explain that the lid override also blocks manual Sleep. Never describe idle
  sleep prevention alone as closed-lid protection.

## Windows Receiver

- Keep the decoded video dominant. The persistent toolbar may expose status and
  panel toggles; detailed statistics and logs belong in optional overlays.
- Maintain readable controls across DPI scales and window sizes, and retain the
  stream's aspect ratio through resizing and fullscreen transitions.
- Keep F1 for overlay visibility, F11 for fullscreen, and Escape for cursor
  release unless a deliberate, documented interaction redesign replaces them.
- Make connection state, active codec/resolution, VSync, keyframe recovery, and
  input confinement inspectable without requiring a console.
- Never let overlays capture input invisibly or prevent the user from releasing
  the cursor and closing the receiver.

## Design Review Checklist

- Does the change keep video and input latency observable and protected?
- Are waiting, connected, degraded, and permission-denied states truthful?
- Can the UI be used with keyboard navigation, scaling, and assistive tools?
- Does it follow the native platform while preserving shared terminology?
- Do disconnect, quit, and failure paths restore display, cursor, and window
  state?
- Has the change been visually inspected on the affected native platform and,
  when shared behavior changed, on both devices?

## Dark Studio Desktop System

The shared control UI under `shared_ui/` uses
[`src/styles.css`](../shared_ui/src/styles.css) as its token and component style
source. Its [Lit components](../shared_ui/src/app.ts) render the same control
layout on both platforms and expose role-specific actions from native
capabilities. The D3D12 video viewer keeps its compact native overlay. During
the migration, the existing SwiftUI and ImGui dashboards remain active until
their native webview bridges pass the parity and safety checks in
[`UI_UPDATE_PLAN.md`](../UI_UPDATE_PLAN.md).

Both platforms share a deliberate dark appearance independent of the OS theme.
Use background `#0E1117`, surface `#171B24`, primary text `#E8EDF7`, muted text
`#99A6BD`, and accent `#5791FF`. Use green for successful permissions, amber for
requirements/errors, and always pair status color with text or an icon.

The shared controls use locally bundled Inter; the current native dashboards use
system typography (Mac system font / Windows Segoe UI). Use 13–17 point
body/control text, 24–28 point page headings, and small uppercase section labels.
Keep a roughly 200-point sidebar, 24–32 point content padding, 8-point spacing
increments, and 10–14 point corner radii. Scroll content at small sizes; never
shrink controls to force everything into a fixed-height panel. Use visible focus
states and keyboard navigation. Windows ImGui does not provide a full native
screen-reader accessibility tree; native assistive-technology parity remains a
limitation of that UI stack.

### Screens

- **Connection:** device role, peer, truthful waiting/preparing/streaming/error
  state, Connect/Disconnect, actual stream values, and platform setup guidance.
- **Settings:** editable endpoints and ports, Mac display selection, Windows
  codec/resolution/FPS/bitrate/VSync, Save and Restart, and Reset to Defaults.
- **Diagnostics:** active endpoints and ports, protocol, stream health where
  available, local logs, and permission refresh on Mac.

The Windows viewer keeps video dominant after START. Dashboard returns to the
full window; Back to Video restores the viewer. F1 toggles viewer overlays, F11
controls fullscreen, and Escape releases the cursor.

### Lifetime

The menu-bar/tray icon menu has exactly **Show** and **Quit**. Close hides the
window; Show reveals and focuses the retained window; Quit drains streaming,
releases input, restores display/window state, removes the icon, and exits.
Launching another instance reveals the existing one. Windows re-adds its icon
after Explorer restarts and falls back to an accessible window if icon creation
fails. Ordinary minimize retains OS minimize behavior.

Hidden sessions stay connected. Windows drains bounded decode queues without
presenting frames and releases cursor confinement and forwarded input. User
Disconnect suspends automatic reconnection until Connect or a fresh app launch.

## Cross mode

Connection offers Screen Share and Cross as mutually exclusive modes. Cross uses
the same Dark Studio colors, spacing, typography, and accessible labels. Show the
peer, actual active interface, measured latency, and current destination (Windows
or Mac) alongside Enable/Pause, arrangement, and pairing controls. Pairing must
present the verification code on both devices and require explicit approval;
unknown/discovered does not mean trusted. Provide Forget Device.

The Windows arrangement canvas fixes local display geometry to Windows settings,
allows Mac tiles to move/scale, and highlights touching edge segments. Include
Identify, zoom/reset, keyboard positioning, Apply, and clear validation errors.
Do not render a video placeholder or stream statistics as if Cross were video.
Topology changes require review and cannot silently remap missing displays.

Close/hide preserves Cross input sharing. Pause, Disconnect, emergency release,
and Quit end remote ownership. Cross forwards ordinary Escape/F1/F11 to the Mac;
the Screen Share viewer shortcuts remain exclusive to Screen Share. Keep the
emergency shortcut visible in the Cross controls. Network/pairing faults and
permission warnings must remain actionable without claiming a successful session.
