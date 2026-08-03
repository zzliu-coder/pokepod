import AppKit
import PokeCapsuleCore
import SwiftUI

struct CapsuleDetailView: View {
    @EnvironmentObject private var model: AppModel
    let record: CapsuleRecord?
    @State private var showVersions = false
    @State private var showEditor = false

    var body: some View {
        if let record {
            ScrollView {
                VStack(alignment: .leading, spacing: 22) {
                    detailHeader(record)

                    if let status = record.visibleProcessingStatus {
                        StatusBadge(text: status,
                                    isError: record.processing?.status == .failed)
                    }

                    if let error = record.userFacingError {
                        NoticePanel(title: "这段录音需要处理", message: error, isError: true)
                        if record.canRetryTranscription {
                            Button {
                                model.retryTranscription(record)
                            } label: {
                                Label("重新转写", systemImage: "arrow.clockwise")
                            }
                            .buttonStyle(.borderedProminent)
                            .disabled(model.isBusy || record.readOnly)
                        }
                    }

                    if !record.warnings.isEmpty {
                        DisclosureGroup("文件检查") {
                            ForEach(record.warnings, id: \.self) { Text($0) }
                        }
                        .foregroundStyle(.secondary)
                    }

                    PlaybackRow(record: record)
                    TextPanel(
                        title: record.primaryTextLabel,
                        text: record.primaryText ?? "转写完成后，文字会出现在这里。",
                        actionTitle: record.trash == nil && !record.readOnly ? "编辑" : nil,
                        action: { showEditor = true })
                    organizeLine(record)
                    versions(record)
                }
                .padding(24)
                .frame(maxWidth: 860, alignment: .leading)
            }
            .background(Color.pokeBackground)
            .sheet(isPresented: $showEditor) {
                VStack(spacing: 0) {
                    HStack {
                        Text("编辑最终文字").font(.headline)
                        Spacer()
                        Button("关闭") { showEditor = false }
                    }
                    .padding(16)
                    Divider()
                    FinalTextEditor(record: record)
                        .padding(18)
                }
                .frame(minWidth: 620, minHeight: 430)
            }
            .onChange(of: record.id) { _ in
                showVersions = false
                showEditor = false
            }
        } else {
            PlaceholderView(title: "选择一个胶囊", systemImage: "waveform")
        }
    }

    private func detailHeader(_ record: CapsuleRecord) -> some View {
        HStack(alignment: .top, spacing: 18) {
            VStack(alignment: .leading, spacing: 8) {
                Text(record.displayTitle)
                    .font(.title2.weight(.semibold))
                    .textSelection(.enabled)
                HStack(spacing: 8) {
                    Text(record.capsule.createdAt, style: .date)
                    Text(record.capsule.createdAt, style: .time)
                    Text(displayFolder(record.relativeFolder))
                    Text(duration(record.processing?.durationMs))
                }
                .font(.callout)
                .foregroundStyle(.secondary)
            }
            Spacer()
            HStack {
                Button { model.setFavorite(record, !record.capsule.favorite) } label: {
                    Label(record.capsule.favorite ? "已收藏" : "收藏",
                          systemImage: record.capsule.favorite ? "star.fill" : "star")
                }
                organizeMenu(record)
                Menu {
                    if record.rawText != nil {
                        Button { model.correct(record) } label: {
                            Label("用 DeepSeek 校对", systemImage: "wand.and.stars")
                        }
                        .disabled(model.isBusy)
                    }
                    Button { copyPrimaryText(record) } label: {
                        Label("复制文字", systemImage: "doc.on.doc")
                    }
                    .disabled(record.primaryText == nil)
                } label: {
                    Label("更多", systemImage: "ellipsis").labelStyle(.iconOnly)
                }
            }
            .buttonStyle(.bordered)
        }
    }

    @ViewBuilder
    private func versions(_ record: CapsuleRecord) -> some View {
        if record.rawText != nil || record.polishedText != nil || record.finalText != nil {
            DisclosureGroup(showVersions ? "收起文字版本" : "查看文字版本",
                            isExpanded: $showVersions) {
                VStack(alignment: .leading, spacing: 18) {
                    VersionText(title: "原始转写", text: record.rawText ?? "暂无")
                    VersionText(title: "校对文字", text: record.polishedText ?? "暂无")
                    VersionText(title: "最终文字", text: record.finalText ?? "暂无")
                }
                .padding(.top, 10)
            }
        }
    }

    private func organizeMenu(_ record: CapsuleRecord) -> some View {
        Menu {
            Menu("移动到目录") {
                ForEach(model.index.folders, id: \.self) { folder in
                    Button(displayFolder(folder)) { model.move(record, to: folder) }
                }
            }
            Menu("添加标签") {
                ForEach(model.tags, id: \.self) { tag in
                    Button("#\(tag)") { model.addTag(tag, to: record) }
                }
            }
            Button(record.capsule.favorite ? "取消收藏" : "收藏") {
                model.setFavorite(record, !record.capsule.favorite)
            }
        } label: {
            Label("整理", systemImage: "tray.full")
        }
    }

    private func organizeLine(_ record: CapsuleRecord) -> some View {
        HStack(spacing: 9) {
            Text("整理到").font(.callout).foregroundStyle(.secondary)
            Menu {
                ForEach(model.index.folders, id: \.self) { folder in
                    Button(displayFolder(folder)) { model.move(record, to: folder) }
                }
            } label: {
                Text(displayFolder(record.relativeFolder))
            }
            .menuStyle(.borderlessButton)
            ForEach(record.capsule.tags, id: \.self) { tag in
                Text("#\(tag)")
                    .font(.caption)
                    .foregroundStyle(Color.pokeAccent)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.pokeAccentSoft, in: Capsule())
            }
            Menu {
                ForEach(model.tags, id: \.self) { tag in
                    Button("#\(tag)") { model.addTag(tag, to: record) }
                }
            } label: {
                Label("标签", systemImage: "plus")
            }
            .menuStyle(.borderlessButton)
            Spacer()
        }
        .padding(.horizontal, 4)
    }

    private func copyPrimaryText(_ record: CapsuleRecord) {
        guard let text = record.primaryText else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }

    private func duration(_ milliseconds: Int?) -> String {
        guard let milliseconds else { return "时长未知" }
        return String(format: "%d:%02d", milliseconds / 60_000,
                      (milliseconds / 1_000) % 60)
    }

    private func displayFolder(_ folder: String) -> String {
        if folder == "Inbox" { return "收件箱" }
        if folder == "Archive" { return "归档" }
        return folder
    }
}

private struct FinalTextEditor: View {
    @EnvironmentObject private var model: AppModel
    let record: CapsuleRecord
    @State private var text = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("编辑最终文字").font(.headline)
                Spacer()
                Button("保存") { model.saveFinalText(text, for: record) }
                    .buttonStyle(.borderedProminent)
                    .disabled(record.trash != nil || record.readOnly || model.isBusy)
            }
            TextEditor(text: $text)
                .font(.body)
                .scrollContentBackground(.hidden)
                .frame(minHeight: 170)
                .padding(10)
                .background(Color.pokeEditor, in: RoundedRectangle(cornerRadius: 10))
                .disabled(record.trash != nil || record.readOnly)
            Text("你的编辑单独保存，原始转写和模型校对不会被覆盖。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .onAppear { resetText() }
        .onChange(of: record.id) { _ in resetText() }
        .onChange(of: record.capsule.revision) { _ in resetText() }
    }

    private func resetText() {
        text = record.finalText ?? record.polishedText ?? record.rawText ?? ""
    }
}
