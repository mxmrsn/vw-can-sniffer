import SwiftUI
import VWDiagKit

/// Recording on the interface's microSD card: start, stop, marks, and copying files off the card.
/// Recordings made by this app (live data CSVs) are shared from the Live tab.
struct LogsView: View {
    @Environment(AppModel.self) private var model
    @State private var log: LogState?
    @State private var mark = ""
    @State private var pollMisfireSet = true
    @State private var files: [Interface.CardFile] = []
    @State private var copying: String?
    @State private var progress = 0.0
    @State private var copied: [String: URL] = [:]
    @State private var copyTask: Task<Void, Never>?

    /// Copied logs live in the app's Documents folder (visible in the Files app).
    static var folder: URL {
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Card logs")
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    static let pollSet = ["rpm", "load", "speed", "stft", "ltft", "timing", "coolant", "map"]

    var body: some View {
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
                    if files.isEmpty { Text("No files, or not listed yet").foregroundStyle(.secondary) }
                    ForEach(files) { f in fileRow(f) }
                    Button("List files") { Task { await listFiles() } }
                } header: {
                    Text("Files on the card")
                } footer: {
                    Text("Copies go to this app's folder in the Files app. Over Bluetooth expect a few kB a second: a minute of a busy bus takes a while. The file being recorded can only be copied once recording stops.")
                }
            }
            .navigationTitle("Card")
            .task {
                // keep the counters current while the card is recording
                while !Task.isCancelled {
                    await refresh()
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                }
            }
    }

    @ViewBuilder
    private func fileRow(_ f: Interface.CardFile) -> some View {
        let busy = log?.active == true && log?.name.caseInsensitiveCompare(f.name) == .orderedSame
        HStack {
            VStack(alignment: .leading) {
                Text(f.name).font(.body.monospaced())
                Text(ByteCountFormatter.string(fromByteCount: Int64(f.size), countStyle: .file) + (busy ? ", recording" : ""))
                    .font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if copying == f.name {
                ProgressView(value: progress).frame(width: 80)
                Button("Stop") { copyTask?.cancel() }.buttonStyle(.borderless)
            } else if let url = copied[f.name] {
                ShareLink(item: url) { Image(systemName: "square.and.arrow.up") }
            } else {
                Button("Copy") { copyTask = Task { await copy(f) } }
                    .buttonStyle(.borderless)
                    .disabled(busy || copying != nil)
            }
        }
    }

    private func listFiles() async {
        guard let ifc = model.interface else { return }
        do {
            files = try await ifc.cardFiles().sorted { $0.name < $1.name }
            for f in files {
                let url = Self.folder.appendingPathComponent(f.name)
                if let size = try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize, size == f.size { copied[f.name] = url }
            }
        } catch InterfaceError.refused(_, .storage) {
            files = []
        } catch { model.fail(error) }
    }

    private func copy(_ f: Interface.CardFile) async {
        guard let ifc = model.interface else { return }
        copying = f.name
        progress = 0
        defer { copying = nil }
        do {
            let data = try await ifc.copyCardFile(f.name) { done, total in
                Task { @MainActor in progress = total > 0 ? Double(done) / Double(total) : 0 }
            }
            let url = Self.folder.appendingPathComponent(f.name)
            try data.write(to: url)
            copied[f.name] = url
        } catch is CancellationError {
        } catch { model.fail(error) }
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
