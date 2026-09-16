import Foundation
import Testing
@testable import ThunderLinkSender

@MainActor
private final class FakeCaptureServices {
    var phases: [AppState.Phase] = []
    var activated: [UInt64] = []
    var started: [UInt64] = []
    var stopReports = 0
    var restores = 0
    var displayChanged = false
    var startup: CheckedContinuation<Void, Error>?
    var pauseStartup = false
    var failPrepare = false
    var failEncoder = false
    var failCapture = false
    var failStart = false
    var startObserved: (() -> Void)?
    var services: StreamLifecycle.Services {
        .init(prepare: { request in
            if self.failPrepare { throw LifecycleError.screenRecordingPermission }
            self.displayChanged = true
            if self.failEncoder { throw LifecycleError.encoderUnavailable(.hevc) }
            return try request.resolved(width: 1920, height: 1080)
        }, startCapture: { request in
            self.started.append(request.sessionID.rawValue)
            if self.failCapture { throw LifecycleError.encoderUnavailable(.hevc) }
            if self.pauseStartup {
                try await withCheckedThrowingContinuation { continuation in
                    self.startup = continuation
                    self.startObserved?()
                }
            }
        }, sendStart: { _ in !self.failStart }, closeDelivery: {}, cleanup: {
            if self.displayChanged { self.restores += 1; self.displayChanged = false }
        }, activate: { self.activated.append($0.sessionID.rawValue) },
        publish: { phase, _, _ in self.phases.append(phase) },
        sendStop: { _, _ in self.stopReports += 1 })
    }
}

@MainActor
final class LifecycleTests {
    private func config(_ sid: UInt64) throws -> StreamConfiguration {
        try StreamConfiguration(sessionID: StreamSessionID(sid)!, width: 0, height: 0,
            fps: 60, codec: .hevc, bitrateKbps: 25000, connectionToken: 1)
    }

    @Test func userDisconnectCanReconnectWithoutRevivingOldSession() async throws {
        let fake = FakeCaptureServices()
        let lifecycle = StreamLifecycle(services: fake.services)
        lifecycle.configure(try config(1))
        await lifecycle.settle()
        lifecycle.disconnect()
        await lifecycle.settle()
        #expect(!fake.displayChanged)
        #expect(fake.phases.last == .waiting)
        lifecycle.configure(try config(2))
        await lifecycle.settle()
        #expect(fake.activated == [1, 2])
        await lifecycle.shutdown()
        #expect(fake.restores == 2)
    }

    @Test func rapidConfigurationSupersedesStartup() async throws {
        let fake = FakeCaptureServices(); fake.pauseStartup = true
        let lifecycle = StreamLifecycle(services: fake.services)
        let first = try config(1)
        await withCheckedContinuation { observed in
            fake.startObserved = { observed.resume() }
            lifecycle.configure(first)
        }
        lifecycle.configure(try config(2))
        lifecycle.configure(try config(3))
        fake.pauseStartup = false; fake.startup?.resume()
        await lifecycle.settle()
        #expect(fake.started == [1, 3])
        #expect(fake.activated == [3])
        #expect(fake.restores == 1)
        await lifecycle.shutdown()
        #expect(fake.restores == 2)
    }

    @Test func disconnectAndQuitDuringStartup() async throws {
        for quit in [false, true] {
            let fake = FakeCaptureServices(); fake.pauseStartup = true
            let lifecycle = StreamLifecycle(services: fake.services)
            let request = try config(1)
            await withCheckedContinuation { observed in
                fake.startObserved = { observed.resume() }
                lifecycle.configure(request)
            }
            if !quit { lifecycle.disconnect() }
            let shutdown = Task { if quit { await lifecycle.shutdown() } }
            await Task.yield()
            fake.startup?.resume()
            await shutdown.value
            await lifecycle.settle()
            #expect(fake.activated.isEmpty)
            #expect(fake.restores == 1)
            await lifecycle.shutdown()
            #expect(fake.restores == 1)
        }
    }

    @Test func failuresCleanUpAndCannotReviveOldSessions() async throws {
        for failure in 0..<4 {
            let fake = FakeCaptureServices()
            fake.failPrepare = failure == 0; fake.failEncoder = failure == 1
            fake.failCapture = failure == 2; fake.failStart = failure == 3
            let lifecycle = StreamLifecycle(services: fake.services)
            lifecycle.configure(try config(1)); await lifecycle.settle()
            #expect(fake.phases.last == .failed)
            #expect(fake.activated.isEmpty)
            #expect(fake.stopReports == 1)
            #expect(fake.restores == (failure == 0 ? 0 : 1))
            lifecycle.runtimeFailure(sessionID: StreamSessionID(999)!, reason: "stale")
            await lifecycle.settle()
            #expect(fake.stopReports == 1)
            await lifecycle.shutdown()
        }
    }

    @Test func runtimeFailureDuringStartupCannotPublishStart() async throws {
        let fake = FakeCaptureServices(); fake.pauseStartup = true
        let lifecycle = StreamLifecycle(services: fake.services)
        let request = try config(1)
        await withCheckedContinuation { observed in
            fake.startObserved = { observed.resume() }
            lifecycle.configure(request)
        }
        lifecycle.runtimeFailure(sessionID: request.sessionID, reason: "capture failed")
        fake.startup?.resume()
        await lifecycle.settle()
        #expect(fake.activated.isEmpty)
        #expect(fake.phases.last == .failed)
        #expect(fake.restores == 1 && fake.stopReports == 1)
        await lifecycle.shutdown()
        #expect(fake.restores == 1)
    }
}
