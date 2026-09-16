import AppKit
import CoreGraphics
import ColorSync

struct CrossDisplayDescriptor: Equatable {
    let id: String
    let name: String
    let bounds: CGRect
    let scale: Double
    let rotation: Double
    let primary: Bool
    let systemID: CGDirectDisplayID

    var wireObject: [String: Any] {
        ["id": id, "name": name, "x": Double(bounds.origin.x), "y": Double(bounds.origin.y),
         "width": Double(bounds.width), "height": Double(bounds.height), "scale": scale,
         "rotation": rotation, "primary": primary]
    }
}

struct CrossDisplayTopology: Equatable {
    let revision: UInt64
    let displays: [CrossDisplayDescriptor]

    var wireObject: [String: Any] {
        ["type": "topology", "revision": NSNumber(value: revision),
         "displays": displays.map(\.wireObject)]
    }

    func display(id: String) -> CrossDisplayDescriptor? { displays.first { $0.id == id } }

    var identityError: String? {
        var seen: Set<String> = []
        let duplicates = Set(displays.compactMap { seen.insert($0.id).inserted ? nil : $0.id })
        guard !duplicates.isEmpty else { return nil }
        return "Multiple active Mac displays reported the same stable identity. Review the displays after reconnecting them"
    }
}

enum CrossCoordinateMapper {
    static func point(x: Float, y: Float, in display: CrossDisplayDescriptor) -> CGPoint? {
        guard x.isFinite, y.isFinite, (0...1).contains(x), (0...1).contains(y),
              display.bounds.width > 0, display.bounds.height > 0 else { return nil }
        // Stay just inside the destination display at 1.0. Posting exactly on
        // maxX/maxY can select the adjacent display in a mixed-origin layout.
        let pixel = 1 / max(display.scale, 1)
        return CGPoint(x: min(display.bounds.minX + CGFloat(x) * display.bounds.width,
                              display.bounds.maxX - pixel),
                       y: min(display.bounds.minY + CGFloat(y) * display.bounds.height,
                              display.bounds.maxY - pixel))
    }
}

final class CrossDisplayTopologyProvider {
    private var revision: UInt64 = 1
    private var lastDescriptors: [CrossDisplayDescriptor] = []

    func current() -> CrossDisplayTopology {
        let descriptors = Self.enumerate()
        if !lastDescriptors.isEmpty && descriptors != lastDescriptors { revision &+= 1 }
        if revision == 0 { revision = 1 }
        lastDescriptors = descriptors
        return CrossDisplayTopology(revision: revision, displays: descriptors)
    }

    static func enumerate() -> [CrossDisplayDescriptor] {
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else { return [] }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(min(count, UInt32(NetworkConfig.crossMaxDisplays))))
        var actual: UInt32 = 0
        guard CGGetActiveDisplayList(UInt32(ids.count), &ids, &actual) == .success else { return [] }
        let names = screenNames()
        return ids.prefix(Int(actual)).filter { CGDisplayMirrorsDisplay($0) == kCGNullDirectDisplay }
            .map { displayID in
                let bounds = CGDisplayBounds(displayID)
                let wide = Double(CGDisplayPixelsWide(displayID))
                let scale = bounds.width > 0 ? max(1, wide / Double(bounds.width)) : 1
                let vendor = CGDisplayVendorNumber(displayID)
                let model = CGDisplayModelNumber(displayID)
                let serial = CGDisplaySerialNumber(displayID)
                let unit = CGDisplayUnitNumber(displayID)
                let stableID: String
                if let uuid = CGDisplayCreateUUIDFromDisplayID(displayID)?.takeRetainedValue() {
                    stableID = "mac-\(CFUUIDCreateString(nil, uuid)! as String)".lowercased()
                } else {
                    // This fallback remains unique for the active topology. A
                    // later unit-number change produces a missing display for
                    // Windows to review rather than silently substituting it.
                    stableID = String(format: "mac-%08x-%08x-%08x-%08x", vendor, model, serial, unit)
                }
                return CrossDisplayDescriptor(id: stableID,
                                              name: names[displayID] ?? "Mac Display \(unit + 1)",
                                              bounds: bounds, scale: scale,
                                              rotation: CGDisplayRotation(displayID),
                                              primary: CGDisplayIsMain(displayID) != 0,
                                              systemID: displayID)
            }
            .sorted { lhs, rhs in
                if lhs.primary != rhs.primary { return lhs.primary }
                if lhs.bounds.minX != rhs.bounds.minX { return lhs.bounds.minX < rhs.bounds.minX }
                return lhs.bounds.minY < rhs.bounds.minY
            }
    }

    private static func screenNames() -> [CGDirectDisplayID: String] {
        if !Thread.isMainThread {
            return DispatchQueue.main.sync { screenNames() }
        }
        var result: [CGDirectDisplayID: String] = [:]
        for screen in NSScreen.screens {
            guard let number = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber else { continue }
            result[CGDirectDisplayID(number.uint32Value)] = screen.localizedName
        }
        return result
    }
}

