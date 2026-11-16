// swift-tools-version: 5.10
// The diagnostic interface's protocol, and what the app knows about VW control modules,
// as a package with no UI so it can be tested on its own (swift test, on the Mac).
import PackageDescription

let package = Package(
    name: "VWDiagKit",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: [.library(name: "VWDiagKit", targets: ["VWDiagKit"])],
    targets: [
        .target(name: "VWDiagKit"),
        .testTarget(name: "VWDiagKitTests", dependencies: ["VWDiagKit"], resources: [.copy("Fixtures")]),
    ]
)
