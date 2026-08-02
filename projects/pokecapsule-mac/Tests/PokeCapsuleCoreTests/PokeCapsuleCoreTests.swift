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

    func testCapsuleListFallsBackToRawWhenCorrectionIsMissing() {
        let id = UUID()
        let raw = "我测试一下，这个新的麦克风好不好用，是不是清晰的？"
        let record = CapsuleRecord(
            capsule: CapsuleMetadata(
                id: id,
                title: "语音时间",
                createdAt: Date(),
                updatedAt: Date()),
            processing: ProcessingMetadata(
                schemaVersion: 1,
                capsuleId: id,
                revision: 1,
                durationMs: 8_800,
                status: .rawReady,
                audioFile: "audio.m4a",
                rawTextFile: "raw.txt",
                polishedTextFile: nil,
                errorStage: nil,
                error: nil,
                attempts: 1,
                engine: "tencent-asr",
                model: "16k_zh"),
            relativeFolder: "Inbox",
            localDirectory: URL(fileURLWithPath: "/tmp/capsule"),
            rawText: raw,
            polishedText: nil,
            warnings: [])
        XCTAssertEqual(record.displayPreview, raw)
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

    func testUSBProbeReportsPhysicalDeviceWhenADBIsUnavailable() {
        let registry = """
          | |   \"USB Product Name\" = \"SDM636-MTP _SN:CDD2F6FE\"
          | |   \"USB Serial Number\" = \"BE87E832\"
          | |   \"UsbExclusiveOwner\" = \"pid 1922, adb\"
          | |   \"USB Vendor Name\" = \"ONYX\"
        """
        let usb = USBDeviceProbe.parseIORegistry(registry)
        XCTAssertEqual(usb.count, 1)
        XCTAssertEqual(usb[0].serial, "BE87E832")
        XCTAssertEqual(usb[0].exclusiveOwner, "pid 1922, adb")
        XCTAssertEqual(
            DeviceParser.state(
                for: [],
                usbDevices: usb,
                preferredSerials: ["BE87E832"]),
            .usbDetectedButADBUnavailable(usb[0]))
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

    func testMetadataFingerprintUsesReadOnlyDeviceScan() throws {
        let runner = SequencedRunner(results: [
            ProcessResult(
                status: 0,
                stdout: "/sdcard/PokeCapsule/Inbox/id/raw.txt|12|100\n",
                stderr: "")
        ])
        let transport = ADBTransport(
            executable: URL(fileURLWithPath: "/fake/adb"),
            serial: "SERIAL",
            runner: runner)
        let fingerprint = try transport.metadataFingerprint()
        XCTAssertEqual(fingerprint, "/sdcard/PokeCapsule/Inbox/id/raw.txt|12|100")
        XCTAssertEqual(Array(runner.calls[0].prefix(5)), [
            "-s", "SERIAL", "exec-out", "sh", "-c"
        ])
        let script = try XCTUnwrap(runner.calls[0].last)
        XCTAssertTrue(script.contains("find /sdcard/PokeCapsule"))
        XCTAssertTrue(script.contains("processing.json"))
        XCTAssertFalse(script.contains(" rm "))
        XCTAssertFalse(script.contains("settings put"))
    }

    func testReadsPermanentDeviceIdentity() throws {
        let runner = SequencedRunner(results: [
            ProcessResult(status: 0, stdout: """
            {
              "schemaVersion": 1,
              "deviceId": "96771507-e949-4073-861d-823862d78217",
              "displayName": "Vivo X Fold3",
              "platform": "android",
              "manufacturer": "vivo",
              "model": "V2303A",
              "androidVersion": "16",
              "createdAt": "2026-07-28T14:29:25.281Z"
            }
            """, stderr: "")
        ])
        let transport = ADBTransport(
            executable: URL(fileURLWithPath: "/fake/adb"),
            serial: "PHONE",
            runner: runner)
        let identity = try XCTUnwrap(transport.readDeviceIdentity())
        XCTAssertEqual(identity.displayName, "Vivo X Fold3")
        XCTAssertEqual(identity.deviceId, "96771507-e949-4073-861d-823862d78217")
        XCTAssertEqual(runner.calls.first, [
            "-s", "PHONE", "exec-out", "cat", "/sdcard/PokeCapsule/device.json"
        ])
    }

    func testDeviceRegistryPersistsIndependentDevices() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let store = DeviceRegistryStore(file: root.appendingPathComponent("registry.json"))
        let devices = [
            RegisteredDevice(
                deviceId: "poke",
                displayName: "Poke3",
                serialAliases: ["BE87E832"]),
            RegisteredDevice(
                deviceId: "phone",
                displayName: "Vivo X Fold3",
                model: "V2303A",
                serialAliases: ["10AE3Q07J4000UK"])
        ]
        try store.save(devices)
        XCTAssertEqual(try store.load(), devices)
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

    func testOfflineQueuePersistsCommandsAndFinalText() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let queue = OfflineQueue(file: root.appendingPathComponent("pending.json"))
        let id = UUID()
        let command = DeviceCommand(
            operation: "commitFinalText",
            capsuleIds: [id],
            expectedRevision: 4,
            finalText: "福斯特建筑事务所商务提案英文翻译")
        _ = try queue.enqueue(command)

        let loaded = try queue.load()
        XCTAssertEqual(loaded.count, 1)
        XCTAssertEqual(loaded[0].command.finalText, "福斯特建筑事务所商务提案英文翻译")
        XCTAssertEqual(loaded[0].command.expectedRevision, 4)
        XCTAssertEqual(loaded[0].state, .queued)
    }

    func testSyncCoordinatorStopsConflictingMutationBeforeExecution() {
        let id = UUID()
        let metadata = CapsuleMetadata(
            id: id,
            createdAt: Date(),
            updatedAt: Date(),
            revision: 5)
        let record = CapsuleRecord(
            capsule: metadata,
            processing: nil,
            relativeFolder: "Inbox",
            localDirectory: URL(fileURLWithPath: "/tmp/\(id.uuidString)"),
            rawText: "原始文字",
            polishedText: nil,
            warnings: [])
        let command = DeviceCommand(
            operation: "moveCapsules",
            capsuleIds: [id],
            destination: "工作",
            expectedRevisions: [id.uuidString: 4])
        var executed = false

        let result = SyncCoordinator().replay(
            pending: [PendingCommand(command: command)],
            currentIndex: CapsuleIndex(records: [record])
        ) { _ in
            executed = true
            return CapsuleIndex(records: [record])
        }

        XCTAssertFalse(executed)
        XCTAssertEqual(result.pending.first?.state, .conflict)
        XCTAssertTrue(result.pending.first?.error?.contains("版本冲突") == true)
    }

    func testTrashRevisionProtectsOfflinePermanentDelete() {
        let id = UUID()
        let metadata = CapsuleMetadata(
            id: id,
            createdAt: Date(),
            updatedAt: Date(),
            revision: 2)
        let record = CapsuleRecord(
            capsule: metadata,
            processing: nil,
            relativeFolder: "回收站",
            localDirectory: URL(fileURLWithPath: "/tmp/\(id.uuidString)"),
            rawText: nil,
            polishedText: nil,
            trash: TrashMetadata(
                capsuleId: id,
                trashedAt: Date(),
                originalFolder: "Inbox",
                revision: 6),
            warnings: [])
        let command = DeviceCommand(
            operation: "purgeCapsules",
            capsuleIds: [id],
            expectedRevisions: [id.uuidString.lowercased(): 5])
        var executed = false
        let result = SyncCoordinator().replay(
            pending: [PendingCommand(command: command)],
            currentIndex: CapsuleIndex(trashRecords: [record])
        ) { _ in
            executed = true
            return CapsuleIndex()
        }
        XCTAssertFalse(executed)
        XCTAssertEqual(result.pending.first?.state, .conflict)
    }

    func testSyncCoordinatorReplaysSameCapsuleInOrder() {
        let id = UUID()
        func record(revision: Int) -> CapsuleRecord {
            CapsuleRecord(
                capsule: CapsuleMetadata(
                    id: id,
                    createdAt: Date(),
                    updatedAt: Date(),
                    revision: revision),
                processing: nil,
                relativeFolder: "Inbox",
                localDirectory: URL(fileURLWithPath: "/tmp/\(id.uuidString)"),
                rawText: nil,
                polishedText: nil,
                warnings: [])
        }
        let first = DeviceCommand(
            operation: "setFavorite",
            capsuleIds: [id],
            favorite: true,
            expectedRevisions: [id.uuidString.lowercased(): 1])
        let second = DeviceCommand(
            operation: "addTags",
            capsuleIds: [id],
            tags: ["商务"],
            expectedRevisions: [id.uuidString.lowercased(): 2])
        var revision = 1
        let result = SyncCoordinator().replay(
            pending: [PendingCommand(command: first), PendingCommand(command: second)],
            currentIndex: CapsuleIndex(records: [record(revision: revision)])
        ) { _ in
            revision += 1
            return CapsuleIndex(records: [record(revision: revision)])
        }
        XCTAssertEqual(result.appliedCount, 2)
        XCTAssertTrue(result.pending.isEmpty)
        XCTAssertEqual(result.index.records.first?.capsule.revision, 3)
    }

    func testScannerReadsFinalTextAndTrashMetadata() throws {
        let root = try makeTemporaryDirectory()
        defer { try? FileManager.default.removeItem(at: root) }
        let activeID = UUID()
        try writeCapsule(root: root, folder: "Inbox", id: activeID, schemaVersion: 1)
        let activeDirectory = try XCTUnwrap(
            FileManager.default.enumerator(
                at: root.appendingPathComponent("Inbox"),
                includingPropertiesForKeys: nil)?
                .compactMap { $0 as? URL }
                .first(where: { $0.lastPathComponent == "capsule.json" })?
                .deletingLastPathComponent())
        try Data("用户最终文字".utf8).write(to: activeDirectory.appendingPathComponent("final.md"))

        let trashID = UUID()
        try writeCapsule(root: root, folder: ".trash", id: trashID, schemaVersion: 1)
        let trashRoot = root.appendingPathComponent(".trash")
        let trashDirectory = try XCTUnwrap(
            FileManager.default.contentsOfDirectory(
                at: trashRoot,
                includingPropertiesForKeys: [.isDirectoryKey])
                .first(where: { $0.lastPathComponent != "trash.json" }))
        let trash = TrashMetadata(
            capsuleId: trashID,
            trashedAt: Date(),
            originalFolder: "工作/提案",
            revision: 2)
        try PokeJSON.encoder.encode(trash).write(
            to: trashDirectory.appendingPathComponent("trash.json"))

        let index = CapsuleScanner().scan(root: root)
        XCTAssertEqual(index.records.first(where: { $0.id == activeID })?.finalText, "用户最终文字")
        XCTAssertEqual(index.trashRecords.first?.trash?.originalFolder, "工作/提案")
    }

    func testBackupIncludesHiddenTrashAndSearchUsesFinalText() throws {
        let mirror = try makeTemporaryDirectory()
        let backups = try makeTemporaryDirectory()
        defer {
            try? FileManager.default.removeItem(at: mirror)
            try? FileManager.default.removeItem(at: backups)
        }
        let trash = mirror.appendingPathComponent(".trash/one", isDirectory: true)
        try FileManager.default.createDirectory(at: trash, withIntermediateDirectories: true)
        try Data("audio".utf8).write(to: trash.appendingPathComponent("audio.m4a"))
        let destination = try BackupManager(root: backups).create(
            from: mirror,
            deviceSerial: "BE87E832",
            capsuleCount: 1)
        let manifest = try PokeJSON.decoder.decode(
            BackupManifest.self,
            from: Data(contentsOf: destination.appendingPathComponent("manifest.json")))
        XCTAssertNotNil(
            manifest.files[".trash/one/audio.m4a"],
            "备份清单实际包含：\(manifest.files.keys.sorted())")

        let id = UUID()
        let record = CapsuleRecord(
            capsule: CapsuleMetadata(id: id, createdAt: Date(), updatedAt: Date()),
            processing: nil,
            relativeFolder: "Inbox",
            localDirectory: mirror,
            rawText: nil,
            polishedText: nil,
            finalText: "福斯特建筑事务所商务提案英文翻译",
            warnings: [])
        XCTAssertEqual(CapsuleSearch.filter([record], query: "商务提案").map(\.id), [id])
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
