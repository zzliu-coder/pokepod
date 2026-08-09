import Foundation

public enum LoginItemFirstRunDecision: Equatable {
    case noAction
    case markExistingConfiguration
    case registerOnceAndMark
}

public enum LoginItemFirstRunPolicy {
    public static func decide(
        hasRecordedDecision: Bool,
        prerequisitesReady: Bool,
        firmwareReady: Bool,
        serviceAlreadyConfigured: Bool
    ) -> LoginItemFirstRunDecision {
        guard !hasRecordedDecision, prerequisitesReady, firmwareReady else { return .noAction }
        return serviceAlreadyConfigured ? .markExistingConfiguration : .registerOnceAndMark
    }
}
