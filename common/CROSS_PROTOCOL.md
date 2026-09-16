# Cross protocol v4

Cross uses a dedicated mutually authenticated TLS TCP connection (default 50004),
Windows client / Mac server. Minimum TLS 1.2. JSON framing is the existing 4-byte
big-endian length plus 1..65536 UTF-8 bytes. No plaintext fallback. Both peers
present a persistent self-signed certificate; certificate SHA-256 pins identify
trusted peers. Unknown certificates are accepted only into a bounded pairing
state, never an input session. Existing pins must match. Pairing requires both
users to approve the same verification code: first 12 uppercase hex characters
of SHA256(lowercase(client certificate SHA256 hex) + lowercase(server certificate
SHA256 hex)), UTF-8, client first. Store identity private keys in native protected
storage, pins in protected credential storage. Never log input contents or keys.

All JSON messages have `type` (string), and HELLO has `version`: 4,
`device_id` (certificate SHA256 lowercase hex), `name`, `role` (`windows`/`mac`).
TLS certificate identity is authoritative, not claimed device_id.
`pair_approve` records local approval; `paired` means both approved (or pinned
on both sides). No topology/control/input before mutual approval. Pairing timeout
120 seconds, handshake/message write timeout 5 seconds. No automatic re-pairing
of a changed known identity; Forget Device first.

Server sends `topology`: `revision` positive integer and `displays` array.
Display: `id`, `name`, `x`, `y`, `width`, `height` (global native coordinates),
`scale`, `rotation`, `primary` boolean. Width/height positive finite, max 32
displays, IDs unique <=256 UTF-8 bytes. Mirrored displays represented once.
Client sends `configure` with `session_id` (nonzero 16 lowercase hex),
`topology_revision`, `swap_ctrl_cmd` bool. Server drains Screen Share before
replying `ready` with matching session_id and topology_revision. Accessibility
failure sends `error` reason and no ready. Cross requires no screen capture.
All following ownership messages include matching session_id and topology_revision.

`enter`: monotonically increasing positive `generation`, display_id, x/y normalized
[0,1]. A valid fresh enter starts the ownership lease, including after an idle
configured period. Server validates, positions cursor then replies `entered` same generation.
Client never sends keys/buttons before entered. `input`: same generation,
`event` object (legacy input JSON fields type, x/y, key_code, modifiers, button,
state, delta_x/y), plus display_id on pointer/button events. All event fields
validated before injection. Mac maps normalized coordinates inside target
CGDisplayBounds, including nonzero origins. No reuse of generations after leave.
`leave`: generation; reset held keys/buttons and invalidate ownership; reply
`local` same generation. `reset` releases ownership too. `stop` ends Cross session.
`ping`: sequence integer, session_id if configured, and optional boolean
`renew_ownership` (default true); `pong` echoes sequence. The input controller
sends ownership pings every 250ms while configured and may omit the field. A
transport-level liveness ping sets `renew_ownership: false`, receives a pong,
and never renews input ownership. Server lease renewal occurs only for a valid
current-session ownership ping. Both sides expire active ownership after 1000ms
without ownership renewal (and return local); an expired generation can never be resumed.
Input itself does not extend a lease. Outbound queues max 256 messages, coalesce
only consecutive mouse_move for same session/generation/display. Overflow closes
connection and releases state; never drop release/barrier events silently.

`identify` after paired asks server to show temporary numbered labels on displays.
`error`: reason (human readable, no input data). Topology changes release ownership,
send fresh topology, and require a new configure/session after layout review.
Disconnect, lock, sleep, permission loss release ownership. Reconnect new session,
local ownership; fresh crossing required. No application data before paired.

Discovery: _thunderlink-cross._tcp local., TXT version=4, role=mac,
id=certificate fingerprint. Records are hints only. Native DNS-SD Windows and
Network.framework Bonjour Mac. All routes IPv4/IPv6 including scoped link-local;
manual address fallback. Never change OS network addresses automatically.
