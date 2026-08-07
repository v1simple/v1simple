// swift-tools-version:5.7
import PackageDescription

// Zero external dependencies on purpose: this must build on a Mac with no
// network access and nothing but the Xcode command line tools installed.
let package = Package(
    name: "v1replay",
    platforms: [.macOS(.v11)],
    targets: [
        .executableTarget(
            name: "v1replay",
            path: "Sources/v1replay"
        )
    ]
)
