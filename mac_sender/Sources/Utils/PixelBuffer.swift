import CoreVideo
import CoreMedia

/// Utility for working with CVPixelBuffer from ScreenCaptureKit output.
enum PixelBufferUtils {

    /// Lock the pixel buffer for read-only access, extract base address and metadata.
    /// Returns nil if the buffer cannot be locked or has no base address.
    static func withLockedBaseAddress<T>(
        _ buffer: CVPixelBuffer,
        body: (UnsafeRawPointer, Int, Int, Int) -> T
    ) -> T? {
        let status = CVPixelBufferLockBaseAddress(buffer, .readOnly)
        guard status == kCVReturnSuccess else { return nil }
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }

        guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return nil }

        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)

        return body(UnsafeRawPointer(baseAddress), width, height, bytesPerRow)
    }

    /// Get the pixel format of the buffer.
    static func getPixelFormat(_ buffer: CVPixelBuffer) -> OSType {
        return CVPixelBufferGetPixelFormatType(buffer)
    }

    /// Check if the buffer is in BGRA format (our required format).
    static func isBGRA(_ buffer: CVPixelBuffer) -> Bool {
        return getPixelFormat(buffer) == kCVPixelFormatType_32BGRA
    }

    /// Get the total data size of the buffer (bytesPerRow * height).
    static func getDataSize(_ buffer: CVPixelBuffer) -> Int {
        let height = CVPixelBufferGetHeight(buffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
        return height * bytesPerRow
    }
}
