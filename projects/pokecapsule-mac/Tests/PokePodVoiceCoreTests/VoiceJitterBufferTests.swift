import Foundation
import XCTest
@testable import PokePodVoiceCore

final class VoiceJitterBufferTests: XCTestCase {
    func testGapProducesSilenceAndDuplicateOrLateFramesDrop() throws {
        var jitter = VoiceJitterBuffer()
        jitter.reset(sessionId: 9)
        XCTAssertEqual(try jitter.ingest(makeFrame(session: 9, sequence: 10)).count, 1)
        let output = try jitter.ingest(makeFrame(session: 9, sequence: 12))
        XCTAssertEqual(output.map(\.sequence), [11, 12])
        XCTAssertTrue(output[0].concealed)
        XCTAssertEqual(output[0].samples, [Int16](repeating: 0, count: 320))
        XCTAssertEqual(jitter.concealedFrames, 1)
        XCTAssertEqual(try jitter.ingest(makeFrame(session: 9, sequence: 12)), [])
        XCTAssertEqual(try jitter.ingest(makeFrame(session: 9, sequence: 11)), [])
        XCTAssertEqual(jitter.droppedFrames, 2)
        XCTAssertEqual(jitter.receivedFrames, 4)
        XCTAssertEqual(jitter.lastAcceptedSequence, 12)
        XCTAssertEqual(jitter.qualitySnapshot, VoiceLinkQualitySnapshot(
            sessionId: 9,
            receivedFrames: 4,
            droppedFrames: 2,
            sequenceGapFrames: 1,
            lastSequence: 12))
    }

    func testSequenceWrapIsForwardProgress() throws {
        var jitter = VoiceJitterBuffer()
        jitter.reset(sessionId: 4)
        _ = try jitter.ingest(makeFrame(session: 4, sequence: UInt32.max))
        let next = try jitter.ingest(makeFrame(session: 4, sequence: 0))
        XCTAssertEqual(next.map(\.sequence), [0])
        XCTAssertFalse(next[0].concealed)
    }

    func testWrongSessionIsDroppedAndCurrentExcessiveGapIsRejected() throws {
        var jitter = VoiceJitterBuffer(maxConcealedFrames: 2)
        jitter.reset(sessionId: 1)
        _ = try jitter.ingest(makeFrame(session: 1, sequence: 0))
        XCTAssertEqual(try jitter.ingest(makeFrame(session: 2, sequence: 1)), [])
        XCTAssertEqual(jitter.sessionId, 1)
        XCTAssertEqual(jitter.expectedSequence, 1)
        XCTAssertEqual(jitter.droppedFrames, 1)
        XCTAssertEqual(jitter.receivedFrames, 2)
        XCTAssertThrowsError(try jitter.ingest(makeFrame(session: 1, sequence: 4))) {
            XCTAssertEqual($0 as? VoiceJitterError, .excessiveGap(3))
        }
    }
}
