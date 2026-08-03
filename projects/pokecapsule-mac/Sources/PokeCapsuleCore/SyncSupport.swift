import CryptoKit
import Foundation

public enum PendingCommandState: String, Codable {
    case queued
    case applying
    case conflict
    case failed
    case completed
}

public struct PendingCommand: Codable, Identifiable, Equatable {
    public var id: UUID { command.transactionId }
    public var command: DeviceCommand
    public var createdAt: Date
    public var state: PendingCommandState
    public var error: String?

    public init(command: DeviceCommand, state: PendingCommandState = .queued, error: String? = nil) {
        self.command = command
        self.createdAt = Date()
        self.state = state
        self.error = error
    }
}

public final class OfflineQueue {
    private let file: URL
    private let fileManager: FileManager

    public init(file: URL, fileManager: FileManager = .default) {
        self.file = file
        self.fileManager = fileManager
    }

    public func load() throws -> [PendingCommand] {
        guard fileManager.fileExists(atPath: file.path) else { return [] }
        return try PokeJSON.decoder.decode([PendingCommand].self, from: Data(contentsOf: file))
    }

    public func save(_ values: [PendingCommand]) throws {
        try fileManager.createDirectory(
            at: file.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try PokeJSON.encoder.encode(values).write(to: file, options: .atomic)
    }

    @discardableResult
    public func enqueue(_ command: DeviceCommand) throws -> [PendingCommand] {
        var values = try load()
        values.append(PendingCommand(command: command))
        try save(values)
        return values
    }

    public func discard(_ id: UUID) throws -> [PendingCommand] {
        let values = try load().filter { $0.id != id }
        try save(values)
        return values
    }
}

public struct QueueReplayResult {
    public var pending: [PendingCommand]
    public var index: CapsuleIndex
    public var appliedCount: Int
}

public struct SyncCoordinator {
    public init() {}

    public func replay(
        pending original: [PendingCommand],
        currentIndex: CapsuleIndex,
        execute: (DeviceCommand) throws -> CapsuleIndex
    ) -> QueueReplayResult {
        var pending = original
        var current = currentIndex
        var applied = 0
        var blockedCapsules = Set<UUID>()

        for index in pending.indices {
            guard pending[index].state == .queued || pending[index].state == .failed else { continue }
            let command = pending[index].command
            let ids = Set(command.capsuleIds ?? [])
            if !ids.isDisjoint(with: blockedCapsules) {
                pending[index].state = .conflict
                pending[index].error = "前面的同一胶囊操作存在冲突"
                continue
            }
            if let conflict = conflictMessage(command: command, index: current) {
                pending[index].state = .conflict
                pending[index].error = conflict
                blockedCapsules.formUnion(ids)
                continue
            }
            pending[index].state = .applying
            do {
                current = try execute(command)
                pending[index].state = .completed
                pending[index].error = nil
                applied += 1
            } catch {
                pending[index].state = .failed
                pending[index].error = error.localizedDescription
                blockedCapsules.formUnion(ids)
            }
        }
        pending.removeAll { $0.state == .completed }
        return QueueReplayResult(pending: pending, index: current, appliedCount: applied)
    }

    public func conflictMessage(command: DeviceCommand, index: CapsuleIndex) -> String? {
        var active: [UUID: Int] = [:]
        for record in index.records + index.trashRecords where active[record.id] == nil {
            if command.operation == .requeueTranscription {
                if let processingRevision = record.processing?.revision {
                    active[record.id] = processingRevision
                }
            } else {
                active[record.id] = record.trash?.revision ?? record.capsule.revision
            }
        }
        if let revision = command.expectedRevision,
           let id = command.capsuleIds?.first {
            guard let actual = active[id] else {
                return "胶囊已被移动、删除或无法读取：\(id.uuidString)"
            }
            if actual != revision {
                return "版本冲突：\(id.uuidString) 预期 \(revision)，设备为 \(actual)"
            }
        }
        guard let expected = command.expectedRevisions else { return nil }
        for (key, revision) in expected {
            guard let id = UUID(uuidString: key), let actual = active[id] else {
                return "胶囊已被移动、删除或无法读取：\(key)"
            }
            if actual != revision {
                return "版本冲突：\(key) 预期 \(revision)，设备为 \(actual)"
            }
        }
        return nil
    }
}

public struct BackupManifest: Codable, Equatable {
    public var createdAt: Date
    public var deviceSerial: String
    public var capsuleCount: Int
    public var files: [String: String]
}

public final class BackupManager {
    private let root: URL
    private let fileManager: FileManager
    private let keepCount: Int
    private let keepDays: Int

