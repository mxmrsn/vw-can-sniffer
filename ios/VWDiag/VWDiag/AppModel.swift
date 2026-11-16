import Foundation
import Observation
import VWDiagKit

/// Everything the screens share: the connection to the interface and what has been read from the car.
@MainActor @Observable
final class AppModel {
    enum Link: Equatable { case none, connecting, connected(String) }

    // connection
    var link: Link = .none
    var interface: Interface?
    var info: InterfaceInfo?
    var state: InterfaceState?
    var power: Interface.PowerState?   // nil on boards that do not sleep
    var gps: Interface.GPSState?       // nil on boards without the GPS connector
    var simulatorHost = UserDefaults.standard.string(forKey: "simHost") ?? "127.0.0.1"
    var simulatorPort = UserDefaults.standard.string(forKey: "simPort") ?? "5555"
    var found: [BLETransport.Found] = []
    private var ble: BLETransport?

    // results
    var scan: [ModuleScan] = []
    var scanning = false
    var message: String?

    struct ModuleScan: Identifiable {
        let module: Module
        var present: Bool
        var partNumber: String?
        var faultCount: Int?
        var id: String { module.key }
    }

    var connected: Bool { if case .connected = link { return true } else { return false } }
    var transmitAllowed: Bool { state.map { !$0.listenOnly } ?? false }

    // MARK: connecting
    func connectSimulator() async {
        UserDefaults.standard.set(simulatorHost, forKey: "simHost")
        UserDefaults.standard.set(simulatorPort, forKey: "simPort")
        guard let port = UInt16(simulatorPort) else { message = "port must be a number"; return }
        link = .connecting
        let t = TCPTransport(host: simulatorHost, port: port)
        do {
            try await t.connect()
            try await attach(Interface(transport: t), label: "Simulator \(simulatorHost):\(port)")
        } catch {
            t.close()
            link = .none
            message = "Could not reach the simulator: \(error.localizedDescription)"
        }
    }

    func scanBluetooth() async {
        let t = ble ?? BLETransport()
        ble = t
        found = []
        t.onFound = { [weak self] f in
            Task { @MainActor in
                guard let self else { return }
                if let i = self.found.firstIndex(where: { $0.id == f.id }) { self.found[i] = f } else { self.found.append(f) }
            }
        }
        do { try await t.scan() } catch { message = error.localizedDescription }
    }

    func connectBluetooth(_ f: BLETransport.Found) async {
        guard let t = ble else { return }
        link = .connecting
        do {
            try await t.connect(f.id)
            try await attach(Interface(transport: t), label: f.name)
        } catch {
            link = .none
            message = "Could not connect: \(error.localizedDescription)"
        }
    }

    private func attach(_ ifc: Interface, label: String) async throws {
        info = try await ifc.open()
        interface = ifc
        link = .connected(label)
        await refresh()
    }

    func disconnect() async {
        await interface?.close()
        interface = nil
        ble = nil
        info = nil
        state = nil
        scan = []
        link = .none
    }

    func refresh() async {
        guard let ifc = interface else { return }
        do {
            state = try await ifc.state()
            power = try await ifc.power()
            gps = try await ifc.gps()
        } catch { fail(error) }
    }

    func setSync(_ hz: Int) async {
        guard let ifc = interface else { return }
        do { gps = try await ifc.gps(syncHz: hz) } catch { fail(error) }
    }

    /// The interface powers up listen-only; reading from modules needs sending allowed.
    func setTransmit(_ on: Bool) async {
        guard let ifc = interface else { return }
        do {
            try await ifc.setListenOnly(!on)
            await refresh()
        } catch { fail(error) }
    }

    func changePower(_ change: Interface.PowerChange) async {
        guard let ifc = interface else { return }
        do { power = try await ifc.power(change) } catch { fail(error) }
    }

    func fail(_ error: Error) {
        message = error.localizedDescription
        if case InterfaceError.disconnected = error {
            interface = nil
            link = .none
        }
    }

    // MARK: autoscan
    func autoscan() async {
        guard let ifc = interface else { return }
        scanning = true
        defer { scanning = false }
        if !transmitAllowed { await setTransmit(true) }
        scan = VW.modules.map { ModuleScan(module: $0, present: false) }
        for (i, m) in VW.modules.enumerated() {
            let c = UDSClient(ifc, m, timeoutMs: 300)
            do {
                try await c.testerPresent()
                scan[i].present = true
            } catch UDSError.negative {
                scan[i].present = true
            } catch UDSError.noResponse {
                continue
            } catch {
                fail(error)
                return
            }
            let full = UDSClient(ifc, m)
            if let pn = try? await full.readDID(0xF187) { scan[i].partNumber = VW.text(pn) }
            if let codes = try? await full.faultCodes() { scan[i].faultCount = codes.count }
        }
    }

    func client(_ m: Module) -> UDSClient? { interface.map { UDSClient($0, m) } }
}
