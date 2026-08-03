import PokeCapsuleCore
import SwiftUI

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
            formContent
            HStack {
                Spacer()
                Button("取消") { isPresented = false }
                Button(action == .delete || action == .purge ? "确认删除" : "确定") {
                    submit()
                    isPresented = false
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 420)
        .onAppear {
            if action == .renameFolder || action == .renameTag {
                text = target.split(separator: "/").last.map(String.init) ?? target
            }
        }
    }

    @ViewBuilder
    private var formContent: some View {
        if action == .delete {
            Text("将删除 \(model.selection.count) 个胶囊。原始音频也会进入设备垃圾箱。")
        } else if action == .purge {
            Text("将永久删除 \(model.selection.count) 个胶囊及其录音。这个操作无法恢复。")
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
                ForEach(model.index.folders.filter {
                    !ProtocolConstants.reservedFolders.contains($0) && !$0.contains("/")
                }, id: \.self) { Text($0).tag($0) }
            }
        }
    }

    private var title: String {
        switch action {
        case .move: return "移动胶囊"
        case .copy: return "复制胶囊"
        case .tag: return "添加标签"
        case .delete: return "确认删除"
        case .purge: return "确认永久删除"
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
        case .purge: model.purgeSelected()
        case .createFolder: model.createFolder(text, parent: parent.isEmpty ? nil : parent)
        case .renameFolder: model.renameFolder(target, to: text)
        case .deleteFolder: model.deleteFolder(target)
        case .renameTag: model.renameTag(target, to: text)
        case .mergeTag: model.mergeTag(target, into: text)
        case .deleteTag: model.deleteTag(target)
        }
    }
}
