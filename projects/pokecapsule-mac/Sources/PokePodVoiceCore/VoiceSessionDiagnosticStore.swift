import Foundation

public struct VoiceSessionDiagnosticEnvelope: Codable, Equatable {
    public let schema: String
    public let capturedAt: Date
    public let timeline: VoiceSessionTimeline

    public init(timeline: VoiceSessionTimeline, capturedAt: Date = Date()) {
        schema = "pokepod.voice-session-diagnostic.v1"
        self.capturedAt = capturedAt
        self.timeline = timeline
    }
}

public enum VoiceSessionDiagnosticStore {
    public static func write(
        _ timeline: VoiceSessionTimeline,
        to url: URL,
        capturedAt: Date = Date()
    ) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        encoder.dateEncodingStrategy = .iso8601
        let data = try encoder.encode(
            VoiceSessionDiagnosticEnvelope(
                timeline: timeline, capturedAt: capturedAt))
        try data.write(to: url, options: .atomic)
    }
}
