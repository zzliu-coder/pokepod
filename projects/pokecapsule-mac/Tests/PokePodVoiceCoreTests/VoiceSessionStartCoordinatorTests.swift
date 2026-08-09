import Foundation
import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionStartCoordinatorTests: XCTestCase {
    func testSessionStartPreparesPlatformBeforeSendingMatchingReadyWire() throws {
        var trace = ["signal-session-start"]
        let platform = StartTracePlatform(trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var readyWire: Data?

        XCTAssertTrue(VoiceSessionStartCoordinator.start(
            sessionId: 0x1234_5678,
            now: 1,
            machine: &machine,
            executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { sessionId in
                trace.append("ready-\(sessionId)")
                readyWire = BLEVoiceCommand(type: .ready, sessionId: sessionId).encoded()
            },
            reject: { trace.append("reject-\($0)") }))

        XCTAssertEqual(trace, [
            "signal-session-start", "save", "switch", "sink-start",
            "jitter-305419896", "ready-305419896"
        ])
        let command = try BLEVoiceCommand.decode(XCTUnwrap(readyWire))
        XCTAssertEqual(command.type, .ready)
        XCTAssertEqual(command.sessionId, 0x1234_5678)
        XCTAssertNotEqual(command.sessionId, 0)
        XCTAssertEqual(machine.phase, .preparing(0x1234_5678))
    }

    func testPlatformFailureRejectsWithoutSessionReady() {
        var trace = ["signal-session-start"]
        let platform = StartTracePlatform(failAt: "sink-start", trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()

        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 9,
            now: 1,
            machine: &machine,
            executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))

        XCTAssertEqual(trace, [
            "signal-session-start", "save", "switch", "sink-start",
            "key-up", "sink-stop", "restore", "reject-9"
        ])
        XCTAssertFalse(trace.contains(where: { $0.hasPrefix("ready-") }))
        XCTAssertFalse(trace.contains(where: { $0.hasPrefix("jitter-") }))
        XCTAssertEqual(machine.phase, .idle)
    }

    func testZeroSessionIsRejectedBeforePlatformSideEffects() {
        var trace = ["signal-session-start"]
        let platform = StartTracePlatform(trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()

        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 0,
            now: 1,
            machine: &machine,
            executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        XCTAssertEqual(trace, ["signal-session-start", "reject-0"])
    }

    func testDuplicateAndStaleStartsCannotPreemptPreparingSession() {
        var trace = [String]()
        let platform = StartTracePlatform(trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        XCTAssertTrue(VoiceSessionStartCoordinator.start(
            sessionId: 40, now: 0, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        let establishedTrace = trace

        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 40, now: 0.01, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 39, now: 0.02, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))

        XCTAssertEqual(establishedTrace, ["save", "switch", "sink-start", "jitter-40", "ready-40"])
        XCTAssertEqual(trace, establishedTrace + ["reject-39"])
        XCTAssertEqual(machine.phase, .preparing(40))
    }

    func testStaleStartCannotResetHeldShortcutOrInputOwnership() {
        var trace = [String]()
        let platform = StartTracePlatform(trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        XCTAssertTrue(VoiceSessionStartCoordinator.start(
            sessionId: 50, now: 0, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        let frame = [Int16](repeating: 1, count: 320)
        for index in 0..<6 {
            XCTAssertTrue(executor.execute(machine.receive(
                sessionId: 50, samples: frame, now: Double(index) * 0.02)))
        }
        XCTAssertEqual(machine.phase, .streaming(50))
        let beforeStale = trace

        XCTAssertFalse(VoiceSessionStartCoordinator.start(
            sessionId: 49, now: 0.11, machine: &machine, executor: executor,
            prepareAudioStream: { trace.append("jitter-\($0)") },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))
        XCTAssertEqual(trace, beforeStale + ["reject-49"])
        XCTAssertEqual(machine.phase, .streaming(50))
        XCTAssertEqual(trace.filter { $0 == "save" }.count, 1)
        XCTAssertEqual(trace.filter { $0 == "switch" }.count, 1)
        XCTAssertFalse(trace.contains("key-up"))
        XCTAssertFalse(trace.contains("restore"))

        XCTAssertTrue(executor.execute(machine.end(sessionId: 50, now: 0.12)))
        XCTAssertTrue(executor.execute(machine.tick(now: 0.24)))
        XCTAssertEqual(trace.filter { $0 == "key-down" }.count, 1)
        XCTAssertEqual(trace.filter { $0 == "key-up" }.count, 1)
    }

    func testDuplicateAndStaleStartCannotPoisonActiveJitterSession() throws {
        var trace = [String]()
        let platform = StartTracePlatform(trace: { trace.append($0) })
        let executor = VoiceActionExecutor(platform: platform)
        var machine = VoiceSessionMachine()
        var jitter = VoiceJitterBuffer()

        XCTAssertTrue(VoiceSessionStartCoordinator.start(
            sessionId: 50, now: 0, machine: &machine, executor: executor,
            prepareAudioStream: {
                trace.append("jitter-\($0)")
                jitter.reset(sessionId: $0)
            },
            sendSessionReady: { trace.append("ready-\($0)") },
            reject: { trace.append("reject-\($0)") }))

        for sequence in 0..<6 {
            let outputs = try jitter.ingest(makeFrame(session: 50, sequence: UInt32(sequence)))
            for output in outputs {
                XCTAssertTrue(executor.execute(machine.receive(
                    sessionId: 50,
                    samples: output.samples,
                    now: Double(sequence) * 0.02)))
            }
        }
        XCTAssertEqual(machine.phase, .streaming(50))
        let jitterResetsBeforeRejectedStarts = trace.filter { $0.hasPrefix("jitter-") }

        for rejected in [UInt32(50), UInt32(49)] {
            XCTAssertFalse(VoiceSessionStartCoordinator.start(
                sessionId: rejected, now: 0.13, machine: &machine, executor: executor,
                prepareAudioStream: {
                    trace.append("jitter-\($0)")
                    jitter.reset(sessionId: $0)
                },
                sendSessionReady: { trace.append("ready-\($0)") },
                reject: { trace.append("reject-\($0)") }))
        }

        XCTAssertEqual(jitter.sessionId, 50)
        XCTAssertEqual(trace.filter { $0.hasPrefix("jitter-") }, jitterResetsBeforeRejectedStarts)
        XCTAssertEqual(try jitter.ingest(makeFrame(session: 49, sequence: 99)), [])
        XCTAssertEqual(machine.phase, .streaming(50))
        let next = try jitter.ingest(makeFrame(session: 50, sequence: 6))
        XCTAssertEqual(next.map(\.sequence), [6])
        XCTAssertTrue(executor.execute(machine.receive(
            sessionId: 50, samples: next[0].samples, now: 0.14)))
        XCTAssertEqual(machine.phase, .streaming(50))
        XCTAssertFalse(trace.contains("key-up"))
        XCTAssertFalse(trace.contains("restore"))
    }
}

private final class StartTracePlatform: VoicePlatformAdapter {
    private let failAt: String?
    private let trace: (String) -> Void

    init(failAt: String? = nil, trace: @escaping (String) -> Void) {
        self.failAt = failAt
        self.trace = trace
    }

    func saveDefaultInput() throws { try record("save") }
    func switchToBlackHole() throws { try record("switch") }
    func startAudioSink() throws { try record("sink-start") }
    func write(samples: [Int16]) throws { try record("write") }
    func shortcutDown() throws { try record("key-down") }
    func shortcutUp() { trace("key-up") }
    func stopAudioSink() { trace("sink-stop") }
    func restoreDefaultInput() { trace("restore") }

    private func record(_ value: String) throws {
        trace(value)
        if value == failAt { throw StartError.failed }
    }

    enum StartError: Error { case failed }
}
