import Foundation

public enum ProtocolConstants {
    public static let schemaVersion = 1
    public static let remoteRoot = "/sdcard/PokeCapsule"
    public static let reservedFolders = ["Inbox", "Archive"]
}

public struct CapsuleMetadata: Codable, Hashable, Identifiable {
    public var schemaVersion: Int
    public var id: UUID
    public var title: String?
    public var createdAt: Date
    public var updatedAt: Date
    public var revision: Int
    public var favorite: Bool
    public var tags: [String]
    public var language: String?
    public var contentHash: String?

    public init(
        schemaVersion: Int = ProtocolConstants.schemaVersion,
        id: UUID,
        title: String? = nil,
        createdAt: Date,
        updatedAt: Date,
        revision: Int = 1,
        favorite: Bool = false,
        tags: [String] = [],
        language: String? = "zh",
        contentHash: String? = nil
    ) {
        self.schemaVersion = schemaVersion
        self.id = id
        self.title = title
        self.createdAt = createdAt
        self.updatedAt = updatedAt
        self.revision = revision
        self.favorite = favorite
        self.tags = tags
        self.language = language
        self.contentHash = contentHash
    }
}

public enum ProcessingStatus: String, Codable, CaseIterable {
    case recording
    case recorded
    case queued
    case transcribing
    case rawReady = "raw_ready"
    case correcting
    case ready
    case failed

    public var localizedName: String {
        switch self {
        case .recording: return "录音中"
        case .recorded: return "已录音"
        case .queued: return "等待转写"
        case .transcribing: return "正在转写"
        case .rawReady: return "待校对"
        case .correcting: return "正在校对"
        case .ready: return "已完成"
        case .failed: return "处理失败"
        }
    }
}

public struct ProcessingMetadata: Codable, Hashable {
    public var schemaVersion: Int
    public var capsuleId: UUID
    public var revision: Int
    public var durationMs: Int
    public var status: ProcessingStatus
    public var audioFile: String
    public var rawTextFile: String?
    public var polishedTextFile: String?
    public var errorStage: String?
    public var error: String?
    public var attempts: Int?
    public var engine: String?
    public var model: String?
}

public struct CapsuleRecord: Identifiable, Hashable {
    public var id: UUID { capsule.id }
    public let capsule: CapsuleMetadata
    public let processing: ProcessingMetadata?
    public let relativeFolder: String
    public let localDirectory: URL
    public let rawText: String?
    public let polishedText: String?
    public let warnings: [String]

    public var readOnly: Bool {
        capsule.schemaVersion != ProtocolConstants.schemaVersion
            || processing.map { $0.schemaVersion != ProtocolConstants.schemaVersion } == true
    }

    public var displayTitle: String {
        let value = capsule.title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? "未命名胶囊" : value
    }

    public var displayPreview: String {
        let durationMs = processing?.durationMs ?? 0
        let raw = normalizedPreview(rawText) ?? ""
        let polished = normalizedPreview(polishedText) ?? ""
        if TranscriptionSanity.isPlausible(text: polished, durationMs: durationMs),
           raw.isEmpty || isPlausibleCorrection(polished: polished, raw: raw) {
            return polished
        }
        if TranscriptionSanity.isPlausible(text: raw, durationMs: durationMs) {
            return raw
        }
        if !polished.isEmpty || !raw.isEmpty {
            return "转写结果异常，请播放录音"
        }
        switch processing?.status {
        case .recording: return "正在录音…"
        case .recorded, .queued: return "等待插电和 Wi‑Fi 转写"
        case .transcribing: return "正在转写…"
        case .failed: return "转写失败，可稍后重试"
        default: return displayTitle
        }
    }

    public var visibleProcessingStatus: String? {
        switch processing?.status {
        case .recording: return "录音中"
        case .recorded, .queued: return "等待转写"
        case .transcribing: return "转写中"
        case .correcting: return "校对中"
        case .failed: return "转写失败"
        default: return nil
        }
    }

    private func normalizedPreview(_ value: String?) -> String? {
        value?
            .split(whereSeparator: \.isWhitespace)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private func isPlausibleCorrection(polished: String, raw: String) -> Bool {
        polished.count <= max(raw.count + 20, raw.count * 2)
    }
}

public struct CapsuleIndex: Equatable {
    public var records: [CapsuleRecord]
    public var folders: [String]
    public var warnings: [String]

    public init(records: [CapsuleRecord] = [], folders: [String] = [], warnings: [String] = []) {
        self.records = records
        self.folders = folders
        self.warnings = warnings
    }
}

public enum DeviceConnectionState: Equatable {
    case noADB
    case noDevice
    case unauthorized([String])
    case offline([String])
    case connected(ADBDevice)
    case multiple([ADBDevice])

