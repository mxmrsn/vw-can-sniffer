import SwiftUI
import VWDiagKit

struct ConnectView: View {
    @Environment(AppModel.self) private var model
    @State private var useBluetooth = false

    var body: some View {
        @Bindable var model = model
        NavigationStack {
            Form {
                if model.connected, let info = model.info {
                    connectedSection(info)
                } else {
                    Picker("Connect to", selection: $useBluetooth) {
                        Text("Simulator").tag(false)
                        Text("Bluetooth").tag(true)
                    }
                    .pickerStyle(.segmented)
                    if useBluetooth {
                        bluetoothSection
                    } else {
                        Section {
                            TextField("Host", text: $model.simulatorHost)
                                .textInputAutocapitalization(.never).autocorrectionDisabled()
                            TextField("Port", text: $model.simulatorPort).keyboardType(.numberPad)
                            Button("Connect") { Task { await model.connectSimulator() } }
                                .disabled(model.link == .connecting)
                        } header: {
                            Text("Simulator on your Mac")
                        } footer: {
                            Text("Start it with  VWDIAG_BLE_PORT=5555 hardware/pico2w/firmware/sim/vw_diag_sim  (add VWSIM=gti for the misfire scenario). From a phone, use the Mac's address instead of 127.0.0.1.")
                        }
                    }
                    if model.link == .connecting { ProgressView("Connecting…") }
                }
            }
            .navigationTitle("Interface")
        }
    }

    private var bluetoothSection: some View {
        Section {
            Button("Look for interfaces") { Task { await model.scanBluetooth() } }
            ForEach(model.found) { f in
                Button {
                    Task { await model.connectBluetooth(f) }
                } label: {
                    HStack {
                        Text(f.name)
                        Spacer()
                        Text("\(f.rssi) dBm").foregroundStyle(.secondary)
                    }
                }
            }
        } footer: {
            Text("The first time, iOS asks for the six-digit passkey: the board's console shows it (command ble). New phones can pair for two minutes after power-up or after pressing MODE.")
        }
    }

    @ViewBuilder
    private func powerSection(_ p: Interface.PowerState) -> some View {
        Section {
            LabeledContent("Woke by", value: p.wake)
            LabeledContent("Battery", value: String(format: "%.2f V", Double(p.batteryMv) / 1000))
            LabeledContent("Kept awake by", value: p.holds.isEmpty ? "nothing" : p.holds.joined(separator: ", "))
            if p.sleepsInS > 0 { LabeledContent("Sleeps in", value: "\(p.sleepsInS) s") }
            if p.batteryLowForS > 0 { LabeledContent("Battery low for", value: "\(p.batteryLowForS) s") }
            Toggle("Stay awake", isOn: Binding(get: { p.stayAwake }, set: { on in Task { await model.changePower(.stayAwake(on)) } }))
            Stepper(value: Binding(get: { p.idleS / 60 }, set: { m in Task { await model.changePower(.idle(seconds: max(1, m) * 60)) } }),
                    in: 1...60) {
                LabeledContent("Sleep after", value: "\(p.idleS / 60) min idle")
            }
            Button("Sleep now") { Task { await model.changePower(.sleepNow) } }
        } header: {
            Text("Power")
        } footer: {
            Text("The dongle switches itself off when nothing needs it (no phone, no requests, engine off, no bus traffic), and always when the battery stays below \(String(format: "%.1f", Double(p.cutoffMv) / 1000)) V with the engine off. It wakes when the engine starts, on bus traffic, or on its button. Asleep, the phone cannot see it.")
        }
    }

    @ViewBuilder
    private func connectedSection(_ info: InterfaceInfo) -> some View {
        Section("Connected") {
            LabeledContent("Interface", value: info.name)
            if case let .connected(label) = model.link { LabeledContent("Via", value: label) }
            LabeledContent("Protocol", value: "version \(info.version)")
        }
        Section {
            Toggle("Allow sending to the car", isOn: Binding(get: { model.transmitAllowed },
                                                              set: { on in Task { await model.setTransmit(on) } }))
        } footer: {
            Text("The interface starts listen-only. Reading modules needs sending allowed; nothing here writes to the car except Clear fault codes, which asks first.")
        }
        if let s = model.state {
            Section("Counters") {
                LabeledContent("Frames received", value: "\(s.framesReceived)")
                LabeledContent("Frames sent", value: "\(s.framesSent)")
                LabeledContent("Receive drops", value: "\(s.receiveDrops)")
                LabeledContent("Bus errors", value: "\(s.busErrors)")
                LabeledContent("Damaged packets", value: "\(s.damagedPackets)")
                LabeledContent("Logging", value: s.logging ? "on" : "off")
                Button("Refresh") { Task { await model.refresh() } }
            }
        }
        if let p = model.power { powerSection(p) }
        Section {
            Button("Disconnect", role: .destructive) { Task { await model.disconnect() } }
        }
    }
}
