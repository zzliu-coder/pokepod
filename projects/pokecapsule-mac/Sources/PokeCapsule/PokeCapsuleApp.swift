import AppKit
import PokeCapsuleCore
import SwiftUI

@main
struct PokeCapsuleApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup("PokeCapsule") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 980, minHeight: 640)
        }
        .commands {
            CommandGroup(after: .importExport) {
                Button("同步 Poke3") { model.sync() }
                    .keyboardShortcut("r", modifiers: [.command])
            }
        }

        Settings {
            SettingsView()
        }
    }
}

struct ContentView: View {
    @EnvironmentObject private var model: AppModel
    @State private var selectedRecord: CapsuleRecord?
    @State private var dialog: ActionDialog?
    @State private var actionTarget = ""

    var body: some View {
        NavigationSplitView {
            SidebarView(dialog: $dialog, actionTarget: $actionTarget)
        } content: {
            CapsuleListView(selectedRecord: $selectedRecord)
        } detail: {
            CapsuleDetailView(record: selectedRecord)
        }
        .safeAreaInset(edge: .bottom) {
            HStack {
                if model.isBusy { ProgressView().controlSize(.small) }
                Text(model.status).lineLimit(2)
                Spacer()
                Text("\(model.selection.count) 项已选")
            }
            .font(.callout)
            .padding(10)
            .background(.bar)
        }
        .toolbar {
            ToolbarItemGroup {
                Button { model.refreshDevices() } label: { Label("检查连接", systemImage: "cable.connector") }
                Button { model.sync() } label: { Label("同步", systemImage: "arrow.clockwise") }
                    .disabled(model.isBusy)
                Menu("批量操作") {
                    Button("移动…") { dialog = .move }
                    Button("复制…") { dialog = .copy }
                    Button("添加标签…") { dialog = .tag }
                    Button("设为收藏") { model.setFavorite(true) }
                    Button("取消收藏") { model.setFavorite(false) }
                    Divider()
                    Button("导入胶囊…") { importCapsules() }
                    Button("导出…") { exportSelection() }
                    Button("删除", role: .destructive) { dialog = .delete }
                }
                .disabled(model.selection.isEmpty || model.isBusy)
            }
        }
        .sheet(item: $dialog) { action in
            ActionSheetView(action: action, target: actionTarget, isPresented: Binding(
                get: { dialog != nil },
                set: { if !$0 { dialog = nil } }
            ))
        }
    }

    private func exportSelection() {
        let panel = NSOpenPanel()
        panel.title = "选择导出目录"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url {
            model.exportSelected(to: url)
        }
    }

    private func importCapsules() {
        let panel = NSOpenPanel()
        panel.title = "选择一个或多个完整胶囊目录"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = true
        if panel.runModal() == .OK {
            model.importCapsules(panel.urls, destination: "Inbox")
        }
    }
}

enum ActionDialog: String, Identifiable {
    case move, copy, tag, delete, createFolder, renameFolder, deleteFolder, renameTag, mergeTag, deleteTag
    var id: String { rawValue }
}

struct SidebarView: View {
    @EnvironmentObject private var model: AppModel
    @Binding var dialog: ActionDialog?
    @Binding var actionTarget: String

    var body: some View {
        List(selection: Binding(
            get: { model.sidebar },
            set: { model.sidebar = $0 ?? .folder("Inbox") }
        )) {
            Section("设备") {
                Label(model.connection.localizedDescription, systemImage: "externaldrive.connected.to.line.below")
                if case .multiple(let devices) = model.connection {
                    ForEach(devices) { device in
                        Button(device.displayName) { model.choose(device) }
                    }
                }
            }
            Section("胶囊") {
                Label("全部", systemImage: "tray.full").tag(SidebarSelection.all)
                Label("Inbox", systemImage: "tray").tag(SidebarSelection.folder("Inbox"))
                Label("Archive", systemImage: "archivebox").tag(SidebarSelection.folder("Archive"))
                Label("收藏", systemImage: "star").tag(SidebarSelection.favorites)
            }
            Section("目录") {
                ForEach(model.index.folders.filter { !ProtocolConstants.reservedFolders.contains($0) }, id: \.self) { folder in
                    Label(folder, systemImage: "folder").tag(SidebarSelection.folder(folder))
                        .contextMenu {
                            Button("改名…") {
                                actionTarget = folder
                                dialog = .renameFolder
                            }
                            Button("删除并把内容移回 Inbox…", role: .destructive) {
                                actionTarget = folder
                                dialog = .deleteFolder
                            }
                        }
                }
                Button { dialog = .createFolder } label: { Label("新建目录", systemImage: "folder.badge.plus") }
            }
            Section("标签") {
                ForEach(model.tags, id: \.self) { tag in
                    Text("#\(tag)").tag(SidebarSelection.tag(tag))
                        .contextMenu {
                            Button("改名…") {
                                actionTarget = tag
                                dialog = .renameTag
                            }
                            Button("合并到…") {
                                actionTarget = tag
                                dialog = .mergeTag
                            }
                            Button("删除标签", role: .destructive) {
                                actionTarget = tag
                                dialog = .deleteTag
                            }
                        }
                }
            }
        }
        .navigationTitle("PokeCapsule")
    }
}

