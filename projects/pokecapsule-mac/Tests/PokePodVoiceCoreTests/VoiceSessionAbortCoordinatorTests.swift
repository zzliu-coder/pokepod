import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionAbortCoordinatorTests: XCTestCase {
    private let frame = [Int16](repeating: 2, count: 320)

    func testLocalAbortCleansUpBeforeMatchingReject() {
        var trace = [String]()
        let platform = AbortTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 70, now: 0)))
        for index in 0..<6 {
            XCTAssertTrue(executor.execute(machine.receive(
                sessionId: 70, samples: frame, now: Double(index) * 0.02)))
        }
        trace.removeAll()

        XCTAssertEqual(VoiceSessionAbortCoordinator.abort(
            reason: "user override",
            report: false,
            wirePolicy: .reject(code: 9),
            machine: &machine,
            completion: &completion,
            executor: executor,
            reject: { trace.append("reject-\($0)-\($1)") }), 70)

        XCTAssertEqual(trace, ["key-up", "sink-stop", "restore", "reject-70-9"])
        XCTAssertEqual(machine.phase, .idle)
    }

    func testDisconnectCleanupNeverWritesToDeadLink() {
        var trace = [String]()
        let platform = AbortTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 71, now: 0)))
        trace.removeAll()

        XCTAssertEqual(VoiceSessionAbortCoordinator.abort(
            reason: "disconnected",
            report: false,
            wirePolicy: .silent,
            machine: &machine,
            completion: &completion,
            executor: executor,
            reject: { trace.append("reject-\($0)-\($1)") }), 71)
        XCTAssertEqual(trace, ["sink-stop", "restore"])
    }

    func testRepairWhileIdleUsesSameIdempotentCleanupWithoutWire() {
        var trace = [String]()
        let platform = AbortTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()

        XCTAssertNil(VoiceSessionAbortCoordinator.abort(
            reason: "repair",
            report: false,
            wirePolicy: .reject(code: 9),
            machine: &machine,
            completion: &completion,
            executor: executor,
            reject: { trace.append("reject-\($0)-\($1)") }))
        XCTAssertEqual(trace, ["key-up", "sink-stop", "restore"])
    }
}

private final class AbortTracePlatform: VoicePlatformAdapter {
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
