import XCTest
@testable import PokePodVoiceCore

final class BLEHandshakePolicyTests: XCTestCase {
    func testMTUProxyBoundary() throws {
        XCTAssertThrowsError(try BLEHandshakePolicy.validate(maximumWriteValueLength: 181)) {
            XCTAssertEqual($0 as? BLEHandshakeError, .mtuProxyTooSmall(actual: 181, required: 182))
        }
        XCTAssertNoThrow(try BLEHandshakePolicy.validate(maximumWriteValueLength: 182))
    }

    func testFirmwareReadyAndMTURejectionAreDistinct() throws {
        XCTAssertTrue(try BLEHandshakePolicy.validate(statusCode: 1))
        XCTAssertFalse(try BLEHandshakePolicy.validate(statusCode: 9))
        XCTAssertThrowsError(try BLEHandshakePolicy.validate(statusCode: 2)) {
            XCTAssertEqual($0 as? BLEHandshakeError, .firmwareMTURejected)
        }
    }

    func testFirstAudioNotificationMustBeAWholeIndependentFrame() {
        XCTAssertNoThrow(try BLEHandshakePolicy.validateFirstAudioNotification(length: 175))
        XCTAssertThrowsError(try BLEHandshakePolicy.validateFirstAudioNotification(length: 174)) {
            XCTAssertEqual($0 as? BLEHandshakeError,
                           .invalidFirstNotificationLength(actual: 174, expected: 175))
        }
    }
}
