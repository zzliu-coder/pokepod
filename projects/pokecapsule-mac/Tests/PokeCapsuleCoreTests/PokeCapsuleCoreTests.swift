import Foundation
import XCTest
@testable import PokeCapsuleCore

final class PokeCapsuleCoreTests: XCTestCase {
    func testCapsuleListPreviewPrefersPolishedTextAndHidesCompletedState() {
        let id = UUID()
        let capsule = CapsuleMetadata(
            id: id,
            title: "语音时间",
            createdAt: Date(),
            updatedAt: Date(),
            favorite: true,
            tags: ["商务"]
        )
        let processing = ProcessingMetadata(
            schemaVersion: 1,
            capsuleId: id,
            revision: 1,
            durationMs: 8_000,
            status: .rawReady,
            audioFile: "audio.m4a",
            rawTextFile: "raw.txt",
            polishedTextFile: "polished.md",
            errorStage: nil,
            error: nil,
            attempts: 1,
            engine: "tencent-asr",
            model: "16k_zh"
        )
        let record = CapsuleRecord(
            capsule: capsule,
            processing: processing,
            relativeFolder: "Inbox",
            localDirectory: URL(fileURLWithPath: "/tmp/capsule"),
            rawText: "原始转写",
            polishedText: "校对后的内容",
            warnings: []
        )
        XCTAssertEqual(record.displayPreview, "校对后的内容")
        XCTAssertNil(record.visibleProcessingStatus)
    }

    func testCapsuleListRejectsExpandedCorrection() {
        let id = UUID()
        let capsule = CapsuleMetadata(
            id: id,
            title: "语音时间",
            createdAt: Date(),
            updatedAt: Date(),
            favorite: false,
            tags: []
        )
        let processing = ProcessingMetadata(
            schemaVersion: 1,
            capsuleId: id,
            revision: 1,
            durationMs: 8_000,
            status: .ready,
            audioFile: "audio.m4a",
            rawTextFile: "raw.txt",
            polishedTextFile: "polished.md",
            errorStage: nil,
            error: nil,
            attempts: 1,
            engine: "tencent-asr",
            model: "16k_zh"
        )
        let record = CapsuleRecord(
            capsule: capsule,
            processing: processing,
            relativeFolder: "Inbox",
            localDirectory: URL(fileURLWithPath: "/tmp/capsule"),
            rawText: "福斯特建筑事务所商务提案英文翻译",
            polishedText: "这是一段与原始录音长度完全不相称的校对扩写内容，其中包含大量录音里没有的信息，因此不能展示。",
            warnings: []
        )
        XCTAssertEqual(record.displayPreview, "福斯特建筑事务所商务提案英文翻译")
    }

    func testPathPolicyAcceptsChineseAndSpaces() throws {
        XCTAssertEqual(try PathPolicy.validatedRelativeFolder("工作 灵感/上海项目"), "工作 灵感/上海项目")
        XCTAssertEqual(try PathPolicy.validatedTag("#待整理"), "待整理")
    }

    func testPathPolicyRejectsTraversalAndUnsafeNames() {
        for value in ["..", ".", ".hidden", "a/b/c", "a\\b", "a/\n"] {
            XCTAssertThrowsError(try PathPolicy.validatedRelativeFolder(value))
        }
        XCTAssertThrowsError(try PathPolicy.safeRemoteReadPath("/sdcard/PokeCapsule/../Books"))
        XCTAssertThrowsError(try PathPolicy.safeRemoteReadPath("/sdcard/Books"))
    }

    func testPathPolicyHandlesQuotesWithoutShellRules() throws {
        XCTAssertEqual(try PathPolicy.normalizedFolderName("他说“你好”"), "他说“你好”")
        XCTAssertEqual(try PathPolicy.normalizedFolderName("Alice's idea"), "Alice's idea")
    }

    func testDeviceParserStates() {
        let output = """
        List of devices attached
        ABC123 device product:Poke3 model:BOOX_Poke3 transport_id:1
        WAIT unauthorized usb:1-2
        """
        let devices = DeviceParser.parse(output)
        XCTAssertEqual(devices.count, 2)
        XCTAssertEqual(devices[0].model, "BOOX_Poke3")
        XCTAssertEqual(DeviceParser.state(for: [devices[0]]), .connected(devices[0]))
        XCTAssertEqual(DeviceParser.state(for: [devices[1]]), .unauthorized(["WAIT"]))
        XCTAssertEqual(DeviceParser.state(for: []), .noDevice)
        XCTAssertEqual(DeviceParser.state(for: [], adbExists: false), .noADB)
    }

