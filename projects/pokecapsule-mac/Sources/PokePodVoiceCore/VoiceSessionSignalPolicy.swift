public enum VoiceSessionSignalPolicy {
    /// Session-scoped firmware errors may arrive after reconnect/start on the
    /// event characteristic. Only the currently owned nonzero session can
    /// affect local shortcut, audio sink or input-device state.
    public static func remoteErrorTargetsActive(
        reportedSessionId: UInt32,
        activeSessionId: UInt32?
    ) -> Bool {
        reportedSessionId != 0 && reportedSessionId == activeSessionId
    }
}
