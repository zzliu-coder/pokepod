import CryptoKit
import Foundation

public enum PathPolicy {
    public static func normalizedFolderName(_ input: String) throws -> String {
        let normalized = input.precomposedStringWithCanonicalMapping
            .trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalized.isEmpty,
              normalized.count <= 80,
              normalized != ".",
              normalized != "..",
              !normalized.hasPrefix("."),
              !normalized.contains("/"),
              !normalized.contains("\\"),
              !normalized.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }),
              !ProtocolConstants.reservedFolders.contains(where: {
                  $0.caseInsensitiveCompare(normalized) == .orderedSame
              }) else {
            throw PokeCapsuleError.invalidFolderName(input)
        }
        return normalized
    }

    public static func validatedRelativeFolder(_ input: String, allowReserved: Bool = true) throws -> String {
        let normalized = input.precomposedStringWithCanonicalMapping
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let parts = normalized.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard (1...2).contains(parts.count), !parts.contains(where: { $0.isEmpty }) else {
            throw PokeCapsuleError.invalidRelativePath(input)
        }
        if allowReserved, parts.count == 1, ProtocolConstants.reservedFolders.contains(parts[0]) {
            return parts[0]
        }
        let clean = try parts.map(normalizedFolderName)
        return clean.joined(separator: "/")
    }

    public static func validatedTag(_ input: String) throws -> String {
        var value = input.precomposedStringWithCanonicalMapping
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if value.hasPrefix("#") { value.removeFirst() }
        guard !value.isEmpty,
              value.count <= 50,
              !value.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else {
            throw PokeCapsuleError.invalidFolderName(input)
        }
        return value
    }

    public static func safeRemoteReadPath(_ path: String) throws -> String {
        let root = ProtocolConstants.remoteRoot
        guard path == root || path.hasPrefix(root + "/"),
              !path.contains("\0"),
              !path.split(separator: "/").contains("..") else {
            throw PokeCapsuleError.invalidRelativePath(path)
        }
        return path
    }
}

public struct CapsuleScanner {
    private let fileManager: FileManager

    public init(fileManager: FileManager = .default) {
        self.fileManager = fileManager
    }

    public func scan(root: URL) -> CapsuleIndex {
        var records: [CapsuleRecord] = []
        var trashRecords: [CapsuleRecord] = []
        var folders = Set(ProtocolConstants.reservedFolders)
        var warnings: [String] = []
        var seen = Set<UUID>()

        guard let levelOne = try? fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else {
            return CapsuleIndex(folders: ProtocolConstants.reservedFolders, warnings: ["镜像目录尚未建立"])
        }

        for first in levelOne where isDirectory(first) {
            guard !first.lastPathComponent.hasPrefix(".") else { continue }
            folders.insert(first.lastPathComponent)
            scanFolder(first, relativeFolder: first.lastPathComponent, depth: 1, records: &records, folders: &folders, warnings: &warnings, seen: &seen)
        }

        let trashRoot = root.appendingPathComponent(".trash", isDirectory: true)
        if let deleted = try? fileManager.contentsOfDirectory(
            at: trashRoot,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: []
        ) {
            for directory in deleted where isDirectory(directory) {
                do {
                    let trashURL = directory.appendingPathComponent("trash.json")
                    let trash = try PokeJSON.decoder.decode(
                        TrashMetadata.self,
                        from: Data(contentsOf: trashURL))
                    trashRecords.append(try decodeCapsule(
                        directory: directory,
                        relativeFolder: "回收站",
                        trash: trash))
                } catch {
                    warnings.append("回收站/\(directory.lastPathComponent)：\(error.localizedDescription)")
                }
            }
        }

        records.sort { $0.capsule.createdAt > $1.capsule.createdAt }
        trashRecords.sort {
            ($0.trash?.trashedAt ?? .distantPast) > ($1.trash?.trashedAt ?? .distantPast)
        }
        return CapsuleIndex(
            records: records,
            trashRecords: trashRecords,
            folders: folders.sorted(),
            warnings: warnings)
    }

    private func scanFolder(
        _ folder: URL,
        relativeFolder: String,
        depth: Int,
        records: inout [CapsuleRecord],
        folders: inout Set<String>,
        warnings: inout [String],
        seen: inout Set<UUID>
    ) {
        guard let children = try? fileManager.contentsOfDirectory(
            at: folder,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles]
        ) else { return }

