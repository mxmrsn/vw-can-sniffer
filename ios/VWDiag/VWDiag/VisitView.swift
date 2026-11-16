import SwiftUI
import VWDiagKit

/// A visit to the car: the logging experiments of docs/logging_experiments.md, guided step by step,
/// with their results kept on the phone and shared as a zip for `vwdiag calibrate`.
struct VisitView: View {
    @State private var results: [URL] = []
    @State private var zips: [URL: URL] = [:]

    static var folder: URL {
        let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("Experiments")
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    ForEach(Experiments.all) { e in
                        NavigationLink(value: e) {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(e.name).font(.body.monospaced())
                                Text(e.title).font(.subheadline)
                                Text(e.carState).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                    }
                } header: {
                    Text("Experiments")
                } footer: {
                    Text("First visit: listen, identify, timing; warm-idle and rev-holds after a drive; cold-start the next morning. They only read from the car.")
                }
                Section("Results on this phone") {
                    if results.isEmpty { Text("None yet").foregroundStyle(.secondary) }
                    ForEach(results, id: \.self) { url in
                        HStack {
                            Text(url.lastPathComponent).font(.caption.monospaced())
                            Spacer()
                            if let zip = zips[url] {
                                ShareLink(item: zip) { Image(systemName: "square.and.arrow.up") }
                            }
                        }
                    }
                    .onDelete { offsets in
                        for i in offsets { try? FileManager.default.removeItem(at: results[i]) }
                        load()
                    }
                }
                Section {
                    NavigationLink("Card: recording and files") { LogsView() }
                }
            }
            .navigationTitle("Visit")
            .navigationDestination(for: ExperimentInfo.self) { ExperimentView(info: $0, onFinish: load) }
            .onAppear(perform: load)
        }
    }

    private func load() {
        let items = (try? FileManager.default.contentsOfDirectory(at: Self.folder, includingPropertiesForKeys: nil)) ?? []
        results = items.filter { $0.hasDirectoryPath }.sorted { $0.lastPathComponent > $1.lastPathComponent }
        zips = Dictionary(uniqueKeysWithValues: results.compactMap { url in zipped(url).map { (url, $0) } })
    }
}

/// A folder as a zip, for sharing (AirDrop it to the Mac, then  vwdiag calibrate  on the unzipped folders).
func zipped(_ folder: URL) -> URL? {
    var out: URL?
    var error: NSError?
    NSFileCoordinator().coordinate(readingItemAt: folder, options: .forUploading, error: &error) { zip in
        let dest = FileManager.default.temporaryDirectory.appendingPathComponent(folder.lastPathComponent + ".zip")
        try? FileManager.default.removeItem(at: dest)
        if (try? FileManager.default.copyItem(at: zip, to: dest)) != nil { out = dest }
    }
    return out
}

/// What the running experiment needs from the screen: answers to prompts, marks, and "finish".
@MainActor @Observable
final class ExperimentController {
    var running = false
    var notes: [String] = []
    var progress = 0.0
    var prompt: String?
    var finished: URL?
    var finishedZip: URL?
    var summaryError: String?
    private var promptContinuation: CheckedContinuation<Void, Never>?
    let shared = Shared()

    /// State the experiment reads from its own task.
    final class Shared: @unchecked Sendable {
        private let lock = NSLock()
        private var marks: [String] = []
        private var stop = false
        func addMark(_ m: String) { lock.lock(); marks.append(m); lock.unlock() }
        func takeMarks() -> [String] { lock.lock(); defer { marks.removeAll(); lock.unlock() }; return marks }
        func requestStop() { lock.lock(); stop = true; lock.unlock() }
        func stopRequested() -> Bool { lock.lock(); defer { lock.unlock() }; return stop }
        func reset() { lock.lock(); stop = false; marks.removeAll(); lock.unlock() }
    }

    func ask(_ text: String) async {
        await withCheckedContinuation { c in
            promptContinuation = c
            prompt = text
        }
    }

    func answer() {
        prompt = nil
        promptContinuation?.resume()
        promptContinuation = nil
    }

