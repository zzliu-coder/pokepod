import XCTest
@testable import PokePodVoiceCore

final class InputRecoveryPolicyTests: XCTestCase {
    func testOriginalDeviceMissingKeepsMarkerThenRestoresWhenDeviceReturns() {
        var markerIsPresent = true
        let missing = InputRecoveryPolicy.decide(
            recoveryNeeded: markerIsPresent,
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            originalUID: "usb-mic",
            availableUIDs: ["blackhole"])
        XCTAssertEqual(missing, .retryLater)
        if missing == .respectCurrentSelection { markerIsPresent = false }
        XCTAssertTrue(markerIsPresent)

        let returned = InputRecoveryPolicy.decide(
            recoveryNeeded: markerIsPresent,
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            originalUID: "usb-mic",
            availableUIDs: ["blackhole", "usb-mic"])
        XCTAssertEqual(returned, .restoreOriginal(uid: "usb-mic"))
        // The adapter clears only after setDefaultInput succeeds.
        markerIsPresent = false
        XCTAssertFalse(markerIsPresent)
    }

    func testUserSelectionAwayFromBlackHoleClearsRecoveryMarker() {
        XCTAssertEqual(InputRecoveryPolicy.decide(
            recoveryNeeded: true,
            currentUID: "user-new-mic",
            blackHoleUID: "blackhole",
            originalUID: "mac-mic",
            availableUIDs: ["blackhole", "mac-mic", "user-new-mic"]),
            .respectCurrentSelection)
    }

    func testSuccessfulRestoreRequiresOriginalToBeAvailableAndNotBlackHole() {
        XCTAssertEqual(InputRecoveryPolicy.decide(
            recoveryNeeded: true,
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            originalUID: "mac-mic",
            availableUIDs: ["blackhole", "mac-mic"]),
            .restoreOriginal(uid: "mac-mic"))
        XCTAssertEqual(InputRecoveryPolicy.decide(
            recoveryNeeded: true,
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            originalUID: "blackhole",
            availableUIDs: ["blackhole"]),
            .retryLater)
    }

    func testNewSessionCannotOverwriteOriginalWithBlackHole() {
        XCTAssertEqual(InputRecoveryPolicy.decideOriginalCapture(
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            pendingOriginalUID: "usb-mic"),
            .rejectKeepingRecovery(originalUID: "usb-mic"))
        XCTAssertEqual(InputRecoveryPolicy.decideOriginalCapture(
            currentUID: "built-in-mic",
            blackHoleUID: "blackhole",
            pendingOriginalUID: "usb-mic"),
            .capture(uid: "built-in-mic"))
    }

    func testNoMarkerMakesNoChangesAndMissingFactsRetry() {
        XCTAssertEqual(InputRecoveryPolicy.decide(
            recoveryNeeded: false,
            currentUID: "blackhole",
            blackHoleUID: "blackhole",
            originalUID: "mac-mic",
            availableUIDs: []), .noAction)
        XCTAssertEqual(InputRecoveryPolicy.decide(
            recoveryNeeded: true,
            currentUID: nil,
            blackHoleUID: "blackhole",
            originalUID: "mac-mic",
            availableUIDs: ["mac-mic"]), .retryLater)
    }

    func testMarkerClearsOnlyForSuccessfulRestoreOrUserSelection() {
        XCTAssertEqual(InputRecoveryPolicy.markerEffect(
            after: .retryLater,
            restorationSucceeded: false), .keep)
        XCTAssertEqual(InputRecoveryPolicy.markerEffect(
            after: .restoreOriginal(uid: "usb-mic"),
            restorationSucceeded: false), .keep)
        XCTAssertEqual(InputRecoveryPolicy.markerEffect(
            after: .restoreOriginal(uid: "usb-mic"),
            restorationSucceeded: true), .clear)
        XCTAssertEqual(InputRecoveryPolicy.markerEffect(
            after: .respectCurrentSelection,
            restorationSucceeded: false), .clear)
    }
}
