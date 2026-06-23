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
    static func apply(requestedWidth w: Int, requestedHeight h: Int) -> (width: Int, height: Int) {
        if w <= 0 || h <= 0 {
            restore()
        } else {
            switchMode(width: w, height: h)
        }

        let bounds = CGDisplayBounds(displayID)
        let cw = Int(bounds.width)  & ~1
        let ch = Int(bounds.height) & ~1
        return (max(cw, 2), max(ch, 2))
    }

    /// Restore the mode captured before the first switch. No-op if unchanged.
    static func restore() {
        guard let mode = originalMode else { return }
        let result = CGDisplaySetDisplayMode(displayID, mode, nil)
        if result == .success {
            logger.info("DisplayController: restored original display mode")
        } else {
            logger.error("DisplayController: restore failed (err \(result.rawValue))")
        }
        originalMode = nil
    }

    // MARK: - Private

    private static func switchMode(width: Int, height: Int) {
        guard let modes = CGDisplayCopyAllDisplayModes(displayID, nil) as? [CGDisplayMode] else {
            logger.warn("DisplayController: could not enumerate display modes")
            return
        }
        if originalMode == nil { originalMode = CGDisplayCopyDisplayMode(displayID) }

        // Match by point size (the space apps and the cursor live in). If several
        // match (different refresh/HiDPI), prefer the highest pixel density.
        let candidates = modes.filter { $0.width == width && $0.height == height }
        guard let mode = candidates.max(by: { $0.pixelWidth < $1.pixelWidth }) else {
            logger.warn("DisplayController: no \(width)x\(height) mode available; capturing at current res")
            return
        }

        let result = CGDisplaySetDisplayMode(displayID, mode, nil)
        if result == .success {
            logger.info("DisplayController: main display set to \(width)x\(height)")
        } else {
            logger.error("DisplayController: failed to set \(width)x\(height) (err \(result.rawValue))")
        }
    }
}
