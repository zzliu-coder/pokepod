import Foundation

public enum BLEVoiceUUID {
    public static let service = "7A530001-4B50-4F44-9000-504F4B45504F"
    public static let deviceInfo = "7A530002-4B50-4F44-9000-504F4B45504F"
    public static let command = "7A530003-4B50-4F44-9000-504F4B45504F"
    public static let event = "7A530004-4B50-4F44-9000-504F4B45504F"
    public static let audio = "7A530005-4B50-4F44-9000-504F4B45504F"
}

public enum BLEVoiceWireError: Error, Equatable {
    case malformedLength(expected: Int, actual: Int)
    case unsupportedVersion(UInt8)
    case unknownType(UInt8)
    case invalidSampleCount(UInt16)
    case invalidStepIndex(UInt8)
    case truncatedAudio(expected: Int, actual: Int)
    case invalidDeviceInfo
}

public struct BLEVoiceDeviceInfo: Codable, Equatable {
    public let protocolVersion: Int
    public let deviceId: String
    public let firmwareVersion: String
    public let codec: String
    public let sampleRateHz: Int
    public let batteryPercent: Int
    /// `notifyAccepted` means that NimBLE accepted the notification into its
    /// host queue. It is deliberately not described as an over-the-air
    /// delivery acknowledgement.
    public let notifyAttempts: UInt64?
    public let notifyAccepted: UInt64?
    public let notifyFailures: UInt64?
    public let queueOverflows: UInt64?
    public let sessionFailures: UInt64?
    public let readyTimeouts: UInt64?
    public let streamTimeouts: UInt64?
    public let stopAckTimeouts: UInt64?
    public let lastErrorCode: UInt64?

    public init(
        protocolVersion: Int = 1,
        deviceId: String,
        firmwareVersion: String,
        codec: String = "ima-adpcm",
        sampleRateHz: Int = 16_000,
        batteryPercent: Int,
        notifyAttempts: UInt64? = nil,
        notifyAccepted: UInt64? = nil,
        notifyFailures: UInt64? = nil,
        queueOverflows: UInt64? = nil,
        sessionFailures: UInt64? = nil,
        readyTimeouts: UInt64? = nil,
        streamTimeouts: UInt64? = nil,
        stopAckTimeouts: UInt64? = nil,
        lastErrorCode: UInt64? = nil
    ) {
        self.protocolVersion = protocolVersion
        self.deviceId = deviceId
        self.firmwareVersion = firmwareVersion
        self.codec = codec
        self.sampleRateHz = sampleRateHz
        self.batteryPercent = batteryPercent
        self.notifyAttempts = notifyAttempts
        self.notifyAccepted = notifyAccepted
        self.notifyFailures = notifyFailures
        self.queueOverflows = queueOverflows
        self.sessionFailures = sessionFailures
        self.readyTimeouts = readyTimeouts
        self.streamTimeouts = streamTimeouts
        self.stopAckTimeouts = stopAckTimeouts
        self.lastErrorCode = lastErrorCode
    }

    public static func decode(_ data: Data) throws -> Self {
        do { return try JSONDecoder().decode(Self.self, from: data) }
        catch { throw BLEVoiceWireError.invalidDeviceInfo }
    }

    public func deterministicJSON() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
        return try encoder.encode(self)
    }
}

public enum BLEVoiceCommandType: UInt8, CaseIterable {
    case ready = 1
    case reject = 2
    case stopAcknowledged = 3
    case ping = 4
}

public enum BLEVoiceRejectCode: UInt16 {
    case mtuProxyTooSmall = 2
    case blackHoleMissing = 3
    case accessibilityMissing = 4
    case applicationNotReady = 5
    case platformSetupFailed = 6
    case audioOutputFailed = 7
    case audioSequenceInvalid = 8
    case sessionAborted = 9
    case forgetBond = 100
}

public struct BLEVoiceCommand: Equatable {
    public static let version: UInt8 = 1
    public static let encodedLength = 8

    public let type: BLEVoiceCommandType
    public let sessionId: UInt32
    public let code: UInt16

    public init(type: BLEVoiceCommandType, sessionId: UInt32, code: UInt16 = 0) {
        self.type = type
        self.sessionId = sessionId
        self.code = code
    }

    public func encoded() -> Data {
        var data = Data([Self.version, type.rawValue])
        data.appendLittleEndian(sessionId)
        data.appendLittleEndian(code)
        return data
    }

    public static func decode(_ data: Data) throws -> Self {
        guard data.count == encodedLength else {
            throw BLEVoiceWireError.malformedLength(expected: encodedLength, actual: data.count)
        }
        guard data[0] == version else { throw BLEVoiceWireError.unsupportedVersion(data[0]) }
        guard let type = BLEVoiceCommandType(rawValue: data[1]) else {
            throw BLEVoiceWireError.unknownType(data[1])
        }
        return Self(type: type, sessionId: data.readLittleEndian(at: 2), code: data.readLittleEndian(at: 6))
    }
}

