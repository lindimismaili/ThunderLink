import ScreenCaptureKit
import CoreGraphics

/// Enumerates and manages available displays for screen capture.
final class DisplayManager {
    private(set) var displays: [SCDisplay] = []

    /// Refresh the list of available displays.
    func refreshDisplays() async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false,
                                                                           onScreenWindowsOnly: true)
        displays = content.displays
        logger.info("DisplayManager: found \(displays.count) display(s)")
        for (i, display) in displays.enumerated() {
            logger.info("  [\(i)] \(display.width)x\(display.height) (ID: \(display.displayID))")
        }
    }

    /// Get the main display (first in the list, typically the primary).
    var mainDisplay: SCDisplay? {
        return displays.first
    }

    /// Get a display by its CGDirectDisplayID.
    func display(byID displayID: CGDirectDisplayID) -> SCDisplay? {
        return displays.first { $0.displayID == displayID }
    }

    /// Get a display by index.
    func display(at index: Int) -> SCDisplay? {
        guard index >= 0 && index < displays.count else { return nil }
        return displays[index]
    }
}
