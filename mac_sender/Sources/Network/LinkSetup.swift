import Foundation

/// Ensures the direct-link adapter actually carries our bind IP
/// (`NetworkConfig.macIP`) before any socket tries to bind to it.
///
/// Without this, a freshly-connected USB-C/Ethernet/Thunderbolt adapter
/// self-assigns a random link-local address (e.g. `169.254.93.133`) instead of
/// the static `169.254.1.1` the app expects, so `bind()` fails with
/// `EADDRNOTAVAIL` (errno 49) and the peer can't reach the Mac.
///
/// Assigning an IP is privileged, so when a change is actually needed this
/// triggers the standard macOS admin-password prompt (once). If the IP is
/// already present, nothing happens and the user is never prompted. The change
/// is made via `networksetup -setmanual`, so it persists across reboots and
/// reconnects (revert with `networksetup -setdhcp "<service>"`).
enum LinkSetup {

    /// Detect the link-local adapter and assign `NetworkConfig.macIP` to it if
    /// it isn't already there. Safe to call on every launch; no-ops (and never
    /// prompts) once the address is in place.
    static func ensureLinkAddress(service selectedService: String? = nil) {

        let ip = NetworkConfig.macIP
        // Nothing to pin when binding to "any", or for an unset address.
        guard ip != "0.0.0.0", ip != "0", !ip.isEmpty else { return }

        if interfaceAddresses().values.contains(where: { $0.contains(ip) }) {
            logger.info("LinkSetup: \(ip) already assigned; nothing to do")
            return
        }

        guard let service = selectedService ?? findLinkLocalService()?.service else {
            logger.error("LinkSetup: no direct-link adapter found. Configure the address in System Settings → Network.")
            return
        }

        logger.info("LinkSetup: assigning \(ip)/\(NetworkConfig.linkSubnet) to '\(service)' — admin prompt may appear")

        // Persistent, route-safe: no router argument, so this never becomes the
        // system default gateway even though the adapter is high in the service order.
        let quotedService = "'" + service.replacingOccurrences(of: "'", with: "'\"'\"'") + "'"
        let command = "/usr/sbin/networksetup -setmanual \(quotedService) \(ip) \(NetworkConfig.linkSubnet)"
        let (ok, errOut) = runWithAdminPrivileges(command)

        guard ok else {
            let detail = errOut.trimmingCharacters(in: .whitespacesAndNewlines)
            logger.error("LinkSetup: failed to assign \(ip) to '\(service)': \(detail.isEmpty ? "unknown error" : detail)")
            return
        }

        // configd applies the change asynchronously; wait briefly so the
        // subsequent bind() sees the address.
        if waitForAddress(ip, timeoutMs: 4000) {
            logger.info("LinkSetup: assigned \(ip) to '\(service)'")
        } else {
            logger.warn("LinkSetup: '\(service)' set to \(ip) but the address hasn't appeared yet; bind may need a retry")
        }
    }

    // MARK: - Interface enumeration

    /// Map of interface name → list of its IPv4 dotted-quad addresses.
    private static func interfaceAddresses() -> [String: [String]] {
        var result: [String: [String]] = [:]
        var head: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&head) == 0 else { return result }
        defer { freeifaddrs(head) }

