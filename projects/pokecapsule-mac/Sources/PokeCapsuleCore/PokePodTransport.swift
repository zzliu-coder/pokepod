import Darwin
import Foundation

public enum LinkV2FrameType: UInt8, Equatable {
    case requestJSON = 1
    case responseJSON = 2
    case data = 3
    case eventJSON = 4
}

public struct LinkV2Frame: Equatable {
    public static let magic = Data("PPV2".utf8)
    public static let version: UInt8 = 2
    public static let headerLength = 20
    public static let maxControlLength = 4_096
    public static let maxDataLength = 16_384

    public let type: LinkV2FrameType
    public let flags: UInt16
    public let requestID: UInt32
    public let payload: Data

    public init(type: LinkV2FrameType, flags: UInt16 = 0, requestID: UInt32, payload: Data) {
        self.type = type
        self.flags = flags
        self.requestID = requestID
        self.payload = payload
    }

    public func encoded() throws -> Data {
        let limit = type == .data ? Self.maxDataLength : Self.maxControlLength
        guard payload.count <= limit else { throw LinkV2Error.payloadTooLarge(payload.count) }
        var result = Self.magic
        result.append(Self.version)
        result.append(type.rawValue)
        result.appendLittleEndian(flags)
        result.appendLittleEndian(requestID)
        result.appendLittleEndian(UInt32(payload.count))
        result.appendLittleEndian(LinkCRC32.checksum(payload))
        result.append(payload)
        return result
    }
}

public enum LinkV2Error: Error, Equatable, LocalizedError {
    case badMagic
    case unsupportedVersion(UInt8)
    case invalidFrameType(UInt8)
    case payloadTooLarge(Int)
    case crcMismatch
    case disconnected
    case timedOut
    case duplicateRequestID(UInt32)
    case malformedResponse(String)
    case deviceRejected(String)
    case busyExhausted

    public var errorDescription: String? {
        switch self {
        case .badMagic: return "PokePod Link 帧头无效"
        case .unsupportedVersion(let value): return "PokePod Link 版本 \(value) 不受支持"
        case .invalidFrameType(let value): return "PokePod Link 帧类型 \(value) 无效"
        case .payloadTooLarge(let value): return "PokePod Link 负载过大：\(value) 字节"
        case .crcMismatch: return "PokePod Link 数据校验失败"
        case .disconnected: return "PokePod USB 连接已断开"
        case .timedOut: return "等待 PokePod 响应超时"
        case .duplicateRequestID(let value): return "PokePod 返回了重复请求号：\(value)"
        case .malformedResponse(let value): return "PokePod 响应无法解析：\(value)"
        case .deviceRejected(let value): return "PokePod 拒绝操作：\(value)"
        case .busyExhausted: return "PokePod 持续忙碌，请稍后重试"
        }
    }
}

public enum LinkCRC32 {
    public static func checksum(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xffff_ffff
        for byte in data {
            crc ^= UInt32(byte)
            for _ in 0..<8 {
                crc = (crc >> 1) ^ (0xedb8_8320 & UInt32(bitPattern: -Int32(crc & 1)))
            }
        }
        return ~crc
    }
}

public struct LinkV2FrameParser {
    private var buffer = Data()

    public init() {}

    public mutating func append(_ data: Data) throws -> [LinkV2Frame] {
        buffer.append(data)
        var frames: [LinkV2Frame] = []
        while buffer.count >= LinkV2Frame.headerLength {
            let bytes = [UInt8](buffer.prefix(LinkV2Frame.headerLength))
            guard Data(bytes[0..<4]) == LinkV2Frame.magic else { throw LinkV2Error.badMagic }
            guard bytes[4] == LinkV2Frame.version else {
                throw LinkV2Error.unsupportedVersion(bytes[4])
            }
            guard let type = LinkV2FrameType(rawValue: bytes[5]) else {
                throw LinkV2Error.invalidFrameType(bytes[5])
            }
            let flags = bytes.littleEndianUInt16(at: 6)
            let requestID = bytes.littleEndianUInt32(at: 8)
            let length = Int(bytes.littleEndianUInt32(at: 12))
            let expectedCRC = bytes.littleEndianUInt32(at: 16)
            let limit = type == .data ? LinkV2Frame.maxDataLength : LinkV2Frame.maxControlLength
            guard length <= limit else { throw LinkV2Error.payloadTooLarge(length) }
            let frameLength = LinkV2Frame.headerLength + length
            guard buffer.count >= frameLength else { break }
            let payload = Data(buffer[LinkV2Frame.headerLength..<frameLength])
            guard LinkCRC32.checksum(payload) == expectedCRC else {
                throw LinkV2Error.crcMismatch
            }
            frames.append(LinkV2Frame(
                type: type, flags: flags, requestID: requestID, payload: payload))
            buffer = Data(buffer.dropFirst(frameLength))
        }
        return frames
    }
}

