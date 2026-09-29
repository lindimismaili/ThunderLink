// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ThunderLinkSender",
    platforms: [.macOS(.v13)],
    dependencies: [.package(path: "../shared_ui/native")],
    targets: [
        .executableTarget(
            name: "ThunderLinkSender",
            dependencies: ["PowerSupport", .product(name: "ThunderLinkUIResources", package: "ThunderLinkUIResources")],
            path: "Sources",
            linkerSettings: [
                // System frameworks for hardware capture/encode/input + UI.
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("CoreGraphics"),
                .linkedFramework("ColorSync"),
                .linkedFramework("AppKit"),
                .linkedFramework("IOKit"),
                .linkedFramework("SwiftUI")
            ]
        ),
        .target(name: "PowerSupport", path: "PowerSupport", linkerSettings: [.linkedFramework("IOKit")]),
        .executableTarget(name: "ThunderLinkPowerHelper", dependencies: ["PowerSupport"], path: "PowerHelper"),
        .testTarget(name: "ThunderLinkSenderTests", dependencies: ["ThunderLinkSender", "PowerSupport"], path: "Tests")
    ]
)
