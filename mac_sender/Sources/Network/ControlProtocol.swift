import Foundation

/// Control-channel message type IDs.
/// Must match `TlnkControlType` in common/protocol.h.
enum TlnkControlType: Int, Codable {
    case hello           = 1
    case start           = 2
    case stop            = 3
    case configure       = 4
    case ping            = 5
    case pong            = 6
    case bye             = 7
    case requestKeyframe = 8
}

/// JSON field names used on the control channel.
/// Must match the literals in windows_receiver/src/Network/NetworkManager.cpp.
enum ControlField {
    static let type      = "type"
    static let version   = "version"
    static let width     = "width"
    static let height    = "height"
    static let fps       = "fps"
    static let codec     = "codec"    // TlnkCodec: 0 = H.264, 1 = HEVC
    static let bitrate   = "bitrate"  // kbps
    static let sentTime  = "sent_time"
}