public enum BLEVoiceEventType: UInt8, CaseIterable {
    case sessionStart = 1
    case sessionEnd = 2
    case status = 3
    case error = 4
}

public struct BLEVoiceEvent: Equatable {
    public static let version: UInt8 = 1
    public static let encodedLength = 8

    public let type: BLEVoiceEventType
    public let sessionId: UInt32
    public let code: UInt16

    public init(type: BLEVoiceEventType, sessionId: UInt32, code: UInt16 = 0) {
        self.type = type
        self.sessionId = sessionId
        self.code = code
    }

    public func encoded() -> Data {
        var data = Data([Self.version, type.rawValue])
        data.appendLittleEndian(sessionId)
        data.appendLittleEndian(code)
        return data
    }

    public static func decode(_ data: Data) throws -> Self {
        guard data.count == encodedLength else {
            throw BLEVoiceWireError.malformedLength(expected: encodedLength, actual: data.count)
        }
        guard data[0] == version else { throw BLEVoiceWireError.unsupportedVersion(data[0]) }
        guard let type = BLEVoiceEventType(rawValue: data[1]) else {
            throw BLEVoiceWireError.unknownType(data[1])
        }
        return Self(type: type, sessionId: data.readLittleEndian(at: 2), code: data.readLittleEndian(at: 6))
    }
}

public struct BLEVoiceAudioFrame: Equatable {
    public static let version: UInt8 = 1
    public static let headerLength = 15
    public static let samplesPerFrame: UInt16 = 320
    public static let encodedLength = 175

    public let flags: UInt8
    public let sessionId: UInt32
    public let sequence: UInt32
    public let sampleCount: UInt16
    public let predictor: Int16
    public let stepIndex: UInt8
    public let payload: Data

    public init(
        flags: UInt8 = 0,
        sessionId: UInt32,
        sequence: UInt32,
        sampleCount: UInt16 = samplesPerFrame,
        predictor: Int16,
        stepIndex: UInt8,
        payload: Data
    ) {
        self.flags = flags
        self.sessionId = sessionId
        self.sequence = sequence
        self.sampleCount = sampleCount
        self.predictor = predictor
        self.stepIndex = stepIndex
        self.payload = payload
    }

    public func encoded() throws -> Data {
        try validate()
        var data = Data([Self.version, flags])
        data.appendLittleEndian(sessionId)
        data.appendLittleEndian(sequence)
        data.appendLittleEndian(sampleCount)
        data.appendLittleEndian(UInt16(bitPattern: predictor))
        data.append(stepIndex)
        data.append(payload)
        return data
    }

    public static func decode(_ data: Data) throws -> Self {
        guard data.count >= headerLength else {
            throw BLEVoiceWireError.malformedLength(expected: headerLength, actual: data.count)
        }
        guard data[0] == version else { throw BLEVoiceWireError.unsupportedVersion(data[0]) }
        let frame = Self(
            flags: data[1],
            sessionId: data.readLittleEndian(at: 2),
            sequence: data.readLittleEndian(at: 6),
            sampleCount: data.readLittleEndian(at: 10),
            predictor: Int16(bitPattern: data.readLittleEndian(at: 12) as UInt16),
            stepIndex: data[14],
            payload: data.subdata(in: headerLength..<data.count))
        try frame.validate()
        return frame
    }

    /// Reads only the stable routing fields from an incoming notification.
    /// This is intentionally tolerant: malformed payloads still need a
    /// session/sequence hint for diagnostics before full decode rejects them.
    public static func notificationMetadata(
        from data: Data
    ) -> (sessionId: UInt32, sequence: UInt32)? {
        guard data.count >= headerLength else { return nil }
        return (
            sessionId: data.readLittleEndian(at: 2),
            sequence: data.readLittleEndian(at: 6))
    }

    private func validate() throws {
        guard sampleCount > 0, sampleCount <= Self.samplesPerFrame else {
            throw BLEVoiceWireError.invalidSampleCount(sampleCount)
        }
        guard stepIndex <= 88 else { throw BLEVoiceWireError.invalidStepIndex(stepIndex) }
        // Predictor is sample 0; payload contains ceil((sampleCount - 1) / 2)
        // bytes, which is sampleCount / 2 for every positive integer count.
        let expected = Int(sampleCount) / 2
        guard payload.count == expected else {
            throw BLEVoiceWireError.truncatedAudio(expected: expected, actual: payload.count)
        }
    }
}

private extension Data {
    mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { append(contentsOf: $0) }
    }

    func readLittleEndian<T: FixedWidthInteger>(at offset: Int) -> T {
        let size = MemoryLayout<T>.size
        var value: T = 0
        _ = Swift.withUnsafeMutableBytes(of: &value) { destination in
            copyBytes(to: destination, from: offset..<(offset + size))
        }
        return T(littleEndian: value)
    }
}
