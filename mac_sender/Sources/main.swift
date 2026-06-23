import AppKit

// ThunderLink Mac Sender - Entry Point
// Launches the NSApplication with our AppDelegate to manage
// screen capture, network streaming, and input injection.
//
// Endpoint/codec overrides may be passed on the command line, e.g.:
//   ThunderLinkSender --peer 169.254.1.2 --bind 169.254.1.1 --codec hevc --bitrate 25
// With no arguments it uses the direct link-local defaults.
//
// On startup the app auto-assigns the --bind IP to the direct-link adapter if
// it isn't already there (one-time admin prompt). Pass --no-auto-ip to skip.

NetworkConfig.applyCommandLine(CommandLine.arguments)
logger.info("ThunderLinkSender config: peer=\(NetworkConfig.winIP) bind=\(NetworkConfig.macIP) " +
            "codec=\(NetworkConfig.defaultCodec) bitrate=\(NetworkConfig.defaultBitrateMbps)Mbps")

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
