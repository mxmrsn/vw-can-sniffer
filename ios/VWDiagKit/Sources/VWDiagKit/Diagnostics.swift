import Foundation

/// UDS (ISO 14229) negative response codes, as control modules report them.
public enum UDSError: Error, LocalizedError, Equatable, Sendable {
    case negative(UInt8)
    case noResponse
    case listenOnly
    case transport(Int8)
    case malformed(String)

    public static let nrcNames: [UInt8: String] = [
        0x10: "general reject", 0x11: "service not supported", 0x12: "sub-function not supported",
        0x13: "incorrect message length or format", 0x14: "response too long", 0x21: "busy, repeat request",
        0x22: "conditions not correct", 0x24: "request sequence error", 0x31: "request out of range",
        0x33: "security access denied", 0x35: "invalid key", 0x36: "exceeded number of attempts",
        0x37: "required time delay not expired", 0x72: "general programming failure",
        0x78: "response pending", 0x7E: "sub-function not supported in active session",
        0x7F: "service not supported in active session",
    ]

    public var errorDescription: String? {
        switch self {
        case let .negative(nrc):
            return String(format: "refused: NRC 0x%02X (%@)", nrc, UDSError.nrcNames[nrc] ?? "unlisted")
        case .noResponse: return "no answer from the control module"
        case .listenOnly: return "the interface is listen-only"
        case let .transport(code): return "transport error \(code)"
        case let .malformed(what): return "malformed answer: \(what)"
        }
    }
}

/// A control module: where to send requests and where answers come from.
public struct Module: Sendable, Hashable, Identifiable {
    public let key: String
    public let request: UInt32
    public let response: UInt32
    public let title: String
    public var id: String { key }
}

/// VW knowledge: addresses and identification items. Kept in step with tools/vwdiag/vw.py.
public enum VW {
    public static let modules: [Module] = [
        Module(key: "engine", request: 0x7E0, response: 0x7E8, title: "Engine control (01)"),
        Module(key: "trans", request: 0x7E1, response: 0x7E9, title: "Transmission (02)"),
        Module(key: "abs", request: 0x713, response: 0x77D, title: "ABS / ESC (03)"),
        Module(key: "cluster", request: 0x714, response: 0x77E, title: "Instrument cluster (17)"),
        Module(key: "airbag", request: 0x715, response: 0x77F, title: "Airbag (15)"),
        Module(key: "gateway", request: 0x710, response: 0x77A, title: "Diagnostic gateway (19)"),
        Module(key: "steer", request: 0x712, response: 0x77C, title: "Power steering (44)"),
        Module(key: "radar", request: 0x757, response: 0x7C1, title: "Front radar (13)"),
        Module(key: "camera", request: 0x74F, response: 0x7B9, title: "Front camera (A5)"),
    ]
    public static var engine: Module { modules[0] }

    public static let identification: [(did: UInt16, label: String)] = [
        (0xF190, "VIN"), (0xF187, "Part number"), (0xF189, "Software version"), (0xF191, "Hardware number"),
        (0xF1A3, "Hardware version"), (0xF197, "System name"), (0xF18C, "Serial number"), (0xF19E, "ODX file"),
        (0xF1A2, "ODX version"), (0xF182, "Coding / dataset"), (0xF1AD, "Engine code"),
    ]

    public static func text(_ data: [UInt8]) -> String {
        String(data.map { (32..<127).contains($0) ? Character(UnicodeScalar($0)) : "." }).trimmingCharacters(in: .whitespaces)
    }
}

/// A fault code: three bytes and a status byte.
public struct FaultCode: Sendable, Hashable, Identifiable {
    public let bytes: [UInt8]
    public let status: UInt8
    public var id: [UInt8] { bytes }

    public init(bytes: [UInt8], status: UInt8) {
        self.bytes = bytes
        self.status = status
    }

    /// SAE J2012 code from the first two bytes, e.g. P0303.
    public static func sae(_ b0: UInt8, _ b1: UInt8) -> String {
        let letter = ["P", "C", "B", "U"][Int(b0 >> 6)]
        return letter + String((b0 >> 4) & 0x3) + String(format: "%X%02X", b0 & 0xF, b1)
    }

    public var code: String { FaultCode.sae(bytes[0], bytes[1]) + String(format: "-%02X", bytes[2]) }

