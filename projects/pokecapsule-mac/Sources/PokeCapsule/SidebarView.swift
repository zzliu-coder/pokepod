import AppKit
import PokeCapsuleCore
import SwiftUI

struct SidebarView: View {
    @EnvironmentObject private var model: AppModel
    @Environment(\.colorScheme) private var colorScheme
    @Binding var dialog: ActionDialog?
    @Binding var actionTarget: String

    var body: some View {
        List(selection: Binding(get: { model.sidebar },
                                set: { model.sidebar = $0 ?? .folder("Inbox") })) {
            Section {
                deviceSelector
            }
            Section("收件箱") {
                scopeRow("全部新胶囊", systemImage: "tray", scope: .folder("Inbox"))
                scopeRow("待转写", systemImage: "clock", scope: .pending)
                scopeRow("转写失败", systemImage: "exclamationmark.triangle", scope: .failed)
            }
            Section("资料库") {
                scopeRow("全部胶囊", systemImage: "tray.full", scope: .all)
                scopeRow("收藏", systemImage: "star", scope: .favorites)
            }
            Section("目录") {
                scopeRow("归档", systemImage: "archivebox", scope: .folder("Archive"))
                ForEach(model.index.folders.filter { !ProtocolConstants.reservedFolders.contains($0) }, id: \.self) { folder in
                    scopeRow(folder, systemImage: "folder", scope: .folder(folder))
                        .contextMenu {
                            Button("改名…") { actionTarget = folder; dialog = .renameFolder }
                            Button("删除并把内容移回 Inbox…", role: .destructive) {
                                actionTarget = folder; dialog = .deleteFolder
                            }
                        }
                }
                Button { dialog = .createFolder } label: {
                    Label("新建目录", systemImage: "folder.badge.plus")
                }
            }
            Section("标签") {
                ForEach(model.tags, id: \.self) { tag in
                    scopeRow("#\(tag)", systemImage: "number", scope: .tag(tag))
                        .contextMenu {
                            Button("改名…") { actionTarget = tag; dialog = .renameTag }
                            Button("合并到…") { actionTarget = tag; dialog = .mergeTag }
                            Button("删除标签", role: .destructive) {
                                actionTarget = tag; dialog = .deleteTag
                            }
                        }
                }
            }
            if !model.pendingCommands.isEmpty {
                Section("待同步 \(model.pendingCommands.count)") {
                    ForEach(model.pendingCommands) { pending in
                        VStack(alignment: .leading, spacing: 3) {
                            Text(commandName(pending.command.operation))
                            if let error = pending.error {
                                Text(error).font(.caption).foregroundStyle(.red).lineLimit(2)
                            } else {
                                Text(pending.state == .conflict ? "版本冲突" : "等待连接")
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .contextMenu {
                            Button("丢弃这项操作", role: .destructive) {
                                model.discardPending(pending.id)
                            }
                            .disabled(model.isBusy)
                        }
                    }
                }
            }
            Section {
                scopeRow("回收站", systemImage: "trash", scope: .trash)
            }
        }
        .navigationTitle(model.selectedRegisteredDevice?.displayName ?? "PokeCapsule")
        .safeAreaInset(edge: .bottom) {
            VStack(alignment: .leading, spacing: 6) {
                if model.isBusy {
                    ProgressView().controlSize(.small)
                }
                Text(model.status)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .padding(.horizontal, 12)
                Button {
                    NSApp.sendAction(Selector(("showSettingsWindow:")), to: nil, from: nil)
                } label: {
                    HStack {
                        Label("设置", systemImage: "gearshape")
                        Spacer()
                    }
                    .padding(.horizontal, 12)
                    .frame(height: 38)
                }
                .buttonStyle(.plain)
            }
            .padding(.top, 8)
            .background(.bar)
        }
    }

    private var deviceSelector: some View {
        Menu {
            ForEach(model.registeredDevices) { device in
                Button {
                    model.selectRegisteredDevice(device.deviceId)
                } label: {
                    Label {
                        Text("\(device.displayName) · \(model.connectionLabel(for: device))")
                    } icon: {
                        Image(systemName: deviceIcon(device))
                    }
                }
            }
            if case .multiple(let devices) = model.connection {
                let unregistered = devices.filter { adb in
                    !model.registeredDevices.contains { $0.serialAliases.contains(adb.serial) }
                }
                if !unregistered.isEmpty {
                    Divider()
                    ForEach(unregistered) { device in
                        Button("添加 \(device.displayName)") { model.choose(device) }
                    }
                }
            }
            let unregisteredPokePods = model.pokePodPorts.filter { port in
                !model.registeredDevices.contains { $0.serialAliases.contains(port.path) }
            }
            if !unregisteredPokePods.isEmpty {
                Divider()
                ForEach(unregisteredPokePods, id: \.path) { port in
                    Button("连接 PokePod · \(port.lastPathComponent)") {
                        model.choosePokePod(port)
                    }
                }
            }
            Divider()
            Button("检查连接") { model.refreshDevices() }
        } label: {
            HStack(spacing: 10) {
                Image(systemName: model.selectedRegisteredDevice.map(deviceIcon) ?? "externaldrive.connected.to.line.below")
                    .font(.title3)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.selectedRegisteredDevice?.displayName ?? "选择设备")
                        .fontWeight(.semibold)
                        .foregroundColor(colorScheme == .dark ? .white : .black)
                    Text(model.selectedRegisteredDevice.map(model.connectionLabel) ?? model.connection.localizedDescription)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Image(systemName: "chevron.up.chevron.down")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .contentShape(Rectangle())
            .padding(.vertical, 5)
        }
        .menuStyle(.borderlessButton)
        .tint(.primary)
    }

    private func scopeRow(_ title: String, systemImage: String, scope: LibraryScope) -> some View {
        HStack {
            Label(title, systemImage: systemImage)
            Spacer()
            Text("\(model.recordCount(in: scope))")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .tag(scope)
    }

    private func deviceIcon(_ device: RegisteredDevice) -> String {
        if device.isPokePod { return "waveform.circle" }
        return device.displayName.localizedCaseInsensitiveContains("poke")
            || device.model?.localizedCaseInsensitiveContains("poke") == true
            ? "book.closed" : "smartphone"
    }

    private func commandName(_ operation: CommandOperation) -> String {
        switch operation {
        case .moveCapsules: return "移动胶囊"
        case .copyCapsules: return "复制胶囊"
        case .deleteCapsules: return "删除到回收站"
        case .restoreCapsules: return "恢复胶囊"
        case .purgeCapsules: return "永久删除"
        case .setFavorite: return "修改收藏"
        case .addTags: return "添加标签"
        case .removeTags: return "移除标签"
        case .commitFinalText: return "保存最终文字"
        default: return "设备操作"
        }
    }
}