final class CrossDisplayTopologyMonitor {
    fileprivate let queue: DispatchQueue
    private let provider: CrossDisplayTopologyProvider
    private let onChange: (CrossDisplayTopology) -> Void
    private var topology: CrossDisplayTopology
    private var registered = false

    init(queue: DispatchQueue, provider: CrossDisplayTopologyProvider,
         initial: CrossDisplayTopology, onChange: @escaping (CrossDisplayTopology) -> Void) {
        self.queue = queue
        self.provider = provider
        self.topology = initial
        self.onChange = onChange
    }

    func start() {
        guard !registered else { return }
        registered = CGDisplayRegisterReconfigurationCallback(crossDisplayReconfigurationCallback,
            Unmanaged.passUnretained(self).toOpaque()) == .success
    }

    func stop() {
        guard registered else { return }
        CGDisplayRemoveReconfigurationCallback(crossDisplayReconfigurationCallback,
            Unmanaged.passUnretained(self).toOpaque())
        registered = false
    }

    fileprivate func refresh() {
        let next = provider.current()
        guard next.displays != topology.displays else { return }
        topology = next
        onChange(next)
    }

    deinit { stop() }
}

private func crossDisplayReconfigurationCallback(_ display: CGDirectDisplayID,
                                                  _ flags: CGDisplayChangeSummaryFlags,
                                                  _ context: UnsafeMutableRawPointer?) {
    guard !flags.contains(.beginConfigurationFlag), let context else { return }
    let monitor = Unmanaged<CrossDisplayTopologyMonitor>.fromOpaque(context).takeUnretainedValue()
    // Core Graphics may notify before NSScreen has published the new bounds.
    monitor.queue.asyncAfter(deadline: .now() + .milliseconds(120)) { monitor.refresh() }
}

@MainActor
final class CrossDisplayIdentifierPresenter {
    static let shared = CrossDisplayIdentifierPresenter()
    private var panels: [NSPanel] = []

    func show(displays: [CrossDisplayDescriptor]) {
        panels.forEach { $0.orderOut(nil) }
        panels.removeAll()
        for (index, display) in displays.enumerated() {
            guard let screen = NSScreen.screens.first(where: {
                ($0.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value == display.systemID
            }) else { continue }
            let panel = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 180, height: 140),
                                styleMask: [.borderless], backing: .buffered, defer: false)
            panel.level = .screenSaver
            panel.isOpaque = false
            panel.backgroundColor = .clear
            panel.ignoresMouseEvents = true
            panel.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
            let label = NSTextField(labelWithString: String(index + 1))
            label.font = .systemFont(ofSize: 72, weight: .bold)
            label.textColor = .white
            label.alignment = .center
            label.wantsLayer = true
            label.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.82).cgColor
            label.layer?.cornerRadius = 20
            label.frame = NSRect(x: 0, y: 0, width: 180, height: 140)
            panel.contentView = label
            panel.setFrameOrigin(NSPoint(x: screen.frame.midX - 90, y: screen.frame.midY - 70))
            panel.orderFrontRegardless()
            panels.append(panel)
        }
        Task { @MainActor in
            try? await Task.sleep(for: .seconds(2))
            self.panels.forEach { $0.orderOut(nil) }
            self.panels.removeAll()
        }
    }
}