    public static let statusBits: [(UInt8, String)] = [
        (0x01, "test failed"), (0x02, "failed this operation cycle"), (0x04, "pending"), (0x08, "confirmed"),
        (0x10, "not tested since clear"), (0x20, "failed since clear"), (0x40, "not tested this cycle"),
        (0x80, "warning lamp requested"),
    ]

    public var statusText: [String] { FaultCode.statusBits.filter { status & $0.0 != 0 }.map(\.1) }

    /// Plain-language description, the same as the tools give (tools/vwdiag/dtc.py).
    public var description: String { FaultCode.describe(FaultCode.sae(bytes[0], bytes[1])) }

    /// Description of a "P0303" style code: from the generic list, the added codes, or the code's SAE group.
    public static func describe(_ code: String) -> String {
        let c = code.uppercased()
        if let d = DTCDescriptions.generic[c] ?? DTCDescriptions.extra[c] { return d }
        let g = DTCDescriptions.groups.filter { c.hasPrefix($0.0) }.max { $0.0.count < $1.0.count }?.1 ?? "reserved"
        let head = g.prefix(1).uppercased() + g.dropFirst()
        return g.contains("manufacturer") ? "\(head) code: VW's own description is not public" : "\(head) code, not in this list"
    }
}

/// A session with one control module.
public struct UDSClient: Sendable {
    public let interface: Interface
    public let module: Module
    public var timeoutMs: UInt16 = 0

    public init(_ interface: Interface, _ module: Module, timeoutMs: UInt16 = 0) {
        self.interface = interface
        self.module = module
        self.timeoutMs = timeoutMs
    }

    public func request(_ payload: [UInt8]) async throws -> [UInt8] {
        try await interface.uds(request: module.request, response: module.response, payload, timeoutMs: timeoutMs)
    }

    public func testerPresent() async throws { _ = try await request([0x3E, 0x00]) }

    public func readDID(_ did: UInt16) async throws -> [UInt8] {
        let r = try await request([0x22, UInt8(did >> 8), UInt8(did & 0xFF)])
        guard r.count >= 3, r[1] == UInt8(did >> 8), r[2] == UInt8(did & 0xFF) else { throw UDSError.malformed("0x22") }
        return Array(r[3...])
    }

    /// Whatever identification the module offers: [(label, text)].
    public func identify() async throws -> [(String, String)] {
        var out: [(String, String)] = []
        for (did, label) in VW.identification {
            do {
                out.append((label, VW.text(try await readDID(did))))
            } catch UDSError.negative {
                continue
            } catch UDSError.noResponse {
                break
            }
        }
        return out
    }

    public func faultCodes(mask: UInt8 = 0xFF) async throws -> [FaultCode] {
        let r = try await request([0x19, 0x02, mask])
        guard r.count >= 3, r[1] == 0x02 else { throw UDSError.malformed("0x19 02") }
        let body = Array(r[3...])
        return stride(from: 0, to: body.count - 3, by: 4).map { FaultCode(bytes: Array(body[$0..<($0 + 3)]), status: body[$0 + 3]) }
    }

    /// Freeze-frame values stored with a code (UDS snapshot record), decoded where the identifier is an OBD one.
    public func snapshot(_ code: FaultCode) async throws -> [(String, Double, String)] {
        let r = try await request([0x19, 0x04] + code.bytes + [0xFF])
        guard r.count >= 6 else { throw UDSError.malformed("0x19 04") }
        var out: [(String, Double, String)] = []
        var i = 6
        while i + 2 <= r.count {
            let count = Int(r[i + 1])
            i += 2
            for _ in 0..<count {
                guard i + 2 <= r.count else { break }
                let did = UInt16(r[i]) << 8 | UInt16(r[i + 1])
                guard did >> 8 == 0xF4, let pid = OBD.byPID[UInt8(did & 0xFF)], i + 2 + pid.size <= r.count else { return out }
                out.append((pid.name, pid.decode(Array(r[(i + 2)..<(i + 2 + pid.size)])), pid.unit))
                i += 2 + pid.size
            }
        }
        return out
    }

    /// Erase this module's fault codes. Changes the car.
    public func clearFaultCodes() async throws { _ = try await request([0x14, 0xFF, 0xFF, 0xFF]) }
}