public final class LinkRequestRegistry {
    private var completed = Set<UInt32>()

    public init() {}

    public func markCompleted(_ requestID: UInt32) throws {
        guard completed.insert(requestID).inserted else {
            throw LinkV2Error.duplicateRequestID(requestID)
        }
        if completed.count > 4_096 { completed.removeAll(keepingCapacity: true) }
    }

    public func isCompleted(_ requestID: UInt32) -> Bool {
        completed.contains(requestID)
    }
}

public protocol LinkV2ByteChannel: AnyObject {
    func write(_ data: Data) throws
    func read(maxLength: Int, timeout: TimeInterval) throws -> Data
}

public final class POSIXCDCChannel: LinkV2ByteChannel {
    private let descriptor: Int32

    public init(deviceURL: URL) throws {
        let value = Darwin.open(deviceURL.path, O_RDWR | O_NOCTTY | O_NONBLOCK)
        guard value >= 0 else {
            throw PokeCapsuleError.deviceUnavailable("无法打开 \(deviceURL.lastPathComponent)")
        }
        var options = termios()
        guard tcgetattr(value, &options) == 0 else {
            Darwin.close(value)
            throw PokeCapsuleError.deviceUnavailable("无法读取 PokePod 串口参数")
        }
        cfmakeraw(&options)
        _ = cfsetspeed(&options, speed_t(B115200))
        guard tcsetattr(value, TCSANOW, &options) == 0 else {
            Darwin.close(value)
            throw PokeCapsuleError.deviceUnavailable("无法设置 PokePod 串口参数")
        }
        _ = tcflush(value, TCIOFLUSH)
        descriptor = value
        _ = fcntl(descriptor, F_SETFL, 0)
    }

    deinit { Darwin.close(descriptor) }

    public func write(_ data: Data) throws {
        try data.withUnsafeBytes { rawBuffer in
            guard let base = rawBuffer.baseAddress else { return }
            var offset = 0
            while offset < rawBuffer.count {
                let count = Darwin.write(descriptor, base.advanced(by: offset), rawBuffer.count - offset)
                if count <= 0 { throw LinkV2Error.disconnected }
                offset += count
            }
        }
    }

    public func read(maxLength: Int, timeout: TimeInterval) throws -> Data {
        var pending = pollfd(fd: descriptor, events: Int16(POLLIN), revents: 0)
        let milliseconds = Int32(max(1, min(timeout * 1_000, Double(Int32.max))))
        let result = Darwin.poll(&pending, 1, milliseconds)
        if result == 0 { throw LinkV2Error.timedOut }
        if result < 0 || (pending.revents & Int16(POLLHUP | POLLERR | POLLNVAL)) != 0 {
            throw LinkV2Error.disconnected
        }
        var bytes = [UInt8](repeating: 0, count: maxLength)
        let count = Darwin.read(descriptor, &bytes, bytes.count)
        guard count > 0 else { throw LinkV2Error.disconnected }
        return Data(bytes.prefix(count))
    }
}

public enum PokePodLinkOperation: String, CaseIterable {
    case hello
    case status
    case identity
    case fingerprint
    case read
    case stageWrite = "stage-write"
    case commit
    case command
    case result
    case configure
    case setTime = "set-time"
    case dictateStart = "dictate-start"
    case dictateStop = "dictate-stop"
    case dictate
    case reboot
}

