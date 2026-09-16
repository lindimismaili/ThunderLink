import AppKit
import SwiftUI
import Combine
import ScreenCaptureKit

/// Observable stream state shared with the SwiftUI status panel.
/// All mutations must happen on the main thread.
final class AppState: ObservableObject {
    enum Mode: String { case screenShare = "Screen Share", cross = "Cross" }
    enum Phase: String {
        case waiting = "Waiting for PC…"
        case configuring = "Preparing stream…"
        case streaming = "Streaming"
        case stopping = "Stopping…"
        case failed = "Stream failed"
    }

    @Published var paused = false
    @Published var busy = false
    @Published var configurationError: String?
    @Published var phase: Phase = .waiting
    @Published var connected = false
    @Published var streaming = false
    @Published var width = 0
    @Published var height = 0
    @Published var fps = 0
    @Published var codec = "—"
    @Published var bitrateKbps = 0
    @Published var peerIP = NetworkConfig.winIP
    @Published var accessibilityGranted = false
    @Published var screenRecordingGranted = false
    @Published var failureReason: String?
    @Published var mode: Mode = .screenShare
    @Published var crossPhase: CrossServicePhase = .stopped
    @Published var crossPeerName: String?
    @Published var crossInterface: String?
    @Published var crossVerificationCode: String?
    @Published var crossError: String?
    @Published var crossConfigured = false
    @Published var crossRemoteOwnership = false
    @Published var pairedPeers: [CrossPairedPeer] = []
}

