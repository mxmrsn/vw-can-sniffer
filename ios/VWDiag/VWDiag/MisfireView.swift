import Charts
import SwiftUI
import VWDiagKit

/// The misfire picture, all in one place: lamp and codes, freeze frame, per-cylinder counts, trims.
struct MisfireView: View {
    @Environment(AppModel.self) private var model
    @State private var lamp: Bool?
    @State private var stored: [String] = []
    @State private var pending: [String] = []
    @State private var freeze: (code: String?, values: [(PID, Double)])?
    @State private var counts: [(thisCycle: Int?, average: Int?)] = []
    @State private var trims: [String: Double] = [:]
    @State private var follow = false

    struct Bar: Identifiable {
        let cylinder: String
        let kind: String
        let count: Int
        var id: String { cylinder + kind }
    }

    var bars: [Bar] {
        counts.enumerated().dropFirst().flatMap { i, c in
            [Bar(cylinder: "Cyl \(i)", kind: "This drive", count: c.thisCycle ?? 0),
             Bar(cylinder: "Cyl \(i)", kind: "Avg of last 10", count: c.average ?? 0)]
        }
    }

    var body: some View {
        NavigationStack {
            List {
                Section("Misfires per cylinder (OBD service 06)") {
                    if counts.isEmpty {
                        Text("Not read yet").foregroundStyle(.secondary)
                    } else {
                        Chart(bars) {
                            BarMark(x: .value("Cylinder", $0.cylinder), y: .value("Misfires", $0.count))
                                .foregroundStyle(by: .value("Period", $0.kind))
                                .position(by: .value("Period", $0.kind))
                        }
                        .frame(height: 200)
                        if let worst = worstCylinder {
                            Label("Most misfires: cylinder \(worst)", systemImage: "flame.fill").foregroundStyle(.orange)
                        }
                    }
                }
                Section("Now") {
                    ForEach(["stft", "ltft", "rpm", "load", "coolant"], id: \.self) { n in
                        if let p = OBD.byName[n] {
                            LabeledContent(p.label, value: trims[n].map { "\(fmt($0)) \(p.unit)" } ?? "–")
                        }
                    }
                    Toggle("Follow every 2 s", isOn: $follow)
                }
                Section("Emission codes") {
                    if let lamp { LabeledContent("Warning lamp", value: lamp ? "ON" : "off") }
                    LabeledContent("Stored", value: stored.isEmpty ? "none" : stored.joined(separator: ", "))
                    LabeledContent("Pending", value: pending.isEmpty ? "none" : pending.joined(separator: ", "))
                }
                if let ff = freeze {
                    Section("Freeze frame\(ff.code.map { " (\($0))" } ?? "")") {
                        if ff.values.isEmpty { Text("None stored").foregroundStyle(.secondary) }
                        ForEach(ff.values, id: \.0.name) { p, v in LabeledContent(p.label, value: "\(fmt(v)) \(p.unit)") }
                    }
                }
                Section {
                    Text("A weak cylinder misfires most at idle and when cold, and pushes the short-term trim positive. Counts that stay on one cylinder after swapping its coil and plug point at compression: a leak-down test tells valve, rings or gasket apart.")
                        .font(.footnote).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Misfire")
            .toolbar { Button("Read") { Task { await readAll() } } }
            .refreshable { await readAll() }
            .task { await readAll() }
            .task(id: follow) {
                while follow && !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    await readLive()
                }
            }
        }
    }

    private var worstCylinder: Int? {
        let totals = counts.enumerated().dropFirst().map { ($0.offset, ($0.element.thisCycle ?? 0) + ($0.element.average ?? 0)) }
        guard let best = totals.max(by: { $0.1 < $1.1 }), best.1 > 0 else { return nil }
        return best.0
    }

    private func readAll() async {
        guard let e = model.client(VW.engine) else { return }
        if !model.transmitAllowed { await model.setTransmit(true) }
        do {
            let ms = try await OBD.monitorStatus(e)
            lamp = ms.lamp
            stored = try await OBD.emissionCodes(e)
            pending = try await OBD.emissionCodes(e, pending: true)
            freeze = try await OBD.freezeFrame(e)
        } catch { model.fail(error) }
        await readLive()
    }

    private func readLive() async {
        guard let e = model.client(VW.engine) else { return }
        do {
            counts = try await OBD.misfireCounts(e)
            for n in ["stft", "ltft", "rpm", "load", "coolant"] {
                if let p = OBD.byName[n] { trims[n] = try? await OBD.read(e, p) }
            }
        } catch { model.fail(error) }
    }
}