        var cursor = head
        while let entry = cursor {
            defer { cursor = entry.pointee.ifa_next }
            guard let sa = entry.pointee.ifa_addr,
                  sa.pointee.sa_family == sa_family_t(AF_INET),
                  (Int32(entry.pointee.ifa_flags) & IFF_LOOPBACK) == 0 else { continue }

            let name = String(cString: entry.pointee.ifa_name)
            var addr = sockaddr_in()
            memcpy(&addr, sa, Int(MemoryLayout<sockaddr_in>.size))
            var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            inet_ntop(AF_INET, &addr.sin_addr, &buf, socklen_t(INET_ADDRSTRLEN))
            result[name, default: []].append(String(cString: buf))
        }
        return result
    }

    /// Find the adapter that looks like the direct link: an interface whose only
    /// IPv4 is a self-assigned link-local (`169.254.x.x`) and which isn't Wi-Fi.
    /// Returns its BSD device name and its network-service name (needed by
    /// `networksetup -setmanual`).
    static func findLinkLocalService() -> (device: String, service: String)? {
        let addresses = interfaceAddresses()
        let serviceForDevice = deviceServiceMap()

        // Purely self-assigned link-local devices (no routable IPv4 alongside).
        let candidates = addresses.filter { name, ips in
            name != "lo0"
                && ips.contains { $0.hasPrefix("169.254.") }
                && !ips.contains { !$0.hasPrefix("169.254.") }
        }.keys.sorted()

        func isWiFi(_ service: String) -> Bool {
            let s = service.lowercased()
            return s.contains("wi-fi") || s.contains("wifi") || s.contains("airport")
        }

        // Prefer a candidate that maps to a non-Wi-Fi service.
        let ranked = candidates.compactMap { dev -> (String, String)? in
            guard let svc = serviceForDevice[dev] else { return nil }
            return (dev, svc)
        }
        if let wired = ranked.first(where: { !isWiFi($0.1) }) { return wired }
        return nil
    }

    /// Parse `networksetup -listnetworkserviceorder` into device → service name.
    private static func deviceServiceMap() -> [String: String] {
        guard let out = runCapture("/usr/sbin/networksetup", ["-listnetworkserviceorder"]) else {
            return [:]
        }
        var map: [String: String] = [:]
        var pendingService: String?

        for raw in out.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("(Hardware Port:") {
                if let dev = parseDevice(line), let svc = pendingService { map[dev] = svc }
                pendingService = nil
            } else if line.hasPrefix("("),
                      let close = line.firstIndex(of: ")"),
                      Int(line[line.index(after: line.startIndex)..<close]) != nil {
                // Numbered service line, e.g. "(1) USB 10/100/1000 LAN".
                pendingService = line[line.index(after: close)...].trimmingCharacters(in: .whitespaces)
            }
        }
        return map
    }

    /// Pull the device out of "(Hardware Port: ..., Device: en9)".
    private static func parseDevice(_ line: String) -> String? {
        guard let r = line.range(of: "Device: ") else { return nil }
        var rest = line[r.upperBound...]
        if let end = rest.firstIndex(of: ")") { rest = rest[..<end] }
        let dev = rest.trimmingCharacters(in: .whitespaces)
        return dev.isEmpty ? nil : dev
    }

    // MARK: - Waiting

    private static func waitForAddress(_ ip: String, timeoutMs: Int) -> Bool {
        var waited = 0
        while waited < timeoutMs {
            if interfaceAddresses().values.contains(where: { $0.contains(ip) }) { return true }
            Thread.sleep(forTimeInterval: 0.1)
            waited += 100
        }
        return interfaceAddresses().values.contains(where: { $0.contains(ip) })
    }

    // MARK: - Process helpers

    /// Run a shell command as root via the standard macOS admin-auth dialog.
    /// Returns success plus any stderr text. The user may cancel the prompt.
    private static func runWithAdminPrivileges(_ shellCommand: String) -> (ok: Bool, output: String) {
        // Embed the command into an AppleScript string literal: escape backslashes first, then quotes.
        let escaped = shellCommand
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
        let script = "do shell script \"\(escaped)\" with administrator privileges"

        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: "/usr/bin/osascript")
        proc.arguments = ["-e", script]
        let errPipe = Pipe()
        proc.standardOutput = Pipe()
        proc.standardError = errPipe
        do {
            try proc.run()
        } catch {
            return (false, "osascript launch failed: \(error)")
        }
        let errData = errPipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        let errStr = String(data: errData, encoding: .utf8) ?? ""
        return (proc.terminationStatus == 0, errStr)
    }

    /// Run a command and capture stdout (no privileges).
    private static func runCapture(_ path: String, _ args: [String]) -> String? {
        let proc = Process()
        proc.executableURL = URL(fileURLWithPath: path)
        proc.arguments = args
        let outPipe = Pipe()
        proc.standardOutput = outPipe
        proc.standardError = Pipe()
        do {
            try proc.run()
        } catch {
            return nil
        }
        let data = outPipe.fileHandleForReading.readDataToEndOfFile()
        proc.waitUntilExit()
        return String(data: data, encoding: .utf8)
    }
}
