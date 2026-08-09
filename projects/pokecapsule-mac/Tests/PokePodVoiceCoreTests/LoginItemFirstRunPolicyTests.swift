import XCTest
@testable import PokePodVoiceCore

final class LoginItemFirstRunPolicyTests: XCTestCase {
    func testRegistersOnceOnlyAfterPrerequisitesAndFirmwareReady() {
        XCTAssertEqual(LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: false,
            prerequisitesReady: false,
            firmwareReady: true,
            serviceAlreadyConfigured: false), .noAction)
        XCTAssertEqual(LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: false,
            prerequisitesReady: true,
            firmwareReady: false,
            serviceAlreadyConfigured: false), .noAction)
        XCTAssertEqual(LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: false,
            prerequisitesReady: true,
            firmwareReady: true,
            serviceAlreadyConfigured: false), .registerOnceAndMark)
    }

    func testRecordedDecisionAlwaysPreventsAutomaticReenable() {
        XCTAssertEqual(LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: true,
            prerequisitesReady: true,
            firmwareReady: true,
            serviceAlreadyConfigured: false), .noAction)
    }

    func testExistingConfigurationIsRecordedWithoutRegisteringAgain() {
        XCTAssertEqual(LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: false,
            prerequisitesReady: true,
            firmwareReady: true,
            serviceAlreadyConfigured: true), .markExistingConfiguration)
    }
}