struct CapsuleListView: View {
    @EnvironmentObject private var model: AppModel
    @Binding var selectedRecord: CapsuleRecord?

    var body: some View {
        List(model.filteredRecords, selection: $model.selection) { record in
            VStack(alignment: .leading, spacing: 5) {
                HStack {
                    Text(record.displayTitle).font(.headline)
                    if record.capsule.favorite { Image(systemName: "star.fill") }
                    Spacer()
                    Text(record.processing?.status.localizedName ?? "状态未知")
                        .foregroundStyle(.secondary)
                }
                HStack {
                    Text(record.capsule.createdAt, style: .date)
                    Text(record.capsule.createdAt, style: .time)
                    Text(duration(record.processing?.durationMs))
                    Text(record.relativeFolder)
                    if record.readOnly { Text("只读").foregroundStyle(.orange) }
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                if !record.capsule.tags.isEmpty {
                    Text(record.capsule.tags.map { "#\($0)" }.joined(separator: "  "))
                        .font(.caption)
                }
            }
            .tag(record.id)
            .contentShape(Rectangle())
            .onTapGesture { selectedRecord = record }
        }
        .navigationTitle("胶囊 \(model.filteredRecords.count)")
        .overlay {
            if model.filteredRecords.isEmpty {
                PlaceholderView(title: "这里还没有胶囊", systemImage: "tray")
            }
        }
    }

    private func duration(_ milliseconds: Int?) -> String {
        guard let milliseconds else { return "时长未知" }
        return String(format: "%d:%02d", milliseconds / 60_000, (milliseconds / 1_000) % 60)
    }
}

struct CapsuleDetailView: View {
    @EnvironmentObject private var model: AppModel
    let record: CapsuleRecord?

