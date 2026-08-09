import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionCompletionCoordinatorTests: XCTestCase {
    private let frame = [Int16](repeating: 3, count: 320)

    func testStopAckFollowsTailShortcutReleaseSinkStopAndInputRestore() {
        var trace = [String]()
        let platform = CompletionTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 60, now: 0)))
        for index in 0..<6 {
            XCTAssertTrue(executor.execute(machine.receive(
                sessionId: 60, samples: frame, now: Double(index) * 0.02)))
        }
        XCTAssertEqual(completion.beginEnding(
            sessionId: 60, now: 0.12, machine: &machine, executor: executor), .accepted)
        XCTAssertEqual(completion.tick(
            now: 0.23, machine: &machine, executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") }), .none)
        XCTAssertEqual(completion.tick(
            now: 0.24, machine: &machine, executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") }), .none)
        XCTAssertFalse(trace.contains(where: { $0.hasPrefix("stop-ack-") }))
        XCTAssertEqual(Array(trace.suffix(2)), ["key-up", "sink-stop"])

        XCTAssertEqual(completion.tick(
            now: 0.50, machine: &machine, executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") }), .completed(60))
        XCTAssertEqual(Array(trace.suffix(2)), ["restore", "stop-ack-60"])
    }

    func testNoAudioSessionAcknowledgesOnlyAfterRestoreWithoutShortcutEvents() {
        var trace = [String]()
        let platform = CompletionTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 61, now: 0)))
        XCTAssertEqual(completion.beginEnding(
            sessionId: 61, now: 0.01, machine: &machine, executor: executor), .accepted)
        XCTAssertEqual(completion.tick(
            now: 0.13, machine: &machine, executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") }), .none)
        XCTAssertEqual(completion.tick(
            now: 0.39, machine: &machine, executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") }), .completed(61))
        XCTAssertFalse(trace.contains("key-down"))
        XCTAssertFalse(trace.contains("key-up"))
        XCTAssertEqual(Array(trace.suffix(2)), ["restore", "stop-ack-61"])
    }

    func testStaleEndNeverCreatesPendingAckAndSecondStartIsRejectedUntilIdle() {
        var trace = [String]()
        let platform = CompletionTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 62, now: 0)))
        XCTAssertEqual(completion.beginEnding(
            sessionId: 99, now: 0.01, machine: &machine, executor: executor), .ignored)
        XCTAssertNil(completion.pendingStopAcknowledgement)

        XCTAssertEqual(completion.beginEnding(
            sessionId: 62, now: 0.02, machine: &machine, executor: executor), .accepted)
        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 63, now: 0.30, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        XCTAssertEqual(trace.last, "reject-63")
        XCTAssertEqual(completion.pendingStopAcknowledgement, 62)
    }

    func testCancelClearsPendingAckForDisconnectOrAbort() {
        let platform = CompletionTracePlatform { _ in }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 64, now: 0)))
        XCTAssertEqual(completion.beginEnding(
            sessionId: 64, now: 0.01, machine: &machine, executor: executor), .accepted)
        completion.cancel()
        XCTAssertNil(completion.pendingStopAcknowledgement)
    }

    func testTickExecutionFailurePreservesMatchingSessionForRuntimeReject() {
        let platform = CompletionTracePlatform { _ in }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 65, now: 0)))
        XCTAssertEqual(completion.beginEnding(
            sessionId: 65, now: 0.01, machine: &machine, executor: executor), .accepted)

        XCTAssertEqual(completion.tick(
            now: 0.14,
            machine: &machine,
            execute: { _ in false },
            acknowledgeStop: { _ in XCTFail("failed completion must not acknowledge") }),
            .aborted(65, .executionFailure))
        XCTAssertNil(completion.pendingStopAcknowledgement)
        XCTAssertEqual(machine.phase, .idle)
    }

    func testWatchdogCleanupPrecedesMatchingAbortReject() {
        var trace = [String]()
        let platform = CompletionTracePlatform { trace.append($0) }
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var completion = VoiceSessionCompletionCoordinator()
        XCTAssertTrue(executor.execute(machine.begin(sessionId: 66, now: 0)))
        trace.removeAll()

        let outcome = completion.tick(
            now: 0.41,
            machine: &machine,
            executor: executor,
            acknowledgeStop: { trace.append("stop-ack-\($0)") })
        if case let .aborted(aborted, .watchdog) = outcome {
            trace.append("reject-\(aborted)")
        }
        XCTAssertEqual(outcome, .aborted(66, .watchdog))
        XCTAssertEqual(trace, ["sink-stop", "restore", "reject-66"])
        XCTAssertFalse(trace.contains(where: { $0.hasPrefix("stop-ack-") }))
    }
}

private final class CompletionTracePlatform: VoicePlatformAdapter {
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
