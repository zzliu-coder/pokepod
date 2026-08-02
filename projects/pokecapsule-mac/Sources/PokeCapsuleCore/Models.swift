import Foundation

public enum ProtocolConstants {
    public static let schemaVersion = 1
    public static let commandSchemaVersion = 2
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
    public var finalTextFile: String?

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
        self.finalTextFile = nil
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
    public let finalText: String?
    public let trash: TrashMetadata?
    public let warnings: [String]

    public init(
        capsule: CapsuleMetadata,
        processing: ProcessingMetadata?,
        relativeFolder: String,
        localDirectory: URL,
        rawText: String?,
        polishedText: String?,
        finalText: String? = nil,
        trash: TrashMetadata? = nil,
        warnings: [String]
    ) {
        self.capsule = capsule
        self.processing = processing
        self.relativeFolder = relativeFolder
        self.localDirectory = localDirectory
        self.rawText = rawText
        self.polishedText = polishedText
        self.finalText = finalText
        self.trash = trash
        self.warnings = warnings
    }

    public var readOnly: Bool {
        capsule.schemaVersion != ProtocolConstants.schemaVersion
            || processing.map { $0.schemaVersion != ProtocolConstants.schemaVersion } == true
    }

    public var displayTitle: String {
        let value = capsule.title?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return value.isEmpty ? "未命名胶囊" : value
    }

    public var displayPreview: String {
        if let text = primaryText { return text }
        let raw = normalizedPreview(rawText) ?? ""
        let polished = normalizedPreview(polishedText) ?? ""
        if !polished.isEmpty || !raw.isEmpty {
            return "转写结果异常，请播放录音"
        }
        switch processing?.status {
        case .recording: return "正在录音…"
        case .recorded, .queued: return "等待转写"
        case .transcribing: return "正在转写…"
        case .failed: return "转写失败，可稍后重试"
        default: return displayTitle
        }
    }

    public var primaryText: String? {
        let durationMs = processing?.durationMs ?? 0
        let raw = normalizedPreview(rawText) ?? ""
        let polished = normalizedPreview(polishedText) ?? ""
        if let final = normalizedPreview(finalText), !final.isEmpty {
            return final
        }
        if !polished.isEmpty,
           TranscriptionSanity.isPlausible(text: polished, durationMs: durationMs),
           raw.isEmpty || isPlausibleCorrection(polished: polished, raw: raw) {
            return polished
        }
        if !raw.isEmpty,
           TranscriptionSanity.isPlausible(text: raw, durationMs: durationMs) {
            return raw
        }
        return nil
    }

    public var primaryTextLabel: String {
        if normalizedPreview(finalText)?.isEmpty == false { return "最终文字" }
        if normalizedPreview(polishedText)?.isEmpty == false { return "校对文字" }
        if normalizedPreview(rawText)?.isEmpty == false { return "原始转写" }
        return "胶囊文字"
    }

    public var userFacingError: String? {
        guard let value = processing?.error?
            .trimmingCharacters(in: .whitespacesAndNewlines),
              !value.isEmpty else { return nil }
        if value.contains("ErrorVoicedataTooLong")
            || value.contains("longer than 60 seconds") {
            return "录音略微超过云端的 60 秒上限。原音已经保留，可以生成安全副本重新转写。"
        }
        if value.hasPrefix("AuthFailure") {
            return "转写服务配置失效，请在设备设置中重新导入。"
        }
        if value.hasPrefix("InvalidParameter")
            || value.hasPrefix("UnsupportedOperation") {
            return "这段录音暂时无法转写，原始录音仍然安全保存。"
        }
        if value.contains("Exception") || value.contains("Error") {
            return "转写暂时没有完成，原始录音仍然安全保存。"
        }
        return value
    }

