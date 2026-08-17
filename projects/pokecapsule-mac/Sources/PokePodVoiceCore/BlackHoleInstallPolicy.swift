import Foundation

/// Pure selection rules for the BlackHole installer.  The application still
/// hands the selected package to Apple's Installer.app, which is responsible
/// for the privileged system-driver installation.
public enum BlackHoleInstallPolicy {
    /// The Developer ID Installer team that publishes the official BlackHole
    /// packages.  Keeping this value in the pure policy layer gives the app and
    /// its tests one trust anchor instead of allowing a downloaded package to
    /// choose its own signer.
    public static let trustedTeamIdentifier = "Q5C99V536K"

    /// Package discovery is deliberately bounded.  BlackHole installers are
    /// expected to be placed directly in Downloads or our private installer
    /// cache; recursively walking user folders is both surprising and a UI
    /// performance hazard.
    public static let maximumEntriesPerRoot = 128

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

    public static let latestReleaseAPIURL = URL(
        string: "https://api.github.com/repos/ExistentialAudio/BlackHole/releases/latest")!

    public static func discoveryRoots(
        homeDirectory: URL,
        applicationSupportDirectory: URL
    ) -> [URL] {
        [
            homeDirectory.appendingPathComponent("Downloads", isDirectory: true),
            applicationSupportDirectory
                .appendingPathComponent("PokePod Voice/Installers", isDirectory: true)
        ]
    }

    /// Finds a package in the direct children of the approved roots.  The
    /// function performs no recursive enumeration and caps each directory so
    /// a hostile or unexpectedly large Downloads folder cannot turn installer
    /// discovery into an unbounded operation.
    public static func discoverPackage(in roots: [URL]) -> URL? {
        let fileManager = FileManager.default
        var candidates = [URL]()
        for root in roots.prefix(2) {
            guard let entries = try? fileManager.contentsOfDirectory(
                at: root,
                includingPropertiesForKeys: [.isRegularFileKey, .contentModificationDateKey],
                options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { continue }
            for url in entries.prefix(maximumEntriesPerRoot) {
                guard let values = try? url.resourceValues(forKeys: [.isRegularFileKey]),
                      values.isRegularFile == true else { continue }
                candidates.append(url)
            }
        }
        return selectPackage(from: candidates)
    }

    public static func isPackagePathAllowed(_ package: URL, within roots: [URL]) -> Bool {
        let packagePath = package.resolvingSymlinksInPath().standardizedFileURL.path
        return roots.contains { root in
            let rootPath = root.resolvingSymlinksInPath().standardizedFileURL.path
            return packagePath == rootPath || packagePath.hasPrefix(rootPath + "/")
        }
    }

    /// The project publishes the signed 2ch package from its own download
    /// host even when the corresponding GitHub release has no binary assets.
    /// Keep the version restricted to numeric release components so a remote
    /// tag can never turn into an arbitrary download path.
    public static func officialPackageURL(forTag tag: String) -> URL? {
        let version = tag.trimmingCharacters(in: .whitespacesAndNewlines)
            .trimmingCharacters(in: CharacterSet(charactersIn: "vV"))
        let components = version.split(separator: ".", omittingEmptySubsequences: false)
        guard (2...4).contains(components.count),
              components.allSatisfy({ !$0.isEmpty && $0.allSatisfy(\.isNumber) }) else {
            return nil
        }
        return URL(string: "https://existential.audio/downloads/BlackHole2ch-\(version).pkg")
    }

    public static func isOfficialDownloadURL(_ url: URL) -> Bool {
        guard url.scheme?.lowercased() == "https",
              let host = url.host?.lowercased() else { return false }
        let path = url.path.lowercased()
        if host == "github.com" {
            return path.hasPrefix("/existentialaudio/blackhole/releases/download/")
        }
        if host == "existential.audio" {
            return path.hasPrefix("/downloads/blackhole2ch-") && path.hasSuffix(".pkg")
        }
        return false
    }

    public static func isSafePackageName(_ name: String) -> Bool {
        guard !name.isEmpty,
              name == URL(fileURLWithPath: name).lastPathComponent,
              !name.contains(".."),
              name.lowercased().hasSuffix(".pkg") else { return false }
        return true
    }

    public static func normalizedSHA256Digest(_ digest: String?) -> String? {
        guard let digest else { return nil }
        let value = digest.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard value.hasPrefix("sha256:") else { return nil }
        let hex = String(value.dropFirst("sha256:".count))
        guard hex.count == 64,
              hex.unicodeScalars.allSatisfy({
                  ($0.value >= 48 && $0.value <= 57)
                      || ($0.value >= 97 && $0.value <= 102)
              }) else { return nil }
        return hex
    }

    public static func isValidDigestDeclaration(_ digest: String?) -> Bool {
        digest == nil || normalizedSHA256Digest(digest) != nil
    }

    public static func signatureOutputIsTrusted(_ output: String) -> Bool {
        let lines = output.replacingOccurrences(of: "\r", with: "").split(separator: "\n")
        guard let status = lines.first(where: { $0.localizedCaseInsensitiveContains("Status:") })
        else { return false }
        let statusText = status.lowercased()
        guard statusText.contains("signed by a certificate trusted"),
              !statusText.contains("not trusted") else { return false }
        return lines.contains { line in
            let text = String(line)
            return text.contains(trustedTeamIdentifier)
                && (text.localizedCaseInsensitiveContains("Developer ID Installer")
                    || text.localizedCaseInsensitiveContains("TeamIdentifier")
                    || text.contains("(\(trustedTeamIdentifier))"))
        }
    }

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
                return name.contains("blackhole2ch")
                    && name.hasSuffix(".pkg")
                    && isOfficialDownloadURL(asset.downloadURL)
                    && isSafePackageName(asset.name)
                    && isValidDigestDeclaration(asset.digest)
            }
            .sorted { lhs, rhs in lhs.name.localizedStandardCompare(rhs.name) == .orderedAscending }
            .first
    }
}
