import Foundation
import XCTest
@testable import PokeCapsuleCore

final class PokePodTransportTests: XCTestCase {
    func testLivePokePodReadOnlyHandshakeWhenRequested() throws {
        guard let path = ProcessInfo.processInfo.environment["POKEPOD_LIVE_PORT"],
              !path.isEmpty else {
            throw XCTSkip("设置 POKEPOD_LIVE_PORT 后运行只读真机验收")
        }
        let transport = try PokePodTransport(deviceURL: URL(fileURLWithPath: path))
        let hello = try transport.hello()
        let status = try transport.status()
        let identity = try XCTUnwrap(transport.readDeviceIdentity())
        let fingerprint = try transport.metadataFingerprint()

        XCTAssertEqual(hello["status"] as? String, "ok")
        XCTAssertEqual(status["status"] as? String, "ok")
        XCTAssertEqual(status["host_connected"] as? Bool, true)
        XCTAssertTrue(identity.isPokePodIdentity)
        XCTAssertFalse(fingerprint.isEmpty)
        print("LIVE_POKEPOD identity=\(identity.deviceId) platform=\(identity.platform) host_connected=true fingerprint=\(fingerprint)")
    }

    func testFrameRoundTripAndStreamingParser() throws {
        let frame = LinkV2Frame(
            type: .requestJSON, flags: 3, requestID: 0x1020_3040,
            payload: Data("{\"operation\":\"hello\"}".utf8))
        let encoded = try frame.encoded()
        var parser = LinkV2FrameParser()
        XCTAssertEqual(try parser.append(encoded.prefix(7)), [])
        XCTAssertEqual(try parser.append(encoded.dropFirst(7)), [frame])
    }

    func testParserRejectsBadMagicVersionLengthAndCRC() throws {
        let valid = try LinkV2Frame(
            type: .responseJSON, requestID: 7,
            payload: Data("{\"status\":\"ok\"}".utf8)).encoded()

        var badMagic = valid
        badMagic[0] = 0
        var magicParser = LinkV2FrameParser()
        XCTAssertThrowsError(try magicParser.append(badMagic)) {
            XCTAssertEqual($0 as? LinkV2Error, .badMagic)
        }

        var badVersion = valid
        badVersion[4] = 3
        var versionParser = LinkV2FrameParser()
        XCTAssertThrowsError(try versionParser.append(badVersion)) {
            XCTAssertEqual($0 as? LinkV2Error, .unsupportedVersion(3))
        }

        var tooLong = valid
        let length = UInt32(LinkV2Frame.maxControlLength + 1).littleEndian
        withUnsafeBytes(of: length) { tooLong.replaceSubrange(12..<16, with: $0) }
        var lengthParser = LinkV2FrameParser()
        XCTAssertThrowsError(try lengthParser.append(tooLong)) {
            XCTAssertEqual($0 as? LinkV2Error, .payloadTooLarge(4_097))
        }

        var badCRC = valid
        badCRC[badCRC.count - 1] ^= 0xff
        var crcParser = LinkV2FrameParser()
        XCTAssertThrowsError(try crcParser.append(badCRC)) {
            XCTAssertEqual($0 as? LinkV2Error, .crcMismatch)
        }
    }

    func testRegistryRejectsDuplicateCompletedRequestID() throws {
        let registry = LinkRequestRegistry()
        try registry.markCompleted(9)
        XCTAssertThrowsError(try registry.markCompleted(9)) {
            XCTAssertEqual($0 as? LinkV2Error, .duplicateRequestID(9))
        }
    }

    func testClientRetriesBusyAndReportsDisconnect() throws {
        let busy = ScriptedLinkChannel(busyResponses: 1)
        let client = PokePodLinkClient(channel: busy, maxBusyRetries: 2)
        XCTAssertEqual(try client.call(.status).control["status"] as? String, "ok")
        XCTAssertEqual(busy.requestCount, 2)

        let disconnected = DisconnectingLinkChannel()
        XCTAssertThrowsError(try PokePodLinkClient(channel: disconnected).call(.hello)) {
            XCTAssertEqual($0 as? LinkV2Error, .disconnected)
        }
    }

    func testClientSupportsNonzeroSessionRequestIDSeed() throws {
        let channel = ScriptedLinkChannel()
        let client = PokePodLinkClient(channel: channel, initialRequestID: 0x1234_5678)
        _ = try client.call(.status)
        XCTAssertEqual(channel.requestIDs, [0x1234_5678])
    }

