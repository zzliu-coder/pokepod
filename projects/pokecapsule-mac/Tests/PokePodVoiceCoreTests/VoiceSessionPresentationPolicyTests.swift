import XCTest
@testable import PokePodVoiceCore

final class VoiceSessionPresentationPolicyTests: XCTestCase {
    func testAbortedOutcomeCanNeverLeavePresentationUnchangedOrListening() {
        let watchdog = VoiceSessionPresentationPolicy.completion(
            .aborted(80, .watchdog), environmentReady: true)
        XCTAssertEqual(watchdog.state, .ready)
        XCTAssertTrue(watchdog.detail?.contains("音频超时") == true)

        let failed = VoiceSessionPresentationPolicy.completion(
            .aborted(81, .executionFailure), environmentReady: true)
        XCTAssertEqual(failed.state, .ready)
        XCTAssertTrue(failed.detail?.contains("安全中止") == true)

        let missingEnvironment = VoiceSessionPresentationPolicy.completion(
            .aborted(82, .watchdog), environmentReady: false)
        XCTAssertEqual(missingEnvironment.state, .setup)
        XCTAssertNotNil(missingEnvironment.detail)
    }

    func testNoneDoesNotMutatePresentationAndCompletionReturnsReady() {
        XCTAssertEqual(
            VoiceSessionPresentationPolicy.completion(.none, environmentReady: true),
            .init(state: .unchanged, detail: nil))
        XCTAssertEqual(
            VoiceSessionPresentationPolicy.completion(.completed(83), environmentReady: true).state,
            .ready)
    }

    func testManualRepairAlwaysLeavesListeningPresentation() {
        XCTAssertEqual(
            VoiceSessionPresentationPolicy.recovery(
                environmentReady: true,
                detail: "麦克风已恢复"),
            .init(state: .ready, detail: "麦克风已恢复"))
        XCTAssertEqual(
            VoiceSessionPresentationPolicy.recovery(
                environmentReady: false,
                detail: "麦克风已恢复").state,
            .setup)
    }

    func testTypedFailureReachesUserWithoutCollapsingToGenericTimeout() {
        let update = VoiceSessionPresentationPolicy.completion(
            .aborted(84, .executionFailure),
            failure: VoiceSessionFailure(
                kind: .blackHoleWriteFailed,
                detail: "OSStatus -50"),
            environmentReady: true)

        XCTAssertEqual(update.state, .ready)
        XCTAssertTrue(update.detail?.contains("BlackHole 音频写入失败") == true)
        XCTAssertTrue(update.detail?.contains("OSStatus -50") == true)
    }
}
