import AppKit
import SwiftUI
import Combine

/// Observable stream state shared with the SwiftUI status panel.
/// All mutations must happen on the main thread.
final class AppState: ObservableObject {
    @Published var connected = false
    @Published var streaming = false
    @Published var width = 0
    @Published var height = 0
    @Published var fps = 0
    @Published var codec = "—"
    @Published var bitrateKbps = 0
    @Published var peerIP = NetworkConfig.winIP
    @Published var accessibilityGranted = false
}

/// Menu-bar status item + SwiftUI popover panel for the Mac sender.
final class StatusController: NSObject {
    let state = AppState()
    var onQuit: (() -> Void)?

    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    private let popover = NSPopover()

    func install() {
        if let button = statusItem.button {
            button.image = NSImage(systemSymbolName: "display",
                                   accessibilityDescription: "ThunderLink")
            button.action = #selector(togglePopover(_:))
            button.target = self
        }

        popover.behavior = .transient
        popover.contentSize = NSSize(width: 280, height: 260)
        popover.contentViewController = NSHostingController(
            rootView: StatusView(state: state, onQuit: { [weak self] in self?.onQuit?() }))
    }

    /// Reflect connection state in the menu-bar icon. Call on the main thread.
    func refreshIcon() {
        let symbol = state.connected ? "display.badge.checkmark" : "display"
        statusItem.button?.image = NSImage(systemSymbolName: symbol,
                                           accessibilityDescription: "ThunderLink")
    }

    @objc private func togglePopover(_ sender: AnyObject?) {
        guard let button = statusItem.button else { return }
        if popover.isShown {
            popover.performClose(sender)
        } else {
            popover.show(relativeTo: button.bounds, of: button, preferredEdge: .minY)
            NSApp.activate(ignoringOtherApps: true)
        }
    }
}

/// Compact SwiftUI panel shown from the menu-bar item.
struct StatusView: View {
    @ObservedObject var state: AppState
    var onQuit: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Image(systemName: "display")
                Text("ThunderLink Sender").font(.headline)
            }

            Divider()

            Label(state.connected ? "Connected" : "Waiting for PC…",
                  systemImage: state.connected ? "checkmark.circle.fill" : "clock")
                .foregroundColor(state.connected ? .green : .secondary)

            if state.streaming {
                Text("\(state.width)×\(state.height) @ \(state.fps) fps")
                Text("\(state.codec) · \(state.bitrateKbps / 1000) Mbps")
                    .foregroundColor(.secondary)
            }

            Text("Peer: \(state.peerIP)")
                .font(.caption)
                .foregroundColor(.secondary)

            Divider()

            Label(state.accessibilityGranted ? "Accessibility granted"
                                             : "Accessibility needed for input",
                  systemImage: state.accessibilityGranted ? "checkmark.shield" : "exclamationmark.shield")
                .font(.caption)
                .foregroundColor(state.accessibilityGranted ? .green : .orange)

            Spacer(minLength: 0)

            HStack {
                Spacer()
                Button("Quit", action: onQuit)
                    .keyboardShortcut("q")
            }
        }
        .padding(14)
        .frame(width: 280)
    }
}
