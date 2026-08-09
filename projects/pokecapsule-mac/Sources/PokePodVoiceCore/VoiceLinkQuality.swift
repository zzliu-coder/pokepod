public struct VoiceLinkQualitySnapshot: Equatable {
    public let sessionId: UInt32
    /// Complete BLE audio notifications delivered to and decoded by the Mac.
    public let receivedFrames: UInt64
    /// Duplicate, late, or cross-session frames discarded by the Mac.
    public let droppedFrames: UInt64
    /// Missing sequence positions replaced with one 20 ms silent frame each.
    public let sequenceGapFrames: UInt64
    public let lastSequence: UInt32?

    public init(
        sessionId: UInt32,
        receivedFrames: UInt64,
        droppedFrames: UInt64,
        sequenceGapFrames: UInt64,
        lastSequence: UInt32?
    ) {
        self.sessionId = sessionId
        self.receivedFrames = receivedFrames
        self.droppedFrames = droppedFrames
        self.sequenceGapFrames = sequenceGapFrames
        self.lastSequence = lastSequence
    }
}

public enum VoiceLinkQualityContext: Equatable {
    case active
    case recent
    case disconnected
}

public enum VoiceLinkQualityFormatter {
    public static func summary(
        _ snapshot: VoiceLinkQualitySnapshot?,
        context: VoiceLinkQualityContext
    ) -> String {
        guard let snapshot else {
            switch context {
            case .active: return "等待首个音频通知"
            case .recent: return "尚无语音会话指标"
            case .disconnected: return "蓝牙已断开 · 尚无语音会话指标"
            }
        }
        let prefix: String
        switch context {
        case .active: prefix = "会话"
        case .recent: prefix = "最近会话"
        case .disconnected: prefix = "蓝牙已断开 · 最近会话"
        }
        let sequence = snapshot.lastSequence.map(String.init) ?? "—"
        return "\(prefix) \(snapshot.sessionId) · 已收并解码 \(snapshot.receivedFrames) 帧 · "
            + "丢弃 \(snapshot.droppedFrames) · 序列缺口 \(snapshot.sequenceGapFrames) 帧（已补静音）· "
            + "最新序号 \(sequence)"
    }
}
