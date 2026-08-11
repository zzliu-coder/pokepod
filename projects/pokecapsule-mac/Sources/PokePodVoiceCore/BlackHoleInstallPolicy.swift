import Foundation

/// Pure selection rules for the BlackHole installer.  The application still
/// hands the selected package to Apple's Installer.app, which is responsible
/// for the privileged system-driver installation.
public enum BlackHoleInstallPolicy {
    public static let officialReleasesURL = URL(
        string: "https://github.com/ExistentialAudio/BlackHole/releases/latest")!

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
}
