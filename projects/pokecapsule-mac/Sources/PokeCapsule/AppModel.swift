import AppKit
import Foundation
import PokeCapsuleCore
import SwiftUI

typealias SidebarSelection = LibraryScope

@MainActor
final class AppModel: ObservableObject {
    @Published var connection: DeviceConnectionState = .noDevice
    @Published var devices: [ADBDevice] = []
    @Published var usbDevices: [USBPhysicalDevice] = []
    @Published var pokePodPorts: [URL] = []
    @Published var registeredDevices: [RegisteredDevice] = []
    @Published var selectedDeviceID: String?
    @Published var selectedSerial: String?
    @Published var index = CapsuleIndex()
    @Published var selection = Set<UUID>()
    @Published var sidebar: SidebarSelection = .folder("Inbox")
    @Published var status = "请连接已开启 USB 调试的 Android 设备"
    @Published var isBusy = false
    @Published var showingSettings = false
    @Published var searchQuery = ""
    @Published var librarySort: LibrarySort = .newestFirst
    @Published var pendingCommands: [PendingCommand] = []

    private var adbURL: URL?
    private var transport: (any DeviceTransport)?
    private var selectedPokePodURL: URL?
    private let workspace = DeviceWorkspace()
    private let playback = CapsulePlaybackController()
    private var monitorTask: Task<Void, Never>?
    private var applicationIsActive = true
    private var lastRemoteFingerprint: String?
    private let monitorIntervalNanoseconds: UInt64 = 5_000_000_000
    private let reconnectIntervalNanoseconds: UInt64 = 10_000_000_000

    var mirrorURL: URL {
        let deviceID = selectedDeviceID
            ?? UserDefaults.standard.string(forKey: "LastDeviceID")
            ?? UserDefaults.standard.string(forKey: "LastDeviceSerial")
            ?? "default"
        return workspace.mirrorURL(deviceID: deviceID)
    }

    var queueURL: URL {
        let deviceID = selectedDeviceID
            ?? UserDefaults.standard.string(forKey: "LastDeviceID")
            ?? UserDefaults.standard.string(forKey: "LastDeviceSerial")
            ?? "default"
        return workspace.queueURL(deviceID: deviceID)
    }

    var backupURL: URL {
        workspace.backupURL
    }

    var filteredRecords: [CapsuleRecord] {
        LibraryQuery.records(in: index, scope: sidebar,
                             search: searchQuery, sort: librarySort)
    }

    var selectedRecords: [CapsuleRecord] {
        (index.records + index.trashRecords).filter { selection.contains($0.id) }
    }

    var tags: [String] {
        Array(Set(index.records.flatMap(\.capsule.tags))).sorted()
    }

    var selectedRegisteredDevice: RegisteredDevice? {
        guard let selectedDeviceID else { return nil }
        return registeredDevices.first { $0.deviceId == selectedDeviceID }
    }

    func recordCount(in scope: LibraryScope) -> Int {
        LibraryQuery.records(in: index, scope: scope, search: "").count
    }

    func connectedDevice(for registered: RegisteredDevice) -> ADBDevice? {
        devices.first {
            $0.state == "device" && registered.serialAliases.contains($0.serial)
        }
    }

    func connectionLabel(for registered: RegisteredDevice) -> String {
        if registered.isPokePod,
           pokePodPorts.contains(where: { registered.serialAliases.contains($0.path) }) {
            return "已连接"
        }
        if connectedDevice(for: registered) != nil { return "已连接" }
        if usbDevices.contains(where: { device in
            guard let serial = device.serial else { return false }
            return registered.serialAliases.contains(serial)
        }) {
            return "已插入，等待 ADB"
        }
        return "离线镜像"
    }

    init() {
        registeredDevices = workspace.loadRegistryAndBootstrapLegacyMirrors()
        selectedDeviceID = UserDefaults.standard.string(forKey: "LastDeviceID")
            ?? UserDefaults.standard.string(forKey: "LastDeviceSerial")
            ?? registeredDevices.first?.deviceId
        loadLocalState()
        refreshDevices()
    }