/// One retained window; the status menu deliberately contains only Show and Quit.
final class StatusController: NSObject, NSWindowDelegate {
    let state = AppState()
    var onQuit: (() -> Void)?
    var onToggleConnection: (() -> Void)?
    var onRestart: (() -> Void)?
    var onRefreshPermissions: (() -> Void)?
    var onApprovePairing: (() -> Void)?
    var onRejectPairing: (() -> Void)?
    var onForgetPeer: ((String) -> Void)?
    private var window: NSWindow?
    private let statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)

    func install() {
        let menu = NSMenu()
        for (title, action) in [("Show", #selector(show)), ("Quit", #selector(quit))] {
            let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
            item.target = self
            menu.addItem(item)
        }
        statusItem.menu = menu
        let mainMenu = NSMenu()
        let appMenuItem = NSMenuItem()
        let appMenu = NSMenu(title: "ThunderLink")
        let showItem = NSMenuItem(title: "Show ThunderLink", action: #selector(show), keyEquivalent: "0")
        showItem.target = self
        appMenu.addItem(showItem)
        let quitItem = NSMenuItem(title: "Quit ThunderLink", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        appMenu.addItem(quitItem)
        appMenuItem.submenu = appMenu
        mainMenu.addItem(appMenuItem)
        // Standard editing actions keep keyboard-based settings entry usable.
        let editItem = NSMenuItem()
        let edit = NSMenu(title: "Edit")
        for (title, selector, key) in [("Cut", "cut:", "x"), ("Copy", "copy:", "c"), ("Paste", "paste:", "v"), ("Select All", "selectAll:", "a")] {
            edit.addItem(NSMenuItem(title: title, action: Selector(selector), keyEquivalent: key))
        }
        editItem.submenu = edit; mainMenu.addItem(editItem)
        NSApp.mainMenu = mainMenu

        refreshIcon()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 940, height: 660),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "ThunderLink"
        window.titlebarAppearsTransparent = true
        window.backgroundColor = NSColor(calibratedRed: 0.055, green: 0.067, blue: 0.09, alpha: 1)
        window.appearance = NSAppearance(named: .darkAqua)
        window.minSize = NSSize(width: 780, height: 560)
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.contentView = NSHostingView(rootView: DesktopView(state: state,
            toggleConnection: { [weak self] in self?.onToggleConnection?() },
            restart: { [weak self] in self?.onRestart?() },
            refreshPermissions: { [weak self] in self?.onRefreshPermissions?() },
            approvePairing: { [weak self] in self?.onApprovePairing?() },
            rejectPairing: { [weak self] in self?.onRejectPairing?() },
            forgetPeer: { [weak self] in self?.onForgetPeer?($0) }))
        window.center()
        self.window = window
        DistributedNotificationCenter.default().addObserver(self, selector: #selector(revealFromNotification(_:)), name: DesktopSettings.showNotification, object: nil)
        DispatchQueue.main.async { [weak self] in self?.show() }
    }

    func refreshIcon() {
        statusItem.button?.image = NSImage(systemSymbolName: state.streaming || state.crossConfigured ? "display.badge.checkmark" : "display",
                                          accessibilityDescription: "ThunderLink")
        statusItem.button?.toolTip = state.mode == .cross ? "ThunderLink — Cross" : state.streaming ? "ThunderLink — Sharing" : "ThunderLink"
    }
    @objc func show() {
        NSApp.setActivationPolicy(.regular)
        window?.deminiaturize(nil)
        window?.makeKeyAndOrderFront(nil)
        logger.info("Desktop window shown (visible=\(window?.isVisible ?? false), number=\(window?.windowNumber ?? -1))")
        NSApp.activate(ignoringOtherApps: true)
        onRefreshPermissions?()
    }
    @objc private func revealFromNotification(_ notification: Notification) { show() }
    @objc private func quit() { onQuit?() }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        sender.orderOut(nil)
        NSApp.setActivationPolicy(.accessory)
        return false
    }
}

private enum Studio {
    static let background = Color(red: 0.055, green: 0.067, blue: 0.09)
    static let surface = Color(red: 0.09, green: 0.105, blue: 0.14)
    static let muted = Color(red: 0.60, green: 0.65, blue: 0.74)
    static let accent = Color(red: 0.34, green: 0.57, blue: 1)
}

private final class DesktopForm: ObservableObject {
    @Published var section = "Connection"
    @Published var peer = NetworkConfig.winIP
    @Published var bind = NetworkConfig.macIP
    @Published var video = String(NetworkConfig.videoPort)
    @Published var input = String(NetworkConfig.inputPort)
    @Published var control = String(NetworkConfig.controlPort)
    @Published var display = String(NetworkConfig.captureDisplayIndex)
    @Published var settingsError: String?
    @Published var setupResult: String?
    struct DisplayChoice: Identifiable { let id: String; let label: String }
    @Published var displays = [DisplayChoice(id: "0", label: "Main display")]
    @Published var displayHint = "Grant Screen Recording access to list additional displays."
    @Published var configuringAdapter = false

    @MainActor func refreshDisplays() async {
        guard CGPreflightScreenCaptureAccess() else { return }
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            displays = [DisplayChoice(id: "0", label: "Main display")]
            for (index, item) in content.displays.enumerated() where index > 0 {
                displays.append(DisplayChoice(id: String(index), label: "Display \(index + 1) · \(item.width) × \(item.height)"))
            }
            displayHint = "Choose the display to share. Changes take effect after restart."
        } catch { displayHint = "Could not list displays. Check Screen Recording access and reopen Settings." }
    }

    init() {
        let saved = (try? RuntimeConfiguration.parse(arguments: ["ThunderLink"] + DesktopSettings.savedArguments)) ?? RuntimeConfiguration()
        peer = saved.winIP; bind = saved.macIP
        video = String(saved.videoPort); input = String(saved.inputPort); control = String(saved.controlPort)
        display = String(saved.captureDisplayIndex)
    }


}

struct DesktopView: View {
    @ObservedObject var state: AppState
    var toggleConnection: () -> Void
    var restart: () -> Void
    var refreshPermissions: () -> Void
    var approvePairing: () -> Void
    var rejectPairing: () -> Void
    var forgetPeer: (String) -> Void
    @ObservedObject private var form: DesktopForm

    init(state: AppState, initialSection: String = "Connection", toggleConnection: @escaping () -> Void,
         restart: @escaping () -> Void, refreshPermissions: @escaping () -> Void,
         approvePairing: @escaping () -> Void = {}, rejectPairing: @escaping () -> Void = {},
         forgetPeer: @escaping (String) -> Void = { _ in }) {
        self.state = state
        self.toggleConnection = toggleConnection
        self.restart = restart
        self.refreshPermissions = refreshPermissions
        self.approvePairing = approvePairing
        self.rejectPairing = rejectPairing
        self.forgetPeer = forgetPeer
        let form = DesktopForm()
        form.section = initialSection
        self.form = form
    }


