import Foundation
import XCTest
@testable import PokePodVoiceCore

final class IMAADPCMTests: XCTestCase {
    func testReferenceNibbleVector() throws {
        let frame = BLEVoiceAudioFrame(
            sessionId: 1,
            sequence: 0,
            sampleCount: 8,
            predictor: 0,
            stepIndex: 0,
            payload: Data([0x10, 0x32, 0x54, 0x76]))
        XCTAssertEqual(try IMAADPCM.decode(frame), [0, 0, 1, 4, 8, 15, 27, 47])
    }

    func testSignClampAndStepIndexBoundaries() throws {
        let positive = BLEVoiceAudioFrame(
            sessionId: 1, sequence: 0, sampleCount: 2,
            predictor: 32_760, stepIndex: 88, payload: Data([0x77]))
        XCTAssertEqual(try IMAADPCM.decode(positive), [32_760, 32_767])

        let negative = BLEVoiceAudioFrame(
            sessionId: 1, sequence: 0, sampleCount: 2,
            predictor: -32_760, stepIndex: 88, payload: Data([0xff]))
        XCTAssertEqual(try IMAADPCM.decode(negative), [-32_760, -32_768])
    }

    func testEveryFrameStartsFromIndependentPredictor() throws {
        let first = makeFrame(sequence: 1, predictor: 10, stepIndex: 0, byte: 0x77)
        let second = makeFrame(sequence: 2, predictor: -10, stepIndex: 0, byte: 0)
        XCTAssertNotEqual(try IMAADPCM.decode(first).last, try IMAADPCM.decode(second).first)
        XCTAssertEqual(try IMAADPCM.decode(second).first, -10)
    }
}
