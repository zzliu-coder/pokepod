import Foundation

public enum VoiceSessionStartCoordinator {
    @discardableResult
    public static func start(
        sessionId: UInt32,
        now: TimeInterval,
        machine: inout VoiceSessionMachine,
        executor: VoiceActionExecutor,
        prepareAudioStream: (UInt32) -> Void,
        sendSessionReady: (UInt32) -> Void,
        reject: (UInt32) -> Void
    ) -> Bool {
        guard sessionId != 0 else {
            reject(sessionId)
            return false
        }
        guard machine.phase == .idle else {
            // A delayed or duplicate start can never preempt the microphone,
            // shortcut or default-input ownership of the active session. The
            // firmware treats a reject matching its current session as a hard
            // abort, so an exact duplicate must be ignored. Other session IDs
            // are rejected without targeting the active firmware session.
            if machine.activeSessionId != sessionId { reject(sessionId) }
            return false
        }
        let actions = machine.begin(sessionId: sessionId, now: now)
        guard executor.execute(actions) else {
            machine = VoiceSessionMachine(timing: machine.timing)
            reject(sessionId)
            return false
        }
        // The receive-side session must be installed only after the platform
        // and state machine own the session, but before firmware is released
        // to send audio. Rejected duplicate/stale starts never reach this hook.
        prepareAudioStream(sessionId)
        // This line is the firmware flow-control gate: audio preparation must
        // finish before the matching nonzero session is released to stream.
        sendSessionReady(sessionId)
        return true
    }
}
