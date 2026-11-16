import Charts
import SwiftUI
import VWDiagKit

/// Poll chosen OBD values from the engine, show them as numbers and a graph, and record to CSV.
struct LiveDataView: View {
    @Environment(AppModel.self) private var model
    @State private var chosen: [String] = ["rpm", "load", "stft", "ltft", "coolant"]
    @State private var samples: [Sample] = []
    @State private var latest: [String: Double] = [:]
    @State private var running = false
    @State private var graphed = "rpm"
    @State private var recording: URL?
    @State private var picking = false

    struct Sample: Identifiable {
        let id = UUID()
        let t: Double
        let name: String
        let value: Double
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Chart(samples.filter { $0.name == graphed }) {
                        LineMark(x: .value("s", $0.t), y: .value(graphed, $0.value))
                    }
                    .frame(height: 180)
                    .chartXAxisLabel("seconds")
                    Picker("Graph", selection: $graphed) {
                        ForEach(chosen, id: \.self) { Text($0).tag($0) }
                    }
                }
                Section("Values") {
                    ForEach(chosen, id: \.self) { name in
                        if let p = OBD.byName[name] {
                            LabeledContent(p.label, value: latest[name].map { "\(fmt($0)) \(p.unit)" } ?? "–")
                        }
                    }
                }
                if let url = recording, !running {
                    Section("Recording") {
                        ShareLink(item: url) { Label(url.lastPathComponent, systemImage: "square.and.arrow.up") }
                    }
                }
            }
            .navigationTitle("Live data")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) { Button("Values") { picking = true }.disabled(running) }
                ToolbarItem(placement: .topBarTrailing) {
                    Button(running ? "Stop" : "Start") { running.toggle() }
                }
            }
            .sheet(isPresented: $picking) { picker }
            .task(id: running) { if running { await poll() } }
        }
    }

    private var picker: some View {
        NavigationStack {
            List(OBD.pids) { p in
                Button {
                    if let i = chosen.firstIndex(of: p.name) { chosen.remove(at: i) } else { chosen.append(p.name) }
                } label: {
                    HStack {
                        Text(p.label)
                        Spacer()
                        if chosen.contains(p.name) { Image(systemName: "checkmark") }
                    }
                }
            }
            .navigationTitle("Values to read")
            .toolbar { Button("Done") { picking = false } }
        }
    }

    /// Read every chosen value in turn, as fast as the car answers (the interface does one request at a time).
    private func poll() async {
        guard let engine = model.client(VW.engine) else { return }
        if !model.transmitAllowed { await model.setTransmit(true) }
        samples = []
        let start = Date()
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("live-\(Int(start.timeIntervalSince1970)).csv")
        var csv = "time_s," + chosen.map { "\($0)_\(OBD.byName[$0]?.unit ?? "")" }.joined(separator: ",") + "\n"
        recording = url
        while !Task.isCancelled {
            var row: [String] = []
            let t = Date().timeIntervalSince(start)
            for name in chosen {
                guard let p = OBD.byName[name] else { continue }
                do {
                    let v = try await OBD.read(engine, p)
                    latest[name] = v
                    samples.append(Sample(t: t, name: name, value: v))
                    row.append(fmt(v, 3))
                } catch UDSError.negative, UDSError.noResponse {
                    row.append("")
                } catch {
                    model.fail(error)
                    running = false
                    return
                }
            }
            csv += String(format: "%.2f,", t) + row.joined(separator: ",") + "\n"
            try? csv.write(to: url, atomically: true, encoding: .utf8)
            if samples.count > 3000 { samples.removeFirst(samples.count - 3000) }
            try? await Task.sleep(nanoseconds: 200_000_000)
        }
    }
}
