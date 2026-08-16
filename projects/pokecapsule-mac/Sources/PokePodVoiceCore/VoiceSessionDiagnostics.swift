import Foundation

/// A stable category for the first actionable failure in one wireless voice
/// session.  The category deliberately describes the boundary that failed;
/// it does not claim that the Mac can prove what happened inside the radio.
public enum VoiceSessionFailureKind: String, Codable, Equatable {
    case deviceDidNotSendFrames = "device_did_not_send_frames"
    case audioNotificationNotReceived = "audio_notification_not_received"
    case audioDecodeFailed = "audio_decode_failed"
    case blackHoleWriteFailed = "blackhole_write_failed"
    case shortcutFailed = "shortcut_failed"
    case sequenceGap = "sequence_gap"
    case disconnected = "disconnected"
    case stopAcknowledgementTimedOut = "stop_acknowledgement_timed_out"
    case watchdog = "watchdog"
    case platformSetupFailed = "platform_setup_failed"
    case unknown = "unknown"

    public var userDescription: String {
        switch self {
        case .deviceDidNotSendFrames:
            return "设备未送出音频帧"
        case .audioNotificationNotReceived:
            return "蓝牙通知未到达 Mac"
        case .audioDecodeFailed:
            return "收到音频通知但解码失败"
        case .blackHoleWriteFailed:
            return "BlackHole 音频写入失败"
        case .shortcutFailed:
            return "快捷键注入失败"
        case .sequenceGap:
            return "音频序列缺口过大"
        case .disconnected:
            return "蓝牙连接中断"
        case .stopAcknowledgementTimedOut:
            return "设备停止确认超时"
        case .watchdog:
            return "音频超时"
        case .platformSetupFailed:
            return "Mac 音频环境准备失败"
        case .unknown:
            return "语音会话失败"
        }
    }
}

public struct VoiceSessionFailure: Codable, Equatable {
    public let kind: VoiceSessionFailureKind
    public let detail: String?

    public init(kind: VoiceSessionFailureKind, detail: String? = nil) {
        self.kind = kind
        self.detail = detail
    }

    public var userDescription: String {
        guard let detail, !detail.isEmpty else { return kind.userDescription }
        return "\(kind.userDescription)：\(detail)"
    }
}

public enum VoiceSessionTimelineEventKind: String, Codable, Equatable {
    case sessionStarted = "session_started"
    case platformPrepared = "platform_prepared"
    case sessionReadySent = "session_ready_sent"
    case audioNotification = "audio_notification"
    case audioDecoded = "audio_decoded"
    case sequenceGap = "sequence_gap"
    case blackHoleWritten = "blackhole_written"
    case shortcutDown = "shortcut_down"
    case shortcutUp = "shortcut_up"
    case sessionEnded = "session_ended"
    case stopAcknowledged = "stop_acknowledged"
    case inputRestored = "input_restored"
    case disconnected = "disconnected"
    case failed = "failed"
}

public struct VoiceSessionTimelineEvent: Codable, Equatable {
    public let kind: VoiceSessionTimelineEventKind
    public let sessionId: UInt32
    public let at: TimeInterval
    public let sequence: UInt32?
    public let count: Int?
    public let detail: String?

    public init(
        kind: VoiceSessionTimelineEventKind,
        sessionId: UInt32,
        at: TimeInterval,
        sequence: UInt32? = nil,
        count: Int? = nil,
        detail: String? = nil
    ) {
        self.kind = kind
        self.sessionId = sessionId
        self.at = at
        self.sequence = sequence
        self.count = count
        self.detail = detail
    }
}

/// A bounded, value-type session trace suitable for UI diagnostics and tests.
/// Starting a new session always clears the previous trace, so a stale BLE
/// callback can never contaminate the next session's report.
public struct VoiceSessionTimeline: Codable, Equatable {
    public private(set) var sessionId: UInt32?
    public private(set) var events: [VoiceSessionTimelineEvent]
    public private(set) var failure: VoiceSessionFailure?

    public init() {
        sessionId = nil
        events = []
        failure = nil
    }

    public var isActive: Bool { sessionId != nil && failure == nil }

    public var firstAudioNotificationAt: TimeInterval? {
        events.first(where: { $0.kind == .audioNotification })?.at
    }

