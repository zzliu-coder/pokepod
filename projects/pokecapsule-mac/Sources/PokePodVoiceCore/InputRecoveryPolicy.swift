import Foundation

public enum InputRecoveryDecision: Equatable {
    case noAction
    case retryLater
    case restoreOriginal(uid: String)
    case respectCurrentSelection
}

public enum InputOriginalCaptureDecision: Equatable {
    case capture(uid: String)
    case rejectKeepingRecovery(originalUID: String?)
}

public enum InputRecoveryMarkerEffect: Equatable {
    case keep
    case clear
}

public enum InputRecoveryPolicy {
    public static func markerEffect(
        after decision: InputRecoveryDecision,
        restorationSucceeded: Bool
    ) -> InputRecoveryMarkerEffect {
        switch decision {
        case .respectCurrentSelection:
            return .clear
        case .restoreOriginal:
            return restorationSucceeded ? .clear : .keep
        case .noAction, .retryLater:
            return .keep
        }
    }

    public static func decide(
        recoveryNeeded: Bool,
        currentUID: String?,
        blackHoleUID: String?,
        originalUID: String?,
        availableUIDs: Set<String>
    ) -> InputRecoveryDecision {
        guard recoveryNeeded else { return .noAction }
        guard let currentUID, let blackHoleUID else { return .retryLater }
        guard currentUID == blackHoleUID else { return .respectCurrentSelection }
        guard let originalUID,
              !originalUID.isEmpty,
              originalUID != blackHoleUID,
              availableUIDs.contains(originalUID) else { return .retryLater }
        return .restoreOriginal(uid: originalUID)
    }

    /// Capturing BlackHole as the "original" input destroys the only route
    /// back to the user's microphone. Keep any pending marker and reject the
    /// new session until recovery succeeds or the user selects another input.
    public static func decideOriginalCapture(
        currentUID: String?,
        blackHoleUID: String?,
        pendingOriginalUID: String?
    ) -> InputOriginalCaptureDecision {
        guard let currentUID, !currentUID.isEmpty else {
            return .rejectKeepingRecovery(originalUID: pendingOriginalUID)
        }
        if let blackHoleUID, currentUID == blackHoleUID {
            return .rejectKeepingRecovery(originalUID: pendingOriginalUID)
        }
        return .capture(uid: currentUID)
    }
}
