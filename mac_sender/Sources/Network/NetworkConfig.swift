import Foundation

/// Network + stream configuration matching common/protocol.h.
///
/// Endpoint/codec fields are `static var` so they can be overridden from the
/// command line at startup (see `applyCommandLine`). They default to the
/// link-local addresses used for a direct USB-C→Ethernet link, so the common
/// "two adapters + a cable" setup needs no flags at all.
enum NetworkConfig {
    // MARK: Overridable endpoints (defaults match the direct link-local link)
    static var macIP = "169.254.1.1"   // our bind address
    static var winIP = "169.254.1.2"   // peer (receiver) address

    // When true (default), at startup the app assigns `macIP` to the direct-link
    // adapter if it isn't already present (see LinkSetup). Disable with --no-auto-ip.
    static var autoAssignIP = true
    static let linkSubnet = "255.255.255.0"

    static var videoPort: UInt16 = 50001   // UDP
    static var inputPort: UInt16 = 50002   // TCP
    static var controlPort: UInt16 = 50003 // TCP

    // MARK: Overridable stream defaults
    // The receiver normally drives res/fps/codec/bitrate via the CONFIGURE
    // handshake; these are fallback defaults / CLI overrides.
    static var defaultCodec: TlnkCodecSwift = .hevc
    static var defaultBitrateMbps: Int = 25
    static var defaultWidth: Int = 2560
    static var defaultHeight: Int = 1440
    static var defaultFPS: Int = 60

    // MARK: Fixed protocol constants
    // Standard 1500-byte Ethernet MTU (jumbo frames are usually off on
    // USB-C->Ethernet adapters). Must match TLNK_MTU / TLNK_MAX_PAYLOAD in
    // common/protocol.h. Oversized UDP datagrams get dropped by a non-jumbo
    // link → black screen while TCP control/input still work.
    static let mtu = 1500
    // Must match TLNK_HEADER_SIZE in common/protocol.h and sizeof(TlnkFrameHeader).
    static let headerSize = 40
    static let maxPayload = 1400

    static let magic: UInt32 = 0x544C4E4B  // "TLNK"
    static let protocolVersion: UInt32 = 2

    static let recvBufferSize = 4 * 1024 * 1024  // 4 MB
    static let sendBufferSize = 4 * 1024 * 1024  // 4 MB

    static let frameTimeoutMs = 50
    static let reconnectIntervalMs = 1000
    static let pingIntervalMs = 1000

    // Video payload format IDs (matching TlnkPixelFormat in protocol.h)
    static let formatBGRA32: UInt16 = 0
    static let formatH264: UInt16 = 1
    static let formatHEVC: UInt16 = 2

    /// Wire format byte for a given codec.
    static func wireFormat(for codec: TlnkCodecSwift) -> UInt16 {
        switch codec {
        case .h264: return formatH264
        case .hevc: return formatHEVC
        }
    }

    /// Parse command-line overrides. Recognized flags:
    ///   --peer <ip>          receiver IP (default 169.254.1.2)
    ///   --bind <ip>          our local IP (default 169.254.1.1; "0.0.0.0" = any)
    ///   --no-auto-ip         don't auto-assign --bind IP to the adapter at startup
    ///   --video-port <n>     --control-port <n>     --input-port <n>
    ///   --codec h264|hevc    --bitrate <Mbps>
    ///   --fps <n>            --res <WxH>
    static func applyCommandLine(_ args: [String]) {
        var i = 1
        func nextValue() -> String? {
            guard i + 1 < args.count else { return nil }
            i += 1
            return args[i]
        }
        while i < args.count {
            let flag = args[i]
            switch flag {
            case "--peer":         if let v = nextValue() { winIP = v }
            case "--bind":         if let v = nextValue() { macIP = v }
            case "--no-auto-ip":   autoAssignIP = false
            case "--video-port":   if let v = nextValue(), let p = UInt16(v) { videoPort = p }
            case "--control-port": if let v = nextValue(), let p = UInt16(v) { controlPort = p }
            case "--input-port":   if let v = nextValue(), let p = UInt16(v) { inputPort = p }
            case "--codec":
                if let v = nextValue() {
                    defaultCodec = (v.lowercased() == "h264") ? .h264 : .hevc
                }
            case "--bitrate":      if let v = nextValue(), let b = Int(v) { defaultBitrateMbps = b }
            case "--fps":          if let v = nextValue(), let f = Int(v) { defaultFPS = f }
            case "--res":
                if let v = nextValue() {
                    let parts = v.lowercased().split(separator: "x")
                    if parts.count == 2, let w = Int(parts[0]), let h = Int(parts[1]) {
                        defaultWidth = w; defaultHeight = h
                    }
                }
            default:
                break
            }
            i += 1
        }
    }
}

/// Swift mirror of TlnkCodec in protocol.h.
enum TlnkCodecSwift {
    case h264
    case hevc
}