    func testClientRejectsDuplicateResponseRequestIDInReceivePath() throws {
        let channel = DuplicateResponseLinkChannel()
        let client = PokePodLinkClient(channel: channel, initialRequestID: 1)
        XCTAssertThrowsError(try client.call(.status)) {
            XCTAssertEqual($0 as? LinkV2Error, .duplicateRequestID(1))
        }
    }

    func testPokePodDictationHoldSendsStartThenStopOperations() throws {
        let channel = ScriptedLinkChannel()
        let transport = try PokePodTransport(
            deviceURL: URL(fileURLWithPath: "/dev/cu.PokePod-contract"),
            channel: channel)

        try transport.beginDictationHold()
        try transport.endDictationHold()

        XCTAssertEqual(channel.operations, ["dictate-start", "dictate-stop"])
        XCTAssertFalse(channel.operations.contains("dictate"))
    }

    func testPokePodIdentityPreservesPlatformAndMatchesLegacyRegistration() throws {
        let channel = ScriptedLinkChannel()
        let transport = try PokePodTransport(
            deviceURL: URL(fileURLWithPath: "/dev/cu.usbmodem-new"),
            channel: channel)
        let identity = try XCTUnwrap(transport.readDeviceIdentity())
        XCTAssertEqual(identity.platform, "pokepod")

        let legacy = RegisteredDevice(
            deviceId: "POKEPOD-CONTRACT",
            displayName: "PokePod",
            platform: "android",
            serialAliases: ["/dev/cu.usbmodem-old"])
        XCTAssertTrue(legacy.isPokePod)
        let match = PokePodPortMatcher.match(
            registered: legacy,
            discoveredPorts: [URL(fileURLWithPath: "/dev/cu.usbmodem-new")]
        ) { _ in identity }
        XCTAssertEqual(match?.path, "/dev/cu.usbmodem-new")
    }

    func testPokePodMatcherUsesExactAliasWithoutProbingAndIgnoresAndroid() throws {
        let current = URL(fileURLWithPath: "/dev/cu.usbmodem-current")
        let legacy = RegisteredDevice(
            deviceId: "pokepod-serial",
            displayName: "PokePod",
            platform: "android",
            serialAliases: [current.path])
        var probeCount = 0
        XCTAssertEqual(PokePodPortMatcher.match(
            registered: legacy,
            discoveredPorts: [current]
        ) { _ in
            probeCount += 1
            return nil
        }, current)
        XCTAssertEqual(probeCount, 0)

        let phone = RegisteredDevice(
            deviceId: "phone", displayName: "Android", serialAliases: [current.path])
        XCTAssertNil(PokePodPortMatcher.match(
            registered: phone,
            discoveredPorts: [current]
        ) { _ in
            probeCount += 1
            return nil
        })
        XCTAssertEqual(probeCount, 0)
    }

    func testV1AndV2AudioMetadataAndSafeBasename() throws {
        let id = UUID()
        let legacy = ProcessingMetadata(
            schemaVersion: 1, capsuleId: id, revision: 1, durationMs: 1_000,
            status: .queued, audioFile: "audio.m4a", rawTextFile: nil,
            polishedTextFile: nil, errorStage: nil, error: nil, attempts: 0,
            engine: nil, model: nil)
        var m4a = legacy
        m4a.schemaVersion = 2
        m4a.audioFormat = "m4a-aac-lc"
        m4a.sampleRateHz = 16_000
        m4a.channels = 1
        m4a.bitsPerSample = 16
        var wav = m4a
        wav.audioFile = "audio.wav"
        wav.audioFormat = "wav-pcm-s16le"
        XCTAssertTrue(AudioPathPolicy.isSupported(legacy))
        XCTAssertTrue(AudioPathPolicy.isSupported(m4a))
        XCTAssertTrue(AudioPathPolicy.isSupported(wav))
        wav.audioFile = "../audio.wav"
        XCTAssertFalse(AudioPathPolicy.isSupported(wav))
        XCTAssertThrowsError(try AudioPathPolicy.resolve(wav.audioFile, in: URL(fileURLWithPath: "/tmp/c")))
    }

    func testUnknownProcessingSchemaWithUnsafeAudioRemainsVisibleReadOnly() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("UnknownProcessing-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let inbox = root.appendingPathComponent("Inbox", isDirectory: true)
        let capsuleID = UUID()
        let capsule = inbox.appendingPathComponent(capsuleID.uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: capsule, withIntermediateDirectories: true)
        let metadata = """
        {"schemaVersion":1,"id":"\(capsuleID.uuidString.lowercased())","revision":1,"title":"未来胶囊","createdAt":"2026-08-07T00:00:00Z","updatedAt":"2026-08-07T00:00:00Z","favorite":false,"tags":[]}
        """
        let processing = """
        {"schemaVersion":99,"capsuleId":"\(capsuleID.uuidString.lowercased())","revision":1,"durationMs":1000,"status":"queued","audioFile":"../outside.wav"}
        """
        try Data(metadata.utf8).write(to: capsule.appendingPathComponent("capsule.json"))
        try Data(processing.utf8).write(to: capsule.appendingPathComponent("processing.json"))

        let index = CapsuleScanner().scan(root: root)
        XCTAssertEqual(index.records.count, 1)
        XCTAssertTrue(index.records[0].readOnly)
        XCTAssertTrue(index.records[0].warnings.contains { $0.contains("暂不支持") })
        XCTAssertTrue(index.records[0].warnings.contains("缺少原始音频"))
    }

