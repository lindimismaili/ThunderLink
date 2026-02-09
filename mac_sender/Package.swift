// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ThunderLinkSender",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "ThunderLinkSender",
            path: "Sources"
        )
    ]
)
