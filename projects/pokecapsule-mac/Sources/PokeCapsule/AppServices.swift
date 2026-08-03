import AVFoundation
import Foundation
import PokeCapsuleCore

struct DeviceSyncResult {
    let index: CapsuleIndex
    let queue: [PendingCommand]
    let appliedCount: Int
    let backupWarning: String?
    let fingerprint: String
}

struct DeviceSyncEngine {
    func run(
        transport: ADBTransport,
        mirror: URL,
        queueFile: URL,
        backups: URL,
        deviceKey: String
    ) throws -> DeviceSyncResult {
        let fingerprint = try transport.metadataFingerprint()
        _ = try MirrorSynchronizer().refresh(using: transport, mirror: mirror)
        let queue = (try? OfflineQueue(file: queueFile).load()) ?? []
        let replay = SyncCoordinator().replay(
            pending: queue,
            currentIndex: CapsuleScanner().scan(root: mirror)
        ) { command in
            try DeviceCommandClient(transport: transport).performMaintenance([command])
            return try MirrorSynchronizer().refresh(using: transport, mirror: mirror)
        }
        try OfflineQueue(file: queueFile).save(replay.pending)
        var backupWarning: String?
        do {
            _ = try BackupManager(root: backups).create(
                from: mirror,
                deviceSerial: deviceKey,
                capsuleCount: replay.index.records.count + replay.index.trashRecords.count)
        } catch {
            backupWarning = error.localizedDescription
        }
        return DeviceSyncResult(index: replay.index, queue: replay.pending,
                                appliedCount: replay.appliedCount,
                                backupWarning: backupWarning, fingerprint: fingerprint)
    }
}

final class CapsulePlaybackController {
    private var player: AVAudioPlayer?

    func play(_ record: CapsuleRecord) throws {
        let audio = record.localDirectory.appendingPathComponent("audio.m4a")
        player = try AVAudioPlayer(contentsOf: audio)
        player?.play()
    }
}

struct CorrectionWorkflow {
    func run(
        record: CapsuleRecord,
        deviceID: String,
        transport: ADBTransport,
        cacheRoot: URL
    ) async throws {
        guard let raw = record.rawText?.trimmingCharacters(in: .whitespacesAndNewlines),
              !raw.isEmpty,
              let revision = record.processing?.revision,
              let durationMs = record.processing?.durationMs else {
            throw PokeCapsuleError.invalidCorrectionResponse
        }
        if let issue = TranscriptionSanity.issue(text: raw, durationMs: durationMs) {
            throw PokeCapsuleError.adbFailure("\(issue)，已阻止 DeepSeek 扩写")
        }
        let defaults = UserDefaults.standard
        let endpointText = defaults.string(forKey: "CorrectionEndpoint")
            ?? "https://api.deepseek.com/chat/completions"
        let modelName = defaults.string(forKey: "CorrectionModel") ?? "deepseek-v4-flash"
        let prompt = defaults.string(forKey: "CorrectionPrompt")
            ?? "只校正识别错误和标点；不解释、不增删原意；无法判断时原样输出。只输出正文。"
        guard let endpoint = URL(string: endpointText),
              !modelName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            throw PokeCapsuleError.adbFailure("校对 API 地址或模型配置无效")
        }
        let cache = cacheRoot.appendingPathComponent(CorrectionCacheKey.fileName(
            capsuleID: record.id,
            revision: revision,
            deviceSerial: deviceID,
            rawText: raw))
        let polished: String
        if let saved = try? String(contentsOf: cache, encoding: .utf8), !saved.isEmpty {
            polished = saved
        } else {
            let configuration = CorrectionConfiguration(
                endpoint: endpoint,
                model: modelName,
                systemPrompt: prompt)
            polished = try await CorrectionAdapter().correct(text: raw, configuration: configuration)
            try FileManager.default.createDirectory(
                at: cache.deletingLastPathComponent(), withIntermediateDirectories: true)
            try Data(polished.utf8).write(to: cache, options: .atomic)
        }
        try await Task.detached(priority: .userInitiated) {
            try DeviceCommandClient(transport: transport).commitCorrection(
                text: polished,
                capsuleID: record.id,
                expectedRevision: revision)
        }.value
        try? FileManager.default.removeItem(at: cache)
    }
}