    public var localizedDescription: String {
        switch self {
        case .noADB: return "没有找到 ADB"
        case .noDevice: return "未连接设备"
        case .unauthorized: return "设备尚未授权，请在 Poke3 上允许 USB 调试"
        case .offline: return "设备离线，请重新插拔 USB"
        case .connected(let device): return "已连接：\(device.displayName)"
        case .multiple: return "检测到多台设备，请选择 Poke3"
        }
    }
}

public struct ADBDevice: Codable, Hashable, Identifiable {
    public var id: String { serial }
    public let serial: String
    public let state: String
    public let model: String?
    public let product: String?
    public let transportID: String?

    public init(serial: String, state: String, model: String? = nil, product: String? = nil, transportID: String? = nil) {
        self.serial = serial
        self.state = state
        self.model = model
        self.product = product
        self.transportID = transportID
    }

    public var displayName: String {
        model?.replacingOccurrences(of: "_", with: " ") ?? serial
    }
}

public struct DeviceCommand: Codable, Equatable {
    public let schemaVersion: Int
    public let transactionId: UUID
    public let operation: String
    public let createdAt: Date
    public var maintenanceId: UUID?
    public var capsuleIds: [UUID]?
    public var destination: String?
    public var folderPath: String?
    public var newFolderPath: String?
    public var tags: [String]?
    public var favorite: Bool?
    public var stagedPath: String?
    public var expectedRevision: Int?

    public init(
        transactionId: UUID = UUID(),
        operation: String,
        maintenanceId: UUID? = nil,
        capsuleIds: [UUID]? = nil,
        destination: String? = nil,
        folderPath: String? = nil,
        newFolderPath: String? = nil,
        tags: [String]? = nil,
        favorite: Bool? = nil,
        stagedPath: String? = nil,
        expectedRevision: Int? = nil
    ) {
        self.schemaVersion = ProtocolConstants.schemaVersion
        self.transactionId = transactionId
        self.operation = operation
        self.createdAt = Date()
        self.maintenanceId = maintenanceId
        self.capsuleIds = capsuleIds
        self.destination = destination
        self.folderPath = folderPath
        self.newFolderPath = newFolderPath
        self.tags = tags
        self.favorite = favorite
        self.stagedPath = stagedPath
        self.expectedRevision = expectedRevision
    }
}

public struct DeviceCommandResponse: Codable, Equatable {
    public let schemaVersion: Int
    public let transactionId: UUID
    public let success: Bool
    public let message: String?
    public let completedAt: Date?
}

public enum PokeCapsuleError: LocalizedError, Equatable {
    case invalidFolderName(String)
    case invalidRelativePath(String)
    case unsupportedSchema(Int)
    case malformedCapsule(String)
    case adbUnavailable
    case adbFailure(String)
    case deviceUnavailable(String)
    case maintenanceRejected(String)
    case commandTimedOut
    case uuidConflict(UUID)
    case hashMismatch(String)
    case missingAPIKey
    case invalidCorrectionResponse
    case invalidAPIKeyFile

    public var errorDescription: String? {
        switch self {
        case .invalidFolderName(let value): return "目录名不合法：\(value)"
        case .invalidRelativePath(let value): return "目录路径不合法：\(value)"
        case .unsupportedSchema(let version): return "协议版本 \(version) 暂不支持，只能读取"
        case .malformedCapsule(let value): return "胶囊数据损坏：\(value)"
        case .adbUnavailable: return "没有找到 ADB"
        case .adbFailure(let value): return "ADB 操作失败：\(value)"
        case .deviceUnavailable(let value): return "设备不可用：\(value)"
        case .maintenanceRejected(let value): return "Poke3 未允许维护操作：\(value)"
        case .commandTimedOut: return "等待 Poke3 确认超时，设备没有被修改"
        case .uuidConflict(let id): return "UUID 冲突：\(id.uuidString)"
        case .hashMismatch(let path): return "文件校验失败：\(path)"
        case .missingAPIKey: return "尚未配置 API 密钥"
        case .invalidCorrectionResponse: return "校对 API 返回了无法识别的内容"
        case .invalidAPIKeyFile: return "api.txt 第一行不是有效的 API 密钥"
        }
    }
}

public enum PokeJSON {
    public static let encoder: JSONEncoder = {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        encoder.dateEncodingStrategy = .iso8601
        return encoder
    }()

    public static let decoder: JSONDecoder = {
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        return decoder
    }()
}
