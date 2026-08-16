import Foundation
import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionDiagnosticStoreTests: XCTestCase {
    func testWritesAtomicDecodableTimeline() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(
            UUID().uuidString, isDirectory: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("last-session.json")
        var timeline = VoiceSessionTimeline()
        timeline.begin(sessionId: 77, at: 1.0)
        timeline.record(.sessionReadySent, at: 1.01)
        timeline.fail(
            VoiceSessionFailure(kind: .deviceDidNotSendFrames), at: 1.4)
        try VoiceSessionDiagnosticStore.write(
            timeline, to: url, capturedAt: Date(timeIntervalSince1970: 123))
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let decoded = try decoder.decode(
            VoiceSessionDiagnosticEnvelope.self, from: Data(contentsOf: url))
        XCTAssertEqual(decoded.schema, "pokepod.voice-session-diagnostic.v1")
        XCTAssertEqual(decoded.timeline, timeline)
        XCTAssertEqual(decoded.capturedAt.timeIntervalSince1970, 123, accuracy: 1)
    }
}
