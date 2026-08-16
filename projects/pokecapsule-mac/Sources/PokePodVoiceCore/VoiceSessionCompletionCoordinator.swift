import Foundation

public enum VoiceSessionEndResult: Equatable {
    case accepted
    case ignored
    case failed(UInt32)
}

public enum VoiceSessionCompletionAbortReason: Equatable {
    case watchdog
    case executionFailure
}

public enum VoiceSessionCompletionOutcome: Equatable {
    case none
    case completed(UInt32)
    case aborted(UInt32, VoiceSessionCompletionAbortReason)
}

public struct VoiceSessionCompletionCoordinator {
    public private(set) var pendingStopAcknowledgement: UInt32?
    public private(set) var lastActions: [VoiceSessionAction]

    public init() {
        pendingStopAcknowledgement = nil
        lastActions = []
    }

    @discardableResult
    public mutating func beginEnding(
        sessionId: UInt32,
        now: TimeInterval,
        machine: inout VoiceSessionMachine,
        executor: VoiceActionExecutor
    ) -> VoiceSessionEndResult {
        lastActions = []
        guard pendingStopAcknowledgement == nil,
              machine.activeSessionId == sessionId else { return .ignored }
        switch machine.phase {
        case .preparing, .streaming: break
        default: return .ignored
        }
        let actions = machine.end(sessionId: sessionId, now: now)
        lastActions = actions
        guard executor.execute(actions) else {
            machine = VoiceSessionMachine(timing: machine.timing)
            return .failed(sessionId)
        }
        pendingStopAcknowledgement = sessionId
        return .accepted
    }

    @discardableResult
    public mutating func tick(
        now: TimeInterval,
        machine: inout VoiceSessionMachine,
        executor: VoiceActionExecutor,
        acknowledgeStop: (UInt32) -> Void
    ) -> VoiceSessionCompletionOutcome {
        lastActions = []
        return tick(
            now: now,
            machine: &machine,
            execute: { executor.execute($0) },
            acknowledgeStop: acknowledgeStop)
    }

    @discardableResult
    public mutating func tick(
        now: TimeInterval,
        machine: inout VoiceSessionMachine,
        execute: ([VoiceSessionAction]) -> Bool,
        acknowledgeStop: (UInt32) -> Void
    ) -> VoiceSessionCompletionOutcome {
        let activeBeforeTick = machine.activeSessionId
        let actions = machine.tick(now: now)
        lastActions = actions
        guard execute(actions) else {
            let failed = pendingStopAcknowledgement ?? activeBeforeTick
            pendingStopAcknowledgement = nil
            machine = VoiceSessionMachine(timing: machine.timing)
            return failed.map {
                .aborted($0, .executionFailure)
            } ?? .none
        }
        if machine.isIdle,
           pendingStopAcknowledgement == nil,
           !actions.isEmpty,
           let activeBeforeTick {
            // The 400 ms watchdog already executed local recovery. The caller
            // now sends a matching reject so firmware releases the microphone.
            return .aborted(activeBeforeTick, .watchdog)
        }
        guard machine.isIdle, let sessionId = pendingStopAcknowledgement else {
            return .none
        }
        pendingStopAcknowledgement = nil
        // restoreDefaultInput has already executed through the action executor.
        acknowledgeStop(sessionId)
        return .completed(sessionId)
    }

    public mutating func cancel() {
        pendingStopAcknowledgement = nil
        lastActions = []
    }
}
