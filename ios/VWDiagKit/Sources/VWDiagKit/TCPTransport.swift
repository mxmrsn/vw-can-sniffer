import Foundation
import Network

/// The simulator's Bluetooth stand-in: the same packets over TCP
/// (VWDIAG_BLE_PORT=5555 hardware/pico2w/firmware/sim/vw_diag_sim, then connect to the Mac's address).
public final class TCPTransport: ByteTransport, @unchecked Sendable {
    private let connection: NWConnection
    private let queue = DispatchQueue(label: "vwdiag.tcp")
    private var continuation: AsyncStream<[UInt8]>.Continuation?
    public let incoming: AsyncStream<[UInt8]>

    public init(host: String, port: UInt16) {
        connection = NWConnection(host: NWEndpoint.Host(host), port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
        var cont: AsyncStream<[UInt8]>.Continuation!
        incoming = AsyncStream { cont = $0 }
        continuation = cont
    }

    /// Connect, waiting up to `timeout` seconds.
    public func connect(timeout: Double = 5) async throws {
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in
            let once = Once()
            connection.stateUpdateHandler = { [weak self] state in
                switch state {
                case .ready:
                    once.run { done.resume() }
                    self?.receive()
                case let .failed(e), let .waiting(e):
                    once.run { done.resume(throwing: e) }
                    if case .failed = state { self?.continuation?.finish() }
                case .cancelled:
                    once.run { done.resume(throwing: InterfaceError.disconnected) }
                    self?.continuation?.finish()
                default:
                    break
                }
            }
            connection.start(queue: queue)
            queue.asyncAfter(deadline: .now() + timeout) { [weak self] in
                once.run {
                    self?.connection.cancel()
                    done.resume(throwing: InterfaceError.noAnswer)
                }
            }
        }
    }

    private func receive() {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 4096) { [weak self] data, _, complete, error in
            if let data, !data.isEmpty { self?.continuation?.yield([UInt8](data)) }
            if complete || error != nil {
                self?.continuation?.finish()
            } else {
                self?.receive()
            }
        }
    }

    public func send(_ bytes: [UInt8]) async throws {
        try await withCheckedThrowingContinuation { (done: CheckedContinuation<Void, Error>) in
            connection.send(content: Data(bytes), completion: .contentProcessed { error in
                if let error { done.resume(throwing: error) } else { done.resume() }
            })
        }
    }

    public func close() {
        connection.cancel()
        continuation?.finish()
    }
}

/// Runs a closure at most once, from any thread.
final class Once: @unchecked Sendable {
    private let lock = NSLock()
    private var done = false
    func run(_ f: () -> Void) {
        lock.lock()
        defer { lock.unlock() }
        guard !done else { return }
        done = true
        f()
    }
}
