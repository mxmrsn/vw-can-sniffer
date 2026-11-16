import Foundation

/// The logging experiments of docs/logging_experiments.md, run from the phone. Each run writes the same
/// folder as `vwdiag experiment` (meta.json, summary.json, sample CSVs, notes.txt), so `vwdiag calibrate`
/// reads results from either. Everything here only reads from the car.
public struct ExperimentInfo: Sendable, Identifiable, Hashable {
    public let name: String
    public let title: String
    public let carState: String
    public let transmits: Bool
    public let defaultSeconds: Double
    public var id: String { name }
}

public enum Experiments {
    public static let all: [ExperimentInfo] = [
        ExperimentInfo(name: "listen", title: "Is there broadcast traffic at the OBD port?",
                       carState: "Ignition off, then on (you are asked)", transmits: false, defaultSeconds: 60),
        ExperimentInfo(name: "identify", title: "Who answers, how fast, and what the engine supports",
                       carState: "Ignition on, engine off", transmits: true, defaultSeconds: 0),
        ExperimentInfo(name: "timing", title: "How fast the engine answers: how fast logging can go",
                       carState: "Ignition on", transmits: true, defaultSeconds: 0),
        ExperimentInfo(name: "cold-start", title: "Cold soak to warm: idle, spark, trims, misfires",
                       carState: "After 6 hours or more parked; start and idle", transmits: true, defaultSeconds: 900),
        ExperimentInfo(name: "warm-idle", title: "Warm idle reference: values, trims, misfires per cylinder",
                       carState: "Fully warm, idling, A/C and lights off", transmits: true, defaultSeconds: 300),
        ExperimentInfo(name: "rev-holds", title: "Misfires and trims at steady engine speeds",
                       carState: "Warm, parked in P; hold each speed when asked", transmits: true, defaultSeconds: 60),
        ExperimentInfo(name: "drive", title: "A drive recorded on the card, with marks",
                       carState: "Driving; marks from a passenger", transmits: true, defaultSeconds: 0),
    ]
    public static let byName: [String: ExperimentInfo] = Dictionary(uniqueKeysWithValues: all.map { ($0.name, $0) })

    public static let idleSet = ["rpm", "load", "coolant", "intake", "maf", "map", "stft", "ltft", "timing", "lambda", "voltage", "runtime"]
    public static let vwSet = ["knock_cyl_1", "knock_cyl_2", "knock_cyl_3", "knock_cyl_4", "ign_avg", "misfires"]
}

public struct ExperimentSettings: Sendable {
    public var seconds: Double?
    public var interval: Double = 0.5
    public var holds: [Int] = [0, 1500, 2500, 3500]
    public var sessionTest = false
    public var target = "car"            // or "simulator"
    public var scenario: String?         // the simulator's scenario, when target is the simulator
    public var speed: Double = 1         // a simulator's engine clock (speed=)

    public init() {}
}

/// How a run talks to the person: prompts wait for them, notes and progress are shown, and a drive
/// ends when they say so.
public struct ExperimentHooks: Sendable {
    public var prompt: @Sendable (String) async -> Void
    public var note: @Sendable (String) -> Void
    public var progress: @Sendable (Double) -> Void
    public var stopRequested: @Sendable () -> Bool
    public var takeMarks: @Sendable () -> [String]

    public init(prompt: @escaping @Sendable (String) async -> Void = { _ in },
                note: @escaping @Sendable (String) -> Void = { _ in },
                progress: @escaping @Sendable (Double) -> Void = { _ in },
                stopRequested: @escaping @Sendable () -> Bool = { false },
                takeMarks: @escaping @Sendable () -> [String] = { [] }) {
        self.prompt = prompt
        self.note = note
        self.progress = progress
        self.stopRequested = stopRequested
        self.takeMarks = takeMarks
    }
}

typealias Row = [String: Double]

/// JSON null for a missing value.
func orNull(_ x: Any?) -> Any { x ?? NSNull() }

/// Summary statistics, rounded as the tools round them.
func stats(_ values: [Double?]) -> [String: Any]? {
    let v = values.compactMap { $0 }.sorted()
    guard !v.isEmpty else { return nil }
    let mean = v.reduce(0, +) / Double(v.count)
    let sd = (v.map { ($0 - mean) * ($0 - mean) }.reduce(0, +) / Double(v.count)).squareRoot()
    let r = { (x: Double) in (x * 100).rounded() / 100 }
    return ["n": v.count, "mean": r(mean), "sd": r(sd), "min": r(v[0]), "p10": r(v[Int(0.1 * Double(v.count - 1))]),
            "p50": r(v[v.count / 2]), "p90": r(v[Int(0.9 * Double(v.count - 1))]), "max": r(v[v.count - 1])]
}

