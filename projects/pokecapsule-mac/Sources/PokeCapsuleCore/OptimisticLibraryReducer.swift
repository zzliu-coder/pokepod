import Foundation

public enum OptimisticLibraryReducer {
    public static func apply(_ command: DeviceCommand, to original: CapsuleIndex,
                             now: Date = Date()) -> CapsuleIndex {
        var index = original
        let ids = Set(command.capsuleIds ?? [])
        switch command.operation {
        case .moveCapsules:
            guard let destination = command.destination else { return index }
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, folder: destination, now: now) : $0
            }
        case .setFavorite:
            guard let favorite = command.favorite else { return index }
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, favorite: favorite, now: now) : $0
            }
        case .addTags:
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, addingTags: command.tags ?? [], now: now) : $0
            }
        case .removeTags:
            let removals = Set(command.tags ?? [])
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, removingTags: removals, now: now) : $0
            }
        case .deleteCapsules:
            let moved = index.records.filter { ids.contains($0.id) }.map {
                CapsuleRecord(capsule: $0.capsule, processing: $0.processing,
                    relativeFolder: "回收站", localDirectory: $0.localDirectory,
                    rawText: $0.rawText, polishedText: $0.polishedText,
                    finalText: $0.finalText,
                    trash: TrashMetadata(schemaVersion: 1, capsuleId: $0.id,
                        trashedAt: now, originalFolder: $0.relativeFolder,
                        revision: $0.capsule.revision + 1), warnings: $0.warnings)
            }
            index.records.removeAll { ids.contains($0.id) }
            index.trashRecords.append(contentsOf: moved)
        case .restoreCapsules:
            let restored = index.trashRecords.filter { ids.contains($0.id) }.map {
                CapsuleRecord(capsule: incremented($0.capsule, now: now),
                    processing: $0.processing,
                    relativeFolder: $0.trash?.originalFolder ?? "Inbox",
                    localDirectory: $0.localDirectory, rawText: $0.rawText,
                    polishedText: $0.polishedText, finalText: $0.finalText,
                    trash: nil, warnings: $0.warnings)
            }
            index.trashRecords.removeAll { ids.contains($0.id) }
            index.records.append(contentsOf: restored)
        case .purgeCapsules:
            index.trashRecords.removeAll { ids.contains($0.id) }
        case .commitFinalText:
            guard let text = command.finalText else { return index }
            index.records = index.records.map { record in
                guard ids.contains(record.id) else { return record }
                return CapsuleRecord(capsule: incremented(record.capsule, now: now),
                    processing: record.processing, relativeFolder: record.relativeFolder,
                    localDirectory: record.localDirectory, rawText: record.rawText,
                    polishedText: record.polishedText, finalText: text,
                    trash: record.trash, warnings: record.warnings)
            }
        default:
            break
        }
        return index
    }

    private static func changed(_ record: CapsuleRecord, folder: String? = nil,
                                favorite: Bool? = nil, addingTags: [String] = [],
                                removingTags: Set<String> = [], now: Date) -> CapsuleRecord {
        var metadata = incremented(record.capsule, now: now)
        if let favorite { metadata.favorite = favorite }
        var tags = metadata.tags.filter { !removingTags.contains($0) }
        for tag in addingTags where !tags.contains(tag) { tags.append(tag) }
        metadata.tags = tags
        return CapsuleRecord(capsule: metadata, processing: record.processing,
            relativeFolder: folder ?? record.relativeFolder,
            localDirectory: record.localDirectory, rawText: record.rawText,
            polishedText: record.polishedText, finalText: record.finalText,
            trash: record.trash, warnings: record.warnings)
    }

    private static func incremented(_ original: CapsuleMetadata, now: Date) -> CapsuleMetadata {
        var value = original
        value.revision += 1
        value.updatedAt = now
        return value
    }
}
