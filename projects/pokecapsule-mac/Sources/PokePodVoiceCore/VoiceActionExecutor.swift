import Foundation

public protocol VoicePlatformAdapter: AnyObject {
    func saveDefaultInput() throws
    func switchToBlackHole() throws
    func startAudioSink() throws
    func write(samples: [Int16]) throws
    func shortcutDown() throws
    func shortcutUp()
    func stopAudioSink()
    func restoreDefaultInput()
}

public final class VoiceActionExecutor {
    private let platform: VoicePlatformAdapter
    private let failure: (String) -> Void

    public init(platform: VoicePlatformAdapter, failure: @escaping (String) -> Void = { _ in }) {
        self.platform = platform
        self.failure = failure
    }

    @discardableResult
    public func execute(_ actions: [VoiceSessionAction]) -> Bool {
        do {
            for action in actions {
                switch action {
                case .saveDefaultInput: try platform.saveDefaultInput()
                case .switchToBlackHole: try platform.switchToBlackHole()
                case .startAudioSink: try platform.startAudioSink()
                case let .writeSamples(samples): try platform.write(samples: samples)
                case .shortcutDown: try platform.shortcutDown()
                case .shortcutUp: platform.shortcutUp()
                case .stopAudioSink: platform.stopAudioSink()
                case .restoreDefaultInput: platform.restoreDefaultInput()
                case let .reportFailure(message): failure(message)
                }
            }
            return true
        } catch {
            // One idempotent recovery path protects every partial setup state.
            platform.shortcutUp()
            platform.stopAudioSink()
            platform.restoreDefaultInput()
            failure(error.localizedDescription)
            return false
        }
    }

    /// Idempotent recovery used by explicit repair and abort paths even when
    /// the in-memory session has already returned to idle.
    public func recover() {
        platform.shortcutUp()
        platform.stopAudioSink()
        platform.restoreDefaultInput()
    }
}