public struct PokePodLinkResponse {
    public let control: [String: Any]
    public let binary: Data
}

public final class PokePodLinkClient {
    private let channel: LinkV2ByteChannel
    private let timeout: TimeInterval
    private let maxBusyRetries: Int
    private var parser = LinkV2FrameParser()
    private var nextRequestID: UInt32
    private let registry = LinkRequestRegistry()
    private let lock = NSLock()

    public init(
        channel: LinkV2ByteChannel,
        timeout: TimeInterval = 5,
        maxBusyRetries: Int = 3,
        initialRequestID: UInt32? = nil
    ) {
        self.channel = channel
        self.timeout = timeout
        self.maxBusyRetries = maxBusyRetries
        self.nextRequestID = initialRequestID.flatMap { $0 == 0 ? nil : $0 }
            ?? UInt32.random(in: 1...UInt32.max)
    }

    public func call(
        _ operation: PokePodLinkOperation,
        fields: [String: Any] = [:],
        binary: Data? = nil
    ) throws -> PokePodLinkResponse {
        lock.lock()
        defer { lock.unlock() }
        var attempt = 0
        while true {
            let requestID = allocateRequestID()
            var request = fields
            request["operation"] = operation.rawValue
            request["version"] = 2
            let control = try JSONSerialization.data(withJSONObject: request, options: [.sortedKeys])
            try channel.write(LinkV2Frame(
                type: .requestJSON, requestID: requestID, payload: control).encoded())
            if let binary {
                for chunk in binary.chunks(of: LinkV2Frame.maxDataLength) {
                    try channel.write(LinkV2Frame(
                        type: .data, flags: chunk.isLast ? 1 : 0,
                        requestID: requestID, payload: chunk.data).encoded())
                }
            }
            let response = try receive(requestID: requestID)
            let status = (response.control["status"] as? String) ?? "ok"
            if status == "busy" {
                attempt += 1
                guard attempt <= maxBusyRetries else { throw LinkV2Error.busyExhausted }
                let requested = response.control["retryAfterMs"] as? NSNumber
                let fallback = 100 * (1 << min(attempt - 1, 4))
                let delay = min(2_000, max(25, requested?.intValue ?? fallback))
                Thread.sleep(forTimeInterval: Double(delay) / 1_000)
                continue
            }
            guard status == "ok" else {
                throw LinkV2Error.deviceRejected(
                    response.control["message"] as? String ?? status)
            }
            try registry.markCompleted(requestID)
            return response
        }
    }

    private func receive(requestID: UInt32) throws -> PokePodLinkResponse {
        let deadline = Date().addingTimeInterval(timeout)
        var response: [String: Any]?
        var binary = Data()
        var expectedBinary = 0
        repeat {
            let incoming = try channel.read(
                maxLength: LinkV2Frame.headerLength + LinkV2Frame.maxDataLength,
                timeout: max(0.01, deadline.timeIntervalSinceNow))
            for frame in try parser.append(incoming) {
                if frame.requestID != requestID {
                    if registry.isCompleted(frame.requestID) {
                        throw LinkV2Error.duplicateRequestID(frame.requestID)
                    }
                    continue
                }
                switch frame.type {
                case .responseJSON:
                    guard response == nil else {
                        throw LinkV2Error.duplicateRequestID(requestID)
                    }
                    guard let object = try JSONSerialization.jsonObject(with: frame.payload) as? [String: Any] else {
                        throw LinkV2Error.malformedResponse("JSON 顶层必须为对象")
                    }
                    response = object
                    expectedBinary = (object["binaryLength"] as? NSNumber)?.intValue ?? 0
                case .data:
                    binary.append(frame.payload)
                case .requestJSON, .eventJSON:
                    continue
                }
            }
            if let response, binary.count >= expectedBinary {
                guard binary.count == expectedBinary else {
                    throw LinkV2Error.malformedResponse("二进制长度不一致")
                }
                return PokePodLinkResponse(control: response, binary: binary)
            }
        } while Date() < deadline
        throw LinkV2Error.timedOut
    }

