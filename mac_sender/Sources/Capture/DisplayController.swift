import CoreGraphics
import Foundation

/// Switches the Mac's main display resolution to match the resolution the
/// Windows side requests, so the captured stream is crisp/native instead of a
/// scaled copy. Used as the "duplicate at a different resolution" fallback: a
/// true second/virtual display needs private CoreGraphics APIs (or a helper like
/// BetterDisplay), so instead we change the main display's mode while streaming
/// and restore it when streaming stops.
enum DisplayController {
    private static var originalMode: CGDisplayMode?
    private static let displayID = CGMainDisplayID()

    /// Apply a requested resolution to the main display:
    ///   * width/height <= 0  -> restore the original (native) mode.
    ///   * otherwise          -> switch to a matching mode if one exists
    ///                           (if none exists, the display is left unchanged).
    ///
    /// Returns the size to capture at, in display POINTS (which is also the
    /// CGEvent coordinate space, so capture size and input mapping stay in sync).
    /// Even-clamped for NV12/HEVC.
    @discardableResult
    static func apply(requestedWidth w: Int, requestedHeight h: Int) throws -> (width: Int, height: Int) {
        if w <= 0 || h <= 0 {
            restore()
        } else {
            try switchMode(width: w, height: h)
        }

        let bounds = CGDisplayBounds(displayID)
        let cw = Int(bounds.width)  & ~1
        let ch = Int(bounds.height) & ~1
        return (max(cw, 2), max(ch, 2))
    }

    /// Point size of the main display (the CGEvent coordinate space used for
    /// input injection, regardless of which display is being captured).
    static func mainPointSize() -> (width: Int, height: Int) {
        let b = CGDisplayBounds(displayID)
        return (max(Int(b.width), 1), max(Int(b.height), 1))
    }

    /// Restore the mode captured before the first switch. No-op if unchanged.
    static func restore() {
        guard let mode = originalMode else { return }
        let result = CGDisplaySetDisplayMode(displayID, mode, nil)
        if result == .success {
            logger.info("DisplayController: restored original display mode")
            originalMode = nil
        } else {
            logger.error("DisplayController: restore failed (err \(result.rawValue))")
        }
    }

    // MARK: - Private

    private static func switchMode(width: Int, height: Int) throws {
        guard let modes = CGDisplayCopyAllDisplayModes(displayID, nil) as? [CGDisplayMode] else {
            throw DisplayConfigurationError.modeEnumerationFailed
        }

        // Match by point size (the space apps and the cursor live in). If several
        // match (different refresh/HiDPI), prefer the highest pixel density.
        let candidates = modes.filter { $0.width == width && $0.height == height }
        guard let mode = candidates.max(by: { $0.pixelWidth < $1.pixelWidth }) else {
            throw DisplayConfigurationError.unsupportedMode(width, height)
        }

        let previous = CGDisplayCopyDisplayMode(displayID)
        let result = CGDisplaySetDisplayMode(displayID, mode, nil)
        if result == .success {
            if originalMode == nil { originalMode = previous }
            logger.info("DisplayController: main display set to \(width)x\(height)")
        } else {
            throw DisplayConfigurationError.modeChangeFailed(width, height, result.rawValue)
        }
    }
}

enum DisplayConfigurationError: Error, LocalizedError {
    case modeEnumerationFailed
    case unsupportedMode(Int, Int)
    case modeChangeFailed(Int, Int, CGError.RawValue)

    var errorDescription: String? {
        switch self {
        case .modeEnumerationFailed:
            return "Could not inspect display modes"
        case .unsupportedMode(let width, let height):
            return "The display does not support \(width)x\(height); choose a smaller preset"
        case .modeChangeFailed(let width, let height, let code):
            return "Could not switch the display to \(width)x\(height) (error \(code))"
        }
    }
}
