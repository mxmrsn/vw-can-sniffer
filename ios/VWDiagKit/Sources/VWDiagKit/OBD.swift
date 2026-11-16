import Foundation

/// A standard OBD-II live value (SAE J1979 service 01). Kept in step with tools/vwdiag/obd.py.
public struct PID: Sendable, Hashable, Identifiable {
    public let name: String
    public let pid: UInt8
    public let unit: String
    public let size: Int
    public let label: String
    let formula: @Sendable ([UInt8]) -> Double
    public var id: String { name }

    public func decode(_ d: [UInt8]) -> Double { formula(d) }

    public static func == (a: PID, b: PID) -> Bool { a.name == b.name }
    public func hash(into h: inout Hasher) { h.combine(name) }
}

public enum OBD {
    private static func ab(_ d: [UInt8]) -> Double { Double(Int(d[0]) * 256 + Int(d[1])) }

    public static let pids: [PID] = [
        PID(name: "rpm", pid: 0x0C, unit: "rpm", size: 2, label: "Engine speed") { ab($0) / 4 },
        PID(name: "speed", pid: 0x0D, unit: "km/h", size: 1, label: "Vehicle speed") { Double($0[0]) },
        PID(name: "load", pid: 0x04, unit: "%", size: 1, label: "Calculated load") { Double($0[0]) * 100 / 255 },
        PID(name: "coolant", pid: 0x05, unit: "°C", size: 1, label: "Coolant") { Double($0[0]) - 40 },
        PID(name: "stft", pid: 0x06, unit: "%", size: 1, label: "Short-term fuel trim") { (Double($0[0]) - 128) * 100 / 128 },
        PID(name: "ltft", pid: 0x07, unit: "%", size: 1, label: "Long-term fuel trim") { (Double($0[0]) - 128) * 100 / 128 },
        PID(name: "map", pid: 0x0B, unit: "kPa", size: 1, label: "Manifold pressure") { Double($0[0]) },
        PID(name: "timing", pid: 0x0E, unit: "°", size: 1, label: "Ignition timing") { Double($0[0]) / 2 - 64 },
        PID(name: "intake", pid: 0x0F, unit: "°C", size: 1, label: "Intake air") { Double($0[0]) - 40 },
        PID(name: "maf", pid: 0x10, unit: "g/s", size: 2, label: "Airflow") { ab($0) / 100 },
        PID(name: "throttle", pid: 0x11, unit: "%", size: 1, label: "Throttle") { Double($0[0]) * 100 / 255 },
        PID(name: "runtime", pid: 0x1F, unit: "s", size: 2, label: "Run time") { ab($0) },
        PID(name: "rail", pid: 0x23, unit: "kPa", size: 2, label: "Fuel rail pressure") { ab($0) * 10 },
        PID(name: "fuel", pid: 0x2F, unit: "%", size: 1, label: "Fuel level") { Double($0[0]) * 100 / 255 },
        PID(name: "baro", pid: 0x33, unit: "kPa", size: 1, label: "Barometric pressure") { Double($0[0]) },
        PID(name: "lambda", pid: 0x34, unit: "λ", size: 4, label: "Lambda, measured") { ab($0) / 32768 },
        PID(name: "voltage", pid: 0x42, unit: "V", size: 2, label: "Module voltage") { ab($0) / 1000 },
        PID(name: "lambda_cmd", pid: 0x44, unit: "λ", size: 2, label: "Lambda, commanded") { ab($0) / 32768 },
        PID(name: "ambient", pid: 0x46, unit: "°C", size: 1, label: "Ambient air") { Double($0[0]) - 40 },
        PID(name: "oil", pid: 0x5C, unit: "°C", size: 1, label: "Oil") { Double($0[0]) - 40 },
    ]
    public static let byName: [String: PID] = Dictionary(uniqueKeysWithValues: pids.map { ($0.name, $0) })
    public static let byPID: [UInt8: PID] = Dictionary(uniqueKeysWithValues: pids.map { ($0.pid, $0) })

