import XCTest
@testable import PokePodVoiceCore

final class OptionZHoldSequenceTests: XCTestCase {
    func testPressUsesPhysicalLeftOptionBeforeZ() {
        XCTAssertEqual(OptionZHoldSequence.press, [
            .init(virtualKey: 58, keyDown: true, alternateDown: true),
            .init(virtualKey: 6, keyDown: true, alternateDown: true),
        ])
    }

    func testReleaseLiftsZBeforeLeftOption() {
        XCTAssertEqual(OptionZHoldSequence.release, [
            .init(virtualKey: 6, keyDown: false, alternateDown: true),
            .init(virtualKey: 58, keyDown: false, alternateDown: false),
        ])
    }
}
