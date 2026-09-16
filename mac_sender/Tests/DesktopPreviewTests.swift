import AppKit
import SwiftUI
import Testing
@testable import ThunderLinkSender

@MainActor
struct DesktopPreviewTests {
    /// Render the real SwiftUI view without opening a window, sockets, or a
    /// permission prompt. These artifacts supplement native interaction QA.
    @Test func renderDesktopAtDefaultAndMinimumSize() throws {
        let directory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent(".build/desktop-previews")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for width in [940, 780] {
            for section in ["Connection", "Settings", "Diagnostics"] {
                let state = AppState()
                let view = DesktopView(state: state, initialSection: section,
                    toggleConnection: {}, restart: {}, refreshPermissions: {})
                    .frame(width: CGFloat(width), height: width == 940 ? 660 : 538)
                _ = NSApplication.shared
                let host = NSHostingView(rootView: view.environment(\.colorScheme, .dark))
                host.frame = NSRect(x: 0, y: 0, width: width, height: width == 940 ? 660 : 538)
                host.appearance = NSAppearance(named: .darkAqua)
                let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
                window.isReleasedWhenClosed = false
                window.contentView = host
                host.layoutSubtreeIfNeeded()
                // AppKit-backed scroll views need a layout pass; ImageRenderer
                // intentionally omits these native surfaces.
                RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.05))
                let bitmap = try #require(host.bitmapImageRepForCachingDisplay(in: host.bounds))
                host.cacheDisplay(in: host.bounds, to: bitmap)
                let data = try #require(bitmap.representation(using: .png, properties: [:]))
                #expect(bitmap.pixelsWide >= width)
                window.close()
                try data.write(to: directory.appendingPathComponent("\(section)-\(width).png"))
            }
        }
        let crossState = AppState()
        crossState.mode = .cross
        crossState.crossPhase = .remote
        crossState.crossPeerName = "Windows PC"
        crossState.crossInterface = "Ethernet / Thunderbolt network · en5"
        crossState.crossConfigured = true
        crossState.crossRemoteOwnership = true
        for width in [940, 780] {
            let crossView = DesktopView(state: crossState, toggleConnection: {}, restart: {}, refreshPermissions: {})
                .frame(width: CGFloat(width), height: width == 940 ? 660 : 538)
            let crossHost = NSHostingView(rootView: crossView.environment(\.colorScheme, .dark))
            crossHost.frame = NSRect(x: 0, y: 0, width: width, height: width == 940 ? 660 : 538)
            crossHost.layoutSubtreeIfNeeded()
            let bitmap = try #require(crossHost.bitmapImageRepForCachingDisplay(in: crossHost.bounds))
            crossHost.cacheDisplay(in: crossHost.bounds, to: bitmap)
            try #require(bitmap.representation(using: .png, properties: [:]))
                .write(to: directory.appendingPathComponent("Connection-Cross-\(width).png"))
        }

        let pairingState = AppState()
        pairingState.crossPhase = .pairing
        pairingState.crossPeerName = "Windows PC"
        pairingState.crossVerificationCode = "2C5A757AF655"
        let pairingView = DesktopView(state: pairingState, toggleConnection: {}, restart: {}, refreshPermissions: {})
            .frame(width: 780, height: 538)
        let pairingHost = NSHostingView(rootView: pairingView.environment(\.colorScheme, .dark))
        pairingHost.frame = NSRect(x: 0, y: 0, width: 780, height: 538)
        pairingHost.layoutSubtreeIfNeeded()
        let pairingBitmap = try #require(pairingHost.bitmapImageRepForCachingDisplay(in: pairingHost.bounds))
        pairingHost.cacheDisplay(in: pairingHost.bounds, to: pairingBitmap)
        try #require(pairingBitmap.representation(using: .png, properties: [:]))
            .write(to: directory.appendingPathComponent("Connection-Cross-Pairing-780.png"))
    }
}
