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

    private var adbURL: URL?
    private var transport: ADBTransport?
    private var audioPlayer: AVAudioPlayer?

    var mirrorURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let serial = selectedSerial ?? "未连接"
        return base.appendingPathComponent("PokeCapsule/Mirrors/\(serial)", isDirectory: true)
    }

    var filteredRecords: [CapsuleRecord] {
        switch sidebar {
        case .all: return index.records
        case .folder(let folder): return index.records.filter { $0.relativeFolder == folder }
        case .tag(let tag): return index.records.filter { $0.capsule.tags.contains(tag) }
        case .favorites: return index.records.filter(\.capsule.favorite)
        }
    }

    var selectedRecords: [CapsuleRecord] {
        index.records.filter { selection.contains($0.id) }
    }

    var tags: [String] {
        Array(Set(index.records.flatMap(\.capsule.tags))).sorted()
    }

    init() {
        refreshDevices()
    }

    func refreshDevices() {
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
        guard let adbURL else { return }
        selectedSerial = device.serial
        transport = ADBTransport(executable: adbURL, serial: device.serial)
        connection = .connected(device)
        status = connection.localizedDescription
    }

    func sync() {
        guard let transport else {
            status = "先连接并选择 Poke3"
            return
        }
        isBusy = true
        status = "正在建立只读镜像…"
        let mirror = mirrorURL
        Task {
            do {
                let newIndex = try await Task.detached(priority: .userInitiated) {
                    try MirrorSynchronizer().refresh(using: transport, mirror: mirror)
                }.value
                self.index = newIndex
                self.selection = self.selection.intersection(Set(newIndex.records.map(\.id)))
                self.status = "已同步 \(newIndex.records.count) 个胶囊"
            } catch {
                self.status = error.localizedDescription
            }
            self.isBusy = false
        }
    }

    func perform(_ command: DeviceCommand) {
        guard let transport else {
            status = "设备未连接"
            return
        }
        guard selectedRecords.allSatisfy({ !$0.readOnly }) else {
            status = "选择中包含未知协议版本，只能读取"
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
                self.sync()
            } catch {
                self.status = error.localizedDescription
            }
            self.isBusy = false
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
            perform(DeviceCommand(operation: "deleteFolderToInbox", folderPath: path))
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
            perform(DeviceCommand(operation: "renameTag", tags: [tag, clean]))
        } catch { status = error.localizedDescription }
    }

    func mergeTag(_ tag: String, into destination: String) {
        do {
            let clean = try PathPolicy.validatedTag(destination)
            perform(DeviceCommand(operation: "mergeTag", tags: [tag, clean]))
        } catch { status = error.localizedDescription }
    }

    func deleteTag(_ tag: String) {
        perform(DeviceCommand(operation: "deleteTag", tags: [tag]))
    }

    func importCapsules(_ directories: [URL], destination: String) {
        guard let transport else {
            status = "设备未连接"
            return
        }
        let existing = Dictionary(uniqueKeysWithValues: index.records.map { ($0.id, $0) })
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
                sync()
            } catch {
                status = error.localizedDescription
            }
            isBusy = false
        }
    }

    func exportSelected(to destination: URL) {
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
}
