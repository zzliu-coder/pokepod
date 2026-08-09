import XCTest
@testable import PokePodVoiceCore

final class VoiceActionExecutorTests: XCTestCase {
    func testExecutesNormalOrderExactly() {
        let platform = FakeVoicePlatform()
        let executor = VoiceActionExecutor(platform: platform)
        XCTAssertTrue(executor.execute([
            .saveDefaultInput, .switchToBlackHole, .startAudioSink,
            .shortcutDown, .writeSamples([1, 2]), .shortcutUp,
            .stopAudioSink, .restoreDefaultInput
        ]))
        XCTAssertEqual(platform.events, [
            "save", "switch", "sink-start", "key-down", "write-2",
            "key-up", "sink-stop", "restore"
        ])
    }

    func testAnyPlatformFailureRunsSafeRecoveryOrder() {
        let platform = FakeVoicePlatform(failAt: "sink-start")
        var message = ""
        let executor = VoiceActionExecutor(platform: platform) { message = $0 }
        XCTAssertFalse(executor.execute([
            .saveDefaultInput, .switchToBlackHole, .startAudioSink, .shortcutDown
        ]))
        XCTAssertEqual(platform.events, [
            "save", "switch", "sink-start", "key-up", "sink-stop", "restore"
        ])
        XCTAssertFalse(message.isEmpty)
    }
}

private final class FakeVoicePlatform: VoicePlatformAdapter {
    let failAt: String?
    var events = [String]()
    init(failAt: String? = nil) { self.failAt = failAt }

    func saveDefaultInput() throws { try record("save") }
    func switchToBlackHole() throws { try record("switch") }
    func startAudioSink() throws { try record("sink-start") }
    func write(samples: [Int16]) throws { try record("write-\(samples.count)") }
    func shortcutDown() throws { try record("key-down") }
    func shortcutUp() { events.append("key-up") }
    func stopAudioSink() { events.append("sink-stop") }
    func restoreDefaultInput() { events.append("restore") }

    private func record(_ event: String) throws {
        events.append(event)
        if event == failAt { throw FakeError.failed }
    }

    enum FakeError: Error { case failed }
}
