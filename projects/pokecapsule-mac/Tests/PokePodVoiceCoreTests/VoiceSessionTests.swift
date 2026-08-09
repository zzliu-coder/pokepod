import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionTests: XCTestCase {
    private let frame = [Int16](repeating: 7, count: 320)

    func testPrebufferTailAndRestoreOrder() {
        var machine = VoiceSessionMachine()
        XCTAssertEqual(machine.begin(sessionId: 1, now: 0), [
            .saveDefaultInput, .switchToBlackHole, .startAudioSink
        ])
        for index in 0..<5 {
            XCTAssertEqual(machine.receive(sessionId: 1, samples: frame, now: Double(index) * 0.02), [])
        }
        let ready = machine.receive(sessionId: 1, samples: frame, now: 0.10)
        XCTAssertEqual(ready.first, .shortcutDown)
        if case let .writeSamples(samples) = ready.last {
            XCTAssertEqual(samples.count, 1_920)
        } else { XCTFail("缺少 120 ms 预缓冲") }
        XCTAssertEqual(machine.receive(sessionId: 1, samples: frame, now: 0.12), [.writeSamples(frame)])

        XCTAssertEqual(machine.end(sessionId: 1, now: 0.13), [])
        XCTAssertEqual(machine.receive(sessionId: 1, samples: frame, now: 0.14), [.writeSamples(frame)])
        XCTAssertEqual(machine.tick(now: 0.249), [])
        XCTAssertEqual(machine.tick(now: 0.25), [.shortcutUp, .stopAudioSink])
        XCTAssertEqual(machine.tick(now: 0.499), [])
        XCTAssertEqual(machine.tick(now: 0.50), [.restoreDefaultInput])
        XCTAssertEqual(machine.phase, .idle)
    }

    func testShortSessionStillBalancesShortcut() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 2, now: 0)
        _ = machine.receive(sessionId: 2, samples: frame, now: 0.02)
        let end = machine.end(sessionId: 2, now: 0.03)
        XCTAssertEqual(end, [.shortcutDown, .writeSamples(frame)])
        XCTAssertEqual(machine.tick(now: 0.15), [.shortcutUp, .stopAudioSink])
        XCTAssertEqual(machine.tick(now: 0.40), [.restoreDefaultInput])
    }

    func testEndBeforeFirstAudioStartsShortcutOnFirstTailFrame() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 20, now: 0)
        XCTAssertEqual(machine.end(sessionId: 20, now: 0.02), [])
        XCTAssertEqual(machine.receive(
            sessionId: 20, samples: frame, now: 0.10), [.shortcutDown, .writeSamples(frame)])
        XCTAssertEqual(machine.tick(now: 0.219), [])
        XCTAssertEqual(machine.tick(now: 0.22), [.shortcutUp, .stopAudioSink])
        XCTAssertEqual(machine.tick(now: 0.47), [.restoreDefaultInput])
    }

    func testEndBeforeAudioAcceptsMultipleTailFramesWithOneShortcutPair() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 21, now: 0)
        _ = machine.end(sessionId: 21, now: 0.02)
        XCTAssertEqual(machine.receive(
            sessionId: 21, samples: frame, now: 0.04), [.shortcutDown, .writeSamples(frame)])
        XCTAssertEqual(machine.receive(
            sessionId: 21, samples: frame, now: 0.06), [.writeSamples(frame)])
        XCTAssertEqual(machine.receive(
            sessionId: 21, samples: frame, now: 0.08), [.writeSamples(frame)])
        XCTAssertEqual(machine.tick(now: 0.16), [.shortcutUp, .stopAudioSink])
        XCTAssertEqual(machine.tick(now: 0.42), [.restoreDefaultInput])
    }

    func testEndWithNoAudioNeverCreatesUnpairedShortcutEvents() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 22, now: 0)
        XCTAssertEqual(machine.end(sessionId: 22, now: 0.02), [])
        XCTAssertEqual(machine.tick(now: 0.14), [.stopAudioSink])
        XCTAssertEqual(machine.tick(now: 0.39), [.restoreDefaultInput])
    }

    func testTailErrorAfterLateAudioBalancesShortcutAndRecovery() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 23, now: 0)
        _ = machine.end(sessionId: 23, now: 0.02)
        _ = machine.receive(sessionId: 23, samples: frame, now: 0.04)
        XCTAssertEqual(machine.abort(reason: "尾帧异常"), [
            .shortcutUp, .stopAudioSink, .restoreDefaultInput, .reportFailure("尾帧异常")
        ])
        XCTAssertEqual(machine.phase, .idle)
    }

    func testWatchdogAndExplicitAbortUseSameSafeOrder() {
        var watchdog = VoiceSessionMachine()
        _ = watchdog.begin(sessionId: 3, now: 0)
        for index in 0..<6 {
            _ = watchdog.receive(sessionId: 3, samples: frame, now: Double(index) * 0.02)
        }
        XCTAssertEqual(watchdog.tick(now: 0.51), [
            .shortcutUp, .stopAudioSink, .restoreDefaultInput, .reportFailure("音频帧超时")
        ])
        XCTAssertEqual(watchdog.phase, .idle)

        var explicit = VoiceSessionMachine()
        _ = explicit.begin(sessionId: 4, now: 0)
        XCTAssertEqual(explicit.abort(reason: "蓝牙断开"), [
            .stopAudioSink, .restoreDefaultInput, .reportFailure("蓝牙断开")
        ])
    }

    func testWrongSessionCannotMutateActiveSession() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 10, now: 0)
        XCTAssertEqual(machine.receive(sessionId: 11, samples: frame, now: 0.1), [])
        XCTAssertEqual(machine.end(sessionId: 11, now: 0.1), [])
        XCTAssertEqual(machine.phase, .preparing(10))
    }
}
