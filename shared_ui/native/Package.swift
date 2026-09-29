// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "ThunderLinkUIResources",
    platforms: [.macOS(.v13)],
    products: [.library(name: "ThunderLinkUIResources", targets: ["ThunderLinkUIResources"])],
    targets: [
        .target(name: "ThunderLinkUIResources", resources: [.copy("Resources")])
    ]
)