    func testFixtureCompatibleWithSharedProtocol() throws {
        let capsuleJSON = """
        {
          "schemaVersion": 1,
          "id": "0d95b7c1-7ce9-4a91-aea2-b64707a05c9f",
          "title": "新胶囊",
          "createdAt": "2026-07-28T08:30:00Z",
          "updatedAt": "2026-07-28T08:30:00Z",
          "revision": 1,
          "favorite": false,
          "tags": [],
          "language": "zh",
          "contentHash": null
        }
        """
        let value = try PokeJSON.decoder.decode(CapsuleMetadata.self, from: Data(capsuleJSON.utf8))
        XCTAssertEqual(value.id.uuidString.lowercased(), "0d95b7c1-7ce9-4a91-aea2-b64707a05c9f")
        XCTAssertEqual(value.language, "zh")
    }

    func testScannerFlagsDuplicateUUIDAndUnknownSchemaReadOnly() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let id = UUID()
        try writeCapsule(root: root, folder: "Inbox", id: id, schemaVersion: 1)
        try writeCapsule(root: root, folder: "Archive", id: id, schemaVersion: 2)
        let index = CapsuleScanner().scan(root: root)
        XCTAssertEqual(index.records.count, 2)
        XCTAssertTrue(index.warnings.contains(where: { $0.contains("重复 UUID") }))
        XCTAssertEqual(index.records.filter(\.readOnly).count, 1)
    }

    func testExportVerifiesAllFilesAndRejectsConflict() throws {
        let root = try makeTemporaryDirectory()
        let output = try makeTemporaryDirectory()
        defer {
            try? FileManager.default.removeItem(at: root)
            try? FileManager.default.removeItem(at: output)
        }
        try writeCapsule(root: root, folder: "Inbox", id: UUID(), schemaVersion: 1)
        let record = try XCTUnwrap(CapsuleScanner().scan(root: root).records.first)
        let exported = try CapsuleExporter().export([record], to: output)
        XCTAssertEqual(exported.count, 1)
        XCTAssertNoThrow(try FileDigest.verifyCopy(from: record.localDirectory, to: exported[0]))
        XCTAssertThrowsError(try CapsuleExporter().export([record], to: output))
    }

    func testImportPackageRequiresCompleteCapsule() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        XCTAssertThrowsError(try CapsulePackage.inspect(root))
        try writeCapsule(root: root, folder: "Inbox", id: UUID(), schemaVersion: 1)
        let capsuleDirectory = try XCTUnwrap(
            FileManager.default.enumerator(at: root.appendingPathComponent("Inbox"), includingPropertiesForKeys: nil)?
                .compactMap { $0 as? URL }
                .first(where: { $0.lastPathComponent == "capsule.json" })?
                .deletingLastPathComponent()
        )
        let package = try CapsulePackage.inspect(capsuleDirectory)
        XCTAssertTrue(package.manifest.keys.contains("audio.m4a"))
    }

    func testADBUsesArgumentArrayForHostilePath() throws {
        let runner = RecordingRunner()
        let transport = ADBTransport(executable: URL(fileURLWithPath: "/fake/adb"), serial: "SERIAL", runner: runner)
        let local = URL(fileURLWithPath: "/tmp/目录 'quoted'\nline")
        _ = try transport.pull(remote: "/sdcard/PokeCapsule/Inbox", local: local)
        XCTAssertEqual(runner.calls.first?.1, ["-s", "SERIAL", "pull", "/sdcard/PokeCapsule/Inbox", local.path])
    }

    func testStayAwakeReadsAndRestoresOriginalValue() throws {
        let runner = SequencedRunner(results: [
            ProcessResult(status: 0, stdout: "1\n", stderr: ""),
            ProcessResult(status: 0, stdout: "", stderr: ""),
            ProcessResult(status: 0, stdout: "", stderr: "")
        ])
        let transport = ADBTransport(executable: URL(fileURLWithPath: "/fake/adb"), serial: "SERIAL", runner: runner)
        let session = StayAwakeSession(transport: transport)
        try session.begin()
        try session.restore()
        XCTAssertEqual(runner.calls[0], ["-s", "SERIAL", "shell", "settings", "get", "global", "stay_on_while_plugged_in"])
        XCTAssertEqual(runner.calls[1].last, "3")
        XCTAssertEqual(runner.calls[2].last, "1")
    }

    func testStayAwakeDoesNotWriteWhenUSBAlreadyEnabled() throws {
        let runner = SequencedRunner(results: [
            ProcessResult(status: 0, stdout: "7\n", stderr: "")
        ])
        let transport = ADBTransport(executable: URL(fileURLWithPath: "/fake/adb"), serial: "SERIAL", runner: runner)
        let session = StayAwakeSession(transport: transport)
        try session.begin()
        try session.restore()
        XCTAssertEqual(runner.calls.count, 1)
    }

    func testCommandEncodingContainsNoSecretOrShell() throws {
        let command = DeviceCommand(
            operation: "moveCapsules",
            capsuleIds: [UUID()],
            destination: "工作/待办"
        )
        let encoded = try PokeJSON.encoder.encode(command)
        let text = String(decoding: encoded, as: UTF8.self)
        XCTAssertTrue(text.contains("工作"))
        XCTAssertFalse(text.contains("apiKey"))
        XCTAssertFalse(text.contains("shell"))
    }

    func testAPIKeyImportUsesOnlyFirstNonEmptyLine() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("api.txt")
        try Data("\nsk-test-secret\n网页说明\n".utf8).write(to: file)
        let secrets = MemorySecrets()
        let localKey = root.appendingPathComponent("local-key")
        let adapter = CorrectionAdapter(secrets: secrets, localKeyURL: localKey)
        try adapter.importAPIKey(from: file)
        XCTAssertEqual(
            try String(contentsOf: localKey, encoding: .utf8)
                .trimmingCharacters(in: .whitespacesAndNewlines),
            "sk-test-secret")
    }

    func testAPIKeyImportRejectsPastedDocumentation() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let file = root.appendingPathComponent("api.txt")
        try Data("DeepSeek API Docs\n".utf8).write(to: file)
        XCTAssertThrowsError(try CorrectionAdapter(
            secrets: MemorySecrets(),
            localKeyURL: root.appendingPathComponent("local-key")
        ).importAPIKey(from: file))
    }

    func testCorrectionCacheKeyBindsDeviceAndRawText() {
        let id = UUID(uuidString: "0d95b7c1-7ce9-4a91-aea2-b64707a05c9f")!
        let first = CorrectionCacheKey.fileName(
            capsuleID: id, revision: 7, deviceSerial: "POKE-A", rawText: "今天下雨")
        let otherDevice = CorrectionCacheKey.fileName(
            capsuleID: id, revision: 7, deviceSerial: "POKE-B", rawText: "今天下雨")
        let otherText = CorrectionCacheKey.fileName(
            capsuleID: id, revision: 7, deviceSerial: "POKE-A", rawText: "今天晴天")
        XCTAssertNotEqual(first, otherDevice)
        XCTAssertNotEqual(first, otherText)
        XCTAssertTrue(first.hasPrefix(id.uuidString.lowercased() + "-r7-"))
    }

    private func makeTemporaryDirectory() throws -> URL {
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("PokeCapsuleTests-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    private func writeCapsule(root: URL, folder: String, id: UUID, schemaVersion: Int) throws {
        let directory = root.appendingPathComponent(folder).appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let metadata = CapsuleMetadata(
            schemaVersion: schemaVersion,
            id: id,
            title: "中文测试",
            createdAt: Date(timeIntervalSince1970: 100),
            updatedAt: Date(timeIntervalSince1970: 100)
        )
        try PokeJSON.encoder.encode(metadata).write(to: directory.appendingPathComponent("capsule.json"))
        let processing = """
        {"schemaVersion":1,"capsuleId":"\(id.uuidString)","revision":1,"durationMs":1000,"status":"queued","audioFile":"audio.m4a"}
        """
        try Data(processing.utf8).write(to: directory.appendingPathComponent("processing.json"))
        try Data("audio".utf8).write(to: directory.appendingPathComponent("audio.m4a"))
    }
}

private final class MemorySecrets: SecretStoring {
    var values: [String: String] = [:]

    func set(_ value: String, account: String) throws {
        values[account] = value
    }

    func get(account: String) throws -> String? {
        values[account]
    }

    func delete(account: String) throws {
        values.removeValue(forKey: account)
    }
}

private final class RecordingRunner: ProcessExecuting {
    var calls: [(URL, [String])] = []
    func run(executable: URL, arguments: [String]) -> ProcessResult {
        calls.append((executable, arguments))
        return ProcessResult(status: 0, stdout: "", stderr: "")
    }
}

private final class SequencedRunner: ProcessExecuting {
    var calls: [[String]] = []
    private var results: [ProcessResult]

    init(results: [ProcessResult]) {
        self.results = results
    }

    func run(executable: URL, arguments: [String]) -> ProcessResult {
        calls.append(arguments)
        return results.isEmpty
            ? ProcessResult(status: 1, stdout: "", stderr: "missing test response")
            : results.removeFirst()
    }
}
