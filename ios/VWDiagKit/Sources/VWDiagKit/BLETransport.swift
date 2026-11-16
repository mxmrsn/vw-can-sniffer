import CoreBluetooth
import Foundation

/// The board over Bluetooth LE: the Nordic UART Service layout (link_protocol.md, "Bluetooth LE").
/// Packets go to the RX characteristic as writes without response, in pieces that fit the
/// negotiated size, and come back as notifications on TX. Never yet run against a board.
public final class BLETransport: NSObject, ByteTransport, @unchecked Sendable {
    public static let service = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    static let rxChar = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")   // app writes
    static let txChar = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")   // interface notifies

    /// An interface seen while scanning.
    public struct Found: Sendable, Identifiable, Hashable {
        public let id: UUID
        public let name: String
        public let rssi: Int
    }

    private let queue = DispatchQueue(label: "vwdiag.ble")
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rx: CBCharacteristic?
    private var seen: [UUID: CBPeripheral] = [:]
    private var continuation: AsyncStream<[UInt8]>.Continuation?
    private var connectWaiter: CheckedContinuation<Void, Error>?
    private var poweredOn: [CheckedContinuation<Void, Error>] = []
    private var writeWaiters: [CheckedContinuation<Void, Never>] = []
    public let incoming: AsyncStream<[UInt8]>
    /// Interfaces found by scan(), newest state each time.
    public var onFound: (@Sendable (Found) -> Void)?

    public override init() {
        var cont: AsyncStream<[UInt8]>.Continuation!
        incoming = AsyncStream { cont = $0 }
        super.init()
        continuation = cont
        central = CBCentralManager(delegate: self, queue: queue)
    }

    private func waitPoweredOn() async throws {
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            queue.async {
                switch self.central.state {
                case .poweredOn: c.resume()
                case .unauthorized, .unsupported: c.resume(throwing: BLEError.unavailable(self.central.state))
                default: self.poweredOn.append(c)
                }
            }
        }
    }

    /// Look for interfaces advertising the service; results go to onFound.
    public func scan() async throws {
        try await waitPoweredOn()
        queue.async { self.central.scanForPeripherals(withServices: [Self.service], options: nil) }
    }

    public func stopScan() {
        queue.async { self.central.stopScan() }
    }

    /// Connect to a found interface and subscribe. iOS asks the user to pair the first time
    /// (the passkey is shown by the board's `ble` console command).
    public func connect(_ id: UUID, timeout: Double = 15) async throws {
        try await waitPoweredOn()
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            queue.async {
                guard let p = self.seen[id] ?? self.central.retrievePeripherals(withIdentifiers: [id]).first else {
                    c.resume(throwing: BLEError.notFound)
                    return
                }
                self.central.stopScan()
                self.peripheral = p
                p.delegate = self
                self.connectWaiter = c
                self.central.connect(p, options: nil)
                self.queue.asyncAfter(deadline: .now() + timeout) {
                    if let w = self.connectWaiter {
                        self.connectWaiter = nil
                        self.central.cancelPeripheralConnection(p)
                        w.resume(throwing: BLEError.timeout)
                    }
                }
            }
        }
    }

    private func finishConnect(_ error: Error?) {
        guard let w = connectWaiter else { return }
        connectWaiter = nil
        if let error { w.resume(throwing: error) } else { w.resume() }
    }

    public func send(_ bytes: [UInt8]) async throws {
        var offset = 0
        while offset < bytes.count {
            let (p, rx) = queue.sync { (self.peripheral, self.rx) }
            guard let p, let rx else { throw InterfaceError.disconnected }
            if !p.canSendWriteWithoutResponse {
                await withCheckedContinuation { c in queue.async { self.writeWaiters.append(c) } }
                continue
            }
            let piece = max(20, p.maximumWriteValueLength(for: .withoutResponse))
            let end = min(bytes.count, offset + piece)
            let chunk = Data(bytes[offset..<end])
            queue.sync { p.writeValue(chunk, for: rx, type: .withoutResponse) }
            offset = end
        }
    }

    public func close() {
        queue.async {
            if let p = self.peripheral { self.central.cancelPeripheralConnection(p) }
            self.peripheral = nil
            self.rx = nil
            self.continuation?.finish()
        }
    }
}

public enum BLEError: Error, LocalizedError {
    case unavailable(CBManagerState), notFound, timeout, noService

    public var errorDescription: String? {
        switch self {
        case let .unavailable(s): return s == .unauthorized ? "Bluetooth permission is off for this app" : "Bluetooth is not available"
        case .notFound: return "that interface is no longer in range"
        case .timeout: return "the interface did not finish connecting"
        case .noService: return "the device does not offer the interface's service"
        }
    }
}

extension BLETransport: CBCentralManagerDelegate, CBPeripheralDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        let waiting = poweredOn
        poweredOn.removeAll()
        for c in waiting {
            if central.state == .poweredOn { c.resume() } else { c.resume(throwing: BLEError.unavailable(central.state)) }
        }
    }

    public func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        seen[peripheral.identifier] = peripheral
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "VW-Diag"
        onFound?(Found(id: peripheral.identifier, name: name, rssi: RSSI.intValue))
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([Self.service])
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        finishConnect(error ?? BLEError.timeout)
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        finishConnect(error ?? InterfaceError.disconnected)
        rx = nil
        continuation?.finish()
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = peripheral.services?.first(where: { $0.uuid == Self.service }) else {
            finishConnect(error ?? BLEError.noService)
            return
        }
        peripheral.discoverCharacteristics([Self.rxChar, Self.txChar], for: s)
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        let chars = service.characteristics ?? []
        guard let rx = chars.first(where: { $0.uuid == Self.rxChar }), let tx = chars.first(where: { $0.uuid == Self.txChar }) else {
            finishConnect(error ?? BLEError.noService)
            return
        }
        self.rx = rx
        // Subscribing needs an encrypted, bonded link: this is where iOS asks to pair.
        peripheral.setNotifyValue(true, for: tx)
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        finishConnect(error)
    }

    public func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let v = characteristic.value, !v.isEmpty { continuation?.yield([UInt8](v)) }
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        let waiting = writeWaiters
        writeWaiters.removeAll()
        waiting.forEach { $0.resume() }
    }
}
