import Foundation

/// Commands, status codes and events of the link protocol (link_protocol.md).
public enum Command: UInt8, Sendable {
    case hello = 0x01, status = 0x02, setSilent = 0x03, setMonitor = 0x04, uds = 0x05, setKeepalive = 0x06
    case setLog = 0x08, setTime = 0x09, logStatus = 0x0A, pollAdd = 0x0B, pollClear = 0x0C, logMark = 0x0D
    case logList = 0x0E, logRead = 0x0F, power = 0x10
    case exit = 0x7F
}

public enum InterfaceStatus: UInt8, Sendable {
    case ok = 0, badArgs, unknownCommand, silent, timeout, negative, transport, storage, busy

    public var text: String {
        switch self {
        case .ok: return "ok"
        case .badArgs: return "bad arguments"
        case .unknownCommand: return "unknown command"
        case .silent: return "listen-only: nothing was sent to the car"
        case .timeout: return "the control module did not answer"
        case .negative: return "refused by the control module"
        case .transport: return "transport error"
        case .storage: return "no card, or the file could not be written or read"
        case .busy: return "that file is being recorded: stop recording first"
        }
    }
}

public enum InterfaceError: Error, LocalizedError, Equatable, Sendable {
    case refused(Command, InterfaceStatus)
    case noAnswer                 // the interface itself did not reply in time
    case disconnected
    case malformed(String)
    case wrongVersion(UInt8)

    public var errorDescription: String? {
        switch self {
        case let .refused(cmd, status): return "interface refused \(cmd): \(status.text)"
        case .noAnswer: return "no reply from the interface"
        case .disconnected: return "the interface stopped answering: it went to sleep, was unplugged, or is out of range"
        case let .malformed(what): return "unexpected reply: \(what)"
        case let .wrongVersion(v): return "interface speaks protocol version \(v); this app speaks 1"
        }
    }
}

/// A CAN frame seen while monitoring.
public struct CANFrame: Sendable, Equatable {
    public let timeMs: UInt32
    public let id: UInt32
    public let extended: Bool
    public let data: [UInt8]

    public var text: String {
        (extended ? String(format: "%08X", id) : String(format: "%03X", id)) + " [\(data.count)]"
            + data.map { String(format: " %02X", $0) }.joined()
    }
}

public struct InterfaceInfo: Sendable, Equatable {
    public let version: UInt8
    public let maxPayload: Int
    public let name: String
}

public struct InterfaceState: Sendable, Equatable {
    public var listenOnly, monitor, keepalive, logging: Bool
    public var framesReceived, receiveDrops, framesSent, busErrors, damagedPackets, eventsDropped: UInt32
}

public struct LogState: Sendable, Equatable {
    public var active: Bool
    public var frames, bytes, lost: UInt32
    public var name: String
}

/// Something that carries the packet byte stream: the simulator's TCP stand-in, or Bluetooth LE.
public protocol ByteTransport: AnyObject, Sendable {
    /// Bytes from the interface, in whatever pieces they arrive. Finishes when the link drops.
    var incoming: AsyncStream<[UInt8]> { get }
    func send(_ bytes: [UInt8]) async throws
    func close()
}

/// Little-endian reading helpers for response data.
extension Array where Element == UInt8 {
    func u16(_ at: Int) -> UInt16 { UInt16(self[at]) | UInt16(self[at + 1]) << 8 }
    func u32(_ at: Int) -> UInt32 {
        UInt32(self[at]) | UInt32(self[at + 1]) << 8 | UInt32(self[at + 2]) << 16 | UInt32(self[at + 3]) << 24
    }
}

func le32(_ v: UInt32) -> [UInt8] { [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v >> 16 & 0xFF), UInt8(v >> 24)] }
func le16(_ v: UInt16) -> [UInt8] { [UInt8(v & 0xFF), UInt8(v >> 8)] }

