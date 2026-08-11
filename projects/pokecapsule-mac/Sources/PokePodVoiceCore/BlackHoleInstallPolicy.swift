import Foundation

/// Pure selection rules for the BlackHole installer.  The application still
/// hands the selected package to Apple's Installer.app, which is responsible
/// for the privileged system-driver installation.
public enum BlackHoleInstallPolicy {
    public struct ReleaseAsset: Equatable, Sendable {
        public let name: String
        public let downloadURL: URL
        public let digest: String?

        public init(name: String, downloadURL: URL, digest: String? = nil) {
            self.name = name
            self.downloadURL = downloadURL
            self.digest = digest
        }
    }

    public static let officialReleasesURL = URL(
        string: "https://github.com/ExistentialAudio/BlackHole/releases/latest")!
    public static let latestReleaseAPIURL = URL(
        string: "https://api.github.com/repos/ExistentialAudio/BlackHole/releases/latest")!

    public static func selectPackage(from urls: [URL]) -> URL? {
        urls
            .filter { url in
                url.pathExtension.caseInsensitiveCompare("pkg") == .orderedSame
                    && url.deletingPathExtension().lastPathComponent
                        .lowercased()
                        .contains("blackhole")
            }
            .sorted { lhs, rhs in
                let lhsDate = (try? lhs.resourceValues(forKeys: [.contentModificationDateKey])
                    .contentModificationDate) ?? .distantPast
                let rhsDate = (try? rhs.resourceValues(forKeys: [.contentModificationDateKey])
                    .contentModificationDate) ?? .distantPast
                if lhsDate != rhsDate { return lhsDate > rhsDate }
                return lhs.path < rhs.path
            }
            .first
    }

    public static func selectReleaseAsset(from assets: [ReleaseAsset]) -> ReleaseAsset? {
        assets
            .filter { asset in
                let name = asset.name.lowercased()
                return name.contains("blackhole2ch") && name.hasSuffix(".pkg")
            }
            .sorted { lhs, rhs in lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending }
            .first
    }
}
