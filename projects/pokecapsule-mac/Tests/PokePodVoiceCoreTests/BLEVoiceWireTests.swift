import Foundation
import XCTest
@testable import PokePodVoiceCore

final class BLEVoiceWireTests: XCTestCase {
    func testUUIDContractIsStable() {
        XCTAssertEqual(BLEVoiceUUID.service, "7A530001-4B50-4F44-9000-504F4B45504F")
        XCTAssertEqual(BLEVoiceUUID.deviceInfo, "7A530002-4B50-4F44-9000-504F4B45504F")
        XCTAssertEqual(BLEVoiceUUID.command, "7A530003-4B50-4F44-9000-504F4B45504F")
        XCTAssertEqual(BLEVoiceUUID.event, "7A530004-4B50-4F44-9000-504F4B45504F")
        XCTAssertEqual(BLEVoiceUUID.audio, "7A530005-4B50-4F44-9000-504F4B45504F")
    }

    func testCommandAndEventUseFixedLittleEndianWire() throws {
        let command = BLEVoiceCommand(type: .ready, sessionId: 0x1234_5678, code: 0x9abc)
        XCTAssertEqual(command.encoded(), Data([1, 1, 0x78, 0x56, 0x34, 0x12, 0xbc, 0x9a]))
        XCTAssertEqual(try BLEVoiceCommand.decode(command.encoded()), command)

        let event = BLEVoiceEvent(type: .sessionStart, sessionId: 0x0102_0304, code: 7)
        XCTAssertEqual(try BLEVoiceEvent.decode(event.encoded()), event)
        XCTAssertThrowsError(try BLEVoiceEvent.decode(Data([2, 1, 0, 0, 0, 0, 0, 0]))) {
            XCTAssertEqual($0 as? BLEVoiceWireError, .unsupportedVersion(2))
        }
        XCTAssertThrowsError(try BLEVoiceCommand.decode(Data([1]))) {
            XCTAssertEqual($0 as? BLEVoiceWireError, .malformedLength(expected: 8, actual: 1))
        }
    }

    func testAudioFrameRoundTripAndLengthContract() throws {
        let frame = makeFrame(session: 0x1122_3344, sequence: 0xaabb_ccdd)
        let encoded = try frame.encoded()
        XCTAssertEqual(encoded.count, BLEVoiceAudioFrame.encodedLength)
        XCTAssertEqual(Array(encoded.prefix(15)), [
            1, 0, 0x44, 0x33, 0x22, 0x11, 0xdd, 0xcc, 0xbb, 0xaa,
            0x40, 0x01, 0xd2, 0x04, 10
        ])
        XCTAssertEqual(try BLEVoiceAudioFrame.decode(encoded), frame)

        var badVersion = encoded
        badVersion[0] = 2
        XCTAssertThrowsError(try BLEVoiceAudioFrame.decode(badVersion))
        XCTAssertThrowsError(try BLEVoiceAudioFrame.decode(encoded.dropLast())) {
            XCTAssertEqual($0 as? BLEVoiceWireError, .truncatedAudio(expected: 160, actual: 159))
        }
    }

    func testDeviceInfoJSONIsDeterministic() throws {
        let info = BLEVoiceDeviceInfo(
            deviceId: "POKEPOD-A994", firmwareVersion: "2.0.0", batteryPercent: 88)
        let first = try info.deterministicJSON()
        let second = try info.deterministicJSON()
        XCTAssertEqual(first, second)
        XCTAssertEqual(try BLEVoiceDeviceInfo.decode(first), info)
        XCTAssertThrowsError(try BLEVoiceDeviceInfo.decode(Data("[]".utf8)))
    }

    func testDeviceInfoAcceptsOptionalSendQueueDiagnosticsAndOldFirmware() throws {
        let oldJSON = Data(#"{"protocolVersion":1,"deviceId":"OLD","firmwareVersion":"1","codec":"ima-adpcm","sampleRateHz":16000,"batteryPercent":50}"#.utf8)
        let old = try BLEVoiceDeviceInfo.decode(oldJSON)
        XCTAssertNil(old.notifyAttempts)
        XCTAssertEqual(DeviceSendQueueQualityFormatter.summary(old), "固件未提供发送队列统计")

        let currentJSON = Data(#"{"protocolVersion":1,"deviceId":"NEW","firmwareVersion":"2","codec":"ima-adpcm","sampleRateHz":16000,"batteryPercent":90,"notifyAttempts":105,"notifyAccepted":100,"notifyFailures":5,"queueOverflows":2,"sessionFailures":1,"readyTimeouts":3,"streamTimeouts":6,"stopAckTimeouts":4,"lastErrorCode":7}"#.utf8)
        let current = try BLEVoiceDeviceInfo.decode(currentJSON)
        XCTAssertEqual(current.notifyAccepted, 100)
        XCTAssertEqual(
            DeviceSendQueueQualityFormatter.summary(current),
            "主机入队 100/尝试 105 · 主机拒绝 5 · 队列溢出 2 · 会话失败 1 · Ready 超时 3 · 音频超时 6 · StopAck 超时 4 · 最近错误码 7")
    }

    func testDeviceInfoRefreshPolicyCoversCompletionAndSessionFailure() {
        XCTAssertTrue(DeviceInfoRefreshPolicy.shouldRefresh(after: .sessionCompleted))
        XCTAssertTrue(DeviceInfoRefreshPolicy.shouldRefresh(after: .sessionFailed))
        XCTAssertFalse(DeviceInfoRefreshPolicy.shouldRefresh(after: .connectionChanged))
    }
}

func makeFrame(
    session: UInt32 = 7,
    sequence: UInt32 = 0,
    predictor: Int16 = 1_234,
    stepIndex: UInt8 = 10,
    byte: UInt8 = 0
) -> BLEVoiceAudioFrame {
    BLEVoiceAudioFrame(
        sessionId: session,
        sequence: sequence,
        predictor: predictor,
        stepIndex: stepIndex,
        payload: Data(repeating: byte, count: 160))
}
