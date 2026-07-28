import AppKit
import AVFoundation
import Foundation
import PokeCapsuleCore
import SwiftUI

enum SidebarSelection: Hashable {
    case all
    case folder(String)
    case tag(String)
    case favorites
    case pending
    case failed
    case trash
}

@MainActor
final class AppModel: ObservableObject {
    @Published var connection: DeviceConnectionState = .noDevice
    @Published var devices: [ADBDevice] = []
    @Published var selectedSerial: String?
    @Published var index = CapsuleIndex()
    @Published var selection = Set<UUID>()
    @Published var sidebar: SidebarSelection = .folder("Inbox")
    @Published var status = "请连接已开启 USB 调试的 Poke3"
    @Published var isBusy = false
    @Published var showingSettings = false
    @Published var searchQuery = ""
    @Published var pendingCommands: [PendingCommand] = []

    private var adbURL: URL?
    private var transport: ADBTransport?
    private var audioPlayer: AVAudioPlayer?

    private var applicationBase: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PokeCapsule", isDirectory: true)
    }

    var mirrorURL: URL {
        let serial = selectedSerial
            ?? UserDefaults.standard.string(forKey: "LastDeviceSerial")
            ?? "default"
        return applicationBase.appendingPathComponent("Mirrors/\(serial)", isDirectory: true)
    }

    var queueURL: URL {
        let serial = selectedSerial
            ?? UserDefaults.standard.string(forKey: "LastDeviceSerial")
            ?? "default"
        return applicationBase.appendingPathComponent("Queues/\(serial).json")
    }

    var backupURL: URL {
        applicationBase.appendingPathComponent("Backups", isDirectory: true)
    }

    var filteredRecords: [CapsuleRecord] {
        let records: [CapsuleRecord]
        switch sidebar {
        case .all: records = index.records
        case .folder(let folder): records = index.records.filter { $0.relativeFolder == folder }
        case .tag(let tag): records = index.records.filter { $0.capsule.tags.contains(tag) }
        case .favorites: records = index.records.filter(\.capsule.favorite)
        case .pending:
            records = index.records.filter {
                [.recorded, .queued, .transcribing].contains($0.processing?.status)
            }
        case .failed: records = index.records.filter { $0.processing?.status == .failed }
        case .trash: records = index.trashRecords
        }
        return CapsuleSearch.filter(records, query: searchQuery)
    }

    var selectedRecords: [CapsuleRecord] {
        (index.records + index.trashRecords).filter { selection.contains($0.id) }
    }

    var tags: [String] {
        Array(Set(index.records.flatMap(\.capsule.tags))).sorted()
    }

    init() {
        selectedSerial = UserDefaults.standard.string(forKey: "LastDeviceSerial")
        loadLocalState()
        refreshDevices()
    }

    private func loadLocalState() {
        if FileManager.default.fileExists(atPath: mirrorURL.path) {
            index = CapsuleScanner().scan(root: mirrorURL)
        }
        pendingCommands = (try? OfflineQueue(file: queueURL).load()) ?? []
        for pending in pendingCommands where pending.state == .queued || pending.state == .failed {
            applyOptimistic(pending.command)
        }
        if !index.records.isEmpty || !index.trashRecords.isEmpty {
            status = "已载入上次镜像；\(pendingCommands.count) 项等待同步"
        }
    }

    func refreshDevices() {
        guard !isBusy else { return }
        guard let adb = ADBLocator.locate() else {
            adbURL = nil
            transport = nil
            devices = []
            connection = .noADB
            status = connection.localizedDescription
            return
        }
        adbURL = adb
        devices = ADBTransport.discover(executable: adb)
        connection = DeviceParser.state(for: devices)
        if case .connected(let device) = connection {
            choose(device)
        } else if let selectedSerial,
                  let selected = devices.first(where: { $0.serial == selectedSerial && $0.state == "device" }) {
            choose(selected)
        } else {
            transport = nil
            status = connection.localizedDescription
        }
    }

    func choose(_ device: ADBDevice) {
        guard !isBusy else { return }
        guard let adbURL else { return }
        selectedSerial = device.serial
        UserDefaults.standard.set(device.serial, forKey: "LastDeviceSerial")
        transport = ADBTransport(executable: adbURL, serial: device.serial)
        connection = .connected(device)
        status = connection.localizedDescription
        if !isBusy { sync() }
    }

    func sync() {
        guard !isBusy else { return }
        guard let transport else {
            status = "先连接并选择 Poke3"
            return
        }
        isBusy = true
        status = "正在建立只读镜像…"
        let mirror = mirrorURL
        let queueFile = queueURL
        let backups = backupURL
        Task {
            do {
                let result = try await Task.detached(priority: .userInitiated) {
                    _ = try MirrorSynchronizer().refresh(using: transport, mirror: mirror)
                    var queue = (try? OfflineQueue(file: queueFile).load()) ?? []
                    let replay = SyncCoordinator().replay(
                        pending: queue,
                        currentIndex: CapsuleScanner().scan(root: mirror)
                    ) { command in
                        try DeviceCommandClient(transport: transport).performMaintenance([command])
                        return try MirrorSynchronizer().refresh(using: transport, mirror: mirror)
                    }
                    queue = replay.pending
                    try OfflineQueue(file: queueFile).save(queue)
                    var backupWarning: String?
                    do {
                        _ = try BackupManager(root: backups).create(
                            from: mirror,
                            deviceSerial: transport.serial,
                            capsuleCount: replay.index.records.count
                                + replay.index.trashRecords.count)
                    } catch {
                        backupWarning = error.localizedDescription
                    }
                    return (replay.index, queue, replay.appliedCount, backupWarning)
                }.value
                self.index = result.0
                self.pendingCommands = result.1
                self.selection = self.selection.intersection(
                    Set((result.0.records + result.0.trashRecords).map(\.id)))
                let conflicts = result.1.filter { $0.state == .conflict }.count
                let syncStatus = conflicts > 0
                    ? "已同步；\(conflicts) 项版本冲突等待处理"
                    : "已同步 \(result.0.records.count) 个胶囊"
                self.status = result.3.map {
                    "\(syncStatus)；自动备份失败：\($0)"
                } ?? syncStatus
                self.isBusy = false
            } catch {
                self.status = error.localizedDescription
                self.isBusy = false
            }
        }
    }

    func perform(_ input: DeviceCommand) {
        guard !isBusy else { return }
        var command = input
        if let ids = command.capsuleIds, command.expectedRevisions == nil {
            command.expectedRevisions = expectedRevisions(for: ids)
        }
        guard selectedRecords.allSatisfy({ !$0.readOnly }) else {
            status = "选择中包含未知协议版本，只能读取"
            return
        }
        guard let transport, selectedDeviceIsConnected() else {
            self.transport = nil
            enqueueOffline(command)
            return
        }
        isBusy = true
        status = "正在等待 Poke3 进入维护状态…"
        Task {
            do {
                try await Task.detached(priority: .userInitiated) {
                    let client = DeviceCommandClient(transport: transport)
                    try client.performMaintenance([command])
                }.value
                self.status = "操作已完成，正在重新同步"
                self.selection.removeAll()
                self.isBusy = false
                self.sync()
            } catch {
                self.status = error.localizedDescription
                self.isBusy = false
            }
        }
    }

    func moveSelected(to folder: String) {
        do {
            let destination = try PathPolicy.validatedRelativeFolder(folder)
            perform(DeviceCommand(operation: "moveCapsules", capsuleIds: Array(selection), destination: destination))
        } catch { status = error.localizedDescription }
    }

    func copySelected(to folder: String) {
        do {
            let destination = try PathPolicy.validatedRelativeFolder(folder)
            perform(DeviceCommand(operation: "copyCapsules", capsuleIds: Array(selection), destination: destination))
        } catch { status = error.localizedDescription }
    }

    func deleteSelected() {
        perform(DeviceCommand(operation: "deleteCapsules", capsuleIds: Array(selection)))
    }

    func restoreSelected() {
        perform(DeviceCommand(operation: "restoreCapsules", capsuleIds: Array(selection)))
    }

    func purgeSelected() {
        perform(DeviceCommand(operation: "purgeCapsules", capsuleIds: Array(selection)))
    }

    func setFavorite(_ value: Bool) {
        perform(DeviceCommand(operation: "setFavorite", capsuleIds: Array(selection), favorite: value))
    }

    func addTag(_ input: String) {
        do {
            let tag = try PathPolicy.validatedTag(input)
            perform(DeviceCommand(operation: "addTags", capsuleIds: Array(selection), tags: [tag]))
        } catch { status = error.localizedDescription }
    }

    func removeTag(_ tag: String) {
        perform(DeviceCommand(operation: "removeTags", capsuleIds: Array(selection), tags: [tag]))
    }

    func createFolder(_ input: String, parent: String? = nil) {
        do {
            let name = try PathPolicy.normalizedFolderName(input)
            let path = try PathPolicy.validatedRelativeFolder(parent.map { "\($0)/\(name)" } ?? name, allowReserved: false)
            perform(DeviceCommand(operation: "createFolder", folderPath: path))
        } catch { status = error.localizedDescription }
    }

    func deleteFolder(_ folder: String) {
        do {
            let path = try PathPolicy.validatedRelativeFolder(folder, allowReserved: false)
            let ids = index.records.filter {
                $0.relativeFolder == path || $0.relativeFolder.hasPrefix(path + "/")
            }.map(\.id)
            perform(DeviceCommand(
                operation: "deleteFolderToInbox",
                capsuleIds: ids,
                folderPath: path))
        } catch { status = error.localizedDescription }
    }

    func renameFolder(_ folder: String, to newName: String) {
        do {
            let oldPath = try PathPolicy.validatedRelativeFolder(folder, allowReserved: false)
            let cleanName = try PathPolicy.normalizedFolderName(newName)
            let parts = oldPath.split(separator: "/").map(String.init)
            let newPath = try PathPolicy.validatedRelativeFolder(
                parts.count == 2 ? "\(parts[0])/\(cleanName)" : cleanName,
                allowReserved: false
            )
            perform(DeviceCommand(operation: "renameFolder", folderPath: oldPath, newFolderPath: newPath))
        } catch { status = error.localizedDescription }
    }

    func renameTag(_ tag: String, to newName: String) {
        do {
            let clean = try PathPolicy.validatedTag(newName)
            perform(DeviceCommand(
                operation: "renameTag",
                capsuleIds: index.records.filter {
                    $0.capsule.tags.contains { $0.caseInsensitiveCompare(tag) == .orderedSame }
                }.map(\.id),
                tags: [tag, clean]))
        } catch { status = error.localizedDescription }
    }

    func mergeTag(_ tag: String, into destination: String) {
        do {
            let clean = try PathPolicy.validatedTag(destination)
            perform(DeviceCommand(
                operation: "mergeTag",
                capsuleIds: index.records.filter {
                    $0.capsule.tags.contains { $0.caseInsensitiveCompare(tag) == .orderedSame }
                }.map(\.id),
                tags: [tag, clean]))
        } catch { status = error.localizedDescription }
    }

    func deleteTag(_ tag: String) {
        perform(DeviceCommand(
            operation: "deleteTag",
            capsuleIds: index.records.filter {
                $0.capsule.tags.contains { $0.caseInsensitiveCompare(tag) == .orderedSame }
            }.map(\.id),
            tags: [tag]))
    }

    func importCapsules(_ directories: [URL], destination: String) {
        guard !isBusy else { return }
        guard let transport else {
            status = "设备未连接"
            return
        }
        var existing: [UUID: CapsuleRecord] = [:]
        for record in index.records where existing[record.id] == nil {
            existing[record.id] = record
        }
        isBusy = true
        status = "正在校验 \(directories.count) 个导入胶囊…"
        Task {
            do {
                let count = try await Task.detached(priority: .userInitiated) {
                    let client = DeviceCommandClient(transport: transport)
                    var imported = 0
                    for directory in directories {
                        let package = try CapsulePackage.inspect(directory)
                        if try client.importCapsule(
                            package,
                            destination: destination,
                            existing: existing[package.metadata.id]
                        ) {
                            imported += 1
                        }
                    }
                    return imported
                }.value
                status = "已导入 \(count) 个胶囊；内容相同的 UUID 已跳过"
                isBusy = false
                sync()
            } catch {
                status = error.localizedDescription
                isBusy = false
            }
        }
    }

    func exportSelected(to destination: URL) {
        guard !isBusy else { return }
        let records = selectedRecords
        guard !records.isEmpty else { return }
        isBusy = true
        Task {
            do {
                let urls = try await Task.detached(priority: .userInitiated) {
                    try CapsuleExporter().export(records, to: destination)
                }.value
                self.status = "已导出并校验 \(urls.count) 个胶囊"
            } catch {
                self.status = error.localizedDescription
            }
            self.isBusy = false
        }
    }

    func play(_ record: CapsuleRecord) {
        let audio = record.localDirectory.appendingPathComponent("audio.m4a")
        do {
            audioPlayer = try AVAudioPlayer(contentsOf: audio)
            audioPlayer?.play()
            status = "正在播放：\(record.displayTitle)"
        } catch {
            status = "无法播放音频：\(error.localizedDescription)"
        }
    }

    func copySelectedText(markdown: Bool) {
        let records = selectedRecords
        guard !records.isEmpty else { return }
        let text = records.map { record in
            if markdown {
                var value = "## \(record.displayTitle)\n\n\(record.displayPreview)\n\n"
                value += "\(record.capsule.createdAt.formatted()) · \(record.relativeFolder)"
                if !record.capsule.tags.isEmpty {
                    value += "\n\n" + record.capsule.tags.map { "#\($0)" }.joined(separator: " ")
                }
                return value
            }
            return record.displayPreview
        }.joined(separator: markdown ? "\n\n---\n\n" : "\n\n")
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        status = "已复制 \(records.count) 条\(markdown ? " Markdown" : "文字")"
    }

    func saveFinalText(_ text: String, for record: CapsuleRecord) {
        perform(DeviceCommand(
            operation: "commitFinalText",
            capsuleIds: [record.id],
            expectedRevision: record.capsule.revision,
            finalText: text))
    }

    func discardPending(_ id: UUID) {
        guard !isBusy else {
            status = "同步进行中，完成后才能丢弃待同步操作"
            return
        }
        do {
            pendingCommands = try OfflineQueue(file: queueURL).discard(id)
            reloadMirrorAndOptimisticQueue()
            status = "已丢弃待同步操作"
        } catch {
            status = error.localizedDescription
        }
    }

    func correct(_ record: CapsuleRecord) {
        guard !isBusy else { return }
        guard let transport else {
            status = "设备未连接"
            return
        }
        guard let deviceSerial = selectedSerial else {
            status = "设备身份不可用，请重新连接"
            return
        }
        guard let raw = record.rawText?.trimmingCharacters(in: .whitespacesAndNewlines),
              !raw.isEmpty,
              let revision = record.processing?.revision,
              let durationMs = record.processing?.durationMs else {
            status = "这条胶囊还没有可校对的原始转写"
            return
        }
        if let issue = TranscriptionSanity.issue(text: raw, durationMs: durationMs) {
            status = "\(issue)，已阻止 DeepSeek 扩写"
            return
        }
        let defaults = UserDefaults.standard
        let endpointText = defaults.string(forKey: "CorrectionEndpoint")
            ?? "https://api.deepseek.com/chat/completions"
        let modelName = defaults.string(forKey: "CorrectionModel")
            ?? "deepseek-v4-flash"
        let prompt = defaults.string(forKey: "CorrectionPrompt")
            ?? "只校正识别错误和标点；不解释、不增删原意；无法判断时原样输出。只输出正文。"
        guard let endpoint = URL(string: endpointText),
              !modelName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            status = "校对 API 地址或模型配置无效"
            return
        }

        isBusy = true
        status = "正在用 \(modelName) 快速校对…"
        Task {
            do {
                let cache = pendingCorrectionURL(
                    for: record.id,
                    revision: revision,
                    deviceSerial: deviceSerial,
                    rawText: raw)
                let polished: String
                if let saved = try? String(contentsOf: cache, encoding: .utf8),
                   !saved.isEmpty {
                    polished = saved
                    self.status = "正在重试写回已完成的校对结果…"
                } else {
                    let configuration = CorrectionConfiguration(
                        endpoint: endpoint,
                        model: modelName,
                        systemPrompt: prompt)
                    polished = try await CorrectionAdapter().correct(
                        text: raw,
                        configuration: configuration)
                    try FileManager.default.createDirectory(
                        at: cache.deletingLastPathComponent(),
                        withIntermediateDirectories: true)
                    try Data(polished.utf8).write(to: cache, options: .atomic)
                }
                try await Task.detached(priority: .userInitiated) {
                    let client = DeviceCommandClient(transport: transport)
                    try client.commitCorrection(
                        text: polished,
                        capsuleID: record.id,
                        expectedRevision: revision)
                }.value
                try? FileManager.default.removeItem(at: cache)
                self.status = "校对完成，已写回 Poke3"
                self.isBusy = false
                self.sync()
            } catch {
                self.status = error.localizedDescription
                self.isBusy = false
            }
        }
    }

    private func pendingCorrectionURL(
        for id: UUID,
        revision: Int,
        deviceSerial: String,
        rawText: String
    ) -> URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PokeCapsule/PendingCorrections", isDirectory: true)
            .appendingPathComponent(CorrectionCacheKey.fileName(
                capsuleID: id,
                revision: revision,
                deviceSerial: deviceSerial,
                rawText: rawText))
    }

    private func expectedRevisions(for ids: [UUID]) -> [String: Int] {
        var records: [UUID: CapsuleRecord] = [:]
        for record in index.records + index.trashRecords where records[record.id] == nil {
            records[record.id] = record
        }
        return Dictionary(uniqueKeysWithValues: ids.compactMap { id in
            records[id].map {
                (id.uuidString.lowercased(), $0.trash?.revision ?? $0.capsule.revision)
            }
        })
    }

    private func selectedDeviceIsConnected() -> Bool {
        guard let adbURL, let selectedSerial else { return false }
        return ADBTransport.discover(executable: adbURL).contains {
            $0.serial == selectedSerial && $0.state == "device"
        }
    }

    private func enqueueOffline(_ command: DeviceCommand) {
        do {
            pendingCommands = try OfflineQueue(file: queueURL).enqueue(command)
            applyOptimistic(command)
            selection.removeAll()
            connection = .noDevice
            status = "设备未连接；操作已加入同步队列"
        } catch {
            status = "无法保存离线操作：\(error.localizedDescription)"
        }
    }

    private func reloadMirrorAndOptimisticQueue() {
        index = CapsuleScanner().scan(root: mirrorURL)
        for pending in pendingCommands where pending.state == .queued || pending.state == .failed {
            applyOptimistic(pending.command)
        }
    }

    private func applyOptimistic(_ command: DeviceCommand) {
        let ids = Set(command.capsuleIds ?? [])
        switch command.operation {
        case "moveCapsules":
            guard let destination = command.destination else { return }
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, folder: destination) : $0
            }
        case "setFavorite":
            guard let favorite = command.favorite else { return }
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, favorite: favorite) : $0
            }
        case "addTags":
            let additions = command.tags ?? []
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, addingTags: additions) : $0
            }
        case "removeTags":
            let removals = Set(command.tags ?? [])
            index.records = index.records.map {
                ids.contains($0.id) ? changed($0, removingTags: removals) : $0
            }
        case "deleteCapsules":
            let moved = index.records.filter { ids.contains($0.id) }.map {
                CapsuleRecord(
                    capsule: $0.capsule,
                    processing: $0.processing,
                    relativeFolder: "回收站",
                    localDirectory: $0.localDirectory,
                    rawText: $0.rawText,
                    polishedText: $0.polishedText,
                    finalText: $0.finalText,
                    trash: TrashMetadata(
                        schemaVersion: 1,
                        capsuleId: $0.id,
                        trashedAt: Date(),
                        originalFolder: $0.relativeFolder,
                        revision: $0.capsule.revision + 1),
                    warnings: $0.warnings)
            }
            index.records.removeAll { ids.contains($0.id) }
            index.trashRecords.append(contentsOf: moved)
        case "restoreCapsules":
            let restored = index.trashRecords.filter { ids.contains($0.id) }.map {
                CapsuleRecord(
                    capsule: incremented($0.capsule),
                    processing: $0.processing,
                    relativeFolder: $0.trash?.originalFolder ?? "Inbox",
                    localDirectory: $0.localDirectory,
                    rawText: $0.rawText,
                    polishedText: $0.polishedText,
                    finalText: $0.finalText,
                    trash: nil,
                    warnings: $0.warnings)
            }
            index.trashRecords.removeAll { ids.contains($0.id) }
            index.records.append(contentsOf: restored)
        case "purgeCapsules":
            index.trashRecords.removeAll { ids.contains($0.id) }
        case "commitFinalText":
            guard let text = command.finalText else { return }
            index.records = index.records.map { record in
                guard ids.contains(record.id) else { return record }
                return CapsuleRecord(
                    capsule: incremented(record.capsule),
                    processing: record.processing,
                    relativeFolder: record.relativeFolder,
                    localDirectory: record.localDirectory,
                    rawText: record.rawText,
                    polishedText: record.polishedText,
                    finalText: text,
                    trash: record.trash,
                    warnings: record.warnings)
            }
        default:
            break
        }
    }

    private func changed(
        _ record: CapsuleRecord,
        folder: String? = nil,
        favorite: Bool? = nil,
        addingTags: [String] = [],
        removingTags: Set<String> = []
    ) -> CapsuleRecord {
        var metadata = incremented(record.capsule)
        if let favorite { metadata.favorite = favorite }
        var tags = metadata.tags.filter { !removingTags.contains($0) }
        for tag in addingTags where !tags.contains(tag) { tags.append(tag) }
        metadata.tags = tags
        return CapsuleRecord(
            capsule: metadata,
            processing: record.processing,
            relativeFolder: folder ?? record.relativeFolder,
            localDirectory: record.localDirectory,
            rawText: record.rawText,
            polishedText: record.polishedText,
            finalText: record.finalText,
            trash: record.trash,
            warnings: record.warnings)
    }

    private func incremented(_ original: CapsuleMetadata) -> CapsuleMetadata {
        var value = original
        value.revision += 1
        value.updatedAt = Date()
        return value
    }
}
