import Foundation
import IOKit
import IOKit.ps

/// A short lease, renewed on the app's main run loop. No paths or commands cross
/// the privileged boundary. Unknown/stale requests always permit sleep.
public struct PowerLease: Codable {
    public let uptime: TimeInterval
    public let requested: Bool
    public init(uptime: TimeInterval, requested: Bool) {
        self.uptime = uptime; self.requested = requested
    }
    public func isFresh(now: TimeInterval) -> Bool {
        uptime.isFinite && now.isFinite && now >= uptime && now - uptime < 4
    }
    public func shouldOverride(now: TimeInterval, onAC: Bool, thermalSafe: Bool) -> Bool {
        requested && isFresh(now: now) && onAC && thermalSafe
    }
}

public enum PowerHardware {
    public static var onAC: Bool {
        guard let info = IOPSCopyPowerSourcesInfo()?.takeRetainedValue(),
              let source = IOPSGetProvidingPowerSourceType(info)?.takeUnretainedValue() else { return false }
        return source as String == kIOPSACPowerValue
    }
    public static var sleepDisabled: Bool? {
        let entry = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"))
        guard entry != 0 else { return nil }
        defer { IOObjectRelease(entry) }
        return IORegistryEntryCreateCFProperty(entry, "SleepDisabled" as CFString,
            kCFAllocatorDefault, 0)?.takeRetainedValue() as? Bool
    }
}

/// Retain cleanup responsibility even when pmset reports failure: it may have
/// partially applied the change. Never relinquish it before verified restore.
public struct SleepOverride {
    public private(set) var needsRestore = false
    public init() {}
    public mutating func update(requested: Bool, set: (Bool) -> Bool) -> Bool {
        if requested {
            needsRestore = true
            return set(true)
        }
        guard needsRestore else { return true }
        if set(false) { needsRestore = false; return true }
        return false
    }
}
