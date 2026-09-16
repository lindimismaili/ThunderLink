# Cross

Cross shares the Windows PC's mouse and keyboard with a Mac's own displays.
It does not stream video. Connect your third monitor to the Mac, then place its
Cross tile next to the Windows screens. Move beyond the connected screen edge
to control the Mac; move back across that edge to return to Windows. The keyboard
follows the pointer. Screen Share and Cross are separate, mutually exclusive modes.

## Connect

1. Open ThunderLink on both computers. Grant Accessibility access on the Mac.
   Cross does not require Screen Recording permission. If macOS requests local
   network access, allow it for discovery and connections.
2. Select **Cross** in the Windows Connection page. Select your Mac from the
   discovered devices, or enter its network address manually.
3. Compare the pairing verification code on both computers and approve only
   when they match. Pairing is required once per device identity. Use Forget
   Device to remove trust; a changed certificate requires pairing again.
4. Open the arrangement panel, identify your displays, position the Mac tiles,
   and Apply. Enable Cross when the arrangement is valid.

Closing the application windows leaves Cross running. Pause Cross, Disconnect,
or Quit to stop it. The default emergency shortcut is **Ctrl+Alt+Shift+F12**;
it returns control to Windows and pauses crossing. Change the shortcut in Cross
settings if it conflicts with your keyboard setup.

Ctrl acts as Command on the Mac by default: Ctrl+C/V/Z use Mac copy/paste/undo.
Windows acts as Mac Control and Alt acts as Option. Native Mac mapping can be
selected instead. Escape and function keys are forwarded normally. Crossing is
held while a key or mouse button is pressed so that a gesture stays on one
computer. Release it before crossing; dragging within the Mac works normally.

## Arrange displays

Windows tiles retain the actual Windows desktop arrangement. Position Mac tiles
beside Windows tiles or other Mac tiles. Tiles must touch along an edge, not only
at a corner. Highlighted shared edge segments are crossing regions. Other edge
segments stop the pointer. Overlaps and Mac islands without a route back to
Windows cannot be applied.

Tile scaling helps match different display sizes. Cross maps tile positions to
the actual target display independently of resolution and Retina scaling.
The canvas does not change either computer's OS display settings. If your
Windows arrangement is wrong, change it in Windows Display Settings first.
Mirrored screens represent one logical surface.

Arrangements are tied to the paired computer and display identities. Unplugging,
rotating, or changing displays releases remote input and requires layout review.
Missing displays are not silently replaced. Cross does not switch the input
source of a physical monitor connected to both computers.

## Supported network paths

| Path | Setup |
| --- | --- |
| Thunderbolt networking | Both computers must expose a usable IP network adapter over the cable. Windows hardware/driver support must be checked. A Thunderbolt or USB-C connector alone does not guarantee networking. |
| Direct Ethernet | Connect compatible adapters and use their existing usable addresses, including link-local addresses. |
| Ethernet LAN | Connect both computers through a switch/router using their assigned addresses. |
| Wi-Fi LAN | Use a network that permits clients to reach each other. |
| Mixed Ethernet/Wi-Fi | Both computers must be reachable on the same LAN or through an allowed local route. |

Cross uses encrypted TCP port **50004** on the Mac. Discovery uses local DNS-SD
multicast. Windows connects outbound to the Mac. Allow the application through
the relevant firewall/local-network controls. Do not configure internet port
forwarding. Manual IPv4, IPv6 (including an interface scope for link-local
addresses), or hostname entry is available when multicast discovery is blocked.
Manual entry cannot bypass a firewall or guest-network client isolation.

Automatic selection prefers a reachable Thunderbolt path, then Ethernet, then
Wi-Fi. Within a class, connection timings from previous attempts rank known
routes; unmeasured routes are tried in discovery order. Cross does not interrupt
a healthy session to benchmark other paths. An interface can be pinned. A
healthy connection stays on its selected path. If it fails, Cross returns
control to Windows before trying another route
to the same authenticated peer. Reconnection starts with Windows ownership;
move across an edge again to control the Mac.

Cross uses existing network addresses. Launch, discovery, and failover do not
assign static IP addresses or change gateways, DNS, or Wi-Fi settings. The
Screen Share direct-link address helper remains a separate explicit action.

## Recovery and limits

A failed connection, permission loss, sleep/lock, topology change, or stopped
peer releases remote keys/buttons. A one-second ownership lease prevents an
unresponsive peer from retaining control indefinitely. The emergency shortcut
is available locally without waiting for a network reply.

Cross v1 supports relative mouse input; an absolute tablet/pen report pauses
Cross instead of treating its coordinates as mouse movement. Unsupported keys
are ignored instead of being translated into unrelated Mac keys.

Cross v1 supports one Windows PC controlling one Mac. Mac-local peripherals
remain usable; simultaneous input is not arbitrated. Clipboard/file transfer,
reverse Mac-to-Windows control, login/secure desktops, exclusive game-input
paths, and simultaneous screen sharing are outside this version.

Both apps require protocol v4. Existing installations default to Screen Share.
Cross settings and layouts are kept in versioned `cross.json` beside the existing
per-user settings. Private device identities and trust records use native
protected storage rather than the JSON file. Diagnostic logs contain state and
network errors, not typed characters.

See [Cross validation](CROSS_VALIDATION.md) for the checks actually performed.
No native build or single-machine test alone proves a physical network path.
