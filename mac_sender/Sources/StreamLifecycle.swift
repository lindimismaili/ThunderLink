import Foundation

/// The single owner of stream transitions. Dependencies allow lifecycle tests
/// to exercise the real sequencing without capture, display or input effects.
@MainActor
final class StreamLifecycle {
    struct Services {
        var prepare: (StreamConfiguration) async throws -> StreamConfiguration
        var startCapture: (StreamConfiguration) async throws -> Void
        var sendStart: (StreamConfiguration) async -> Bool
        var closeDelivery: () -> Void
        var cleanup: () async -> Void
        var activate: (StreamConfiguration) -> Void
        var publish: (AppState.Phase, StreamConfiguration?, String?) -> Void
        var sendStop: (StreamConfiguration, String) -> Void
    }
    private let services: Services
    private var generation: UInt64 = 0
    private var operation: Task<Void, Never>?
    private var current: StreamConfiguration?
    private var stopped = false

    init(services: Services) { self.services = services }

    func configure(_ request: StreamConfiguration) {
        guard !stopped else { return }
        generation &+= 1
        let ticket = generation
        services.closeDelivery()
        current = request
        let preceding = operation
        operation = Task { [weak self] in
            await preceding?.value
            guard let self, self.valid(ticket) else { return }
            self.services.publish(.configuring, nil, nil)
            await self.services.cleanup()
            guard self.valid(ticket) else { return }
            do {
                let actual = try await self.services.prepare(request)
                guard self.valid(ticket) else { return }
                try await self.services.startCapture(actual)
                guard self.valid(ticket) else { return }
                guard await self.services.sendStart(actual) else {
                    throw StartError.disconnected
                }
                guard self.valid(ticket) else { return }
                self.current = actual
                self.services.activate(actual)
                self.services.publish(.streaming, actual, nil)
            } catch {
                guard self.valid(ticket) else { return }
                await self.fail(request, ticket: ticket, reason: error.localizedDescription)
            }
        }
    }

    func runtimeFailure(sessionID: StreamSessionID, reason: String) {
        guard !stopped, let request = current, request.sessionID == sessionID else { return }
        generation &+= 1
        let ticket = generation
        services.closeDelivery()
        let preceding = operation
        operation = Task { [weak self] in
            await preceding?.value
            guard let self, self.valid(ticket) else { return }
            await self.fail(request, ticket: ticket, reason: reason)
        }
    }

    func disconnect() {
        guard !stopped else { return }
        generation &+= 1
        let ticket = generation
        current = nil
        services.closeDelivery()
        let preceding = operation
        operation = Task { [weak self] in
            await preceding?.value
            guard let self, self.valid(ticket) else { return }
            await self.services.cleanup()
            guard self.valid(ticket) else { return }
            self.services.publish(.waiting, nil, nil)
        }
    }

    func shutdown() async {
        guard !stopped else { await operation?.value; return }
        stopped = true
        generation &+= 1
        current = nil
        services.closeDelivery()
        services.publish(.stopping, nil, nil)
        await operation?.value
        await services.cleanup()
    }

    func settle() async { await operation?.value }
    private func valid(_ ticket: UInt64) -> Bool { !stopped && generation == ticket }

    private func fail(_ request: StreamConfiguration, ticket: UInt64, reason: String) async {
        services.closeDelivery()
        await services.cleanup()
        guard valid(ticket) else { return }
        current = nil
        services.publish(.failed, request, reason)
        services.sendStop(request, reason)
    }

    private enum StartError: LocalizedError {
        case disconnected
        var errorDescription: String? { "The receiver disconnected before START completed" }
    }
}