    public var canRetryTranscription: Bool {
        guard processing?.status == .failed else { return false }
        let value = processing?.error ?? ""
        if value.hasPrefix("AuthFailure")
            || value.contains("配置失效")
            || value.contains("自动裁剪失败")
            || value.contains("超过自动修复范围")
            || value.contains("音轨")
            || value.contains("本地音频") {
            return false
        }
        return value.isEmpty
            || value.contains("ErrorVoicedataTooLong")
            || value.contains("longer than 60 seconds")
            || value.contains("网络")
            || value.contains("服务")
            || value.contains("稍后")
            || value.contains("连续失败")
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

public struct TrashMetadata: Codable, Hashable {
    public var schemaVersion: Int
    public var capsuleId: UUID
    public var trashedAt: Date
    public var originalFolder: String
    public var revision: Int

    public init(
        schemaVersion: Int = 1,
        capsuleId: UUID,
        trashedAt: Date,
        originalFolder: String,
        revision: Int
    ) {
        self.schemaVersion = schemaVersion
        self.capsuleId = capsuleId
        self.trashedAt = trashedAt
        self.originalFolder = originalFolder
        self.revision = revision
    }
}

public struct CapsuleIndex: Equatable {
    public var records: [CapsuleRecord]
    public var trashRecords: [CapsuleRecord]
    public var folders: [String]
    public var warnings: [String]

    public init(
        records: [CapsuleRecord] = [],
        trashRecords: [CapsuleRecord] = [],
        folders: [String] = [],
        warnings: [String] = []
    ) {
        self.records = records
        self.trashRecords = trashRecords
        self.folders = folders
        self.warnings = warnings
    }
}

public enum DeviceConnectionState: Equatable {
    case noADB
    case noDevice
    case unauthorized([String])
    case offline([String])
    case usbDetectedButADBUnavailable(USBPhysicalDevice)
    case connected(ADBDevice)
    case multiple([ADBDevice])

    public var localizedDescription: String {
        switch self {
        case .noADB: return "没有找到 ADB"
        case .noDevice: return "未连接设备"
        case .unauthorized: return "设备尚未授权，请在 Android 设备上允许 USB 调试"
        case .offline: return "设备离线，请重新插拔 USB"
        case .usbDetectedButADBUnavailable(let device):
            let owner = device.exclusiveOwner == nil ? "" : "；USB 正由另一进程使用"
            return "已识别到 \(device.displayName)；等待 ADB\(owner)"
        case .connected(let device): return "已连接：\(device.displayName)"
        case .multiple: return "检测到多台设备，请选择一台"
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

public struct USBPhysicalDevice: Equatable, Hashable, Identifiable {
    public var id: String { serial ?? "\(vendor ?? "unknown")-\(product ?? "unknown")" }
    public let serial: String?
    public let vendor: String?
    public let product: String?
    public let exclusiveOwner: String?

    public init(
        serial: String? = nil,
        vendor: String? = nil,
        product: String? = nil,
        exclusiveOwner: String? = nil
    ) {
        self.serial = serial
        self.vendor = vendor
        self.product = product
        self.exclusiveOwner = exclusiveOwner
    }

    public var displayName: String {
        product ?? vendor ?? serial ?? "Android USB 设备"
    }

    public var isLikelyAndroid: Bool {
        let fingerprint = [vendor, product].compactMap { $0 }.joined(separator: " ").lowercased()
        return ["android", "onyx", "boox", "vivo", "xiaomi", "redmi", "huawei", "honor", "oppo", "oneplus", "samsung", "google", "motorola"]
            .contains { fingerprint.contains($0) }
    }
}

public struct DeviceIdentity: Codable, Hashable, Identifiable {
    public var id: String { deviceId }
    public let schemaVersion: Int
    public let deviceId: String
    public let displayName: String
    public let platform: String
    public let manufacturer: String?
    public let model: String?
    public let androidVersion: String?
    public let createdAt: String?

    public init(
        schemaVersion: Int = 1,
        deviceId: String,
        displayName: String,
        platform: String = "android",
        manufacturer: String? = nil,
        model: String? = nil,
        androidVersion: String? = nil,
        createdAt: String? = nil
    ) {
        self.schemaVersion = schemaVersion
        self.deviceId = deviceId
        self.displayName = displayName
        self.platform = platform
        self.manufacturer = manufacturer
        self.model = model
        self.androidVersion = androidVersion
        self.createdAt = createdAt
    }
}

public struct RegisteredDevice: Codable, Hashable, Identifiable {
    public var id: String { deviceId }
    public var deviceId: String
    public var displayName: String
    public var platform: String
    public var manufacturer: String?
    public var model: String?
    public var serialAliases: [String]
    public var lastSeenAt: Date?

    public init(
        deviceId: String,
        displayName: String,
        platform: String = "android",
        manufacturer: String? = nil,
        model: String? = nil,
        serialAliases: [String] = [],
        lastSeenAt: Date? = nil
    ) {
        self.deviceId = deviceId
        self.displayName = displayName
        self.platform = platform
        self.manufacturer = manufacturer
        self.model = model
        self.serialAliases = serialAliases
        self.lastSeenAt = lastSeenAt
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
    public var expectedRevisions: [String: Int]?
    public var finalText: String?

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
        expectedRevision: Int? = nil,
        expectedRevisions: [String: Int]? = nil,
        finalText: String? = nil
    ) {
        self.schemaVersion = ProtocolConstants.commandSchemaVersion
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
        self.expectedRevisions = expectedRevisions
        self.finalText = finalText
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
        case .maintenanceRejected(let value): return "设备未允许维护操作：\(value)"
        case .commandTimedOut: return "等待设备确认超时，设备没有被修改"
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
