import XCTest
@testable import PokeCapsuleCore

final class TranscriptionSanityTests: XCTestCase {
    func testRejectsEmptyTranscript() {
        XCTAssertNotNil(TranscriptionSanity.issue(text: " \n", durationMs: 8_000))
    }

    func testRejectsShortRecording() {
        XCTAssertNotNil(TranscriptionSanity.issue(text: "测试", durationMs: 1_999))
    }

    func testAcceptsEightSecondReferencePhrase() {
        XCTAssertNil(TranscriptionSanity.issue(
            text: "福斯特建筑事务所商务提案英文翻译",
            durationMs: 8_000))
    }

    func testRejectsImplausiblyLongOutput() {
        let hallucination = String(repeating: "这是错误的幻觉文本", count: 20)
        XCTAssertNotNil(TranscriptionSanity.issue(text: hallucination, durationMs: 1_572))
    }
}
