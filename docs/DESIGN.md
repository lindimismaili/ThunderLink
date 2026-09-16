# ThunderLink Product and Interface Design

## Product Principles

- **Video first:** the remote Mac display is the primary content. Controls and
  diagnostics must remain compact, optional, and clear of the viewing area.
- **Low latency by default:** avoid UI or processing choices that add buffering,
  block capture/render loops, or hide performance regressions. Offer stability
  controls such as VSync without making them mandatory.
- **Native on each platform:** use macOS menu-bar and SwiftUI conventions for
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

- Remain a menu-bar accessory unless the product model intentionally changes.
- Use system symbols, semantic colors, native typography, standard spacing, and
  SwiftUI controls rather than bespoke decoration.
- Keep the panel compact: identity, connection/stream state, negotiated stream
  details, peer, permission state, then session controls.
- Reflect Screen Recording and Accessibility requirements separately because
  capture can work while input injection does not.
- Quit and disconnect paths must stop capture/encoding and restore any display
  mode changed by the session.

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
