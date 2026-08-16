import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionDiagnosticsTests: XCTestCase {
    func testTimelineRecordsFirstNotificationDecodeWriteStopAckAndInputRestore() {
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 101, at: 10.0)
        timeline.record(.platformPrepared, at: 10.01)
        timeline.record(.sessionReadySent, at: 10.02)
        timeline.record(.audioNotification, at: 10.120, sequence: 7, count: 175)
        timeline.record(.audioDecoded, at: 10.125, sequence: 7, count: 320)
        timeline.record(.blackHoleWritten, at: 10.126, count: 320)
        timeline.record(.inputRestored, at: 10.400)
        timeline.finish(at: 10.401)

        XCTAssertEqual(timeline.firstAudioLatency ?? -1, 0.120, accuracy: 0.000_001)
        XCTAssertNotNil(timeline.firstDecodedAt)
        XCTAssertNotNil(timeline.firstBlackHoleWriteAt)
        XCTAssertTrue(timeline.inputWasRestored)
        XCTAssertNil(timeline.failure)
        XCTAssertTrue(timeline.summary.contains("首个通知"))
        XCTAssertTrue(timeline.summary.contains("已解码"))
        XCTAssertTrue(timeline.summary.contains("已写入 BlackHole"))
        XCTAssertTrue(timeline.summary.contains("输入已恢复"))
        XCTAssertTrue(timeline.summary.contains("已完成"))
        XCTAssertEqual(timeline.events.last?.kind, .stopAcknowledged)
    }

    func testZeroFrameWatchdogCanBeClassifiedAsDeviceDidNotSendFrames() {
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 102, at: 0)
        timeline.fail(
            VoiceSessionFailure(
                kind: .deviceDidNotSendFrames,
                detail: "400 ms watchdog"),
            at: 0.400)

        XCTAssertEqual(timeline.failure?.kind, .deviceDidNotSendFrames)
        XCTAssertTrue(timeline.summary.contains("未收到设备音频帧"))
        XCTAssertTrue(timeline.summary.contains("设备未送出音频帧"))
    }

    func testDisconnectBeforeFirstNotificationIsDistinctFromDeviceNoFrame() {
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 103, at: 4)
        timeline.record(.disconnected, at: 4.2, detail: "连接已断开")
        timeline.fail(
            VoiceSessionFailure(
                kind: .audioNotificationNotReceived,
                detail: "连接已断开"),
            at: 4.2)

        XCTAssertEqual(timeline.failure?.kind, .audioNotificationNotReceived)
        XCTAssertFalse(timeline.summary.contains("设备未送出音频帧"))
        XCTAssertTrue(timeline.summary.contains("蓝牙通知未到达 Mac"))
    }

    func testDecodeFailureAndOutputFailuresRemainTyped() {
        XCTAssertEqual(
            VoiceSessionFailure(
                kind: .audioDecodeFailed,
                detail: "坏帧").userDescription,
            "收到音频通知但解码失败：坏帧")
        XCTAssertEqual(
            VoiceSessionFailure(
                kind: .blackHoleWriteFailed,
                detail: "sink").userDescription,
            "BlackHole 音频写入失败：sink")
        XCTAssertEqual(
            VoiceSessionFailure(
                kind: .shortcutFailed,
                detail: "权限").userDescription,
            "快捷键注入失败：权限")
    }

    func testSecondSessionReplacesPreviousTimelineCompletely() {
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 104, at: 0)
        timeline.record(.audioNotification, at: 0.1, sequence: 1, count: 175)
        timeline.fail(VoiceSessionFailure(kind: .audioDecodeFailed), at: 0.2)

        timeline.begin(sessionId: 105, at: 1)

        XCTAssertEqual(timeline.sessionId, 105)
        XCTAssertEqual(timeline.events.map(\.kind), [.sessionStarted])
        XCTAssertNil(timeline.failure)
        XCTAssertNil(timeline.firstAudioNotificationAt)
    }

    func testWatchdogBoundaryRemainsExactly400Milliseconds() {
        var machine = VoiceSessionMachine()
        _ = machine.begin(sessionId: 106, now: 0)
        XCTAssertEqual(machine.tick(now: 0.399), [])
        XCTAssertEqual(
            machine.tick(now: 0.400),
            [.stopAudioSink, .restoreDefaultInput, .reportFailure("音频帧超时")])
    }

    func testSequenceGapIsRecordedWithoutTurningConcealmentIntoFailure() {
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 107, at: 0)
        timeline.record(.audioNotification, at: 0.1, sequence: 0, count: 175)
        timeline.record(.sequenceGap, at: 0.2, sequence: 1, count: 1, detail: "静音补帧")
        timeline.record(.audioDecoded, at: 0.2, sequence: 2, count: 320)

        XCTAssertNil(timeline.failure)
        XCTAssertEqual(
            timeline.events.filter { $0.kind == .sequenceGap }.count,
            1)
        XCTAssertTrue(timeline.summary.contains("已解码"))
    }
}