    private func loadLocalState() {
        index = CapsuleIndex()
        pendingCommands = []
        selection.removeAll()
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
        let adb = ADBLocator.locate()
        adbURL = adb
        devices = adb.map { ADBTransport.discover(executable: $0) } ?? []
        pokePodPorts = PokePodTransport.discover()
        if devices.contains(where: { $0.state == "device" || $0.state == "unauthorized" || $0.state == "offline" }) {
            usbDevices = []
        } else {
            usbDevices = USBDeviceProbe.discover()
        }
        connection = adb == nil ? .noADB : DeviceParser.state(
            for: devices, usbDevices: usbDevices,
            preferredSerials: selectedRegisteredDevice?.serialAliases ?? [])
        if let selectedDevice = selectedRegisteredDevice,
           let port = matchingPokePodPort(for: selectedDevice) {
            choosePokePod(port)
        } else if let selectedDevice = selectedRegisteredDevice,
           let selected = connectedDevice(for: selectedDevice) {
            choose(selected)
        } else if case .connected(let device) = connection {
            choose(device)
        } else {
            stopMonitoring()
            transport = nil
            selectedPokePodURL = nil
            status = pokePodPorts.isEmpty
                ? connection.localizedDescription
                : "检测到 PokePod USB；请从设备菜单连接"
            scheduleMonitor(afterNanoseconds: reconnectIntervalNanoseconds)
        }
    }

    func choose(_ device: ADBDevice) {
        guard !isBusy else { return }
        guard let adbURL else { return }
        let candidateTransport = ADBTransport(executable: adbURL, serial: device.serial)
        let identity = (try? candidateTransport.readDeviceIdentity())
            ?? DeviceIdentity(
                deviceId: device.serial,
                displayName: device.displayName,
                manufacturer: nil,
                model: device.model)
        let deviceID = workspace.register(
            identity: identity, adbDevice: device, devices: &registeredDevices)
        let changedDevice = selectedDeviceID != deviceID
        selectedDeviceID = deviceID
        selectedSerial = device.serial
        UserDefaults.standard.set(deviceID, forKey: "LastDeviceID")
        UserDefaults.standard.set(device.serial, forKey: "LastDeviceSerial")
        transport = candidateTransport
        selectedPokePodURL = nil
        connection = .connected(device)
        status = "已连接：\(identity.displayName)"
        if changedDevice {
            stopMonitoring()
            lastRemoteFingerprint = nil
            loadLocalState()
        }
        if !isBusy { sync() }
    }

    func choosePokePod(_ port: URL) {
        guard !isBusy else { return }
        do {
            let candidate = try PokePodTransport(deviceURL: port)
            _ = try candidate.hello()
            let identity = try candidate.readDeviceIdentity() ?? DeviceIdentity(
                deviceId: "pokepod-\(port.lastPathComponent)",
                displayName: "PokePod",
                platform: "pokepod",
                manufacturer: "PokePod",
                model: "AMOLED")
            let deviceID = workspace.register(
                identity: identity, transportAlias: port.path,
                fallbackModel: "AMOLED", devices: &registeredDevices)
            let changedDevice = selectedDeviceID != deviceID
            selectedDeviceID = deviceID
            selectedSerial = nil
            selectedPokePodURL = port
            UserDefaults.standard.set(deviceID, forKey: "LastDeviceID")
            transport = candidate
            status = "已连接：\(identity.displayName)"
            if changedDevice {
                stopMonitoring()
                lastRemoteFingerprint = nil
                loadLocalState()
            }
            if !isBusy { sync() }
        } catch {
            transport = nil
            selectedPokePodURL = nil
            status = error.localizedDescription
            scheduleMonitor(afterNanoseconds: reconnectIntervalNanoseconds)
        }
    }