    private func allocateRequestID() -> UInt32 {
        defer { nextRequestID = nextRequestID == UInt32.max ? 1 : nextRequestID + 1 }
        return nextRequestID
    }
}

public final class PokePodTransport: DeviceTransport {
    public let deviceURL: URL
    public let deviceIdentifier: String
    private let client: PokePodLinkClient
    private let fileManager: FileManager

    public init(
        deviceURL: URL,
        channel: LinkV2ByteChannel? = nil,
        fileManager: FileManager = .default
    ) throws {
        self.deviceURL = deviceURL
        self.deviceIdentifier = deviceURL.lastPathComponent
        self.fileManager = fileManager
        self.client = PokePodLinkClient(channel: try channel ?? POSIXCDCChannel(deviceURL: deviceURL))
    }

    public static func discover(fileManager: FileManager = .default) -> [URL] {
        let root = URL(fileURLWithPath: "/dev")
        let names = (try? fileManager.contentsOfDirectory(atPath: root.path)) ?? []
        return names.filter {
            $0.hasPrefix("cu.usbmodem") || $0.hasPrefix("cu.PokePod")
        }.sorted().map { root.appendingPathComponent($0) }
    }

    @discardableResult
    public func hello() throws -> [String: Any] {
        try client.call(.hello, fields: ["client": "PokeCapsuleMac"]).control
    }

    public func status() throws -> [String: Any] { try client.call(.status).control }

    public func readDeviceIdentity() throws -> DeviceIdentity? {
        let value = try client.call(.identity).control
        guard let deviceID = value["deviceId"] as? String else { return nil }
        return DeviceIdentity(
            deviceId: deviceID,
            displayName: value["displayName"] as? String ?? "PokePod",
            platform: value["platform"] as? String ?? "pokepod",
            manufacturer: value["manufacturer"] as? String ?? "PokePod",
            model: value["model"] as? String ?? "AMOLED")
    }

    public func metadataFingerprint() throws -> String {
        let value = try client.call(.fingerprint).control
        guard let fingerprint = value["fingerprint"] as? String else {
            throw LinkV2Error.malformedResponse("缺少 fingerprint")
        }
        return fingerprint
    }

    public func fetchLibrary(to local: URL) throws {
        try fileManager.createDirectory(at: local, withIntermediateDirectories: true)
        var cursor: String?
        var seen = Set<String>()
        repeat {
            var fields: [String: Any] = ["path": ".", "recursive": true]
            if let cursor { fields["cursor"] = cursor }
            let listing = try client.call(.read, fields: fields).control
            guard let files = listing["files"] as? [[String: Any]] else {
                throw LinkV2Error.malformedResponse("递归读取缺少 files")
            }
            for item in files {
                guard let path = item["path"] as? String else { continue }
                let relative = try safeRelativePath(path)
                guard seen.insert(relative).inserted else {
                    throw LinkV2Error.malformedResponse("清单含有重复路径：\(relative)")
                }
                let result = try client.call(.read, fields: ["path": relative])
                if let expected = item["length"] as? NSNumber,
                   expected.intValue != result.binary.count {
                    throw LinkV2Error.malformedResponse("\(relative) 长度不一致")
                }
                let destination = local.appendingPathComponent(relative)
                try fileManager.createDirectory(
                    at: destination.deletingLastPathComponent(), withIntermediateDirectories: true)
                try result.binary.write(to: destination, options: .atomic)
            }
            cursor = (listing["nextCursor"] as? String).flatMap { $0.isEmpty ? nil : $0 }
        } while cursor != nil
    }

    public func submitCommandFile(local: URL, transactionID: UUID) throws {
        _ = try client.call(.command, fields: [
            "transactionId": transactionID.uuidString.lowercased(),
            "binaryLength": (try Data(contentsOf: local)).count
        ], binary: try Data(contentsOf: local))
    }