    var body: some View {
        HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 10) {
                    Image(systemName: "bolt.horizontal.circle.fill").font(.system(size: 29)).foregroundStyle(Studio.accent)
                    Text("ThunderLink").font(.system(size: 19, weight: .semibold))
                }.padding(.bottom, 30)
                Text("WORKSPACE").font(.system(size: 10, weight: .semibold)).foregroundStyle(.secondary).padding(.leading, 12)
                ForEach(["Connection", "Settings", "Diagnostics"], id: \.self) { item in
                    Button { form.section = item } label: {
                        Label(item, systemImage: item == "Connection" ? "display.2" : item == "Settings" ? "slider.horizontal.3" : "waveform.path.ecg")
                            .font(.system(size: 13, weight: .medium)).frame(maxWidth: .infinity, alignment: .leading)
                            .padding(12).background(form.section == item ? Studio.accent.opacity(0.16) : .clear, in: RoundedRectangle(cornerRadius: 9))
                    }.buttonStyle(.plain).foregroundStyle(form.section == item ? Studio.accent : Studio.muted)
                }
                Spacer()
                Label("MAC · \(state.mode == .cross ? "CROSS RECEIVER" : "SCREEN SENDER")", systemImage: "desktopcomputer")
                    .font(.system(size: 10, weight: .medium)).foregroundStyle(.secondary)
                Text("Close to keep running in the menu bar.")
                    .font(.caption).foregroundStyle(.secondary).padding(.top, 8)
            }.padding(22).frame(width: 210).background(Studio.surface.opacity(0.55))
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    Text(form.section.uppercased()).font(.system(size: 10, weight: .semibold)).tracking(2).foregroundStyle(Studio.accent)
                    Text(form.section == "Connection" ? (state.mode == .cross ? "One keyboard. Every display." : "Your Mac. On the big screen.") : form.section == "Settings" ? "Make the connection yours." : "Everything, in view.")
                        .font(.system(size: 27, weight: .semibold))
                    Text(form.section == "Connection" ? (state.mode == .cross ? "Windows input is moving across your arranged Mac displays." : "Share your display with your Windows PC over a direct connection.") : form.section == "Settings" ? "Changes take effect after a safe restart." : "Connection details and local diagnostics.")
                        .foregroundStyle(.secondary)
                    if let error = state.configurationError {
                        card {
                            Label("Settings need attention", systemImage: "exclamationmark.triangle").foregroundStyle(.orange)
                            Text(error).font(.callout)
                            Button("Reset to Defaults") { resetSettings() }
                        }
                    }
                    if form.section == "Connection" { connection }
                    else if form.section == "Settings" { settings }
                    else { diagnostics }
                }.padding(32).frame(maxWidth: .infinity, alignment: .leading)
            }
        }.background(Studio.background).foregroundStyle(Color(red: 0.91, green: 0.93, blue: 0.97))
            .tint(Studio.accent).preferredColorScheme(.dark)
    }

    private func card<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 15, content: content)
            .frame(maxWidth: .infinity, alignment: .leading).padding(22)
            .background(Studio.surface, in: RoundedRectangle(cornerRadius: 14))
            .overlay(RoundedRectangle(cornerRadius: 14).stroke(.white.opacity(0.06), lineWidth: 1))
    }

    private var connection: some View {
        VStack(spacing: 18) {
            card {
                HStack {
                    Image(systemName: state.mode == .cross ? "rectangle.connected.to.line.below" : state.streaming ? "display.badge.checkmark" : "display.2")
                        .font(.system(size: 32)).foregroundStyle(Studio.accent)
                    VStack(alignment: .leading, spacing: 5) {
                        Text(state.paused ? "Disconnected" : state.mode == .cross ? state.crossPhase.rawValue : state.phase.rawValue).font(.system(size: 20, weight: .semibold))
                        Text(state.mode == .cross ? (state.crossPeerName ?? "Windows PC") : "Windows PC · \(state.peerIP)").foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(state.paused ? "Connect" : "Disconnect", action: toggleConnection)
                        .buttonStyle(.borderedProminent).disabled(state.busy || state.configurationError != nil)
                }
                if state.streaming {
                    Divider()
                    HStack(spacing: 26) {
                        metric("RESOLUTION", "\(state.width) × \(state.height)")
                        metric("FRAME RATE", "\(state.fps) fps")
                        metric("ENCODING", "\(state.codec) · \(state.bitrateKbps / 1000) Mbps")
                    }
                }
                if state.mode == .cross {
                    Divider()
                    HStack(spacing: 26) {
                        metric("MODE", "Cross")
                        metric("INPUT", state.crossRemoteOwnership ? "On Mac" : "On Windows")
                        metric("NETWORK", state.crossInterface ?? "Local network")
                    }
                }
                if let failure = state.failureReason { Text(failure).font(.callout).foregroundStyle(.orange) }
                if let failure = state.crossError { Text(failure).font(.callout).foregroundStyle(.orange) }
                Text("Reconnects automatically. Hiding this window keeps your session running.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let code = state.crossVerificationCode {
                card {
                    Label("Approve Cross pairing", systemImage: "lock.shield").font(.headline)
                    Text("Confirm that this code matches the one on your Windows PC.").foregroundStyle(.secondary)
                    Text(code).font(.system(size: 28, weight: .semibold, design: .monospaced)).textSelection(.enabled)
                    HStack {
                        Button("Approve", action: approvePairing).buttonStyle(.borderedProminent)
                        Button("Decline", action: rejectPairing)
                    }
                    Text("Discovery does not grant control. Cross accepts input only after both computers approve this certificate identity.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            card {
                Text(state.mode == .cross ? "Cross permissions" : "Ready to share").font(.headline)
                permission("Screen Recording", granted: state.screenRecordingGranted,
                           detail: state.mode == .cross ? "Needed only for Screen Share; Cross runs without it." : "Allows ThunderLink to share your display.") {
                    if !CGPreflightScreenCaptureAccess() { _ = CGRequestScreenCaptureAccess() }
                    openPrivacy("Privacy_ScreenCapture")
                }
                Divider()
                permission("Accessibility", granted: state.accessibilityGranted, detail: "Allows mouse and keyboard control from Windows.") {
                    openPrivacy("Privacy_Accessibility")
                }
                Text("After granting access, quit and reopen ThunderLink if macOS requests it.").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    private func metric(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 7) {
            Text(title).font(.system(size: 9, weight: .semibold)).foregroundStyle(.secondary)
            Text(value).font(.system(size: 14, weight: .medium)).monospacedDigit()
        }
    }
    private func permission(_ title: String, granted: Bool, detail: String, action: @escaping () -> Void) -> some View {
        HStack {
            Image(systemName: granted ? "checkmark.circle.fill" : "exclamationmark.circle").foregroundStyle(granted ? .green : .orange)
            VStack(alignment: .leading, spacing: 4) { Text(title).font(.callout.weight(.medium)); Text(detail).font(.caption).foregroundStyle(.secondary) }
            Spacer()
            Button(granted ? "Settings" : "Grant Access", action: action)
        }
    }
    private func field(_ label: String, text: Binding<String>) -> some View {
        HStack { Text(label).frame(width: 150, alignment: .leading); TextField(label, text: text).textFieldStyle(.roundedBorder) }
    }
    private var settings: some View {
        VStack(spacing: 18) {
            card {
                Text("Connection").font(.headline)
                field("Windows address", text: $form.peer)
                field("Local bind address", text: $form.bind)
                field("Video port", text: $form.video)
                field("Input port", text: $form.input)
                field("Control port", text: $form.control)
                HStack {
                    Text("Share display").frame(width: 150, alignment: .leading)
                    Picker("Share display", selection: $form.display) {
                        ForEach(form.displays) { item in Text(item.label).tag(item.id) }
                        if !form.displays.contains(where: { $0.id == form.display }) {
                            Text("Saved display unavailable").tag(form.display)
                        }
                    }.labelsHidden().frame(maxWidth: .infinity, alignment: .leading)
                }
                Text(form.displayHint).font(.caption).foregroundStyle(.secondary)

                if let error = form.settingsError { Text(error).foregroundStyle(.orange).font(.callout) }
                HStack {
                    Button("Save and Restart") { saveSettings() }.buttonStyle(.borderedProminent).disabled(state.busy)
                    Button("Reset to Defaults") { resetSettings() }
                }
            }
            card {
                Text("Direct-link setup").font(.headline)
                Text("Connect your dedicated adapter, then assign this Mac's saved address to it. Your PC needs a matching address and subnet.")
                    .font(.callout).foregroundStyle(.secondary)
                Button("Configure Mac Adapter…") { configureAdapter() }.disabled(form.configuringAdapter || state.busy)
                if let result = form.setupResult { Text(result).font(.caption).foregroundStyle(.secondary) }
            }
            card {
                Text("Cross pairing").font(.headline)
                Text("Cross works over Thunderbolt networking, Ethernet, or a reachable Wi-Fi/LAN connection. Trusted device identities are stored in your Keychain.")
                    .font(.callout).foregroundStyle(.secondary)
                if state.pairedPeers.isEmpty {
                    Text("No paired Windows PCs").font(.caption).foregroundStyle(.secondary)
                } else {
                    ForEach(state.pairedPeers) { peer in
                        HStack {
                            VStack(alignment: .leading) {
                                Text(peer.name)
                                Text(String(peer.fingerprint.prefix(12)).uppercased()).font(.caption.monospaced()).foregroundStyle(.secondary)
                            }
                            Spacer()
                            Button("Forget") { forgetPeer(peer.fingerprint) }
                        }
                    }
                }
            }
        }.task { await form.refreshDisplays() }
    }
    private var diagnostics: some View {
        card {
            Text("Session overview").font(.headline)
            LabeledContent("Status", value: state.paused ? "Disconnected" : state.phase.rawValue)
            LabeledContent("Local address", value: NetworkConfig.macIP)
            LabeledContent("Peer address", value: state.peerIP)
            LabeledContent("Mode", value: state.mode.rawValue)
            LabeledContent("Protocol", value: "v\(NetworkConfig.protocolVersion)")
            LabeledContent("Video / Input / Control / Cross", value: "\(NetworkConfig.videoPort) / \(NetworkConfig.inputPort) / \(NetworkConfig.controlPort) / \(NetworkConfig.crossPort)")
            if let interface = state.crossInterface { LabeledContent("Cross route", value: interface) }
            Divider()
            Text("Logs stay on this Mac. Review them before sharing.").font(.callout).foregroundStyle(.secondary)
            Button("Open Logs") { NSWorkspace.shared.open(TLLogger.logFileURL().deletingLastPathComponent()) }
            Button("Refresh Permissions", action: refreshPermissions)
            Text("ThunderLink · Personal desktop build").font(.caption).foregroundStyle(.secondary)
        }
    }
    private func openPrivacy(_ pane: String) {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") { NSWorkspace.shared.open(url) }
    }
    private func saveSettings() {
        do {
            // Keep saved stream options without persisting this launch's CLI overrides.
            let prior = try RuntimeConfiguration.parse(arguments: ["ThunderLink"] + DesktopSettings.savedArguments)
            let args = ["--bind", form.bind, "--peer", form.peer, "--video-port", form.video, "--input-port", form.input,
                        "--control-port", form.control, "--display", form.display, "--no-auto-ip",
                        "--codec", prior.codec == .hevc ? "hevc" : "h264", "--bitrate", String(prior.bitrateKbps / 1000),
                        "--fps", String(prior.fps), "--res", "\(prior.width)x\(prior.height)"]
            try DesktopSettings.save(args)
            restart()
        } catch { form.settingsError = error.localizedDescription }
    }
    private func resetSettings() {
        do { try DesktopSettings.save([]); restart() }
        catch { form.settingsError = error.localizedDescription }
    }
    private func configureAdapter() {
        guard let (_, service) = LinkSetup.findLinkLocalService() else {
            form.setupResult = "No direct-link adapter found. Connect the cable or configure the address in System Settings → Network."
            return
        }
        let alert = NSAlert()
        alert.messageText = "Configure \(service)?"
        alert.informativeText = "Assign \(NetworkConfig.macIP) with subnet \(NetworkConfig.linkSubnet). This persists until you change it in Network settings. Unsaved fields above are not used."
        alert.addButton(withTitle: "Configure")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        form.configuringAdapter = true
        DispatchQueue.global(qos: .userInitiated).async {
            LinkSetup.ensureLinkAddress(service: service)
            DispatchQueue.main.async {
                form.configuringAdapter = false
                form.setupResult = "Setup attempt finished. Check connection status; detailed results are in Diagnostics → Open Logs."
            }
        }
    }
}
