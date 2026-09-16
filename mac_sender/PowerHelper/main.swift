import Foundation
import Darwin
import PowerSupport

// Ephemeral, administrator-authorized watchdog. It installs no daemon or sudo
// rule. The only privileged mutation is the fixed pmset command below.
func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8)); exit(1)
}
guard CommandLine.arguments.count == 3, geteuid() == 0,
      let owner = uid_t(CommandLine.arguments[2]), owner != 0 else { fail("Invalid power helper invocation") }
let directory = CommandLine.arguments[1]
let prefix = "/private/tmp/thunderlink-power-"
guard directory.hasPrefix(prefix), UUID(uuidString: String(directory.dropFirst(prefix.count))) != nil else {
    fail("Invalid lease directory")
}
let dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
var metadata = stat()
guard dir >= 0, fstat(dir, &metadata) == 0, metadata.st_uid == owner,
      metadata.st_mode & 0o777 == 0o700 else { fail("Invalid lease owner") }
defer { close(dir) }

// Serialize overrides across users and app instances. Never follow a symlink,
// truncate, or unlink a pre-existing lock file.
let lock = open("/var/run/com.thunderlink.power.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0o600)
guard lock >= 0, fstat(lock, &metadata) == 0, metadata.st_uid == 0,
      metadata.st_mode & 0o777 == 0o600,
      metadata.st_mode & S_IFMT == S_IFREG, metadata.st_nlink == 1,
      flock(lock, LOCK_EX | LOCK_NB) == 0 else { fail("Another power helper is already running") }
defer { close(lock) }

func openClientFile(_ name: String, flags: Int32) -> Int32? {
    let fd = openat(dir, name, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK)
    var info = stat()
    guard fd >= 0 else { return nil }
    guard fstat(fd, &info) == 0, info.st_uid == owner, info.st_mode & S_IFMT == S_IFREG,
          info.st_mode & 0o777 == 0o600, info.st_nlink == 1, info.st_size <= 1024 else {
        close(fd); return nil
    }
    return fd
}
guard let statusFD = openClientFile("status", flags: O_WRONLY) else { fail("Invalid status file") }
defer { close(statusFD) }
func report(_ status: String) {
    let data = Data(status.utf8)
    _ = data.withUnsafeBytes { pwrite(statusFD, $0.baseAddress, $0.count, 0) }
    _ = ftruncate(statusFD, off_t(data.count))
}
func readLease() -> PowerLease? {
    guard let fd = openClientFile("lease", flags: O_RDONLY) else { return nil }
    defer { close(fd) }
    var bytes = [UInt8](repeating: 0, count: 1024)
    let count = read(fd, &bytes, bytes.count)
    guard count > 0 else { return nil }
    return try? JSONDecoder().decode(PowerLease.self, from: Data(bytes.prefix(count)))
}
guard PowerHardware.sleepDisabled == false else {
    report("conflict"); fail("Sleep is already disabled outside ThunderLink, or its state cannot be read")
}

func setOverride(_ enabled: Bool) -> Bool {
    if PowerHardware.sleepDisabled == enabled { return true }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/bin/pmset")
    process.arguments = ["-a", "disablesleep", enabled ? "1" : "0"]
    process.standardOutput = FileHandle.nullDevice
    process.standardError = FileHandle.nullDevice
    do { try process.run() } catch { return false }
    // Avoid a blocked pmset preventing the watchdog from checking AC/leases.
    let deadline = ProcessInfo.processInfo.systemUptime + 2
    while process.isRunning && ProcessInfo.processInfo.systemUptime < deadline { usleep(20_000) }
    if process.isRunning { kill(process.processIdentifier, SIGKILL) }
    process.waitUntilExit()
    return process.terminationStatus == 0 && PowerHardware.sleepDisabled == enabled
}

signal(SIGTERM, SIG_IGN); signal(SIGINT, SIG_IGN); signal(SIGHUP, SIG_IGN)
var stopping = false
let signals = [SIGTERM, SIGINT, SIGHUP].map { value -> DispatchSourceSignal in
    let source = DispatchSource.makeSignalSource(signal: value, queue: .main)
    source.setEventHandler { stopping = true }; source.resume(); return source
}
var override = SleepOverride()
report("ready")
while true {
    let lease = readLease()
    // Timestamp after the read: a concurrent atomic heartbeat publication must
    // not look like a future lease merely because file I/O took time.
    let now = ProcessInfo.processInfo.systemUptime
    let fresh = lease?.isFresh(now: now) == true
    let thermal = ProcessInfo.processInfo.thermalState
    let requested = !stopping && lease?.shouldOverride(now: now, onAC: PowerHardware.onAC,
        thermalSafe: thermal != .serious && thermal != .critical) == true
    let success = override.update(requested: requested, set: setOverride)
    if requested && !success { stopping = true }
    report(success ? (requested ? "active" : "ready") : "error")
    if (!fresh || stopping) && !override.needsRestore { break }
    // A failed restoration keeps this watchdog alive, retrying, even after the
    // app exits. It never reports successful cleanup before kernel readback.
    RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.5))
}
withExtendedLifetime(signals) {}
