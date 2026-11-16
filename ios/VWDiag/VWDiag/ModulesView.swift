import SwiftUI
import VWDiagKit

/// Autoscan: every known module, present or not, with part number and fault count.
struct AutoscanView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        NavigationStack {
            List {
                if model.scan.isEmpty && !model.scanning {
                    ContentUnavailableView("Not scanned yet", systemImage: "magnifyingglass",
                                           description: Text("Scan asks every known control module whether it is there."))
                }
                ForEach(model.scan) { s in
                    if s.present {
                        NavigationLink(value: s.module) { row(s) }
                    } else {
                        row(s).foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Modules")
            .navigationDestination(for: Module.self) { ModuleView(module: $0) }
            .toolbar {
                Button(model.scanning ? "Scanning…" : "Scan") { Task { await model.autoscan() } }
                    .disabled(model.scanning)
            }
        }
    }

    private func row(_ s: AppModel.ModuleScan) -> some View {
        HStack {
            VStack(alignment: .leading) {
                Text(s.module.title)
                Text(s.present ? (s.partNumber ?? "present") : "no answer").font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if let n = s.faultCount {
                Text(n == 0 ? "OK" : "\(n) fault\(n == 1 ? "" : "s")")
                    .font(.caption.bold())
                    .padding(.horizontal, 8).padding(.vertical, 3)
                    .background(n == 0 ? Color.green.opacity(0.2) : Color.orange.opacity(0.25), in: Capsule())
            }
        }
    }
}

/// One module: identification and fault codes, with each code's freeze frame.
struct ModuleView: View {
    @Environment(AppModel.self) private var model
    let module: Module
    @State private var ident: [(String, String)] = []
    @State private var codes: [FaultCode] = []
    @State private var snapshots: [FaultCode: [(String, Double, String)]] = [:]
    @State private var loading = false
    @State private var confirmClear = false

    var body: some View {
        List {
            Section("Identification") {
                if ident.isEmpty && loading { ProgressView() }
                ForEach(ident, id: \.0) { LabeledContent($0.0, value: $0.1) }
            }
            Section("Fault codes") {
                if codes.isEmpty && !loading { Text("None stored").foregroundStyle(.secondary) }
                ForEach(codes) { c in
                    DisclosureGroup {
                        Text(c.statusText.joined(separator: ", ")).font(.caption)
                        if let snap = snapshots[c] {
                            ForEach(snap, id: \.0) { s in LabeledContent(s.0, value: "\(fmt(s.1)) \(s.2)") }
                        } else {
                            Button("Read freeze frame") { Task { await readSnapshot(c) } }
                        }
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(c.code).font(.body.monospaced())
                                if c.status & 0x80 != 0 { Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange) }
                            }
                            Text(c.description).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            if !codes.isEmpty {
                Section {
                    Button("Clear fault codes…", role: .destructive) { confirmClear = true }
                }
            }
        }
        .navigationTitle(module.title)
        .refreshable { await load() }
        .task { await load() }
        .confirmationDialog("Erase every fault code in \(module.title)?", isPresented: $confirmClear, titleVisibility: .visible) {
            Button("Clear fault codes", role: .destructive) { Task { await clear() } }
        } message: {
            Text("This changes the car: stored codes, freeze frames and the misfire history are erased. Save or screenshot them first.")
        }
    }

    private func load() async {
        guard let c = model.client(module) else { return }
        loading = true
        defer { loading = false }
        do {
            ident = try await c.identify()
            codes = try await c.faultCodes()
        } catch { model.fail(error) }
    }

    private func readSnapshot(_ code: FaultCode) async {
        guard let c = model.client(module) else { return }
        do { snapshots[code] = try await c.snapshot(code) } catch { snapshots[code] = []; model.fail(error) }
    }

    private func clear() async {
        guard let c = model.client(module) else { return }
        do {
            try await c.clearFaultCodes()
            snapshots = [:]
            await load()
        } catch { model.fail(error) }
    }
}