    func testScannerAndImportReadV2WavWithoutMigratingIt() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("V2Wav-\(UUID().uuidString)", isDirectory: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let id = UUID()
        let capsule = root.appendingPathComponent("Inbox/\(id.uuidString.lowercased())", isDirectory: true)
        try FileManager.default.createDirectory(at: capsule, withIntermediateDirectories: true)
        let now = "2026-08-07T00:00:00Z"
        try Data("""
        {"schemaVersion":1,"id":"\(id.uuidString)","title":"WAV","createdAt":"\(now)","updatedAt":"\(now)","revision":1,"favorite":false,"tags":[]}
        """.utf8).write(to: capsule.appendingPathComponent("capsule.json"))
        try Data("""
        {"schemaVersion":2,"capsuleId":"\(id.uuidString)","revision":1,"durationMs":1000,"status":"recorded","audioFile":"audio.wav","audioFormat":"wav-pcm-s16le","sampleRateHz":16000,"channels":1,"bitsPerSample":16}
        """.utf8).write(to: capsule.appendingPathComponent("processing.json"))
        try Data("RIFF".utf8).write(to: capsule.appendingPathComponent("audio.wav"))

        let index = CapsuleScanner().scan(root: root)
        XCTAssertEqual(index.records.count, 1)
        XCTAssertFalse(index.records[0].readOnly)
        XCTAssertEqual(index.records[0].audioURL?.lastPathComponent, "audio.wav")
        XCTAssertEqual(try CapsulePackage.inspect(capsule).metadata.id, id)

        let stored = try Data(contentsOf: capsule.appendingPathComponent("processing.json"))
        XCTAssertTrue(String(decoding: stored, as: UTF8.self).contains("\"schemaVersion\":2"))
    }

    func testSharedProtocolFixturesDecodeAcrossV1AndV2() throws {
        var projects = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        for _ in 0..<3 { projects.deleteLastPathComponent() }
        let fixtures = projects.appendingPathComponent("pokecapsule-protocol/fixtures")
        for name in ["processing-v1-m4a.json", "processing-v2-m4a.json", "processing-v2-wav.json"] {
            let metadata = try PokeJSON.decoder.decode(
                ProcessingMetadata.self,
                from: Data(contentsOf: fixtures.appendingPathComponent(name)))
            XCTAssertTrue(AudioPathPolicy.isSupported(metadata), name)
        }
    }

    func testADBAndPokePodRunTheSameTransportContract() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent("TransportContract-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let adb = ADBTransport(
            executable: URL(fileURLWithPath: "/fake/adb"), serial: "ANDROID-CONTRACT",
            runner: ContractADBRunner())
        try assertTransportContract(adb, root: root.appendingPathComponent("adb"))

        let channel = ScriptedLinkChannel()
        let pokePod = try PokePodTransport(
            deviceURL: URL(fileURLWithPath: "/dev/cu.PokePod-contract"), channel: channel)
        try assertTransportContract(pokePod, root: root.appendingPathComponent("pokepod"))
    }

    private func assertTransportContract(
        _ transport: any DeviceTransport,
        root: URL
    ) throws {
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        XCTAssertNotNil(try transport.readDeviceIdentity())
        XCTAssertFalse(try transport.metadataFingerprint().isEmpty)

        let mirror = root.appendingPathComponent("mirror", isDirectory: true)
        try transport.fetchLibrary(to: mirror)
        XCTAssertTrue(FileManager.default.fileExists(atPath: mirror.path))

        let transactionID = UUID()
        let command = root.appendingPathComponent("command.json")
        try Data("{}".utf8).write(to: command)
        try transport.submitCommandFile(local: command, transactionID: transactionID)

        let capsule = root.appendingPathComponent("capsule", isDirectory: true)
        try FileManager.default.createDirectory(at: capsule, withIntermediateDirectories: true)
        try Data("text".utf8).write(to: capsule.appendingPathComponent("polished.md"))
        try transport.stageImport(local: capsule, transactionID: transactionID, capsuleID: UUID())

        let result = root.appendingPathComponent("result.json")
        XCTAssertTrue(try transport.fetchCommandResult(transactionID: transactionID, to: result))
        XCTAssertTrue(FileManager.default.fileExists(atPath: result.path))
        let token = try transport.beginHostSession()
        try transport.endHostSession(token)
    }
}