public final class ExperimentRunner: @unchecked Sendable {
    let interface: Interface
    let info: ExperimentInfo
    var settings: ExperimentSettings
    let hooks: ExperimentHooks
    public let folder: URL
    let engine: UDSClient
    private var notes: [String] = []
    private var meta: [String: Any] = [:]

    public init(interface: Interface, experiment: ExperimentInfo, settings: ExperimentSettings, root: URL, hooks: ExperimentHooks) {
        self.interface = interface
        self.info = experiment
        self.settings = settings
        self.hooks = hooks
        if self.settings.seconds == nil { self.settings.seconds = experiment.defaultSeconds }
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyyMMdd-HHmmss"
        folder = root.appendingPathComponent(f.string(from: Date()) + "-" + experiment.name)
        engine = UDSClient(interface, VW.engine)
    }

    static func stamp() -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f.string(from: Date())
    }

    func note(_ s: String) {
        notes.append(s)
        hooks.note(s)
    }

    /// Run the experiment and save its folder. Returns the summary (or ["error": ...]).
    @discardableResult
    public func run() async -> [String: Any] {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        meta = ["experiment": info.name, "started": Self.stamp(), "target": settings.target,
                "scenario": orNull(settings.scenario), "app": "VWDiag iOS",
                "settings": ["seconds": settings.seconds ?? 0, "interval": settings.interval,
                             "holds": settings.holds.map(String.init).joined(separator: ",")]]
        note("experiment \(info.name): \(info.title)")
        var summary: [String: Any]
        do {
            switch info.name {
            case "listen": summary = try await listen()
            case "identify": summary = try await identify()
            case "timing": summary = try await timing()
            case "cold-start": summary = try await coldStart()
            case "warm-idle": summary = try await warmIdle()
            case "rev-holds": summary = try await revHolds()
            case "drive": summary = try await drive()
            default: summary = ["error": "unknown experiment"]
            }
        } catch {
            summary = ["error": error.localizedDescription]
            note("stopped: \(error.localizedDescription)")
        }
        try? await interface.setListenOnly(true)
        meta["finished"] = Self.stamp()
        write("meta.json", meta)
        write("summary.json", summary)
        try? (notes.joined(separator: "\n") + "\n").write(to: folder.appendingPathComponent("notes.txt"), atomically: true, encoding: .utf8)
        note("saved in \(folder.lastPathComponent)")
        return summary
    }

    func write(_ name: String, _ object: [String: Any]) {
        if let data = try? JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: folder.appendingPathComponent(name))
        }
    }

    // MARK: reading
    func read(_ name: String) async -> Double? {
        do {
            if let p = OBD.byName[name] { return try await retry { try await OBD.read(self.engine, p) } }
            if let v = Simos18.byKey[name] { return ((try await Simos18.read(engine, v)) * 1000).rounded() / 1000 }
        } catch {}
        return nil
    }

    func retry<T>(_ tries: Int = 2, _ f: () async throws -> T) async throws -> T {
        var last: Error = UDSError.noResponse
        for _ in 0..<tries {
            do { return try await f() } catch let e as UDSError {
                last = e
                if case let .negative(nrc) = e, ![0x21, 0x22, 0x78].contains(nrc) { throw e }
                try? await Task.sleep(nanoseconds: 200_000_000)
            }
        }
        throw last
    }

    func available(_ names: [String]) async -> [String] {
        var out: [String] = []
        for n in names where await read(n) != nil { out.append(n) }
        return out
    }

    func misfires() async -> [Int]? {
        guard let c = try? await OBD.misfireCounts(engine) else { return nil }
        return (1...4).map { c[$0].thisCycle ?? 0 }
    }

    /// Sample `names` every `interval` s for `seconds`, per-cylinder misfire counts every 10 s of
    /// engine time. Writes a CSV and returns the rows.
    func sample(_ names: [String], seconds: Double, file: String) async -> [Row] {
        let header = ["t"] + names + ["mis_c1", "mis_c2", "mis_c3", "mis_c4"]
        var csv = header.joined(separator: ",") + "\n"
        var rows: [Row] = []
        let start = Date()
        var nextMis = 0.0
        let misEvery = 10.0 / max(settings.speed, 1e-6)
        while Date().timeIntervalSince(start) < seconds, !hooks.stopRequested(), !Task.isCancelled {
            let t0 = Date().timeIntervalSince(start)
            var row: Row = ["t": (t0 * 100).rounded() / 100]
            for n in names { if let v = await read(n) { row[n] = v } }
            if t0 >= nextMis, let m = await misfires() {
                for i in 0..<4 { row["mis_c\(i + 1)"] = Double(m[i]) }
                nextMis = t0 + misEvery
            }
            rows.append(row)
            csv += header.map { k in row[k].map { fmtCSV($0) } ?? "" }.joined(separator: ",") + "\n"
            try? csv.write(to: folder.appendingPathComponent(file), atomically: true, encoding: .utf8)
            hooks.progress(min(1, Date().timeIntervalSince(start) / seconds))
            let wait = settings.interval - (Date().timeIntervalSince(start) - t0)
            if wait > 0 { try? await Task.sleep(nanoseconds: UInt64(wait * 1e9)) }
        }
        return rows
    }

    func fmtCSV(_ v: Double) -> String { v == v.rounded() && abs(v) < 1e15 ? String(Int64(v)) : String(v) }

    // MARK: summaries (the same as tools/vwdiag/experiments.py)
    func misfireRates(_ rows: [Row]) -> [String: Any]? {
        let withMis = rows.filter { $0["mis_c1"] != nil }
        guard withMis.count >= 2, let a = withMis.first, let b = withMis.last else { return nil }
        let rpms = rows.compactMap { $0["rpm"] }.filter { $0 > 0 }
        let rpm = rpms.isEmpty ? 0 : rpms.reduce(0, +) / Double(rpms.count)
        var seconds: Double, clock: String
        if let ra = a["runtime"], let rb = b["runtime"], rb > ra { seconds = rb - ra; clock = "engine" } else { seconds = (b["t"] ?? 0) - (a["t"] ?? 0); clock = "wall" }
        let firings = rpm / 120 * seconds
        var out: [String: Any] = ["seconds": (seconds * 10).rounded() / 10, "clock": clock, "mean_rpm": (rpm * 10).rounded() / 10,
                                  "firings_per_cylinder": firings.rounded()]
        for c in 1...4 {
            let d = Int((b["mis_c\(c)"] ?? 0) - (a["mis_c\(c)"] ?? 0))
            let pct: Any = firings > 0 && d >= 0 ? ((100 * Double(d) / firings) * 1000).rounded() / 1000 : NSNull()
            out["cyl\(c)"] = ["misfires": d, "pct": pct]
        }
        return out
    }

    func valueStats(_ rows: [Row], _ names: [String]) -> [String: Any] {
        var out: [String: Any] = [:]
        for n in names { out[n] = orNull(stats(rows.map { $0[n] })) }
        return out
    }

    func totalTrim(_ rows: [Row]) -> [String: Any]? {
        stats(rows.compactMap { r in r["stft"].flatMap { s in r["ltft"].map { s + $0 } } })
    }

    static func worst(_ rates: [String: Any]?) -> String {
        guard let rates else { return "-" }
        let c = (1...4).max { ((rates["cyl\($0)"] as? [String: Any])?["misfires"] as? Int ?? 0) < ((rates["cyl\($1)"] as? [String: Any])?["misfires"] as? Int ?? 0) } ?? 1
        let pct = (rates["cyl\(c)"] as? [String: Any])?["pct"] ?? "-"
        return "cyl \(c) (\(pct) %)"
    }

    // MARK: frames
    /// Monitored frames for a while, with the interface's timestamps.
    func collectFrames(_ seconds: Double, during work: (() async -> Void)? = nil) async -> [CANFrame] {
        await interface.startCollecting()
        if let work { await work() }
        try? await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        // a status request flushes any events still on their way in front of its answer
        _ = try? await interface.state()
        return await interface.stopCollecting()
    }

    // MARK: the experiments
    func listen() async throws -> [String: Any] {
        try await interface.setListenOnly(true)
        var out: [String: Any] = [:]
        let half = (settings.seconds ?? 60) / 2
        for (phase, what) in [("off", "Ignition OFF (key out, doors shut a minute)."), ("on", "Ignition ON, engine OFF.")] {
            await hooks.prompt(what)
            try await interface.setMonitor(true)
            let frames = await collectFrames(half)
            try await interface.setMonitor(false)
            var csv = "t,id,ext,data\n"
            let t0 = frames.first?.timeMs ?? 0
            var byID: [String: [Double]] = [:]
            for f in frames {
                let t = Double(f.timeMs &- t0) / 1000
                let id = String(format: "%X", f.id)
                csv += String(format: "%.4f", t) + ",\(id),\(f.extended ? 1 : 0)," + f.data.map { String(format: "%02x", $0) }.joined() + "\n"
                byID[id, default: []].append(t)
            }
            try? csv.write(to: folder.appendingPathComponent("frames_\(phase).csv"), atomically: true, encoding: .utf8)
            var rates: [String: Any] = [:]
            for (id, ts) in byID {
                let periods = zip(ts, ts.dropFirst()).map { 1000 * ($1 - $0) }
                rates[id] = ["count": ts.count, "hz": (Double(ts.count) / max(half, 1e-6) * 100).rounded() / 100,
                             "period_ms": orNull(stats(periods))]
            }
            out[phase] = ["frames": frames.count, "ids": rates]
            note("ignition \(phase): \(frames.count) frames, \(byID.count) IDs")
        }
        let visible = ((out["on"] as? [String: Any])?["frames"] as? Int ?? 0) > 0
        out["broadcast_visible"] = visible
        note("broadcast traffic at the OBD port: " + (visible ? "yes" : "none (behind the gateway)"))
        return out
    }

    func identify() async throws -> [String: Any] {
        await hooks.prompt("Ignition ON, engine OFF.")
        try await interface.setListenOnly(false)
        var modules: [String: Any] = [:]
        for (i, m) in VW.modules.enumerated() {
            hooks.progress(Double(i) / Double(VW.modules.count))
            let quick = UDSClient(interface, m, timeoutMs: 300)
            do {
                try await retry(3) { try await quick.testerPresent() }
            } catch UDSError.negative {
            } catch {
                modules[m.key] = ["present": false]
                note("\(m.key) no answer")
                continue
            }
            var lat: [Double] = []
            for _ in 0..<20 {
                let t0 = Date()
                if (try? await quick.testerPresent()) != nil { lat.append(1000 * Date().timeIntervalSince(t0)) }
            }
            try await interface.setMonitor(true)
            let client = UDSClient(interface, m)
            let frames = await collectFrames(0.2) {
                _ = try? await client.request([0x22, 0xF1, 0x87, 0xF1, 0x89, 0xF1, 0x97, 0xF1, 0x90])
            }
            try await interface.setMonitor(false)
            let fcs = frames.filter { $0.id == m.response && ($0.data.first ?? 0) >> 4 == 3 && $0.data.count >= 3 }
            var ident: [String: String] = [:]
            for (k, v) in (try? await client.identify()) ?? [] { ident[k] = v }
            modules[m.key] = ["present": true, "latency_ms": orNull(stats(lat)),
                              "flow_control": fcs.prefix(3).map { ["bs": Int($0.data[1]), "stmin": Int($0.data[2])] },
                              "identification": ident]
            note("\(m.key) present, TesterPresent \((stats(lat)?["p50"]).map { "\($0)" } ?? "-") ms (p50)")
        }
        var out: [String: Any] = ["modules": modules]
        try await interface.setMonitor(true)
        let frames = await collectFrames(0.5) {
            _ = try? await self.interface.uds(request: 0x7DF, response: 0x7E8, [0x01, 0x00])
        }
        try await interface.setMonitor(false)
        let answering = Set(frames.filter { (0x7E8...0x7EF).contains($0.id) }.map { String(format: "%X", $0.id) }).sorted()
        out["functional_obd_answers"] = answering
        note("functional OBD request answered by: " + answering.joined(separator: ", "))
        out["obd_pids"] = ((try? await OBD.supported(engine)) ?? []).map { String(format: "%02X", $0) }
        var mids: [String] = []
        var base = 0
        while base < 0x100 {
            guard let r = try? await engine.request([0x06, UInt8(base)]), r.count >= 6 else { break }
            let bits = UInt32(r[2]) << 24 | UInt32(r[3]) << 16 | UInt32(r[4]) << 8 | UInt32(r[5])
            for i in 0..<32 where bits & (1 << (31 - i)) != 0 && (base + i + 1) % 0x20 != 0 { mids.append(String(format: "%02X", base + i + 1)) }
            if bits & 1 == 0 { break }
            base += 0x20
        }
        out["monitor_ids"] = mids
        var values: [String: Any] = [:]
        for v in Simos18.values {
            do {
                let d = try await engine.readDID(v.did)
                values[v.key] = ["did": String(format: "%04X", v.did), "answered": true, "bytes": d.count]
            } catch let UDSError.negative(nrc) {
                values[v.key] = ["did": String(format: "%04X", v.did), "answered": false, "nrc": String(format: "%02X", nrc)]
            } catch {
                values[v.key] = ["did": String(format: "%04X", v.did), "answered": false]
            }
        }
        out["simos18_values"] = values
        let answered = values.values.filter { (($0 as? [String: Any])?["answered"] as? Bool) == true }.count
        note("engine: \(mids.count) monitors, \(answered) of \(Simos18.values.count) Simos 18 values answered")
        if settings.sessionTest {
            var s3: [String: Any] = [:]
            for wait in [2, 4, 6, 8] {
                do {
                    _ = try await engine.request([0x10, 0x03])
                    try await Task.sleep(nanoseconds: UInt64(wait) * 1_000_000_000)
                    s3[String(wait)] = Int(try await engine.readDID(0xF186).first ?? 0)
                } catch { s3[String(wait)] = error.localizedDescription }
            }
            _ = try? await engine.request([0x10, 0x01])
            out["s3"] = s3
        }
        return out
    }

    func timing() async throws -> [String: Any] {
        try await interface.setListenOnly(false)
        var lat: [Double] = []
        for i in 0..<100 {
            hooks.progress(Double(i) / 130)
            let t0 = Date()
            if (try? await engine.request([0x01, 0x0C])) != nil { lat.append(1000 * Date().timeIntervalSince(t0)) }
        }
        var out: [String: Any] = ["single_pid_ms": orNull(stats(lat))]
        do {
            let r = try await engine.request([0x01, 0x0C, 0x0D, 0x05, 0x04, 0x06, 0x07])
            out["multi_pid"] = ["supported": true, "bytes": r.count]
        } catch {
            out["multi_pid"] = ["supported": false, "error": error.localizedDescription]
        }
        var did: [Double] = []
        for _ in 0..<30 {
            let t0 = Date()
            guard (try? await engine.readDID(0xF40C)) != nil else { break }
            did.append(1000 * Date().timeIntervalSince(t0))
        }
        out["did_read_ms"] = orNull(stats(did))
        note("engine answers a PID in \((stats(lat)?["p50"]).map { "\($0)" } ?? "-") ms (p50)")
        return out
    }

    func engineCapture(_ prompt: String) async throws -> ([Row], [String]) {
        try await interface.setListenOnly(false)
        let names = await available(Experiments.idleSet) + (await available(Experiments.vwSet))
        note("reading " + names.joined(separator: ", "))
        await hooks.prompt(prompt)
        return (await sample(names, seconds: settings.seconds ?? 300, file: "samples.csv"), names)
    }

    func coldStart() async throws -> [String: Any] {
        try await interface.setListenOnly(false)
        let before = await read("coolant")
        note("coolant before start \(before.map { fmtCSV($0) } ?? "-") C")
        let (rows, names) = try await engineCapture("Start the engine and leave it idling, hands off, for the whole run.")
        var out: [String: Any] = ["coolant_at_start": orNull(before), "rows": rows.count]
        var bands: [String: Double] = [:]
        for lo in stride(from: -10, to: 110, by: 10) {
            let v = rows.compactMap { r -> Double? in
                guard let c = r["coolant"], let rpm = r["rpm"], rpm > 0, c >= Double(lo), c < Double(lo + 10) else { return nil }
                return rpm
            }
            if !v.isEmpty { bands["\(lo)-\(lo + 10)"] = ((v.reduce(0, +) / Double(v.count)) * 10).rounded() / 10 }
        }
        out["idle_rpm_by_coolant"] = bands
        for target in [40.0, 60, 80] {
            let hit = rows.first { ($0["coolant"] ?? -99) >= target && $0["runtime"] != nil }?["runtime"]
            out["runtime_to_\(Int(target))C"] = orNull(hit)
        }
        out["timing_first_60s"] = orNull(stats(rows.filter { ($0["runtime"] ?? 999) <= 60 }.map { $0["timing"] }))
        out["closed_loop_after_s"] = orNull(rows.first { ($0["stft"] ?? 0) != 0 && $0["runtime"] != nil }?["runtime"])
        let cold = rows.filter { ($0["coolant"] ?? 999) < 60 }
        let warm = rows.filter { ($0["coolant"] ?? -99) >= 75 }
        out["trim_cold"] = orNull(totalTrim(cold.filter { ($0["stft"] ?? 0) != 0 }))
        out["trim_warm"] = orNull(totalTrim(warm))
        out["misfire_cold"] = orNull(misfireRates(cold))
        out["misfire_warm"] = orNull(misfireRates(warm))
        out["values"] = valueStats(rows, names)
        note("coolant reached 60 C after \(out["runtime_to_60C"] ?? "-") s, 80 C after \(out["runtime_to_80C"] ?? "-") s")
        return out
    }

    func warmIdle() async throws -> [String: Any] {
        let (rows, names) = try await engineCapture("Engine warm (coolant at 90 C or more), idling, A/C and lights off.")
        let mis = misfireRates(rows)
        let v = valueStats(rows, names)
        let rpm = (v["rpm"] as? [String: Any])?["mean"] ?? "-"
        note("warm idle: \(rpm) rpm, worst cylinder \(Self.worst(mis))")
        return ["rows": rows.count, "values": v, "trim_total": orNull(totalTrim(rows)), "misfire": orNull(mis)]
    }

    func revHolds() async throws -> [String: Any] {
        try await interface.setListenOnly(false)
        let names = await available(Experiments.idleSet)
        var holds: [String: Any] = [:]
        for rpm in settings.holds {
            await hooks.prompt(rpm == 0 ? "Let it idle." : "Hold \(rpm) rpm steadily (parked).")
            let rows = await sample(names, seconds: settings.seconds ?? 60, file: "hold_\(rpm).csv")
            let mis = misfireRates(rows)
            holds[String(rpm)] = ["values": valueStats(rows, names), "trim_total": orNull(totalTrim(rows)), "misfire": orNull(mis)]
            note("\(rpm) rpm: worst \(Self.worst(mis))")
        }
        return ["holds": holds]
    }

    func misfirePicture() async -> [String: Any] {
        var out: [String: Any] = [:]
        if let ms = try? await OBD.monitorStatus(engine) { out["lamp"] = ms.lamp; out["stored_count"] = ms.count }
        if let s = try? await OBD.emissionCodes(engine) { out["stored"] = s }
        if let p = try? await OBD.emissionCodes(engine, pending: true) { out["pending"] = p }
        if let ff = try? await OBD.freezeFrame(engine) {
            var values: [String: Double] = [:]
            for (p, v) in ff.values { values[p.name] = v }
            out["freeze_frame"] = ["code": orNull(ff.code), "values": values]
        }
        if let c = try? await OBD.misfireCounts(engine) {
            var m: [String: Any] = [:]
            for (i, x) in c.enumerated() { m[String(i)] = ["this_cycle": orNull(x.thisCycle), "avg_10_cycles": orNull(x.average)] }
            out["misfires"] = m
        }
        return out
    }

    func drive() async throws -> [String: Any] {
        try await interface.setListenOnly(false)
        let before = await misfirePicture()
        let e = VW.engine
        let poll = ["rpm", "load", "speed", "stft", "ltft", "timing", "coolant", "map"]
        try await interface.setTime()
        try await interface.pollClear()
        for n in poll { if let p = OBD.byName[n] { try await interface.pollAdd(request: e.request, response: e.response, periodMs: 250, [0x01, p.pid]) } }
        let started = try await interface.setLogging(true)
        note("logging to \(started.name), polling \(poll.joined(separator: ", ")) every 0.25 s")
        await hooks.prompt("Drive. Add marks as things happen; tap Finish when done.")
        var marks: [String] = []
        let start = Date()
        while !hooks.stopRequested(), !Task.isCancelled {
            if let s = settings.seconds, s > 0, Date().timeIntervalSince(start) >= s { break }
            for m in hooks.takeMarks() {
                try? await interface.logMark(m)
                marks.append(m)
            }
            try? await Task.sleep(nanoseconds: 500_000_000)
        }
        try await interface.pollClear()
        let state = try await interface.setLogging(false)
        note("card log \(state.name): \(state.frames) frames, \(state.lost) lost")
        return ["log": ["active": state.active, "name": state.name, "frames": state.frames, "bytes": state.bytes, "lost": state.lost],
                "marks": marks, "misfire_before": before, "misfire_after": await misfirePicture()]
    }
}