    var body: some View {
        if let record {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    HStack {
                        Text(record.displayTitle).font(.title2.bold())
                        Spacer()
                        Button("DeepSeek 校对") { model.correct(record) }
                            .disabled(record.rawText == nil || model.isBusy)
                        Button("播放录音") { model.play(record) }
                    }
                    LabeledContent("位置", value: record.relativeFolder)
                    LabeledContent("状态", value: record.processing?.status.localizedName ?? "未知")
                    if let error = record.processing?.error {
                        GroupBox("处理错误") { Text(error).foregroundStyle(.red) }
                    }
                    if !record.warnings.isEmpty {
                        GroupBox("文件警告") {
                            ForEach(record.warnings, id: \.self) { Text($0) }
                        }
                    }
                    GroupBox("校对文字") {
                        Text(record.polishedText ?? "尚未生成").textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    GroupBox("原始转写") {
                        Text(record.rawText ?? "尚未生成").textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
                .padding()
            }
        } else {
            PlaceholderView(title: "选择一个胶囊", systemImage: "waveform")
        }
    }
}

struct PlaceholderView: View {
    let title: String
    let systemImage: String

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: systemImage).font(.system(size: 38))
            Text(title).font(.headline)
        }
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct ActionSheetView: View {
    @EnvironmentObject private var model: AppModel
    let action: ActionDialog
    let target: String
    @Binding var isPresented: Bool
    @State private var text = ""
    @State private var parent = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(title).font(.title2.bold())
            if action == .delete {
                Text("将删除 \(model.selection.count) 个胶囊。原始音频也会进入设备垃圾箱。")
            } else if action == .deleteFolder {
                Text("目录“\(target)”中的所有胶囊会先移回 Inbox，然后删除目录。")
            } else if action == .deleteTag {
                Text("标签 #\(target) 会从全部胶囊中移除，胶囊和音频不会删除。")
            } else if action == .move || action == .copy {
                Picker("目标目录", selection: $text) {
                    ForEach(model.index.folders, id: \.self) { Text($0).tag($0) }
                }
                .onAppear { text = model.index.folders.first ?? "Inbox" }
            } else if action == .tag || action == .renameTag || action == .mergeTag {
                TextField(action == .mergeTag ? "合并到标签" : "标签名称", text: $text)
            } else if action == .renameFolder {
                TextField("新目录名称", text: $text)
            } else {
                TextField("目录名称", text: $text)
                Picker("上级目录（可选）", selection: $parent) {
                    Text("根目录").tag("")
                    ForEach(model.index.folders.filter { !ProtocolConstants.reservedFolders.contains($0) && !$0.contains("/") }, id: \.self) {
                        Text($0).tag($0)
                    }
                }
            }
            HStack {
                Spacer()
                Button("取消") { isPresented = false }
                Button(action == .delete ? "确认删除" : "确定") {
                    submit()
                    isPresented = false
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 420)
        .onAppear {
            if action == .renameFolder || action == .renameTag { text = target.split(separator: "/").last.map(String.init) ?? target }
        }
    }

    private var title: String {
        switch action {
        case .move: return "移动胶囊"
        case .copy: return "复制胶囊"
        case .tag: return "添加标签"
        case .delete: return "确认删除"
        case .createFolder: return "新建目录"
        case .renameFolder: return "目录改名"
        case .deleteFolder: return "删除目录"
        case .renameTag: return "标签改名"
        case .mergeTag: return "合并标签"
        case .deleteTag: return "删除标签"
        }
    }

    private func submit() {
        switch action {
        case .move: model.moveSelected(to: text)
        case .copy: model.copySelected(to: text)
        case .tag: model.addTag(text)
        case .delete: model.deleteSelected()
        case .createFolder: model.createFolder(text, parent: parent.isEmpty ? nil : parent)
        case .renameFolder: model.renameFolder(target, to: text)
        case .deleteFolder: model.deleteFolder(target)
        case .renameTag: model.renameTag(target, to: text)
        case .mergeTag: model.mergeTag(target, into: text)
        case .deleteTag: model.deleteTag(target)
        }
    }
}

struct SettingsView: View {
    @AppStorage("CorrectionEndpoint") private var endpoint = "https://api.deepseek.com/chat/completions"
    @AppStorage("CorrectionModel") private var model = "deepseek-v4-flash"
    @AppStorage("CorrectionPrompt") private var prompt = "只校正识别错误和标点；不解释、不增删原意；无法判断时原样输出。只输出正文。"
    @AppStorage("AutoCorrection") private var autoCorrection = true
    @AppStorage("ADBPath") private var adbPath = ""
    @State private var apiKey = ""
    @State private var message = ""

    var body: some View {
        Form {
            TextField("ADB 路径", text: $adbPath)
            TextField("校对 API 地址", text: $endpoint)
            TextField("模型", text: $model)
            TextField("校对提示词", text: $prompt)
            Toggle("同步后自动校对待处理胶囊", isOn: $autoCorrection)
            SecureField("API 密钥", text: $apiKey)
            HStack {
                Button("保存密钥") {
                    do {
                        try CorrectionAdapter().saveAPIKey(apiKey)
                        apiKey = ""
                        message = "密钥已保存到 macOS 钥匙串"
                    } catch { message = error.localizedDescription }
                }
                Button("从桌面 api.txt 导入") {
                    do {
                        let file = FileManager.default.homeDirectoryForCurrentUser
                            .appendingPathComponent("Desktop/api.txt")
                        try CorrectionAdapter().importAPIKey(from: file)
                        apiKey = ""
                        message = "已读取第一行并保存到 macOS 钥匙串"
                    } catch { message = error.localizedDescription }
                }
                Text(message).foregroundStyle(.secondary)
            }
            Text("默认使用 DeepSeek V4 Flash，并显式关闭思考。地址、模型和提示词都可以修改。密钥只保存在 macOS 钥匙串。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(24)
        .frame(width: 560)
    }
}
