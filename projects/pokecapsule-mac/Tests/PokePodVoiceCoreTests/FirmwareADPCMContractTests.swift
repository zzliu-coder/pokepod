import Foundation
import XCTest
@testable import PokePodVoiceCore

final class FirmwareADPCMContractTests: XCTestCase {
    func testFirmwareConstantFramePreservesHeaderSampleAndProduces320Samples() throws {
        let frame = try BLEVoiceAudioFrame.decode(firmwareConstantWire(predictor: 1_234))
        let decoded = try IMAADPCM.decode(frame)
        XCTAssertEqual(decoded.count, 320)
        XCTAssertEqual(decoded.first, 1_234)
        XCTAssertEqual(decoded, [Int16](repeating: 1_234, count: 320))
    }

    func testFirmwareFinalHighPaddingNibbleDoesNotAffectPCM() throws {
        let normal = firmwareConstantWire(predictor: 1_234, paddingHighNibble: 0)
        let hostilePadding = firmwareConstantWire(predictor: 1_234, paddingHighNibble: 0x0f)
        XCTAssertNotEqual(normal.last, hostilePadding.last)
        XCTAssertEqual(
            try IMAADPCM.decode(BLEVoiceAudioFrame.decode(normal)),
            try IMAADPCM.decode(BLEVoiceAudioFrame.decode(hostilePadding)))
    }

    func testFirmwareFramesSeedIndependentPredictors() throws {
        let first = try IMAADPCM.decode(BLEVoiceAudioFrame.decode(
            firmwareConstantWire(predictor: 12_000)))
        let second = try IMAADPCM.decode(BLEVoiceAudioFrame.decode(
            firmwareConstantWire(predictor: -12_000)))
        XCTAssertEqual(first.first, 12_000)
        XCTAssertEqual(second.first, -12_000)
        XCTAssertEqual(second, [Int16](repeating: -12_000, count: 320))
    }

    private func firmwareConstantWire(
        predictor: Int16,
        paddingHighNibble: UInt8 = 0
    ) -> Data {
        let bits = UInt16(bitPattern: predictor)
        var bytes = Data([
            1, 0,                   // version, flags
            7, 0, 0, 0,            // sessionId
            9, 0, 0, 0,            // sequence
            0x40, 0x01,             // 320 samples
            UInt8(bits & 0xff), UInt8(bits >> 8),
            0                       // step index
        ])
        bytes.append(Data(repeating: 0, count: 160))
        bytes[bytes.count - 1] = paddingHighNibble << 4
        return bytes
    }
}
