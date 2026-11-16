import XCTest
@testable import VWDiagKit

/// Plays recorded simulator byte streams back to the protocol client, in random pieces,
/// answering whatever sequence number the client chose.
final class ReplayTransport: ByteTransport, @unchecked Sendable {
    struct Exchange: Decodable { let name: String; let request: String; let wire: String }
    private var script: [Exchange]
    private var continuation: AsyncStream<[UInt8]>.Continuation!
    let incoming: AsyncStream<[UInt8]>
    private var rng = SystemRandomNumberGenerator()
    var unmatched: [[UInt8]] = []

    init() throws {
        let url = Bundle.module.url(forResource: "simulator", withExtension: "json", subdirectory: "Fixtures")!
        script = try JSONDecoder().decode([Exchange].self, from: Data(contentsOf: url))
        var c: AsyncStream<[UInt8]>.Continuation!
        incoming = AsyncStream { c = $0 }
        continuation = c
    }

    func send(_ bytes: [UInt8]) async throws {
        var d = PacketDecoder()
        guard let p = d.feed(bytes).first else { return }
        let body = Array(p[1...])
        guard let i = script.firstIndex(where: { hex($0.request) == body }) else {
            unmatched.append(body)
            return
        }
        let e = script.remove(at: i)
        // re-number the recorded response for this request, keep everything else byte for byte
        var out: [UInt8] = []
        var r = PacketDecoder()
        for packet in r.feed(hex(e.wire)) {
            var q = packet
            if q[0] != 0 { q[0] = p[0] }
            out += Framing.encode(q)
        }
        var i2 = 0
        while i2 < out.count {
            let n = Int.random(in: 1...40, using: &rng)
            continuation.yield(Array(out[i2..<min(out.count, i2 + n)]))
            i2 += n
        }
    }

    func close() { continuation.finish() }
}

func hex(_ s: String) -> [UInt8] {
    var out: [UInt8] = []
    var i = s.startIndex
    while i < s.endIndex {
        let j = s.index(i, offsetBy: 2)
        out.append(UInt8(s[i..<j], radix: 16)!)
        i = j
    }
    return out
}

final class ReplayTests: XCTestCase {
    func testRecordedSession() async throws {
        let t = try ReplayTransport()
        let ifc = Interface(transport: t)
        let info = try await ifc.open()
        XCTAssertEqual(info.version, 1)
        XCTAssertEqual(info.maxPayload, 4160)

        var state = try await ifc.state()
        XCTAssertTrue(state.listenOnly)

        let engine = UDSClient(ifc, VW.engine)
        do {
            try await engine.testerPresent()
            XCTFail("listen-only must refuse")
        } catch UDSError.listenOnly {}

        try await ifc.setListenOnly(false)
        let vin = try await engine.readDID(0xF190)
        XCTAssertEqual(vin, Array("WVWZZZAUZJW123456".utf8))
        let engineCode = try await engine.readDID(0xF1AD)
        XCTAssertEqual(VW.text(engineCode), "DKFA")
        do {
            _ = try await engine.readDID(0x1234)
            XCTFail()
        } catch let UDSError.negative(nrc) { XCTAssertEqual(nrc, 0x31) }
        do {
            try await UDSClient(ifc, VW.modules.first { $0.key == "radar" }!, timeoutMs: 200).testerPresent()
            XCTFail()
        } catch UDSError.noResponse {}

        let codes = try await engine.faultCodes()
        XCTAssertEqual(codes.map(\.code), ["P0299-00", "P130A-00", "P0303-00"])
        let snap = try await engine.snapshot(codes[2])
        XCTAssertEqual(snap.first?.0, "rpm")
        XCTAssertEqual(snap.first?.1, 812)

        let supported = try await OBD.supported(engine)
        XCTAssertTrue(supported.contains(0x0C))
        let ms = try await OBD.monitorStatus(engine)
        XCTAssertTrue(ms.lamp)
        XCTAssertEqual(ms.count, 3)
        let stored = try await OBD.emissionCodes(engine)
        XCTAssertEqual(stored, ["P0299", "P130A", "P0303"])
        let ff = try await engine.request([0x02, 0x02, 0x00])
        XCTAssertEqual(FaultCode.sae(ff[3], ff[4]), "P0303")
        let mis = try await engine.request([0x06, 0xA4])
        XCTAssertEqual(Int(mis[12]) << 8 | Int(mis[13]), 0)       // this cycle, just started
        XCTAssertEqual(Int(mis[4]) << 8 | Int(mis[5]), 47)        // ten-cycle average

        // monitoring: events arrive between requests and the reply still matches
        try await ifc.setMonitor(true)
        state = try await ifc.state()
        XCTAssertTrue(state.monitor)
        try await ifc.setMonitor(false)
        var frames: [CANFrame] = []
        for await f in ifc.frames {
            frames.append(f)
            if frames.count == 3 { break }
        }
        XCTAssertEqual(frames.count, 3)

        _ = try? await ifc.logState()   // no card in the recording: either answer is fine here
        XCTAssertEqual(t.unmatched, [])
        await ifc.close()
    }
}
