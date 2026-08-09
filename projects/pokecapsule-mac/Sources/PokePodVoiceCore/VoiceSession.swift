import Foundation

public struct VoiceSessionTiming: Equatable {
    public var prebufferSeconds: TimeInterval
    public var tailSeconds: TimeInterval
    public var watchdogSeconds: TimeInterval
    public var restoreDelaySeconds: TimeInterval
    public var sampleRate: Int

    public init(
        prebufferSeconds: TimeInterval = 0.120,
        tailSeconds: TimeInterval = 0.120,
        watchdogSeconds: TimeInterval = 0.400,
        restoreDelaySeconds: TimeInterval = 0.250,
        sampleRate: Int = 16_000
    ) {
        self.prebufferSeconds = prebufferSeconds
        self.tailSeconds = tailSeconds
        self.watchdogSeconds = watchdogSeconds
        self.restoreDelaySeconds = restoreDelaySeconds
        self.sampleRate = sampleRate
    }

    public var prebufferSamples: Int { Int(prebufferSeconds * Double(sampleRate)) }
}

public enum VoiceSessionPhase: Equatable {
    case idle
    case preparing(UInt32)
    case streaming(UInt32)
    case tail(UInt32)
    case restoring(UInt32)
    case failed(String)
}

public enum VoiceSessionAction: Equatable {
    case saveDefaultInput
    case switchToBlackHole
    case startAudioSink
    case writeSamples([Int16])
    case shortcutDown
    case shortcutUp
    case stopAudioSink
    case restoreDefaultInput
    case reportFailure(String)
}

public struct VoiceSessionMachine {
    public private(set) var phase: VoiceSessionPhase = .idle
    public let timing: VoiceSessionTiming
    private var buffered = [Int16]()
    private var shortcutHeld = false
    private var lastAudioAt: TimeInterval = 0
    private var phaseDeadline: TimeInterval?

    public init(timing: VoiceSessionTiming = .init()) {
        self.timing = timing
    }

    public mutating func begin(sessionId: UInt32, now: TimeInterval) -> [VoiceSessionAction] {
        guard phase == .idle else { return [] }
        phase = .preparing(sessionId)
        buffered.removeAll(keepingCapacity: true)
        shortcutHeld = false
        lastAudioAt = now
        phaseDeadline = nil
        return [.saveDefaultInput, .switchToBlackHole, .startAudioSink]
    }

    public mutating func receive(
        sessionId: UInt32,
        samples: [Int16],
        now: TimeInterval
    ) -> [VoiceSessionAction] {
        guard activeSessionId == sessionId else { return [] }
        lastAudioAt = now
        switch phase {
        case .preparing:
            buffered.append(contentsOf: samples)
            guard buffered.count >= timing.prebufferSamples else { return [] }
            phase = .streaming(sessionId)
            shortcutHeld = true
            let ready = buffered
            buffered.removeAll(keepingCapacity: true)
            return [.shortcutDown, .writeSamples(ready)]
        case .streaming:
            return [.writeSamples(samples)]
        case .tail:
            // Event and audio notifications use different characteristics and may
            // arrive a few frames out of order. Keep accepting the bounded tail.
            guard !samples.isEmpty else { return [] }
            if !shortcutHeld {
                shortcutHeld = true
                // session-end may win the cross-characteristic race before the
                // first audio notification. Give that first real frame a full
                // bounded tail window instead of releasing immediately.
                phaseDeadline = now + timing.tailSeconds
                return [.shortcutDown, .writeSamples(samples)]
            }
            return [.writeSamples(samples)]
        default:
            return []
        }
    }

    public mutating func end(sessionId: UInt32, now: TimeInterval) -> [VoiceSessionAction] {
        guard activeSessionId == sessionId else { return [] }
        switch phase {
        case .preparing, .streaming: break
        default: return []
        }
        var actions = [VoiceSessionAction]()
        if case .preparing = phase, !buffered.isEmpty {
            shortcutHeld = true
            actions.append(.shortcutDown)
            actions.append(.writeSamples(buffered))
            buffered.removeAll(keepingCapacity: true)
        }
        phase = .tail(sessionId)
        phaseDeadline = now + timing.tailSeconds
        return actions
    }

    public mutating func tick(now: TimeInterval) -> [VoiceSessionAction] {
        switch phase {
        case .preparing, .streaming:
            if now - lastAudioAt >= timing.watchdogSeconds {
                return abort(reason: "音频帧超时")
            }
        case let .tail(sessionId):
            if let deadline = phaseDeadline, now >= deadline {
                var actions = [VoiceSessionAction]()
                if shortcutHeld { actions.append(.shortcutUp) }
                actions.append(.stopAudioSink)
                shortcutHeld = false
                phase = .restoring(sessionId)
                phaseDeadline = now + timing.restoreDelaySeconds
                return actions
            }
        case .restoring:
            if let deadline = phaseDeadline, now >= deadline {
                clear()
                return [.restoreDefaultInput]
            }
        case .idle, .failed:
            break
        }
        return []
    }

    public mutating func abort(reason: String) -> [VoiceSessionAction] {
        guard phase != .idle else { return [] }
        var actions = [VoiceSessionAction]()
        if shortcutHeld { actions.append(.shortcutUp) }
        actions.append(.stopAudioSink)
        actions.append(.restoreDefaultInput)
        actions.append(.reportFailure(reason))
        clear()
        return actions
    }

    public var activeSessionId: UInt32? {
        switch phase {
        case let .preparing(id), let .streaming(id), let .tail(id), let .restoring(id): return id
        case .idle, .failed: return nil
        }
    }

    public var isIdle: Bool { phase == .idle }

    private mutating func clear() {
        phase = .idle
        buffered.removeAll(keepingCapacity: true)
        shortcutHeld = false
        phaseDeadline = nil
    }
}