        for child in children where isDirectory(child) {
            let metadataURL = child.appendingPathComponent("capsule.json")
            if fileManager.fileExists(atPath: metadataURL.path) {
                do {
                    let record = try decodeCapsule(
                        directory: child,
                        relativeFolder: relativeFolder,
                        trash: nil)
                    if !seen.insert(record.id).inserted {
                        warnings.append("发现重复 UUID：\(record.id.uuidString)")
                    }
                    records.append(record)
                } catch {
                    warnings.append("\(relativeFolder)/\(child.lastPathComponent)：\(error.localizedDescription)")
                }
            } else if depth < 2 {
                let nested = relativeFolder + "/" + child.lastPathComponent
                folders.insert(nested)
                scanFolder(child, relativeFolder: nested, depth: depth + 1, records: &records, folders: &folders, warnings: &warnings, seen: &seen)
            }
        }
    }

    private func decodeCapsule(
        directory: URL,
        relativeFolder: String,
        trash: TrashMetadata?
    ) throws -> CapsuleRecord {
        let metadataURL = directory.appendingPathComponent("capsule.json")
        let capsule: CapsuleMetadata
        do {
            capsule = try PokeJSON.decoder.decode(CapsuleMetadata.self, from: Data(contentsOf: metadataURL))
        } catch {
            throw PokeCapsuleError.malformedCapsule(error.localizedDescription)
        }

        let processingURL = directory.appendingPathComponent("processing.json")
        var processing: ProcessingMetadata?
        var localWarnings: [String] = []
        if fileManager.fileExists(atPath: processingURL.path) {
            do {
                processing = try PokeJSON.decoder.decode(ProcessingMetadata.self, from: Data(contentsOf: processingURL))
                if processing?.capsuleId != capsule.id {
                    localWarnings.append("processing.json 的 UUID 不一致")
                }
            } catch {
                localWarnings.append("processing.json 无法解析")
            }
        } else {
            localWarnings.append("缺少 processing.json")
        }

        let audioURL = directory.appendingPathComponent("audio.m4a")
        if !fileManager.fileExists(atPath: audioURL.path) {
            localWarnings.append("缺少原始音频")
        }
        let rawURL = directory.appendingPathComponent("raw.txt")
        let polishedURL = directory.appendingPathComponent("polished.md")
        let finalURL = directory.appendingPathComponent("final.md")
        return CapsuleRecord(
            capsule: capsule,
            processing: processing,
            relativeFolder: relativeFolder,
            localDirectory: directory,
            rawText: try? String(contentsOf: rawURL, encoding: .utf8),
            polishedText: try? String(contentsOf: polishedURL, encoding: .utf8),
            finalText: try? String(contentsOf: finalURL, encoding: .utf8),
            trash: trash,
            warnings: localWarnings
        )
    }

    private func isDirectory(_ url: URL) -> Bool {
        (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
    }
}

public enum FileDigest {
    public static func sha256(of file: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: file)
        defer { try? handle.close() }
        var digest = SHA256()
        while true {
            let data = try handle.read(upToCount: 1024 * 1024) ?? Data()
            if data.isEmpty { break }
            digest.update(data: data)
        }
        return digest.finalize().map { String(format: "%02x", $0) }.joined()
    }

    public static func manifest(of directory: URL, fileManager: FileManager = .default) throws -> [String: String] {
        guard let enumerator = fileManager.enumerator(
            at: directory,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else { return [:] }
        var result: [String: String] = [:]
        let canonicalRoot = directory.resolvingSymlinksInPath().standardizedFileURL.path
        while let url = enumerator.nextObject() as? URL {
            guard (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else { continue }
            let canonicalFile = url.resolvingSymlinksInPath().standardizedFileURL.path
            guard canonicalFile.hasPrefix(canonicalRoot + "/") else {
                throw PokeCapsuleError.invalidRelativePath(canonicalFile)
            }
            let relative = String(canonicalFile.dropFirst(canonicalRoot.count + 1))
            result[relative] = try sha256(of: url)
        }
        return result
    }

    public static func verifyCopy(from source: URL, to destination: URL) throws {
        let sourceManifest = try manifest(of: source)
        let destinationManifest = try manifest(of: destination)
        guard sourceManifest == destinationManifest else {
            let mismatched = Set(sourceManifest.keys).symmetricDifference(Set(destinationManifest.keys)).sorted().first
                ?? sourceManifest.keys.sorted().first(where: { sourceManifest[$0] != destinationManifest[$0] })
                ?? destination.lastPathComponent
            throw PokeCapsuleError.hashMismatch(mismatched)
        }
    }
}

public struct CapsuleExporter {
    private let fileManager: FileManager

    public init(fileManager: FileManager = .default) {
        self.fileManager = fileManager
    }

    @discardableResult
    public func export(_ records: [CapsuleRecord], to destination: URL) throws -> [URL] {
        try fileManager.createDirectory(at: destination, withIntermediateDirectories: true)
        var completed: [URL] = []
        for record in records {
            let output = destination.appendingPathComponent(record.id.uuidString, isDirectory: true)
            guard !fileManager.fileExists(atPath: output.path) else {
                throw PokeCapsuleError.uuidConflict(record.id)
            }
            try fileManager.copyItem(at: record.localDirectory, to: output)
            do {
                try FileDigest.verifyCopy(from: record.localDirectory, to: output)
                completed.append(output)
            } catch {
                try? fileManager.removeItem(at: output)
                throw error
            }
        }
        return completed
    }
}

public struct CapsulePackage {
    public let metadata: CapsuleMetadata
    public let directory: URL
    public let manifest: [String: String]

    public static func inspect(_ directory: URL, fileManager: FileManager = .default) throws -> CapsulePackage {
        let metadataURL = directory.appendingPathComponent("capsule.json")
        let processingURL = directory.appendingPathComponent("processing.json")
        let audioURL = directory.appendingPathComponent("audio.m4a")
        guard fileManager.fileExists(atPath: metadataURL.path),
              fileManager.fileExists(atPath: processingURL.path),
              fileManager.fileExists(atPath: audioURL.path) else {
            throw PokeCapsuleError.malformedCapsule("导入目录必须包含 capsule.json、processing.json 和 audio.m4a")
        }
        let metadata = try PokeJSON.decoder.decode(CapsuleMetadata.self, from: Data(contentsOf: metadataURL))
        guard metadata.schemaVersion == ProtocolConstants.schemaVersion else {
            throw PokeCapsuleError.unsupportedSchema(metadata.schemaVersion)
        }
        return CapsulePackage(
            metadata: metadata,
            directory: directory,
            manifest: try FileDigest.manifest(of: directory, fileManager: fileManager)
        )
    }
}