    public init(
        root: URL,
        fileManager: FileManager = .default,
        keepCount: Int = 10,
        keepDays: Int = 30
    ) {
        self.root = root
        self.fileManager = fileManager
        self.keepCount = max(1, keepCount)
        self.keepDays = max(1, keepDays)
    }

    @discardableResult
    public func create(from mirror: URL, deviceSerial: String, capsuleCount: Int) throws -> URL {
        guard fileManager.fileExists(atPath: mirror.path) else {
            throw PokeCapsuleError.malformedCapsule("镜像不存在，不能创建备份")
        }
        try fileManager.createDirectory(at: root, withIntermediateDirectories: true)
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss-SSS"
        let safeSerial = DeviceStorageKey.fileComponent(deviceSerial)
        var destination = root.appendingPathComponent(
            "\(formatter.string(from: Date()))-\(safeSerial)", isDirectory: true)
        if fileManager.fileExists(atPath: destination.path) {
            destination = root.appendingPathComponent(
                "\(formatter.string(from: Date()))-\(safeSerial)-\(UUID().uuidString.prefix(8))",
                isDirectory: true)
        }
        let dataDirectory = destination.appendingPathComponent("data", isDirectory: true)
        try fileManager.createDirectory(at: destination, withIntermediateDirectories: false)
        do {
            try fileManager.copyItem(at: mirror, to: dataDirectory)
            let manifest = BackupManifest(
                createdAt: Date(),
                deviceSerial: safeSerial,
                capsuleCount: capsuleCount,
                files: try completeManifest(of: dataDirectory))
            try PokeJSON.encoder.encode(manifest).write(
                to: destination.appendingPathComponent("manifest.json"),
                options: .atomic)
            try prune()
            return destination
        } catch {
            try? fileManager.removeItem(at: destination)
            throw error
        }
    }

    private func completeManifest(of directory: URL) throws -> [String: String] {
        guard let enumerator = fileManager.enumerator(
            at: directory,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: []
        ) else { return [:] }
        var result: [String: String] = [:]
        let canonicalRoot = directory.resolvingSymlinksInPath().standardizedFileURL.path
        while let file = enumerator.nextObject() as? URL {
            guard (try? file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else {
                continue
            }
            let canonicalFile = file.resolvingSymlinksInPath().standardizedFileURL.path
            guard canonicalFile.hasPrefix(canonicalRoot + "/") else {
                throw PokeCapsuleError.invalidRelativePath(canonicalFile)
            }
            let relative = String(canonicalFile.dropFirst(canonicalRoot.count + 1))
            result[relative] = try FileDigest.sha256(of: file)
        }
        return result
    }

    private func prune() throws {
        let directories = try fileManager.contentsOfDirectory(
            at: root,
            includingPropertiesForKeys: [.creationDateKey, .isDirectoryKey],
            options: [.skipsHiddenFiles])
            .filter {
                (try? $0.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
            }
            .sorted {
                let left = (try? $0.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                let right = (try? $1.resourceValues(forKeys: [.creationDateKey]).creationDate) ?? .distantPast
                return left > right
            }
        let cutoff = Date().addingTimeInterval(TimeInterval(-keepDays * 86_400))
        for (offset, directory) in directories.enumerated() {
            let date = (try? directory.resourceValues(forKeys: [.creationDateKey]).creationDate)
                ?? .distantPast
            if offset >= keepCount && date < cutoff {
                try fileManager.removeItem(at: directory)
            }
        }
    }
}

public enum CapsuleSearch {
    public static func filter(_ records: [CapsuleRecord], query: String) -> [CapsuleRecord] {
        let needle = query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !needle.isEmpty else { return records }
        return records.filter {
            [
                $0.displayTitle,
                $0.finalText ?? "",
                $0.polishedText ?? "",
                $0.rawText ?? "",
                $0.relativeFolder,
                $0.capsule.tags.joined(separator: " ")
            ]
            .joined(separator: "\n")
            .lowercased()
            .contains(needle)
        }
    }
}
