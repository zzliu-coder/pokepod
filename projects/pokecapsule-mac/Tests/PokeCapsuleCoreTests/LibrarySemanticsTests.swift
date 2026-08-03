import Foundation
import XCTest
@testable import PokeCapsuleCore

final class LibrarySemanticsTests: XCTestCase {
    func testDeviceStorageKeyCannotEscapeApplicationSupport() {
        XCTAssertEqual(
            DeviceStorageKey.fileComponent("../../Library/Secrets"),
            "device-8f5c21c38b331366edb60af5d49bc95462ca58610e0eade41b051fc66d1c12b0")
        XCTAssertEqual(DeviceStorageKey.fileComponent("BE87E832"), "BE87E832")
        XCTAssertFalse(DeviceStorageKey.fileComponent("../device").contains("/"))
        XCTAssertNotEqual(
            DeviceStorageKey.fileComponent(String(repeating: "a", count: 64) + "x"),
            DeviceStorageKey.fileComponent(String(repeating: "a", count: 64) + "y"))
        let base = URL(fileURLWithPath: "/tmp/PokeCapsule", isDirectory: true)
        let mirror = DeviceStorageKey.mirrorURL(
            applicationBase: base, deviceID: "../../Library/Secrets")
        let queue = DeviceStorageKey.queueURL(
            applicationBase: base, deviceID: "../../Library/Secrets")
        XCTAssertTrue(mirror.path.hasPrefix(base.appendingPathComponent("Mirrors").path + "/"))
        XCTAssertTrue(queue.path.hasPrefix(base.appendingPathComponent("Queues").path + "/"))
    }

    func testScopesSearchAndSortUseDerivedState() {
        let older = record(id: UUID(), date: Date(timeIntervalSince1970: 1),
                           folder: "Inbox", text: "第一条", favorite: false,
                           status: .ready, tags: ["工作"])
        let newer = record(id: UUID(), date: Date(timeIntervalSince1970: 2),
                           folder: "项目/商务", text: "福斯特提案", favorite: true,
                           status: .queued, tags: ["建筑"])
        let deleted = record(id: UUID(), date: Date(timeIntervalSince1970: 3),
                             folder: "回收站", text: "删除内容", favorite: false,
                             status: .failed, tags: [], trashed: true)
        let index = CapsuleIndex(records: [older, newer], trashRecords: [deleted])

        XCTAssertEqual(LibraryQuery.records(in: index, scope: .folder("Inbox"), search: "").map(\.id), [older.id])
        XCTAssertEqual(LibraryQuery.records(in: index, scope: .pending, search: "提案").map(\.id), [newer.id])
        XCTAssertEqual(LibraryQuery.records(in: index, scope: .all, search: "").map(\.id), [newer.id, older.id])
        XCTAssertEqual(LibraryQuery.records(in: index, scope: .trash, search: "").map(\.id), [deleted.id])
    }

    func testTypedCommandPreservesWireJSON() throws {
        let command = DeviceCommand(operation: .moveCapsules,
                                    capsuleIds: [UUID()], destination: "Archive")
        XCTAssertEqual(command.operation, .moveCapsules)
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(command)
        XCTAssertTrue(String(decoding: data, as: UTF8.self).contains("\"operation\":\"moveCapsules\""))
    }

    func testSharedDisplayPolicyFixtureDrivesEveryMacDisplayCase() throws {
        var projects = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<3 { projects.deleteLastPathComponent() }
        let fixture = projects.appendingPathComponent("pokecapsule-protocol/display-policy-fixtures.json")
        let policy = try JSONDecoder().decode(DisplayFixture.self, from: Data(contentsOf: fixture))
        XCTAssertEqual(policy.schemaVersion, 1)
        XCTAssertEqual(policy.cases.count, 5)
        for item in policy.cases {
            let id = UUID()
            let status = try XCTUnwrap(ProcessingStatus(rawValue: item.status))
            let value = CapsuleRecord(
                capsule: CapsuleMetadata(id: id, title: "", createdAt: Date(), updatedAt: Date()),
                processing: ProcessingMetadata(schemaVersion: 1, capsuleId: id,
                    revision: 1, durationMs: item.durationMs, status: status,
                    audioFile: "audio.m4a", rawTextFile: "raw.txt",
                    polishedTextFile: "polished.md", errorStage: nil, error: nil,
                    attempts: 0, engine: nil, model: nil),
                relativeFolder: "Inbox",
                localDirectory: URL(fileURLWithPath: "/tmp/\(id.uuidString)"),
                rawText: item.rawText,
                polishedText: item.polishedText,
                finalText: item.finalText,
                warnings: [])
            XCTAssertEqual(value.displayPreview, item.expected, item.name)
        }
    }

    private func record(id: UUID, date: Date, folder: String, text: String,
                        favorite: Bool, status: ProcessingStatus, tags: [String],
                        trashed: Bool = false) -> CapsuleRecord {
        CapsuleRecord(
            capsule: CapsuleMetadata(id: id, createdAt: date, updatedAt: date,
                                     favorite: favorite, tags: tags),
            processing: ProcessingMetadata(schemaVersion: 1, capsuleId: id,
                revision: 1, durationMs: 8_000, status: status,
                audioFile: "audio.m4a", rawTextFile: "raw.txt",
                polishedTextFile: nil, errorStage: nil, error: nil,
                attempts: 0, engine: nil, model: nil),
            relativeFolder: folder,
            localDirectory: URL(fileURLWithPath: "/tmp/\(id.uuidString)"),
            rawText: text, polishedText: nil,
            trash: trashed ? TrashMetadata(schemaVersion: 1, capsuleId: id,
                trashedAt: date, originalFolder: "Inbox", revision: 2) : nil,
            warnings: [])
    }
}

private struct DisplayFixture: Decodable {
    let schemaVersion: Int
    let cases: [DisplayFixtureCase]
}

private struct DisplayFixtureCase: Decodable {
    let name: String
    let durationMs: Int
    let status: String
    let finalText: String
    let polishedText: String
    let rawText: String
    let expected: String
}
