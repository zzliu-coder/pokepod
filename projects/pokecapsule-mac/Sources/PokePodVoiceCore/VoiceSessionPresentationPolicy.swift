public enum VoiceSessionPresentationState: Equatable {
    case unchanged
    case ready
    case setup
}

public struct VoiceSessionPresentationUpdate: Equatable {
    public let state: VoiceSessionPresentationState
    public let detail: String?

    public init(state: VoiceSessionPresentationState, detail: String?) {
        self.state = state
        self.detail = detail
    }
}

public enum VoiceSessionPresentationPolicy {
    public static func recovery(
        environmentReady: Bool,
        detail: String
    ) -> VoiceSessionPresentationUpdate {
        .init(
            state: environmentReady ? .ready : .setup,
            detail: detail)
    }

    public static func completion(
        _ outcome: VoiceSessionCompletionOutcome,
        environmentReady: Bool
    ) -> VoiceSessionPresentationUpdate {
        switch outcome {
        case .none:
            return .init(state: .unchanged, detail: nil)
        case .completed:
            return .init(
                state: environmentReady ? .ready : .setup,
                detail: environmentReady
                    ? "输入结束，默认麦克风已恢复"
                    : "输入已结束，请完成运行环境设置")
        case let .aborted(_, reason):
            return .init(
                state: environmentReady ? .ready : .setup,
                detail: environmentReady
                    ? (reason == .watchdog
                        ? "本次输入因音频超时已中止，可以立即重试"
                        : "本次输入已安全中止，可以再次按住说话")
                    : "本次输入已中止，请完成运行环境设置")
        }
    }
}