    func selectRegisteredDevice(_ deviceID: String) {
        guard !isBusy, selectedDeviceID != deviceID else { return }
        stopMonitoring()
        selectedDeviceID = deviceID
        UserDefaults.standard.set(deviceID, forKey: "LastDeviceID")
        lastRemoteFingerprint = nil
        if let registered = registeredDevices.first(where: { $0.deviceId == deviceID }),
           let port = matchingPokePodPort(for: registered) {
            loadLocalState()
            choosePokePod(port)
            return
        } else if let registered = registeredDevices.first(where: { $0.deviceId == deviceID }),
           let connected = connectedDevice(for: registered),
           let adbURL {
            selectedSerial = connected.serial
            transport = ADBTransport(executable: adbURL, serial: connected.serial)
            connection = .connected(connected)
            status = "已选择并连接：\(registered.displayName)"
        } else {
            selectedSerial = nil
            selectedPokePodURL = nil
            transport = nil
            connection = DeviceParser.state(
                for: devices,
                usbDevices: usbDevices,
                preferredSerials: selectedRegisteredDevice?.serialAliases ?? [])
            status = connection == .noDevice
                ? "已打开离线镜像：\(selectedRegisteredDevice?.displayName ?? "Android 设备")"
                : connection.localizedDescription
        }
        loadLocalState()
        if transport != nil { sync() }
        else { scheduleMonitor(afterNanoseconds: reconnectIntervalNanoseconds) }
    }

    func setApplicationActive(_ active: Bool) {
        applicationIsActive = active
        if active {
            refreshDevices()
        } else {
            stopMonitoring()
        }
    }

    func sync() {
        guard !isBusy else { return }
        guard let transport else {
            status = "先连接当前选择的 Android 设备"
            return
        }
        stopMonitoring()
        isBusy = true
        status = "正在建立只读镜像…"
        let mirror = mirrorURL
        let queueFile = queueURL
        let backups = backupURL
        let deviceKey = selectedDeviceID ?? transport.deviceIdentifier
        Task {
            do {
                let result = try await Task.detached(priority: .userInitiated) {
                    try DeviceSyncEngine().run(
                        transport: transport,
                        mirror: mirror,
                        queueFile: queueFile,
                        backups: backups,
                        deviceKey: deviceKey)
                }.value
                self.index = result.index
                self.pendingCommands = result.queue
                self.lastRemoteFingerprint = result.fingerprint
                self.selection = self.selection.intersection(
                    Set((result.index.records + result.index.trashRecords).map(\.id)))
                let conflicts = result.queue.filter { $0.state == .conflict }.count
                let syncStatus = conflicts > 0
                    ? "已同步；\(conflicts) 项版本冲突等待处理；自动同步已开启"
                    : "已同步 \(result.index.records.count) 个胶囊；自动同步已开启"
                self.status = result.backupWarning.map {
                    "\(syncStatus)；自动备份失败：\($0)"
                } ?? syncStatus
                self.isBusy = false
                if UserDefaults.standard.bool(forKey: "AutomaticCorrectionEnabled"),
                   let candidate = result.index.records.first(where: {
                       $0.processing?.status == .rawReady
                           && $0.polishedText == nil && !$0.readOnly
                   }) {
                    self.runCorrections([candidate], automatic: true)
                } else {
                    self.scheduleMonitor()
                }
            } catch {
                self.isBusy = false
                if !self.selectedDeviceIsConnected() {
                    self.transport = nil
                    self.connection = .noDevice
                    self.status = "设备已断开；当前镜像和离线队列仍可使用"
                    self.refreshDevices()
                } else {
                    self.status = error.localizedDescription
                    self.scheduleMonitor()
                }
            }
        }
    }

