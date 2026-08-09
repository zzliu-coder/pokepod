import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionSignalPolicyTests: XCTestCase {
    func testStaleRemoteErrorCannotAbortNewActiveSession() {
        var trace = [String]()
        let platform = SignalTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 50, now: 0)))
        let frame = [Int16](repeating: 1, count: 320)
        for index in 0..<6 {
            XCTAssertTrue(executor.execute(machine.receive(
                sessionId: 50, samples: frame, now: Double(index) * 0.02)))
        }
        trace.removeAll()

        XCTAssertFalse(VoiceSessionSignalPolicy.remoteErrorTargetsActive(
            reportedSessionId: 49, activeSessionId: machine.activeSessionId))
        XCTAssertFalse(VoiceSessionSignalPolicy.remoteErrorTargetsActive(
            reportedSessionId: 0, activeSessionId: machine.activeSessionId))
        XCTAssertTrue(VoiceSessionSignalPolicy.remoteErrorTargetsActive(
            reportedSessionId: 50, activeSessionId: machine.activeSessionId))

        XCTAssertTrue(executor.execute(machine.receive(
            sessionId: 50, samples: frame, now: 0.14)))
        XCTAssertEqual(machine.phase, .streaming(50))
        XCTAssertEqual(trace, ["write"])
        XCTAssertFalse(trace.contains("key-up"))
        XCTAssertFalse(trace.contains("restore"))
    }
}

private final class SignalTracePlatform: VoicePlatformAdapter {
    private let trace: (String) -> Void
    init(trace: @escaping (String) -> Void) { self.trace = trace }
    func saveDefaultInput() throws { trace("save") }
    func switchToBlackHole() throws { trace("switch") }
    func startAudioSink() throws { trace("sink-start") }
    func write(samples: [Int16]) throws { trace("write") }
    func shortcutDown() throws { trace("key-down") }
    func shortcutUp() { trace("key-up") }
    func stopAudioSink() { trace("sink-stop") }
    func restoreDefaultInput() { trace("restore") }
}
