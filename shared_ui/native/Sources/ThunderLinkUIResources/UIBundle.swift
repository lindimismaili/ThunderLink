import Foundation

public enum ThunderLinkUIResources {
    public static let contractVersion = 1
    public static var bundle: Bundle { Bundle.module }

    public static func asset(_ name: String) -> URL? {
        let parts = name.split(separator: "/", omittingEmptySubsequences: false)
        guard !parts.isEmpty,
              parts.allSatisfy({ !$0.isEmpty && $0 != "." && $0 != ".." }),
              !name.contains("\\"),
              !name.contains(":") else { return nil }
        return bundle.resourceURL?.appendingPathComponent("Resources", isDirectory: true)
            .appendingPathComponent(name, isDirectory: false)
    }
}
