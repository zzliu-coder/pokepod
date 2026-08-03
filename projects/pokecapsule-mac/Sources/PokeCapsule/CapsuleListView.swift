import PokeCapsuleCore
import SwiftUI

struct CapsuleListView: View {
    @EnvironmentObject private var model: AppModel
    @Binding var selectedRecord: CapsuleRecord?
    @Binding var dialog: ActionDialog?

    var body: some View {
        VStack(spacing: 0) {
            listHeader
            Divider()
            List(model.filteredRecords, selection: $model.selection) { record in
                VStack(alignment: .leading, spacing: 7) {
                    HStack {
                        Text(record.displayPreview)
                            .font(.body.weight(.medium))
                            .lineLimit(1)
                            .multilineTextAlignment(.leading)
                        Spacer()
                        if record.capsule.favorite {
                            Image(systemName: "star.fill")
                                .foregroundStyle(.primary)
                                .accessibilityLabel("已收藏")
                        }
                    }
                    HStack {
                        Text(record.capsule.createdAt, style: .date)
                        Text(record.capsule.createdAt, style: .time)
                        Text(displayFolder(record.relativeFolder))
                        Text(duration(record.processing?.durationMs))
                        if let status = record.visibleProcessingStatus {
                            Text(status)
                                .foregroundStyle(record.processing?.status == .failed ? .red : .secondary)
                        }
                        if record.readOnly { Text("只读").foregroundStyle(.orange) }
                        if record.trash != nil { Text("已删除").foregroundStyle(.secondary) }
                        if !record.capsule.tags.isEmpty {
                            Text(record.capsule.tags.map { "#\($0)" }.joined(separator: " "))
                        }
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                }
                .padding(.vertical, 4)
                .tag(record.id)
                .contentShape(Rectangle())
            }
            .overlay {
                if model.filteredRecords.isEmpty {
                    PlaceholderView(title: emptyTitle, systemImage: "tray")
                }
            }
        }
        .onChange(of: model.selection) { selection in
            if selection.isEmpty {
                selectedRecord = nil
            } else if selectedRecord.map({ selection.contains($0.id) }) != true {
                selectedRecord = model.filteredRecords.first { selection.contains($0.id) }
            }
        }
        .safeAreaInset(edge: .bottom) {
            if !model.selection.isEmpty { batchBar }
        }
    }

    private var listHeader: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(model.sidebar.title)
                    .font(.title3.weight(.semibold))
                Text("\(model.filteredRecords.count)")
                    .foregroundStyle(.secondary)
                Spacer()
                Menu {
                    Picker("排列", selection: $model.librarySort) {
                        Text("最新在前").tag(LibrarySort.newestFirst)
                        Text("最旧在前").tag(LibrarySort.oldestFirst)
                    }
                } label: {
                    Label(model.librarySort == .newestFirst ? "最新在前" : "最旧在前",
                          systemImage: "arrow.up.arrow.down")
                }
                .menuStyle(.borderlessButton)
            }
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                TextField("搜索文字、标签或目录", text: $model.searchQuery)
                    .textFieldStyle(.plain)
                if !model.searchQuery.isEmpty {
                    Button {
                        model.searchQuery = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("清除搜索")
                }
            }
            .padding(.horizontal, 10)
            .frame(height: 34)
            .background(Color(nsColor: .controlBackgroundColor), in: RoundedRectangle(cornerRadius: 8))

            if inboxFiltersVisible {
                HStack(spacing: 4) {
                    filterButton("全部", scope: .folder("Inbox"))
                    filterButton("待转写 \(model.recordCount(in: .pending))", scope: .pending)
                    filterButton("转写失败 \(model.recordCount(in: .failed))", scope: .failed)
                }
                .controlSize(.small)
            }
        }
        .padding(.horizontal, 14)
        .padding(.top, 12)
        .padding(.bottom, 10)
    }

    private var inboxFiltersVisible: Bool {
        model.sidebar == .folder("Inbox") || model.sidebar == .pending || model.sidebar == .failed
    }

    @ViewBuilder
    private func filterButton(_ title: String, scope: LibraryScope) -> some View {
        if model.sidebar == scope {
            Button(title) { model.sidebar = scope }
                .buttonStyle(.borderedProminent)
        } else {
            Button(title) { model.sidebar = scope }
                .buttonStyle(.bordered)
        }
    }

    private var emptyTitle: String {
        model.searchQuery.isEmpty ? "这里还没有胶囊" : "没有匹配的胶囊"
    }

    private var batchBar: some View {
        HStack(spacing: 8) {
            Text("已选择 \(model.selection.count) 个").fontWeight(.semibold)
            Spacer()
            if model.sidebar == .trash {
                Button("恢复") { model.restoreSelected() }
                Button("复制文字") { model.copySelectedText(markdown: false) }
                Button("永久删除", role: .destructive) { dialog = .purge }
            } else {
                Button("移动") { dialog = .move }
                Button("标签") { dialog = .tag }
                Button("收藏") { model.setFavorite(true) }
                Button("复制") { model.copySelectedText(markdown: false) }
                Button("删除", role: .destructive) { dialog = .delete }
            }
        }
        .buttonStyle(.bordered)
        .padding(10)
        .background(.bar)
    }

    private func duration(_ milliseconds: Int?) -> String {
        guard let milliseconds else { return "时长未知" }
        return String(format: "%d:%02d", milliseconds / 60_000, (milliseconds / 1_000) % 60)
    }

    private func displayFolder(_ folder: String) -> String {
        if folder == "Inbox" { return "收件箱" }
        if folder == "Archive" { return "归档" }
        return folder
    }
}
