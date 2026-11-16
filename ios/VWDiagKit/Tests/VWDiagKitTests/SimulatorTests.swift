#if os(macOS)
import XCTest
@testable import VWDiagKit

/// End to end against the real simulator, started here (macOS only; skipped if it is not built).
final class SimulatorTests: XCTestCase {
    var proc: Process?

    func startSimulator(_ scenario: String, card: String = "", env: [String: String] = [:]) throws -> UInt16 {
        let here = URL(fileURLWithPath: #filePath)
        let repo = here.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
        let sim = repo.appendingPathComponent("hardware/pico2w/firmware/sim/vw_diag_sim")
        guard FileManager.default.isExecutableFile(atPath: sim.path) else {
            throw XCTSkip("simulator not built: make -C hardware/pico2w/firmware/sim")
        }
        let port = UInt16.random(in: 20000...40000)
        let p = Process()
        p.executableURL = sim
        p.environment = ["VWDIAG_BLE_PORT": String(port), "VWSIM": scenario, "VWDIAG_SD": card].merging(env) { $1 }
        p.standardInput = Pipe()
        p.standardOutput = FileHandle.nullDevice
        try p.run()
        proc = p
        return port
    }

    override func tearDown() {
        proc?.terminate()
    }

    func connect(_ port: UInt16) async throws -> Interface {
        for _ in 0..<50 {
            let t = TCPTransport(host: "127.0.0.1", port: port)
            do {
                try await t.connect(timeout: 1)
                let ifc = Interface(transport: t)
                _ = try await ifc.open()
                return ifc
            } catch {
                t.close()
                try await Task.sleep(nanoseconds: 100_000_000)
            }
        }
        throw InterfaceError.noAnswer
    }

    func testMisfireScenarioOverTCP() async throws {
        let ifc = try await connect(try startSimulator("gti"))
        try await ifc.setListenOnly(false)
        let engine = UDSClient(ifc, VW.engine)
        let ident = try await engine.identify()
        XCTAssertEqual(ident.first { $0.0 == "Engine code" }?.1, "DKFA")
        let rpm = try await OBD.read(engine, OBD.byName["rpm"]!)
        XCTAssertGreaterThan(rpm, 0)
        let ff = try await OBD.freezeFrame(engine)
        XCTAssertEqual(ff.code, "P0303")
        XCTAssertEqual(ff.values.first { $0.0.name == "coolant" }?.1, 41)
        try await Task.sleep(nanoseconds: 3_000_000_000)
        let counts = try await OBD.misfireCounts(engine)
        XCTAssertEqual(counts[3].average, 47)
        XCTAssertGreaterThan(counts[3].thisCycle ?? 0, 0)
        XCTAssertEqual(counts[1].thisCycle, 0)
        await ifc.close()
    }

    func testCopyLogsOffTheCard() async throws {
        let card = FileManager.default.temporaryDirectory.appendingPathComponent("vwdiag-card-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: card, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: card) }
        let big = Data((0..<9001).map { UInt8(($0 * 7919) % 251) })
        try big.write(to: card.appendingPathComponent("BIG.BIN"))
        let ifc = try await connect(try startSimulator("realistic,gateway=open", card: card.path))
        _ = try await ifc.setLogging(true)
        do {
            _ = try await ifc.readCardFile("LOG00001.VWL", offset: 0)
            XCTFail("the file being recorded must be refused")
        } catch InterfaceError.refused(_, .busy) {}
        try await Task.sleep(nanoseconds: 1_000_000_000)
        _ = try await ifc.setLogging(false)
        let files = try await ifc.cardFiles()
        XCTAssertEqual(Set(files.map(\.name)), ["BIG.BIN", "LOG00001.VWL"])
        let copied = try await ifc.copyCardFile("BIG.BIN")
        XCTAssertEqual(copied, big)
        let log = try await ifc.copyCardFile("LOG00001.VWL")
        XCTAssertEqual(log, try Data(contentsOf: card.appendingPathComponent("LOG00001.VWL")))
        await ifc.close()
    }

    func testExperimentsFeedTheToolsCalibration() async throws {
        // a stand-in car with known numbers; the phone's results must calibrate back to them
        let car = "realistic,gateway=closed,latency=10-30,fc=4/2,misfire=2,idle=680,mis_idle=1.2,trim_idle=4.5,history=0"
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("vwdiag-exp-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        var folders: [URL] = []
        for (name, scenario, seconds, speed) in [("identify", car, 0.0, 1.0), ("listen", car, 1.0, 1.0),
                                                 ("warm-idle", car + ",start=warm,speed=10", 20.0, 10.0)] {
            let ifc = try await connect(try startSimulator(scenario))
            var settings = ExperimentSettings()
            settings.seconds = seconds
            settings.interval = 0.3
            settings.target = "simulator"
            settings.scenario = scenario
            settings.speed = speed
            let runner = ExperimentRunner(interface: ifc, experiment: Experiments.byName[name]!, settings: settings,
                                          root: root, hooks: ExperimentHooks())
            let summary = await runner.run()
            XCTAssertNil(summary["error"], "\(name): \(summary["error"] ?? "")")
            folders.append(runner.folder)
            await ifc.close()
            proc?.terminate()
            proc?.waitUntilExit()
        }
        let ident = try JSONSerialization.jsonObject(with: Data(contentsOf: folders[0].appendingPathComponent("summary.json"))) as! [String: Any]
        let abs = (ident["modules"] as! [String: Any])["abs"] as! [String: Any]
        XCTAssertEqual((abs["flow_control"] as! [[String: Int]]).first, ["bs": 4, "stmin": 2])
        XCTAssertEqual(ident["functional_obd_answers"] as? [String], ["7E8", "7E9"])

        // the tools read the phone's folders unchanged
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        let py = Process()
        py.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        py.arguments = ["python3", "-m", "vwdiag", "calibrate"] + folders.map(\.path) + ["--out", root.appendingPathComponent("calib").path]
        py.currentDirectoryURL = repo.appendingPathComponent("tools")
        py.standardOutput = FileHandle.nullDevice
        try py.run()
        py.waitUntilExit()
        XCTAssertEqual(py.terminationStatus, 0)
        let scenario = try String(contentsOf: root.appendingPathComponent("calib/scenario.txt"), encoding: .utf8)
        for expected in ["gateway=closed", "fc=4/2", "trans_obd=1", "misfire=2"] {
            XCTAssertTrue(scenario.contains(expected), "\(expected) in \(scenario)")
        }
        let idle = scenario.split(separator: ",").first { $0.hasPrefix("idle=") }.flatMap { Double($0.dropFirst(5)) } ?? 0
        XCTAssertEqual(idle, 680, accuracy: 15)
    }

    func testEngineMeasuringValues() async throws {
        let ifc = try await connect(try startSimulator("gti"))
        try await ifc.setListenOnly(false)
        let engine = UDSClient(ifc, VW.engine)
        let rpm = try await Simos18.read(engine, Simos18.byKey["engine_speed"]!)
        XCTAssertGreaterThan(rpm, 600)
        let knock = try await Simos18.read(engine, Simos18.byKey["knock_cyl_3"]!)
        XCTAssertEqual(knock, 0)
        let ign = try await Simos18.read(engine, Simos18.byKey["ign_avg"]!)
        XCTAssertLessThan(ign, 0)            // a cold start retards the spark
        XCTAssertEqual(Simos18.byKey["oil_temp"]!.decode([0x0E, 0x2F])!, 89.96, accuracy: 0.001)   // (3631 - 2731.4) / 10
        await ifc.close()
    }

    func testDongleSleepsAndTheLinkDrops() async throws {
        let ifc = try await connect(try startSimulator("realistic,ignition=off:0", env: ["VWDIAG_USB": "0", "VWDIAG_WAKE": "button"]))
        try await Task.sleep(nanoseconds: 700_000_000)     // the power manager looks twice a second
        let p = try await ifc.power()
        XCTAssertEqual(p?.wake, "button")
        XCTAssertEqual(p?.holds, ["phone", "activity"])     // the app is connected: a phone
        XCTAssertEqual(Double(p?.batteryMv ?? 0), 12500, accuracy: 50)
        let q = try await ifc.power(.idle(seconds: 120))
        XCTAssertEqual(q?.idleS, 120)
        _ = try? await ifc.power(.sleepNow)
        try await Task.sleep(nanoseconds: 1_500_000_000)
        do {
            _ = try await ifc.state()
            XCTFail("a sleeping dongle does not answer")
        } catch InterfaceError.disconnected {}
        await ifc.close()
    }

    func testGPSAndCameraSync() async throws {
        let ifc = try await connect(try startSimulator("realistic", env: ["VWDIAG_GPS": "1", "VWDIAG_GPS_DRIFT": "-30"]))
        try await Task.sleep(nanoseconds: 3_300_000_000)
        let g = try await ifc.gps(syncHz: 10)
        XCTAssertEqual(g?.present, true)
        XCTAssertEqual(g?.fix, true)
        XCTAssertEqual(g?.pulseLocked, true)
        XCTAssertEqual(g?.logClockFromGPS, true)
        XCTAssertEqual(g?.satellites, 12)
        XCTAssertEqual(g?.latitude ?? 0, 52.4227, accuracy: 1e-5)
        XCTAssertEqual(g?.driftPpm ?? 0, -30, accuracy: 0.6)
        XCTAssertEqual(g?.syncHz, 10)
        try await Task.sleep(nanoseconds: 400_000_000)
        let h = try await ifc.gps(syncHz: 0)
        XCTAssertGreaterThanOrEqual(h?.syncEdges ?? 0, 3)
        XCTAssertEqual(h?.syncHz, 0)
        await ifc.close()
    }

    func testScanFindsSevenModules() async throws {
        let ifc = try await connect(try startSimulator("realistic"))
        try await ifc.setListenOnly(false)
        var present: [String] = []
        for m in VW.modules {
            do {
                try await UDSClient(ifc, m, timeoutMs: 300).testerPresent()
                present.append(m.key)
            } catch UDSError.noResponse {}
        }
        XCTAssertEqual(present, ["engine", "trans", "abs", "cluster", "airbag", "gateway", "steer"])
        await ifc.close()
    }
}
#endif
