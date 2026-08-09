import Foundation

public enum VoiceSessionAbortWirePolicy: Equatable {
    case reject(code: UInt16)
    case silent
}

/// One ordering point for local cleanup and the matching firmware reject.
/// The wire notification always follows shortcut/audio/input recovery.
public enum VoiceSessionAbortCoordinator {
    @discardableResult
    public static func abort(
        reason: String,
        report: Bool,
        wirePolicy: VoiceSessionAbortWirePolicy,
        machine: inout VoiceSessionMachine,
        completion: inout VoiceSessionCompletionCoordinator,
        executor: VoiceActionExecutor,
        reject: (UInt32, UInt16) -> Void
    ) -> UInt32? {
        let sessionId = machine.activeSessionId
        completion.cancel()
        let actions = machine.abort(reason: reason).filter {
            if case .reportFailure = $0 { return report }
            return true
        }
        if actions.isEmpty {
            executor.recover()
        } else {
            _ = executor.execute(actions)
        }
        machine = VoiceSessionMachine(timing: machine.timing)

        if let sessionId, case let .reject(code) = wirePolicy {
            reject(sessionId, code)
        }
        return sessionId
    }
}