private final class DisconnectingLinkChannel: LinkV2ByteChannel {
    func write(_ data: Data) throws {}
    func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        throw LinkV2Error.disconnected
    }
}

private final class DuplicateResponseLinkChannel: LinkV2ByteChannel {
    private var parser = LinkV2FrameParser()
    private var response = Data()

    func write(_ data: Data) throws {
        for frame in try parser.append(data) where frame.type == .requestJSON {
            let payload = try JSONSerialization.data(
                withJSONObject: ["status": "ok"], options: [.sortedKeys])
            let encoded = try LinkV2Frame(
                type: .responseJSON, requestID: frame.requestID, payload: payload).encoded()
            response.append(encoded)
            response.append(encoded)
        }
    }

    func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        guard !response.isEmpty else { throw LinkV2Error.timedOut }
        defer { response.removeAll() }
        return response
    }
}

private final class ScriptedLinkChannel: LinkV2ByteChannel {
    private var parser = LinkV2FrameParser()
    private var responses: [Data] = []
    private var busyRemaining: Int
    private(set) var requestCount = 0
    private(set) var operations: [String] = []
    private(set) var requestIDs: [UInt32] = []

    init(busyResponses: Int = 0) {
        busyRemaining = busyResponses
    }

    func write(_ data: Data) throws {
        for frame in try parser.append(data) where frame.type == .requestJSON {
            requestCount += 1
            requestIDs.append(frame.requestID)
            guard let request = try JSONSerialization.jsonObject(with: frame.payload) as? [String: Any],
                  let operation = request["operation"] as? String else {
                throw LinkV2Error.malformedResponse("测试请求缺少 operation")
            }
            operations.append(operation)
            if busyRemaining > 0 {
                busyRemaining -= 1
                try enqueue(["status": "busy", "retryAfterMs": 1], requestID: frame.requestID)
                continue
            }
            switch operation {
            case "identity":
                try enqueue([
                    "status": "ok", "deviceId": "POKEPOD-CONTRACT",
                    "displayName": "PokePod", "platform": "pokepod"
                ], requestID: frame.requestID)
            case "fingerprint":
                try enqueue(["status": "ok", "fingerprint": "contract-fingerprint"], requestID: frame.requestID)
            case "read":
                try enqueue(["status": "ok", "files": []], requestID: frame.requestID)
            case "result":
                try enqueue([
                    "status": "ok", "available": true, "binaryLength": 2
                ], binary: Data("{}".utf8), requestID: frame.requestID)
            default:
                try enqueue(["status": "ok"], requestID: frame.requestID)
            }
        }
    }

    func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        guard !responses.isEmpty else { throw LinkV2Error.timedOut }
        return responses.removeFirst()
    }

    private func enqueue(
        _ object: [String: Any],
        binary: Data? = nil,
        requestID: UInt32
    ) throws {
        responses.append(try LinkV2Frame(
            type: .responseJSON, requestID: requestID,
            payload: JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])).encoded())
        if let binary {
            responses.append(try LinkV2Frame(
                type: .data, flags: 1, requestID: requestID, payload: binary).encoded())
        }
    }
}

private final class ContractADBRunner: ProcessExecuting {
    func run(executable: URL, arguments: [String]) -> ProcessResult {
        if arguments.contains("cat") {
            return ProcessResult(status: 0, stdout: """
            {"schemaVersion":1,"deviceId":"0d95b7c1-7ce9-4a91-aea2-b64707a05c9f","displayName":"Android","platform":"android"}
            """, stderr: "")
        }
        if arguments.contains("settings"), arguments.contains("get") {
            return ProcessResult(status: 0, stdout: "7\n", stderr: "")
        }
        if arguments.contains("pull"), let destination = arguments.last {
            let url = URL(fileURLWithPath: destination)
            if arguments.contains(where: { $0.contains("/.commands/results/") }) {
                try? Data("{}".utf8).write(to: url, options: .atomic)
            } else {
                try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
            }
            return ProcessResult(status: 0, stdout: "pulled", stderr: "")
        }
        if arguments.contains("find /sdcard/PokeCapsule") {
            return ProcessResult(status: 0, stdout: "contract-fingerprint", stderr: "")
        }
        return ProcessResult(status: 0, stdout: "ok", stderr: "")
    }
}
