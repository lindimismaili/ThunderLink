// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ThunderLinkSender",
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "ThunderLinkSender",
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
                .linkedFramework("SwiftUI")
            ]
        ),
        .testTarget(name: "ThunderLinkSenderTests", dependencies: ["ThunderLinkSender"], path: "Tests")
    ]
)
