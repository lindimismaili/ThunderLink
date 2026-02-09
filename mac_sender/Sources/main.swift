import AppKit

// ThunderLink Mac Sender - Entry Point
// Launches the NSApplication with our AppDelegate to manage
// screen capture, network streaming, and input injection.

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.run()
