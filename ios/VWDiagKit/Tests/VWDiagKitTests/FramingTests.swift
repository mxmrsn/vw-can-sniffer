import XCTest
@testable import VWDiagKit

final class FramingTests: XCTestCase {
    func testCRCCheckValue() {
        XCTAssertEqual(Framing.crc16(Array("123456789".utf8)), 0x29B1)
    }

    func testWorkedExampleFromTheProtocolDocument() {
        // link_protocol.md: read the VIN, request number 2
        let payload: [UInt8] = [0x02, 0x05, 0xE0, 0x07, 0, 0, 0xE8, 0x07, 0, 0, 0, 0, 0x22, 0xF1, 0x90]
        XCTAssertEqual(Framing.encode(payload),
                       [0x05, 0x02, 0x05, 0xE0, 0x07, 0x01, 0x03, 0xE8, 0x07, 0x01, 0x01, 0x01, 0x06, 0x22, 0xF1, 0x90, 0xAD, 0x12, 0x00])
        XCTAssertEqual(Framing.encode([0x01, 0x01]), [0x05, 0x01, 0x01, 0x3E, 0x1F, 0x00])
    }

    func testRoundTripsIncludingLongRunsAndZeros() {
        for n in [1, 2, 253, 254, 255, 600, 4160] {
            let p = (0..<n).map { UInt8(($0 * 37 + 11) % 256) }
            var d = PacketDecoder()
            XCTAssertEqual(d.feed(Framing.encode(p)), n >= 2 ? [p] : [], "length \(n)")
        }
    }

    func testPiecesOfAnySizeAndDamage() {
        let a: [UInt8] = [0x01, 0x81, 0x00, 0x01]
        let b: [UInt8] = [0x00, 0x01, 0x10, 0x20]
        var wire = Framing.encode(a) + [0x00] + Framing.encode(b)
        var corrupted = Framing.encode(a)
        corrupted[2] ^= 0x40
        wire = corrupted + wire
        for piece in [1, 3, 7, 20, 182] {
            var d = PacketDecoder()
            var got: [[UInt8]] = []
            var i = 0
            while i < wire.count {
                got += d.feed(wire[i..<min(wire.count, i + piece)])
                i += piece
            }
            XCTAssertEqual(got, [a, b], "pieces of \(piece)")
            XCTAssertEqual(d.damaged, 1)
        }
    }

    func testFaultCodeText() {
        XCTAssertEqual(FaultCode(bytes: [0x03, 0x03, 0x00], status: 0xA8).code, "P0303-00")
        XCTAssertEqual(FaultCode(bytes: [0xC1, 0x21, 0x00], status: 0x08).code, "U0121-00")
        XCTAssertEqual(FaultCode(bytes: [0x03, 0x03, 0x00], status: 0xA8).statusText,
                       ["confirmed", "failed since clear", "warning lamp requested"])
    }

    func testDescriptionsMatchTheTools() {
        XCTAssertEqual(FaultCode.describe("P0303"), "Cylinder 3 Misfire Detected")
        XCTAssertEqual(FaultCode.describe("P0299"), "Turbocharger/Supercharger \"A\" Underboost Condition")
        XCTAssertEqual(FaultCode.describe("P130A"), "Manufacturer-specific powertrain code: VW's own description is not public")
        XCTAssertEqual(FaultCode.describe("P03FF"), "Ignition system or misfire code, not in this list")
        XCTAssertEqual(FaultCode(bytes: [0xC1, 0x21, 0x00], status: 8).description,
                       "Lost Communication With Anti-Lock Brake System (ABS) Control Module")
    }

    func testOBDFormulas() {
        XCTAssertEqual(OBD.byName["rpm"]!.decode([0x0A, 0xF0]), 700)
        XCTAssertEqual(OBD.byName["stft"]!.decode([0x8A]), 7.8125)
        XCTAssertEqual(OBD.byName["coolant"]!.decode([130]), 90)
        XCTAssertEqual(OBD.byName["lambda"]!.decode([0x80, 0x00, 0x80, 0x00]), 1.0)
    }
}
