import Foundation

/// Packet framing of the link protocol (hardware/pico2w/docs/link_protocol.md):
/// `COBS(payload + CRC-16 big-endian) 00`. Works the same over USB and Bluetooth LE.
public enum Framing {
    public static let maxPayload = 4160

    /// CRC-16/CCITT-FALSE: polynomial 0x1021, start 0xFFFF, no reflection. "123456789" gives 0x29B1.
    public static func crc16<C: Collection>(_ data: C) -> UInt16 where C.Element == UInt8 {
        var crc: UInt16 = 0xFFFF
        for b in data {
            crc ^= UInt16(b) << 8
            for _ in 0..<8 {
                crc = (crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1
            }
        }
        return crc
    }

    /// One packet on the wire, ending with its zero byte.
    public static func encode(_ payload: [UInt8]) -> [UInt8] {
        let crc = crc16(payload)
        let raw = payload + [UInt8(crc >> 8), UInt8(crc & 0xFF)]
        var out: [UInt8] = []
        out.reserveCapacity(raw.count + raw.count / 254 + 3)
        var block: [UInt8] = []
        for b in raw {
            if b == 0 {
                out.append(UInt8(block.count + 1))
                out += block
                block.removeAll(keepingCapacity: true)
            } else {
                block.append(b)
                if block.count == 254 {
                    out.append(0xFF)
                    out += block
                    block.removeAll(keepingCapacity: true)
                }
            }
        }
        out.append(UInt8(block.count + 1))
        out += block
        out.append(0)
        return out
    }

    /// The payload of one packet (without its trailing zero), or nil if it is damaged.
    public static func decode(_ packet: ArraySlice<UInt8>) -> [UInt8]? {
        var out: [UInt8] = []
        out.reserveCapacity(packet.count)
        var i = packet.startIndex
        while i < packet.endIndex {
            let code = Int(packet[i])
            if code == 0 || i + code > packet.endIndex { return nil }
            out += packet[(i + 1)..<(i + code)]
            i += code
            if code != 0xFF && i < packet.endIndex { out.append(0) }
        }
        guard out.count >= 3 else { return nil }
        let body = out[..<(out.count - 2)]
        let crc = UInt16(out[out.count - 2]) << 8 | UInt16(out[out.count - 1])
        return crc16(body) == crc ? Array(body) : nil
    }
}

/// Reassembles packets from a byte stream that arrives in pieces of any size
/// (several BLE notifications per packet, or several packets per read).
public struct PacketDecoder {
    private var buffer: [UInt8] = []
    public private(set) var damaged = 0

    public init() {}

    /// Add received bytes; returns every complete, intact payload.
    public mutating func feed<C: Collection>(_ bytes: C) -> [[UInt8]] where C.Element == UInt8 {
        var out: [[UInt8]] = []
        for b in bytes {
            if b != 0 {
                buffer.append(b)
                if buffer.count > Framing.maxPayload * 2 { buffer.removeAll() }   // runaway: resynchronise
                continue
            }
            defer { buffer.removeAll(keepingCapacity: true) }
            if buffer.isEmpty { continue }   // empty packets are ignored
            if let p = Framing.decode(buffer[...]), p.count >= 2 {
                out.append(p)
            } else {
                damaged += 1
            }
        }
        return out
    }
}
