#if os(macOS)
import XCTest
@testable import VWDiagKit

/// End to end against the real simulator, started here (macOS only; skipped if it is not built).
final class SimulatorTests: XCTestCase {
    var proc: Process?

    func startSimulator(_ scenario: String, card: String = "") throws -> UInt16 {
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
        p.environment = ["VWDIAG_BLE_PORT": String(port), "VWSIM": scenario, "VWDIAG_SD": card]
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
