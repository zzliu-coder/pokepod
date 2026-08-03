import Foundation

public enum LibraryScope: Hashable {
    case all
    case folder(String)
    case tag(String)
    case favorites
    case pending
    case failed
    case trash

    public var title: String {
        switch self {
        case .all: return "全部胶囊"
        case .folder(let folder):
            if folder == "Inbox" { return "收件箱" }
            if folder == "Archive" { return "归档" }
            return folder
        case .tag(let tag): return "#\(tag)"
        case .favorites: return "收藏"
        case .pending: return "待转写"
        case .failed: return "转写失败"
        case .trash: return "回收站"
        }
    }
}

public enum LibrarySort: String, CaseIterable {
    case newestFirst
    case oldestFirst
}

public enum LibraryQuery {
    public static func records(
        in index: CapsuleIndex,
        scope: LibraryScope,
        search: String,
        sort: LibrarySort = .newestFirst
    ) -> [CapsuleRecord] {
        let scoped: [CapsuleRecord]
        switch scope {
        case .all: scoped = index.records
        case .folder(let folder): scoped = index.records.filter { $0.relativeFolder == folder }
        case .tag(let tag): scoped = index.records.filter { $0.capsule.tags.contains(tag) }
        case .favorites: scoped = index.records.filter(\.capsule.favorite)
        case .pending:
            scoped = index.records.filter {
                [.recorded, .queued, .transcribing].contains($0.processing?.status)
            }
        case .failed: scoped = index.records.filter { $0.processing?.status == .failed }
        case .trash: scoped = index.trashRecords
        }
        let searched = CapsuleSearch.filter(scoped, query: search)
        return searched.sorted {
            if sort == .newestFirst {
                return $0.capsule.createdAt > $1.capsule.createdAt
            }
            return $0.capsule.createdAt < $1.capsule.createdAt
        }
    }
}

public enum CommandOperation: String, Codable, CaseIterable {
    case beginMaintenance
    case endMaintenance
    case rescan
    case importTencentCredentials
    case exportTencentCredentials
    case moveCapsules
    case copyCapsules
    case deleteCapsules
    case restoreCapsules
    case purgeCapsules
    case setFavorite
    case addTags
    case removeTags
    case commitFinalText
    case createFolder
    case deleteFolderToInbox
    case renameFolder
    case renameTag
    case mergeTag
    case deleteTag
    case requeueTranscription
    case commitImport
    case commitCorrection
}