    public func stageImport(local: URL, transactionID: UUID, capsuleID: UUID) throws {
        let base = local.resolvingSymlinksInPath().standardizedFileURL
        guard let enumerator = fileManager.enumerator(
            at: base, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles]) else {
            throw PokeCapsuleError.malformedCapsule("无法读取待写入文件")
        }
        while let file = enumerator.nextObject() as? URL {
            guard (try file.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else { continue }
            let canonical = file.resolvingSymlinksInPath().standardizedFileURL
            guard canonical.path.hasPrefix(base.path + "/") else {
                throw PokeCapsuleError.invalidRelativePath(canonical.path)
            }
            let relative = try safeRelativePath(String(canonical.path.dropFirst(base.path.count + 1)))
            let data = try Data(contentsOf: canonical)
            _ = try client.call(.stageWrite, fields: [
                "transactionId": transactionID.uuidString.lowercased(),
                "capsuleId": capsuleID.uuidString.lowercased(),
                "path": relative,
                "binaryLength": data.count
            ], binary: data)
        }
        _ = try client.call(.commit, fields: [
            "transactionId": transactionID.uuidString.lowercased(),
            "capsuleId": capsuleID.uuidString.lowercased(),
            "stagingOnly": true
        ])
    }

    public func fetchCommandResult(transactionID: UUID, to local: URL) throws -> Bool {
        let response = try client.call(.result, fields: [
            "transactionId": transactionID.uuidString.lowercased()
        ])
        if (response.control["available"] as? Bool) == false { return false }
        guard !response.binary.isEmpty else {
            throw LinkV2Error.malformedResponse("命令结果为空")
        }
        try response.binary.write(to: local, options: .atomic)
        return true
    }

    public func configure(_ values: [String: Any]) throws {
        _ = try client.call(.configure, fields: ["values": values])
    }

    public func setTime(_ date: Date) throws {
        _ = try client.call(.setTime, fields: ["unixTimeMs": Int64(date.timeIntervalSince1970 * 1_000)])
    }

    public func beginDictationHold() throws { _ = try client.call(.dictateStart) }
    public func endDictationHold() throws { _ = try client.call(.dictateStop) }

    @available(*, deprecated, message: "Use beginDictationHold()/endDictationHold() for press-and-hold dictation")
    public func dictate() throws { _ = try client.call(.dictate) }
    public func reboot() throws { _ = try client.call(.reboot) }

    private func safeRelativePath(_ value: String) throws -> String {
        let normalized = value.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !normalized.isEmpty,
              !normalized.contains("\0"),
              !normalized.split(separator: "/").contains("..") else {
            throw PokeCapsuleError.invalidRelativePath(value)
        }
        return normalized
    }
}

public enum PokePodPortMatcher {
    public static func match(
        registered: RegisteredDevice,
        discoveredPorts: [URL],
        identify: (URL) throws -> DeviceIdentity?
    ) -> URL? {
        guard registered.isPokePod else { return nil }
        if let exact = discoveredPorts.first(where: {
            registered.serialAliases.contains($0.path)
        }) {
            return exact
        }
        for port in discoveredPorts {
            guard let identity = try? identify(port) else { continue }
            if identity.deviceId == registered.deviceId { return port }
        }
        return nil
    }
}

private extension Data {
    mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
        var encoded = value.littleEndian
        Swift.withUnsafeBytes(of: &encoded) { append(contentsOf: $0) }
    }

    func chunks(of size: Int) -> [(data: Data, isLast: Bool)] {
        guard !isEmpty else { return [(Data(), true)] }
        var chunks: [(Data, Bool)] = []
        var offset = 0
        while offset < count {
            let end = Swift.min(offset + size, count)
            chunks.append((Data(self[offset..<end]), end == count))
            offset = end
        }
        return chunks
    }
}

private extension Array where Element == UInt8 {
    func littleEndianUInt16(at offset: Int) -> UInt16 {
        UInt16(self[offset]) | (UInt16(self[offset + 1]) << 8)
    }

    func littleEndianUInt32(at offset: Int) -> UInt32 {
        UInt32(self[offset])
            | (UInt32(self[offset + 1]) << 8)
            | (UInt32(self[offset + 2]) << 16)
            | (UInt32(self[offset + 3]) << 24)
    }
}
