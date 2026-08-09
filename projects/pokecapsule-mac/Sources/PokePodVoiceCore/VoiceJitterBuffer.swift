import Foundation

public enum VoiceJitterError: Error, Equatable {
    case wrongSession(expected: UInt32, actual: UInt32)
    case excessiveGap(UInt32)
}

public struct VoiceJitterOutput: Equatable {
    public let sequence: UInt32
    public let concealed: Bool
    public let samples: [Int16]
}

public struct VoiceJitterBuffer {
    public let maxConcealedFrames: UInt32
    public private(set) var sessionId: UInt32?
    public private(set) var expectedSequence: UInt32?
    public private(set) var droppedFrames: UInt64 = 0
    public private(set) var concealedFrames: UInt64 = 0
    public private(set) var receivedFrames: UInt64 = 0
    public private(set) var lastAcceptedSequence: UInt32?

    public init(maxConcealedFrames: UInt32 = 20) {
        self.maxConcealedFrames = maxConcealedFrames
    }

    public mutating func reset(sessionId: UInt32) {
        self.sessionId = sessionId
        expectedSequence = nil
        droppedFrames = 0
        concealedFrames = 0
        receivedFrames = 0
        lastAcceptedSequence = nil
    }

    public mutating func ingest(_ frame: BLEVoiceAudioFrame) throws -> [VoiceJitterOutput] {
        if sessionId == nil { reset(sessionId: frame.sessionId) }
        let samples = try IMAADPCM.decode(frame)
        receivedFrames += 1
        guard sessionId == frame.sessionId else {
            // Event/audio characteristics can deliver a frame from the prior
            // session after a new one is active. Treat it like a late packet;
            // it must never abort or advance the current session.
            droppedFrames += 1
            return []
        }
        guard let expected = expectedSequence else {
            expectedSequence = frame.sequence &+ 1
            lastAcceptedSequence = frame.sequence
            return [VoiceJitterOutput(sequence: frame.sequence, concealed: false, samples: samples)]
        }
        let delta = frame.sequence &- expected
        if delta == UInt32.max || delta >= 0x8000_0000 {
            droppedFrames += 1
            return []
        }
        guard delta <= maxConcealedFrames else { throw VoiceJitterError.excessiveGap(delta) }
        var output = [VoiceJitterOutput]()
        if delta > 0 {
            for offset in 0..<delta {
                output.append(VoiceJitterOutput(
                    sequence: expected &+ offset,
                    concealed: true,
                    samples: [Int16](repeating: 0, count: Int(frame.sampleCount))))
            }
            concealedFrames += UInt64(delta)
        }
        output.append(VoiceJitterOutput(sequence: frame.sequence, concealed: false, samples: samples))
        expectedSequence = frame.sequence &+ 1
        lastAcceptedSequence = frame.sequence
        return output
    }

    public var qualitySnapshot: VoiceLinkQualitySnapshot? {
        guard let sessionId else { return nil }
        return VoiceLinkQualitySnapshot(
            sessionId: sessionId,
            receivedFrames: receivedFrames,
            droppedFrames: droppedFrames,
            sequenceGapFrames: concealedFrames,
            lastSequence: lastAcceptedSequence)
    }
}
