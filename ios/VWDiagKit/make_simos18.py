"""Regenerate Sources/VWDiagKit/Simos18.swift from the tools' Simos 18 list.

    python3 ios/VWDiagKit/make_simos18.py        (from the repository root)

Every formula in the list is linear (value = raw * scale + offset); this checks that before writing,
because the Swift side relies on it.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from vwdiag import simos18  # noqa: E402

HEAD = '''// Engine measuring values for the Simos 18 engine module (EA888 Gen 3), generated from
// tools/vwdiag/data/simos18_parameters_22.csv (VW_Flash, BSD 2-clause; see tools/vwdiag/data/LICENSE.VW_Flash)
// by ios/VWDiagKit/make_simos18.py. Every formula in that list is linear: value = raw * scale + offset.
// Not yet confirmed on this car.

public struct SimosValue: Sendable, Hashable, Identifiable {
    public let key: String
    public let name: String
    public let did: UInt16
    public let length: Int
    public let signed: Bool
    public let unit: String
    public let scale: Double
    public let offset: Double
    public var id: String { key }

    public func decode(_ data: [UInt8]) -> Double? {
        guard data.count >= length else { return nil }
        var raw: Int64 = 0
        for b in data.prefix(length) { raw = raw << 8 | Int64(b) }
        if signed && length < 8 && raw & (1 << (Int64(length) * 8 - 1)) != 0 { raw -= 1 << (Int64(length) * 8) }
        return Double(raw) * scale + offset
    }
}

public enum Simos18 {
    public static let values: [SimosValue] = ['''

TAIL = '''    ]
    public static let byKey: [String: SimosValue] = Dictionary(uniqueKeysWithValues: values.map { ($0.key, $0) })

    /// Read one value from the engine module.
    public static func read(_ c: UDSClient, _ v: SimosValue) async throws -> Double {
        let data = try await c.readDID(v.did)
        guard let x = v.decode(data) else { throw UDSError.malformed("short answer for \\(v.key)") }
        return x
    }
}
'''


def main():
    rows = []
    for key, (did, length, signed, unit, decode, name) in sorted(simos18.VALUES.items()):
        offset = decode(0)
        scale = decode(1) - offset
        for x in (7, 300, 4000):
            if abs(decode(x) - (scale * x + offset)) > 1e-6 * max(1.0, abs(decode(x))):
                raise SystemExit("%s: formula is not linear" % key)
        rows.append('        SimosValue(key: "%s", name: "%s", did: 0x%04X, length: %d, signed: %s, unit: "%s", '
                    'scale: %r, offset: %r),' % (key, name, did, length, "true" if signed else "false",
                                                 unit.replace('"', '\\"'), scale, offset))
    out = os.path.join(HERE, "Sources", "VWDiagKit", "Simos18.swift")
    with open(out, "w") as f:
        f.write("\n".join([HEAD] + rows) + "\n" + TAIL)
    print("wrote %d values to %s" % (len(rows), out))


if __name__ == "__main__":
    main()