    private func scheduleMonitor(
        afterNanoseconds delay: UInt64? = nil
    ) {
        stopMonitoring()
        guard applicationIsActive else { return }
        let wait = delay ?? (transport == nil
            ? reconnectIntervalNanoseconds
            : monitorIntervalNanoseconds)
        monitorTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: wait)
            guard !Task.isCancelled, let self else { return }
            if self.transport == nil {
                self.refreshDevices()
            } else {
                self.checkForRemoteChanges()
            }
        }
    }

    private func checkForRemoteChanges() {
        guard applicationIsActive, !isBusy else {
            scheduleMonitor()
            return
        }
        guard let transport else {
            refreshDevices()
            return
        }
        Task {
            do {
                let fingerprint = try await Task.detached(priority: .utility) {
                    try transport.metadataFingerprint()
                }.value
                if let previous = self.lastRemoteFingerprint,
                   previous != fingerprint {
                    self.lastRemoteFingerprint = fingerprint
                    self.sync()
                } else {
                    self.lastRemoteFingerprint = fingerprint
                    self.scheduleMonitor()
                }
            } catch {
                self.transport = nil
                self.connection = .noDevice
                self.status = "设备已断开；自动同步等待重新连接"
                self.refreshDevices()
            }
        }
    }

    private func stopMonitoring() {
        monitorTask?.cancel()
        monitorTask = nil
    }

    func perform(_ input: DeviceCommand) {
        guard !isBusy else { return }
        var command = input
        if let ids = command.capsuleIds, command.expectedRevisions == nil {
            command.expectedRevisions = expectedRevisions(for: ids, operation: command.operation)
        }
        if let ids = command.capsuleIds {
            let targetIDs = Set(ids)
            let targets = (index.records + index.trashRecords).filter { targetIDs.contains($0.id) }
            guard targets.allSatisfy({ !$0.readOnly }) else {
                status = "目标中包含损坏或未知协议胶囊，只能读取"
                return
            }
        }
        guard let transport, selectedDeviceIsConnected() else {
            self.transport = nil
            enqueueOffline(command)
            return
        }
        isBusy = true
        status = "正在等待设备进入维护状态…"
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
                self.isBusy = false
                if !self.selectedDeviceIsConnected() {
                    self.transport = nil
                    self.connection = .noDevice
                    self.enqueueOffline(command)
                } else {
                    self.status = error.localizedDescription
                }
            }
        }
    }

    func moveSelected(to folder: String) {
        do {
            let destination = try PathPolicy.validatedRelativeFolder(folder)
            perform(DeviceCommand(operation: .moveCapsules, capsuleIds: Array(selection), destination: destination))
        } catch { status = error.localizedDescription }
    }

    func copySelected(to folder: String) {
        do {
            let destination = try PathPolicy.validatedRelativeFolder(folder)
            perform(DeviceCommand(operation: .copyCapsules, capsuleIds: Array(selection), destination: destination))
        } catch { status = error.localizedDescription }
    }

    func deleteSelected() {
        perform(DeviceCommand(operation: .deleteCapsules, capsuleIds: Array(selection)))
    }

    func restoreSelected() {
        perform(DeviceCommand(operation: .restoreCapsules, capsuleIds: Array(selection)))
    }

    func purgeSelected() {
        perform(DeviceCommand(operation: .purgeCapsules, capsuleIds: Array(selection)))
    }

    func setFavorite(_ value: Bool) {
        perform(DeviceCommand(operation: .setFavorite, capsuleIds: Array(selection), favorite: value))
    }

    func setFavorite(_ record: CapsuleRecord, _ value: Bool) {
        perform(DeviceCommand(operation: .setFavorite, capsuleIds: [record.id], favorite: value))
    }

    func move(_ record: CapsuleRecord, to folder: String) {
        guard let destination = try? PathPolicy.validatedRelativeFolder(folder) else { return }
        perform(DeviceCommand(operation: .moveCapsules,
                              capsuleIds: [record.id], destination: destination))
    }

    func addTag(_ input: String, to record: CapsuleRecord) {
        guard let tag = try? PathPolicy.validatedTag(input) else { return }
        perform(DeviceCommand(operation: .addTags, capsuleIds: [record.id], tags: [tag]))
    }

    func addTag(_ input: String) {
        do {
            let tag = try PathPolicy.validatedTag(input)
            perform(DeviceCommand(operation: .addTags, capsuleIds: Array(selection), tags: [tag]))
        } catch { status = error.localizedDescription }
    }

    func removeTag(_ tag: String) {
        perform(DeviceCommand(operation: .removeTags, capsuleIds: Array(selection), tags: [tag]))
    }

    func createFolder(_ input: String, parent: String? = nil) {
        do {
            let name = try PathPolicy.normalizedFolderName(input)
            let path = try PathPolicy.validatedRelativeFolder(parent.map { "\($0)/\(name)" } ?? name, allowReserved: false)
            perform(DeviceCommand(operation: .createFolder, folderPath: path))
        } catch { status = error.localizedDescription }
    }

    func deleteFolder(_ folder: String) {
        do {
            let path = try PathPolicy.validatedRelativeFolder(folder, allowReserved: false)
            let ids = index.records.filter {
                $0.relativeFolder == path || $0.relativeFolder.hasPrefix(path + "/")
            }.map(\.id)
            perform(DeviceCommand(
                operation: .deleteFolderToInbox,
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
            perform(DeviceCommand(operation: .renameFolder, folderPath: oldPath, newFolderPath: newPath))
        } catch { status = error.localizedDescription }
    }

    func renameTag(_ tag: String, to newName: String) {
        do {
            let clean = try PathPolicy.validatedTag(newName)
            perform(DeviceCommand(
                operation: .renameTag,
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
                operation: .mergeTag,
                capsuleIds: index.records.filter {
                    $0.capsule.tags.contains { $0.caseInsensitiveCompare(tag) == .orderedSame }
                }.map(\.id),
                tags: [tag, clean]))
        } catch { status = error.localizedDescription }
    }

    func deleteTag(_ tag: String) {
        perform(DeviceCommand(
            operation: .deleteTag,
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
        do {
            try playback.play(record)
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
            operation: .commitFinalText,
            capsuleIds: [record.id],
            expectedRevision: record.capsule.revision,
            finalText: text))
    }

    func retryTranscription(_ record: CapsuleRecord) {
        guard let processingRevision = record.processing?.revision else {
            status = "缺少转写状态版本，已停止重新转写"
            return
        }
        perform(DeviceCommand(
            operation: .requeueTranscription,
            capsuleIds: [record.id],
            expectedRevisions: [record.id.uuidString.lowercased(): processingRevision]))
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
        runCorrections([record], automatic: false)
    }

    func correctSelected() {
        let records = selectedRecords.filter { $0.rawText != nil && !$0.readOnly }
        guard !records.isEmpty else {
            status = "选中的胶囊没有可校对的原始转写"
            return
        }
        runCorrections(records, automatic: false)
    }

    private func runCorrections(_ records: [CapsuleRecord], automatic: Bool) {
        guard !isBusy else { return }
        guard let transport else {
            status = "设备未连接"
            return
        }
        guard let deviceSerial = selectedDeviceID else {
            status = "设备身份不可用，请重新连接"
            return
        }
        isBusy = true
        status = automatic ? "正在自动校对一条新转写…" : "正在校对 \(records.count) 条转写…"
        Task {
            do {
                for record in records {
                    try await CorrectionWorkflow().run(
                        record: record,
                        deviceID: deviceSerial,
                        transport: transport,
                        cacheRoot: workspace.applicationBase
                            .appendingPathComponent("PendingCorrections", isDirectory: true))
                }
                self.status = "已校对 \(records.count) 条并写回当前设备"
                self.isBusy = false
                self.selection.removeAll()
                self.sync()
            } catch {
                self.status = error.localizedDescription
                self.isBusy = false
            }
        }
    }

    private func expectedRevisions(
        for ids: [UUID],
        operation: CommandOperation
    ) -> [String: Int] {
        var records: [UUID: CapsuleRecord] = [:]
        for record in index.records + index.trashRecords where records[record.id] == nil {
            records[record.id] = record
        }
        var result: [String: Int] = [:]
        for id in ids {
            guard let record = records[id] else { continue }
            let revision = operation == .requeueTranscription
                ? record.processing?.revision
                : (record.trash?.revision ?? record.capsule.revision)
            if let revision { result[id.uuidString.lowercased()] = revision }
        }
        return result
    }

    private func selectedDeviceIsConnected() -> Bool {
        if let selectedPokePodURL {
            return PokePodTransport.discover().contains(selectedPokePodURL)
        }
        guard let adbURL, let selectedSerial else { return false }
        return ADBTransport.discover(executable: adbURL).contains {
            $0.serial == selectedSerial && $0.state == "device"
        }
    }

    private func matchingPokePodPort(for registered: RegisteredDevice) -> URL? {
        PokePodPortMatcher.match(
            registered: registered,
            discoveredPorts: pokePodPorts
        ) { port in
            let candidate = try PokePodTransport(deviceURL: port)
            _ = try candidate.hello()
            return try candidate.readDeviceIdentity()
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
        index = OptimisticLibraryReducer.apply(command, to: index)
    }
}