    public var firstDecodedAt: TimeInterval? {
        events.first(where: { $0.kind == .audioDecoded })?.at
    }

    public var firstBlackHoleWriteAt: TimeInterval? {
        events.first(where: { $0.kind == .blackHoleWritten })?.at
    }

    public var inputWasRestored: Bool {
        events.contains(where: { $0.kind == .inputRestored })
    }

    public var sequenceGapCount: Int {
        events.filter { $0.kind == .sequenceGap }.count
    }

    public var firstAudioLatency: TimeInterval? {
        guard let start = events.first(where: { $0.kind == .sessionStarted })?.at,
              let first = firstAudioNotificationAt else { return nil }
        return first - start
    }

    public mutating func begin(sessionId: UInt32, at: TimeInterval) {
        self.sessionId = sessionId
        events.removeAll(keepingCapacity: true)
        failure = nil
        append(.init(kind: .sessionStarted, sessionId: sessionId, at: at))
    }

    public mutating func record(
        _ kind: VoiceSessionTimelineEventKind,
        at: TimeInterval,
        sequence: UInt32? = nil,
        count: Int? = nil,
        detail: String? = nil
    ) {
        guard let sessionId, failure == nil else { return }
        append(.init(
            kind: kind,
            sessionId: sessionId,
            at: at,
            sequence: sequence,
            count: count,
            detail: detail))
    }

    public mutating func fail(_ failure: VoiceSessionFailure, at: TimeInterval) {
        guard let sessionId, self.failure == nil else { return }
        self.failure = failure
        append(.init(
            kind: .failed,
            sessionId: sessionId,
            at: at,
            detail: failure.userDescription))
    }

    public mutating func finish(at: TimeInterval, detail: String? = nil) {
        guard let sessionId, failure == nil else { return }
        if !events.contains(where: { $0.kind == .stopAcknowledged }) {
            append(.init(
                kind: .stopAcknowledged,
                sessionId: sessionId,
                at: at,
                detail: detail))
        }
    }

    public var summary: String {
        guard let sessionId else { return "尚无语音会话指标" }
        var parts = ["会话 \(sessionId)"]
        if let latency = firstAudioLatency {
            parts.append(String(format: "首个通知 %.0f ms", latency * 1_000))
        } else if failure?.kind == .deviceDidNotSendFrames {
            parts.append("未收到设备音频帧")
        }
        if firstDecodedAt != nil { parts.append("已解码") }
        if firstBlackHoleWriteAt != nil { parts.append("已写入 BlackHole") }
        if sequenceGapCount > 0 { parts.append("序列缺口 \(sequenceGapCount) 次") }
        if inputWasRestored { parts.append("输入已恢复") }
        if let failure { parts.append("失败：\(failure.userDescription)") }
        else if events.contains(where: { $0.kind == .stopAcknowledged }) { parts.append("已完成") }
        return parts.joined(separator: " · ")
    }

    private mutating func append(_ event: VoiceSessionTimelineEvent) {
        events.append(event)
        if events.count > 128 { events.removeFirst(events.count - 128) }
    }
}

public enum VoiceActionFailureKind: String, Codable, Equatable {
    case saveDefaultInput = "save_default_input"
    case switchToBlackHole = "switch_to_blackhole"
    case startAudioSink = "start_audio_sink"
    case writeSamples = "write_samples"
    case shortcutDown = "shortcut_down"
}

public struct VoiceActionExecutionFailure: Codable, Equatable {
    public let kind: VoiceActionFailureKind
    public let detail: String?

    public init(kind: VoiceActionFailureKind, detail: String? = nil) {
        self.kind = kind
        self.detail = detail
    }
}

public extension VoiceActionExecutionFailure {
    var sessionFailure: VoiceSessionFailure {
        let sessionKind: VoiceSessionFailureKind
        switch kind {
        case .writeSamples: sessionKind = .blackHoleWriteFailed
        case .shortcutDown: sessionKind = .shortcutFailed
        case .saveDefaultInput, .switchToBlackHole, .startAudioSink:
            sessionKind = .platformSetupFailed
        }
        return VoiceSessionFailure(kind: sessionKind, detail: detail)
    }
}
