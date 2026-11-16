// Engine measuring values for the Simos 18 engine module (EA888 Gen 3), generated from
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
    public static let values: [SimosValue] = [
        SimosValue(key: "airflow", name: "Airflow", did: 0x2032, length: 2, signed: false, unit: "kg/hr", scale: 1, offset: 0),
        SimosValue(key: "ambient_press", name: "Ambient Press", did: 0x13CA, length: 2, signed: false, unit: "kpa", scale: 0.008291752498665402, offset: 0.0),
        SimosValue(key: "ambient_temp", name: "Ambient Temp", did: 0x1004, length: 2, signed: true, unit: "°C", scale: 0.0078125, offset: 0.0),
        SimosValue(key: "battery_voltage", name: "Battery Voltage", did: 0x14A6, length: 1, signed: false, unit: "V", scale: 0.1015625, offset: 0.0),
        SimosValue(key: "coolant_temp", name: "Coolant Temp", did: 0x11CD, length: 1, signed: false, unit: "°C", scale: 1, offset: -40),
        SimosValue(key: "cooling_fan", name: "Cooling Fan", did: 0x101E, length: 1, signed: false, unit: "%", scale: 0.390625, offset: 0.0),
        SimosValue(key: "cpu_load", name: "CPU Load", did: 0x14EC, length: 2, signed: false, unit: "%", scale: 0.09765625, offset: 0.0),
        SimosValue(key: "cruise", name: "Cruise", did: 0x203C, length: 2, signed: false, unit: "", scale: 1, offset: 0),
        SimosValue(key: "engine_speed", name: "Engine Speed", did: 0xF40C, length: 2, signed: false, unit: "rpm", scale: 0.25, offset: 0.0),
        SimosValue(key: "eth_content", name: "Eth Content", did: 0xF452, length: 1, signed: false, unit: "%", scale: 0.3921568627450981, offset: 0.0),
        SimosValue(key: "exhaust_cam_pos", name: "Exhaust Cam Pos", did: 0x201A, length: 2, signed: true, unit: "°", scale: 0.10000000000000142, offset: -24.0),
        SimosValue(key: "fp_di", name: "FP DI", did: 0x2027, length: 2, signed: false, unit: "bar", scale: 0.1, offset: 0.0),
        SimosValue(key: "fp_di_sp", name: "FP DI SP", did: 0x293B, length: 2, signed: false, unit: "bar", scale: 0.1, offset: 0.0),
        SimosValue(key: "fp_mpi", name: "FP MPI", did: 0x2025, length: 2, signed: false, unit: "bar", scale: 0.001, offset: 0.0),
        SimosValue(key: "fp_mpi_sp", name: "FP MPI SP", did: 0x2932, length: 2, signed: false, unit: "bar", scale: 0.001, offset: 0.0),
        SimosValue(key: "gear", name: "Gear", did: 0x210F, length: 2, signed: false, unit: "", scale: 1, offset: 0),
        SimosValue(key: "hpfp_eff_vol", name: "HPFP Eff Vol", did: 0x209A, length: 2, signed: false, unit: "%", scale: 0.01, offset: 0.0),
        SimosValue(key: "iat", name: "IAT", did: 0x1001, length: 1, signed: false, unit: "°C", scale: 0.75, offset: -48.0),
        SimosValue(key: "ign_avg", name: "Ign Avg", did: 0x2004, length: 2, signed: true, unit: "°", scale: 0.01, offset: 0.0),
        SimosValue(key: "inj_pw_di", name: "Inj PW DI", did: 0x13A0, length: 2, signed: false, unit: "ms", scale: 0.004, offset: 0.0),
        SimosValue(key: "inj_pw_mpi", name: "Inj PW MPI", did: 0x13AC, length: 2, signed: false, unit: "ms", scale: 0.004, offset: 0.0),
        SimosValue(key: "int_cam_pos", name: "Int Cam Pos", did: 0x201E, length: 2, signed: true, unit: "°", scale: -0.10000000000000142, offset: 30.0),
        SimosValue(key: "knock_cyl_1", name: "Knock Cyl 1", did: 0x200A, length: 2, signed: true, unit: "°", scale: 0.01, offset: 0.0),
        SimosValue(key: "knock_cyl_2", name: "Knock Cyl 2", did: 0x200B, length: 2, signed: true, unit: "°", scale: 0.01, offset: 0.0),
        SimosValue(key: "knock_cyl_3", name: "Knock Cyl 3", did: 0x200C, length: 2, signed: true, unit: "°", scale: 0.01, offset: 0.0),
        SimosValue(key: "knock_cyl_4", name: "Knock Cyl 4", did: 0x200D, length: 2, signed: true, unit: "°", scale: 0.01, offset: 0.0),
        SimosValue(key: "lambda", name: "Lambda", did: 0x10C0, length: 2, signed: false, unit: "l", scale: 0.0009765625, offset: 0.0),
        SimosValue(key: "lambda_sp", name: "Lambda SP", did: 0xF444, length: 2, signed: false, unit: "l", scale: 3.0517578125e-05, offset: 0.0),
        SimosValue(key: "lpfp_duty", name: "LPFP Duty", did: 0x2028, length: 2, signed: false, unit: "%", scale: 0.01, offset: 0.0),
        SimosValue(key: "ltft", name: "LTFT", did: 0xF407, length: 1, signed: false, unit: "%", scale: 0.78125, offset: -100.0),
        SimosValue(key: "map", name: "MAP", did: 0x39C0, length: 2, signed: false, unit: "kpa", scale: 0.1, offset: 0.0),
        SimosValue(key: "map_sp", name: "MAP SP", did: 0x39C1, length: 2, signed: false, unit: "kpa", scale: 0.1, offset: 0.0),
        SimosValue(key: "misfires", name: "Misfires", did: 0x2904, length: 2, signed: false, unit: "", scale: 1, offset: 0),
        SimosValue(key: "oil_temp", name: "Oil Temp", did: 0x202F, length: 2, signed: false, unit: "°C", scale: 0.0999999999999659, offset: -273.14),
        SimosValue(key: "pedal_pos", name: "Pedal Pos", did: 0x1070, length: 2, signed: false, unit: "%", scale: 0.09765625, offset: 0.0),
        SimosValue(key: "port_flap_pos", name: "Port Flap Pos", did: 0x295C, length: 1, signed: false, unit: "", scale: 1, offset: 0),
        SimosValue(key: "put", name: "PUT", did: 0x202A, length: 2, signed: false, unit: "kpa", scale: 0.1, offset: 0.0),
        SimosValue(key: "put_sp", name: "PUT SP", did: 0x2029, length: 2, signed: false, unit: "kpa", scale: 0.1, offset: 0.0),
        SimosValue(key: "stft", name: "STFT", did: 0xF406, length: 1, signed: false, unit: "%", scale: 0.78125, offset: -100.0),
        SimosValue(key: "torque", name: "Torque", did: 0x437C, length: 2, signed: true, unit: "Nm", scale: 0.1, offset: 0.0),
        SimosValue(key: "torque_req", name: "Torque Req", did: 0x4380, length: 2, signed: true, unit: "Nm", scale: 0.1, offset: 0.0),
        SimosValue(key: "tps", name: "TPS", did: 0x20BA, length: 2, signed: true, unit: "%", scale: 0.1, offset: 0.0),
        SimosValue(key: "turbo_air_temp", name: "Turbo Air Temp", did: 0x1041, length: 2, signed: true, unit: "°C", scale: 0.005859375, offset: 144.0),
        SimosValue(key: "turbo_speed", name: "Turbo Speed", did: 0x1040, length: 2, signed: false, unit: "rpm", scale: 0.006103515625, offset: 0.0),
        SimosValue(key: "valve_lift_pos", name: "Valve Lift Pos", did: 0x167C, length: 1, signed: false, unit: "", scale: 1, offset: 0),
        SimosValue(key: "vehicle_speed", name: "Vehicle Speed", did: 0x2033, length: 2, signed: false, unit: "km/hr", scale: 0.01, offset: 0.0),
        SimosValue(key: "wastegate", name: "Wastegate", did: 0x39A2, length: 2, signed: false, unit: "%", scale: -0.010000000000005116, offset: 100.0),
        SimosValue(key: "wastegate_sp", name: "Wastegate SP", did: 0x39A3, length: 2, signed: false, unit: "%", scale: -0.010000000000005116, offset: 100.0),
    ]
    public static let byKey: [String: SimosValue] = Dictionary(uniqueKeysWithValues: values.map { ($0.key, $0) })

    /// Read one value from the engine module.
    public static func read(_ c: UDSClient, _ v: SimosValue) async throws -> Double {
        let data = try await c.readDID(v.did)
        guard let x = v.decode(data) else { throw UDSError.malformed("short answer for \(v.key)") }
        return x
    }
}
