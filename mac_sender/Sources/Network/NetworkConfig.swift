import Foundation

/// Network configuration constants matching common/protocol.h
enum NetworkConfig {
    static let macIP = "169.254.1.1"
    static let winIP = "169.254.1.2"

    static let videoPort: UInt16 = 50001   // UDP
    static let inputPort: UInt16 = 50002   // TCP
    static let controlPort: UInt16 = 50003 // TCP

    static let mtu = 9000
    static let headerSize = 32
    static let maxPayload = 8192

    static let magic: UInt32 = 0x544C4E4B  // "TLNK"
    static let protocolVersion: UInt32 = 1

    static let recvBufferSize = 4 * 1024 * 1024  // 4 MB
    static let sendBufferSize = 4 * 1024 * 1024  // 4 MB

    static let frameTimeoutMs = 50
    static let reconnectIntervalMs = 1000
    static let pingIntervalMs = 1000

    // Pixel format IDs (matching TlnkPixelFormat)
    static let formatBGRA32: UInt16 = 0
}