    public static func read(_ c: UDSClient, _ p: PID) async throws -> Double {
        let r = try await c.request([0x01, p.pid])
        guard r.count >= 2 + p.size, r[1] == p.pid else { throw UDSError.malformed("PID \(p.pid)") }
        return p.decode(Array(r[2..<(2 + p.size)]))
    }

    /// Which service-01 PIDs the engine implements.
    public static func supported(_ c: UDSClient) async throws -> [UInt8] {
        var found: [UInt8] = []
        var base: UInt8 = 0
        while true {
            let r: [UInt8]
            do { r = try await c.request([0x01, base]) } catch UDSError.negative { break } catch UDSError.noResponse { break }
            guard r.count >= 6 else { break }
            let bits = UInt32(r[2]) << 24 | UInt32(r[3]) << 16 | UInt32(r[4]) << 8 | UInt32(r[5])
            for i in 0..<32 where bits & (1 << (31 - i)) != 0 { found.append(base + UInt8(i) + 1) }
            if bits & 1 == 0 || base >= 0xC0 { break }
            base += 0x20
        }
        return found.filter { $0 % 0x20 != 0 }
    }

    /// Warning lamp and stored-code count (PID 01).
    public static func monitorStatus(_ c: UDSClient) async throws -> (lamp: Bool, count: Int) {
        let r = try await c.request([0x01, 0x01])
        guard r.count >= 3 else { throw UDSError.malformed("PID 01") }
        return (r[2] & 0x80 != 0, Int(r[2] & 0x7F))
    }

    /// Stored (service 03) or pending (service 07) emission codes, e.g. "P0303".
    public static func emissionCodes(_ c: UDSClient, pending: Bool = false) async throws -> [String] {
        let r = try await c.request([pending ? 0x07 : 0x03])
        guard r.count >= 2 else { throw UDSError.malformed("service 03/07") }
        let n = Int(r[1])
        return (0..<n).compactMap { i in 3 + 2 * i < r.count ? FaultCode.sae(r[2 + 2 * i], r[3 + 2 * i]) : nil }
    }

    public static let freezeNames = ["rpm", "speed", "load", "coolant", "intake", "stft", "ltft", "map", "maf", "timing", "throttle", "runtime"]

    /// Freeze frame 0 (service 02): the code that caused it and the values stored with it.
    public static func freezeFrame(_ c: UDSClient) async throws -> (code: String?, values: [(PID, Double)]) {
        let r = try await c.request([0x02, 0x02, 0x00])
        guard r.count >= 5 else { throw UDSError.malformed("service 02") }
        if r[3] == 0 && r[4] == 0 { return (nil, []) }
        var values: [(PID, Double)] = []
        for name in freezeNames {
            guard let p = byName[name] else { continue }
            guard let v = try? await c.request([0x02, p.pid, 0x00]), v.count >= 3 + p.size else { continue }
            values.append((p, p.decode(Array(v[3..<(3 + p.size)]))))
        }
        return (FaultCode.sae(r[3], r[4]), values)
    }

    /// Misfire monitor (service 06): per cylinder, this driving cycle and the average of the last ten.
    /// Index 0 is the whole engine.
    public static func misfireCounts(_ c: UDSClient, cylinders: Int = 4) async throws -> [(thisCycle: Int?, average: Int?)] {
        var out: [(Int?, Int?)] = []
        for cyl in 0...cylinders {
            let r = try await c.request([0x06, 0xA1 + UInt8(cyl)])
            var now: Int?, avg: Int?
            var i = 2
            while i + 8 <= r.count {
                let v = Int(r[i + 2]) << 8 | Int(r[i + 3])
                if r[i] == 0x0C { now = v }
                if r[i] == 0x0B { avg = v }
                i += 8
            }
            out.append((now, avg))
        }
        return out
    }
}
