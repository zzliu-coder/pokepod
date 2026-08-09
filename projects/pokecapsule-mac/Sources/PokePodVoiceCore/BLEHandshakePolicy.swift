import Foundation

public enum BLEHandshakeError: Error, Equatable {
    case mtuProxyTooSmall(actual: Int, required: Int)
    case firmwareMTURejected
    case invalidFirstNotificationLength(actual: Int, expected: Int)
}

public enum BLEHandshakePolicy {
    public static let minimumWritePayload = 182 // ATT MTU 185 minus ATT header.
    public static let readyStatusCode: UInt16 = 1
    public static let mtuRejectedStatusCode: UInt16 = 2

    public static func validate(maximumWriteValueLength: Int) throws {
        guard maximumWriteValueLength >= minimumWritePayload else {
            throw BLEHandshakeError.mtuProxyTooSmall(
                actual: maximumWriteValueLength, required: minimumWritePayload)
        }
    }

    public static func validateFirstAudioNotification(length: Int) throws {
        guard length == BLEVoiceAudioFrame.encodedLength else {
            throw BLEHandshakeError.invalidFirstNotificationLength(
                actual: length, expected: BLEVoiceAudioFrame.encodedLength)
        }
    }

    public static func validate(statusCode: UInt16) throws -> Bool {
        if statusCode == readyStatusCode { return true }
        if statusCode == mtuRejectedStatusCode { throw BLEHandshakeError.firmwareMTURejected }
        return false
    }
}
