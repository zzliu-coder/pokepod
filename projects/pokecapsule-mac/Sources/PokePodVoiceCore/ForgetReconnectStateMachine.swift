import Foundation

public enum ForgetReconnectPhase: Equatable {
    case idle
    case awaitingWrite(deadline: TimeInterval)
    case disconnecting
}

public enum ForgetReconnectAction: Equatable {
    case sendForget
    case disconnect
    case reconnect
}

/// Orders bond removal so the forget command has a chance to reach PokePod.
/// A bounded timeout keeps a missing CoreBluetooth write callback from trapping
/// the menu app in a half-disconnected state.
public struct ForgetReconnectStateMachine {
    public let writeTimeoutSeconds: TimeInterval
    public private(set) var phase: ForgetReconnectPhase = .idle

    public init(writeTimeoutSeconds: TimeInterval = 0.75) {
        self.writeTimeoutSeconds = writeTimeoutSeconds
    }

    public mutating func begin(now: TimeInterval) -> [ForgetReconnectAction] {
        guard phase == .idle else { return [] }
        phase = .awaitingWrite(deadline: now + writeTimeoutSeconds)
        return [.sendForget]
    }

    public mutating func writeCompleted() -> [ForgetReconnectAction] {
        guard case .awaitingWrite = phase else { return [] }
        phase = .disconnecting
        return [.disconnect]
    }

    public mutating func tick(now: TimeInterval) -> [ForgetReconnectAction] {
        guard case let .awaitingWrite(deadline) = phase, now >= deadline else { return [] }
        phase = .disconnecting
        return [.disconnect]
    }

    public mutating func disconnected() -> [ForgetReconnectAction] {
        // Firmware may erase the bond and disconnect synchronously inside its
        // command write callback. CoreBluetooth can therefore report the link
        // loss before didWriteValueFor. Both orders complete the same flow.
        switch phase {
        case .awaitingWrite, .disconnecting: break
        case .idle: return []
        }
        phase = .idle
        return [.reconnect]
    }

    public mutating func cancel() {
        phase = .idle
    }

    public var isAwaitingWrite: Bool {
        if case .awaitingWrite = phase { return true }
        return false
    }
}
