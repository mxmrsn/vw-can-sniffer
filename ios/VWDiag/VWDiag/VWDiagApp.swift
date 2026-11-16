import SwiftUI
import VWDiagKit

@main
struct VWDiagApp: App {
    @State private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(model)
        }
    }
}

struct ContentView: View {
    @Environment(AppModel.self) private var model

    var body: some View {
        @Bindable var model = model
        TabView {
            ConnectView().tabItem { Label("Interface", systemImage: "antenna.radiowaves.left.and.right") }
            NeedsConnection { AutoscanView() }.tabItem { Label("Modules", systemImage: "square.grid.2x2") }
            NeedsConnection { LiveDataView() }.tabItem { Label("Live", systemImage: "waveform.path.ecg") }
            NeedsConnection { MisfireView() }.tabItem { Label("Misfire", systemImage: "flame") }
            NeedsConnection { LogsView() }.tabItem { Label("Logs", systemImage: "list.bullet.rectangle") }
        }
        .alert("Something went wrong", isPresented: Binding(get: { model.message != nil }, set: { if !$0 { model.message = nil } })) {
            Button("OK") { model.message = nil }
        } message: {
            Text(model.message ?? "")
        }
    }
}

/// Shows its content only while an interface is connected.
struct NeedsConnection<Content: View>: View {
    @Environment(AppModel.self) private var model
    @ViewBuilder var content: () -> Content

    var body: some View {
        if model.connected {
            content()
        } else {
            ContentUnavailableView("No interface", systemImage: "cable.connector.slash",
                                   description: Text("Connect on the Interface tab first."))
        }
    }
}

/// "12.3" style numbers without trailing zeros.
func fmt(_ v: Double, _ digits: Int = 2) -> String {
    let s = String(format: "%.\(digits)f", v)
    guard s.contains(".") else { return s }
    var t = s
    while t.hasSuffix("0") { t.removeLast() }
    if t.hasSuffix(".") { t.removeLast() }
    return t
}