/// The interface, spoken to in packets. One request at a time, as the interface expects;
/// events (monitored CAN frames) can arrive in between and go to `frames`.
public actor Interface {
    private let transport: ByteTransport
    private var decoder = PacketDecoder()
    private var seq: UInt8 = 0
    private struct Pending {
        let seq: UInt8
        let cmd: Command
        let cont: CheckedContinuation<(InterfaceStatus, [UInt8]), Error>
        var timer: Task<Void, Never>?
    }
    private var pending: Pending?
    private var busy = false
    private var queue: [CheckedContinuation<Void, Never>] = []
    private var reader: Task<Void, Never>?
    private var closed = false
    private var frameContinuation: AsyncStream<CANFrame>.Continuation?
    public private(set) var info: InterfaceInfo?
    private var collecting = false
    private var collected: [CANFrame] = []

    /// Monitored CAN frames, while monitoring is on.
    public nonisolated let frames: AsyncStream<CANFrame>

    public init(transport: ByteTransport) {
        self.transport = transport
        var cont: AsyncStream<CANFrame>.Continuation!
        frames = AsyncStream(bufferingPolicy: .bufferingNewest(2000)) { cont = $0 }
        frameContinuation = cont
    }

    /// Start reading and say hello. Throws if the interface does not answer or speaks another version.
    public func open() async throws -> InterfaceInfo {
        if reader == nil {
            let stream = transport.incoming
            reader = Task { [weak self] in
                for await chunk in stream { await self?.received(chunk) }
                await self?.linkClosed()
            }
        }
        let d = try await call(.hello, timeout: 3)
        guard d.count >= 3 else { throw InterfaceError.malformed("hello") }
        let info = InterfaceInfo(version: d[0], maxPayload: Int(d.u16(1)), name: String(decoding: d[3...], as: UTF8.self))
        guard info.version == 1 else { throw InterfaceError.wrongVersion(info.version) }
        self.info = info
        return info
    }

    public func close() {
        closed = true
        reader?.cancel()
        transport.close()
        failPending(InterfaceError.disconnected)
        frameContinuation?.finish()
    }

    public var damagedPackets: Int { decoder.damaged }

    /// Keep monitored frames in a buffer as well as the stream, for measurements that want
    /// them afterwards (the stream is for live display and ends if its reader is cancelled).
    public func startCollecting() {
        collected.removeAll()
        collecting = true
    }

    /// The frames since startCollecting(); collection stops.
    public func stopCollecting() -> [CANFrame] {
        collecting = false
        defer { collected.removeAll() }
        return collected
    }

    // MARK: packets in
    private func received(_ chunk: [UInt8]) {
        for p in decoder.feed(chunk) {
            if p[0] == 0 {
                event(p)
            } else if let w = pending, p[0] == w.seq, p[1] == w.cmd.rawValue | 0x80, p.count >= 3 {
                pending = nil
                w.timer?.cancel()
                w.cont.resume(returning: (InterfaceStatus(rawValue: p[2]) ?? .badArgs, Array(p[3...])))
            }
            // anything else is a stale reply to an earlier request: ignore it
        }
    }

    private func event(_ p: [UInt8]) {
        guard p[1] == 0x01, p.count >= 11 else { return }
        let raw = p.u32(6)
        let len = Int(p[10])
        guard p.count >= 11 + len else { return }
        let frame = CANFrame(timeMs: p.u32(2), id: raw & 0x1FFF_FFFF, extended: raw >> 31 != 0, data: Array(p[11..<(11 + len)]))
        if collecting && collected.count < 50_000 { collected.append(frame) }
        frameContinuation?.yield(frame)
    }

    private func linkClosed() {
        closed = true
        failPending(InterfaceError.disconnected)
        frameContinuation?.finish()
    }

    private func failPending(_ error: Error) {
        if let w = pending {
            pending = nil
            w.timer?.cancel()
            w.cont.resume(throwing: error)
        }
    }

    // MARK: requests
    /// Send one command and wait for its answer (status and data).
    public func exchange(_ cmd: Command, _ args: [UInt8] = [], timeout: Double = 15) async throws -> (InterfaceStatus, [UInt8]) {
        while busy { await withCheckedContinuation { queue.append($0) } }
        busy = true
        defer {
            busy = false
            if !queue.isEmpty { queue.removeFirst().resume() }
        }
        guard !closed else { throw InterfaceError.disconnected }
        seq = seq % 255 + 1
        let mySeq = seq
        let packet = Framing.encode([mySeq, cmd.rawValue] + args)
        let transport = self.transport
        return try await withCheckedThrowingContinuation { cont in
            let timer = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
                if !Task.isCancelled { await self?.timeoutFired(mySeq, error: InterfaceError.noAnswer) }
            }
            pending = Pending(seq: mySeq, cmd: cmd, cont: cont, timer: timer)
            Task { [weak self] in
                do { try await transport.send(packet) } catch { await self?.timeoutFired(mySeq, error: InterfaceError.disconnected) }
            }
        }
    }

    private func timeoutFired(_ s: UInt8, error: Error) {
        if let w = pending, w.seq == s {
            pending = nil
            w.timer?.cancel()
            w.cont.resume(throwing: error)
        }
    }

    /// As exchange(), but anything other than OK is an error.
    @discardableResult
    public func call(_ cmd: Command, _ args: [UInt8] = [], timeout: Double = 15) async throws -> [UInt8] {
        let (status, data) = try await exchange(cmd, args, timeout: timeout)
        guard status == .ok else { throw InterfaceError.refused(cmd, status) }
        return data
    }

    // MARK: the interface's commands
    public func state() async throws -> InterfaceState {
        let d = try await call(.status)
        guard d.count >= 21 else { throw InterfaceError.malformed("status") }
        let f = d[0]
        return InterfaceState(listenOnly: f & 1 != 0, monitor: f & 2 != 0, keepalive: f & 4 != 0, logging: f & 8 != 0,
                              framesReceived: d.u32(1), receiveDrops: d.u32(5), framesSent: d.u32(9), busErrors: d.u32(13),
                              damagedPackets: d.u32(17), eventsDropped: d.count >= 25 ? d.u32(21) : 0)
    }

    /// Listen-only is the power-up state. Requests to the car need it off.
    public func setListenOnly(_ on: Bool) async throws { try await call(.setSilent, [on ? 1 : 0]) }
    public func setMonitor(_ on: Bool) async throws { try await call(.setMonitor, [on ? 1 : 0]) }
    public func setKeepalive(_ on: Bool) async throws { try await call(.setKeepalive, [on ? 1 : 0]) }

    /// One diagnostic request. Returns the positive response (service ID + 0x40 first).
    public func uds(request: UInt32, response: UInt32, _ payload: [UInt8], timeoutMs: UInt16 = 0) async throws -> [UInt8] {
        let args = le32(request) + le32(response) + le16(timeoutMs) + payload
        let (status, data) = try await exchange(.uds, args, timeout: Double(timeoutMs == 0 ? 1000 : timeoutMs) / 1000 + 70)
        switch status {
        case .ok: return data
        case .negative: throw UDSError.negative(data.first ?? 0)
        case .timeout: throw UDSError.noResponse
        case .silent: throw UDSError.listenOnly
        case .transport: throw UDSError.transport(Int8(bitPattern: data.first ?? 0))
        default: throw InterfaceError.refused(.uds, status)
        }
    }

    public func setTime(_ date: Date = Date()) async throws { try await call(.setTime, le32(UInt32(date.timeIntervalSince1970))) }

    private func logReply(_ d: [UInt8]) throws -> LogState {
        guard d.count >= 13 else { throw InterfaceError.malformed("log state") }
        return LogState(active: d[0] != 0, frames: d.u32(1), bytes: d.u32(5), lost: d.u32(9),
                        name: String(decoding: d[13...], as: UTF8.self))
    }

    public func logState() async throws -> LogState { try logReply(try await call(.logStatus)) }
    public func setLogging(_ on: Bool) async throws -> LogState { try logReply(try await call(.setLog, [on ? 1 : 0])) }
    public func logMark(_ text: String) async throws { try await call(.logMark, Array(text.utf8.prefix(200))) }

    public func pollAdd(request: UInt32, response: UInt32, periodMs: UInt32, _ payload: [UInt8]) async throws {
        try await call(.pollAdd, le32(request) + le32(response) + le32(periodMs) + payload)
    }

    public func pollClear() async throws { try await call(.pollClear) }

    // MARK: power (the dongle; boards without it answer unknownCommand)
    public struct PowerState: Sendable, Equatable {
        public let holds: [String]          // what keeps it awake: usb, phone, activity, engine, bus, logging, stay
        public let wake: String             // power-on, usb, can, engine, button
        public let batteryMv: Int
        public let sleepsInS: Int           // 0 = not counting down
        public let batteryLowForS: Int
        public let idleS: Int
        public let cutoffMv: Int
        public let runningMv: Int
        public let stayAwake: Bool
    }

    public enum PowerChange: Sendable {
        case stayAwake(Bool), idle(seconds: Int), cutoff(mV: Int), sleepNow
    }

    static let powerHolds = ["usb", "phone", "activity", "engine", "bus", "logging", "stay"]
    static let powerWakes = ["unknown", "power-on", "usb", "can", "engine", "button"]

    /// The power state, after an optional change. nil: this board has no power management.
    public func power(_ change: PowerChange? = nil) async throws -> PowerState? {
        var args: [UInt8] = []
        switch change {
        case .none: break
        case let .stayAwake(on)?: args = [1] + le32(on ? 1 : 0)
        case let .idle(s)?: args = [2] + le32(UInt32(s))
        case let .cutoff(mv)?: args = [3] + le32(UInt32(mv))
        case .sleepNow?: args = [4] + le32(0)
        }
        let (status, d) = try await exchange(.power, args)
        if status == .unknownCommand { return nil }
        guard status == .ok, d.count >= 18 else { throw InterfaceError.refused(.power, status) }
        let holds = Self.powerHolds.enumerated().filter { d[0] & (1 << $0.offset) != 0 }.map(\.element)
        return PowerState(holds: holds, wake: Int(d[1]) < Self.powerWakes.count ? Self.powerWakes[Int(d[1])] : "unknown",
                          batteryMv: Int(d.u16(4)), sleepsInS: Int(d.u16(6)), batteryLowForS: Int(d.u16(8)),
                          idleS: Int(d.u32(10)), cutoffMv: Int(d.u16(14)), runningMv: Int(d.u16(16)), stayAwake: d[3] != 0)
    }

    // MARK: copying files off the card
    public struct CardFile: Sendable, Hashable, Identifiable {
        public let name: String
        public let size: Int
        public var id: String { name }
    }

    /// Every file on the card (asked for in pages, as the interface sends them).
    public func cardFiles() async throws -> [CardFile] {
        var out: [CardFile] = []
        var first: UInt16 = 0
        while true {
            let d = try await call(.logList, le16(first))
            guard d.count >= 3 else { throw InterfaceError.malformed("file list") }
            let total = Int(d.u16(0)), count = Int(d[2])
            var pos = 3
            for _ in 0..<count {
                guard pos + 5 <= d.count else { throw InterfaceError.malformed("file list") }
                let size = Int(d.u32(pos)), n = Int(d[pos + 4])
                guard pos + 5 + n <= d.count else { throw InterfaceError.malformed("file list") }
                out.append(CardFile(name: String(decoding: d[(pos + 5)..<(pos + 5 + n)], as: UTF8.self), size: size))
                pos += 5 + n
            }
            first += UInt16(count)
            if count == 0 || Int(first) >= total { return out }
        }
    }

    public static let readChunk = 4000

    /// Part of a file: its size and up to `length` bytes from `offset`.
    public func readCardFile(_ name: String, offset: Int, length: Int = readChunk) async throws -> (size: Int, data: [UInt8]) {
        let d = try await call(.logRead, le32(UInt32(offset)) + le16(UInt16(min(length, Self.readChunk))) + Array(name.utf8))
        guard d.count >= 4 else { throw InterfaceError.malformed("file read") }
        return (Int(d.u32(0)), Array(d[4...]))
    }

    /// A whole file. `progress(done, total)` after each piece.
    public func copyCardFile(_ name: String, progress: (@Sendable (Int, Int) -> Void)? = nil) async throws -> Data {
        var data = Data()
        var size = Int.max
        while data.count < size {
            try Task.checkCancellation()
            let piece = try await readCardFile(name, offset: data.count)
            size = piece.size
            if piece.data.isEmpty { break }
            data.append(contentsOf: piece.data)
            progress?(data.count, size)
        }
        return data
    }
}
