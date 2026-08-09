import XCTest
@testable import PokePodVoiceCore

final class VoiceLinkQualityTests: XCTestCase {
    func testSummaryStatesExactlyWhatMacCanProve() {
        let snapshot = VoiceLinkQualitySnapshot(
            sessionId: 42,
            receivedFrames: 17,
            droppedFrames: 2,
            sequenceGapFrames: 3,
            lastSequence: 21)
        XCTAssertEqual(VoiceLinkQualityFormatter.summary(snapshot, context: .active),
            "会话 42 · 已收并解码 17 帧 · 丢弃 2 · 序列缺口 3 帧（已补静音）· 最新序号 21")
        XCTAssertEqual(VoiceLinkQualityFormatter.summary(snapshot, context: .recent),
            "最近会话 42 · 已收并解码 17 帧 · 丢弃 2 · 序列缺口 3 帧（已补静音）· 最新序号 21")
        XCTAssertEqual(VoiceLinkQualityFormatter.summary(snapshot, context: .disconnected),
            "蓝牙已断开 · 最近会话 42 · 已收并解码 17 帧 · 丢弃 2 · 序列缺口 3 帧（已补静音）· 最新序号 21")
    }

    func testNoFrameAndNoSessionHaveTruthfulPlaceholders() {
        let waiting = VoiceLinkQualitySnapshot(
            sessionId: 43,
            receivedFrames: 0,
            droppedFrames: 0,
            sequenceGapFrames: 0,
            lastSequence: nil)
        XCTAssertTrue(VoiceLinkQualityFormatter.summary(waiting, context: .active)
            .contains("最新序号 —"))
        XCTAssertEqual(VoiceLinkQualityFormatter.summary(nil, context: .active),
            "等待首个音频通知")
        XCTAssertEqual(VoiceLinkQualityFormatter.summary(nil, context: .disconnected),
            "蓝牙已断开 · 尚无语音会话指标")
    }

    func testSummaryDoesNotClaimUnobservedRadioMetrics() {
        let summary = VoiceLinkQualityFormatter.summary(.init(
            sessionId: 1,
            receivedFrames: 1,
            droppedFrames: 0,
            sequenceGapFrames: 0,
            lastSequence: 0), context: .recent)
        XCTAssertFalse(summary.localizedCaseInsensitiveContains("RSSI"))
        XCTAssertFalse(summary.contains("成功率"))
        XCTAssertFalse(summary.contains("空中丢包"))
    }
}
