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
        Section {
            Button("Disconnect", role: .destructive) { Task { await model.disconnect() } }
        }
    }
}
