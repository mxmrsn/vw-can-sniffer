import SwiftUI
import VWDiagKit

/// Recording on the interface's microSD card: start, stop, marks. Recordings made by this app
/// (live data CSVs) are shared from the Live tab.
struct LogsView: View {
    @Environment(AppModel.self) private var model
    @State private var log: LogState?
    @State private var mark = ""
    @State private var pollMisfireSet = true

    static let pollSet = ["rpm", "load", "speed", "stft", "ltft", "timing", "coolant", "map"]

    var body: some View {
        NavigationStack {
            Form {
                Section("Card") {
                    if let log {
                        LabeledContent("Recording", value: log.active ? "on" : "off")
                        if !log.name.isEmpty { LabeledContent("File", value: log.name) }
                        LabeledContent("Frames", value: "\(log.frames)")
                        LabeledContent("Size", value: ByteCountFormatter.string(fromByteCount: Int64(log.bytes), countStyle: .file))
                        if log.lost > 0 { LabeledContent("Lost", value: "\(log.lost) frames") }
                    } else {
                        Text("No card, or not read yet").foregroundStyle(.secondary)
                    }
                    Button("Refresh") { Task { await refresh() } }
                }
                Section {
                    Toggle("Also poll the misfire set", isOn: $pollMisfireSet)
                    if log?.active == true {
                        Button("Stop recording", role: .destructive) { Task { await setLogging(false) } }
                    } else {
                        Button("Start recording") { Task { await setLogging(true) } }
                    }
                } footer: {
                    Text("The card keeps every frame the interface sees, and keeps recording after the app disconnects. Polling asks the engine for \(Self.pollSet.joined(separator: ", ")) every half second so the answers land in the log.")
                }
                if log?.active == true {
                    Section("Mark a moment") {
                        TextField("e.g. stumble at idle", text: $mark)
                        Button("Add mark") { Task { await addMark() } }
                    }
                }
                Section {
                    Text("Copying log files off the card over Bluetooth is not in the protocol yet: take the card out and read it with  vwdiag logread.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Logs")
            .task {
                // keep the counters current while the card is recording
                while !Task.isCancelled {
                    await refresh()
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                }
            }
        }
    }

    private func refresh() async {
        guard let ifc = model.interface else { return }
        do { log = try await ifc.logState() } catch InterfaceError.refused(_, .storage) { log = nil } catch { model.fail(error) }
    }

    private func setLogging(_ on: Bool) async {
        guard let ifc = model.interface else { return }
        do {
            if on {
                try await ifc.setTime()
                try await ifc.pollClear()
                if pollMisfireSet {
                    if !model.transmitAllowed { await model.setTransmit(true) }
                    let e = VW.engine
                    for n in Self.pollSet {
                        if let p = OBD.byName[n] { try await ifc.pollAdd(request: e.request, response: e.response, periodMs: 500, [0x01, p.pid]) }
                    }
                }
            } else {
                try await ifc.pollClear()
            }
            log = try await ifc.setLogging(on)
        } catch { model.fail(error) }
    }

    private func addMark() async {
        guard let ifc = model.interface else { return }
        do {
            try await ifc.logMark(mark.isEmpty ? "mark" : mark)
            mark = ""
            await refresh()
        } catch { model.fail(error) }
    }
}