    func hooks() -> ExperimentHooks {
        let shared = self.shared
        return ExperimentHooks(
            prompt: { text in await self.ask(text) },
            note: { text in Task { @MainActor in self.notes.append(text) } },
            progress: { p in Task { @MainActor in self.progress = p } },
            stopRequested: { shared.stopRequested() },
            takeMarks: { shared.takeMarks() })
    }
}

struct ExperimentView: View {
    @Environment(AppModel.self) private var model
    let info: ExperimentInfo
    var onFinish: () -> Void
    @State private var controller = ExperimentController()
    @State private var minutes: Double = 0
    @State private var interval = 0.5
    @State private var sessionTest = false
    @State private var holds = "0,1500,2500,3500"
    @State private var mark = ""
    @State private var task: Task<Void, Never>?

    var body: some View {
        Form {
            Section {
                Text(info.title)
                LabeledContent("Car", value: info.carState)
            }
            if !controller.running && controller.finished == nil {
                Section("Settings") {
                    if info.defaultSeconds > 0 {
                        Stepper(value: $minutes, in: 0.5...60, step: 0.5) {
                            LabeledContent(info.name == "rev-holds" ? "Each hold" : (info.name == "listen" ? "Ignition off + on" : "Length"),
                                           value: "\(fmt(minutes)) min")
                        }
                    }
                    if ["cold-start", "warm-idle", "rev-holds"].contains(info.name) {
                        Stepper(value: $interval, in: 0.2...5, step: 0.1) { LabeledContent("Sample every", value: "\(fmt(interval)) s") }
                    }
                    if info.name == "identify" { Toggle("Also time the extended session", isOn: $sessionTest) }
                    if info.name == "rev-holds" { TextField("Speeds to hold, rpm", text: $holds).keyboardType(.numbersAndPunctuation) }
                }
                Section {
                    Button("Start") { start() }
                } footer: {
                    Text(info.transmits ? "Reads from the car (sending is switched on for the run, and off again after)." : "Listen only: nothing is sent to the car.")
                }
            }
            if controller.running || controller.finished != nil {
                Section {
                    ProgressView(value: controller.progress)
                    if info.name == "drive" && controller.running {
                        TextField("Mark, e.g. stumble pulling away", text: $mark)
                        Button("Add mark") {
                            controller.shared.addMark(mark.isEmpty ? "mark" : mark)
                            mark = ""
                        }
                        Button("Finish the drive") { controller.shared.requestStop() }
                    } else if controller.running {
                        Button("Stop early", role: .destructive) { controller.shared.requestStop() }
                    }
                }
                Section("Log") {
                    ForEach(Array(controller.notes.enumerated()), id: \.offset) { _, n in Text(n).font(.footnote) }
                }
            }
            if let folder = controller.finished {
                Section {
                    if let zip = controller.finishedZip {
                        ShareLink(item: zip) { Label("Share \(folder.lastPathComponent)", systemImage: "square.and.arrow.up") }
                    }
                } footer: {
                    Text("Saved in the app's Experiments folder (Files app). On the Mac:  python3 -m vwdiag calibrate <folders>")
                }
            }
        }
        .navigationTitle(info.name)
        .onAppear { minutes = info.defaultSeconds / 60 }
        .onDisappear { if controller.running { controller.shared.requestStop() } }
        .alert("Ready?", isPresented: Binding(get: { controller.prompt != nil }, set: { _ in })) {
            Button("Ready") { controller.answer() }
        } message: {
            Text(controller.prompt ?? "")
        }
    }

    private func start() {
        guard let ifc = model.interface else { return }
        var settings = ExperimentSettings()
        settings.seconds = info.defaultSeconds > 0 ? minutes * 60 : (info.name == "drive" ? 0 : nil)
        settings.interval = interval
        settings.sessionTest = sessionTest
        settings.holds = holds.split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }
        if case let .connected(label) = model.link, label.hasPrefix("Simulator") { settings.target = "simulator" }
        controller.shared.reset()
        controller.notes = []
        controller.running = true
        let runner = ExperimentRunner(interface: ifc, experiment: info, settings: settings, root: VisitView.folder,
                                      hooks: controller.hooks())
        task = Task {
            _ = await runner.run()
            controller.running = false
            controller.finished = runner.folder
            controller.finishedZip = zipped(runner.folder)
            await model.refresh()
            onFinish()
        }
    }
}
