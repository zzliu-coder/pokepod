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
                .tint(.pokeAccent)
        }
        .commands {
            CommandGroup(after: .importExport) {
                Button("同步当前设备") { model.sync() }
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
    @Environment(\.scenePhase) private var scenePhase
    @State private var selectedRecord: CapsuleRecord?
    @State private var dialog: ActionDialog?
    @State private var actionTarget = ""

    var body: some View {
        NavigationSplitView {
            SidebarView(dialog: $dialog, actionTarget: $actionTarget)
                .navigationSplitViewColumnWidth(min: 220, ideal: 248, max: 300)
        } content: {
            CapsuleListView(selectedRecord: $selectedRecord, dialog: $dialog)
                .navigationSplitViewColumnWidth(min: 320, ideal: 360, max: 460)
        } detail: {
            CapsuleDetailView(record: selectedRecord)
        }
        .toolbar {
            ToolbarItemGroup {
                Button { model.refreshDevices() } label: {
                    Label("检查设备", systemImage: "cable.connector")
                }
                Button { model.sync() } label: {
                    Label("同步", systemImage: "arrow.clockwise")
                }
                .disabled(model.isBusy)
                Button { importCapsules() } label: {
                    Label("导入胶囊", systemImage: "square.and.arrow.down")
                }
                .disabled(model.isBusy)
            }
        }
        .sheet(item: $dialog) { action in
            ActionSheetView(action: action, target: actionTarget, isPresented: Binding(
                get: { dialog != nil },
                set: { if !$0 { dialog = nil } }
            ))
        }
        .onChange(of: model.sidebar) { _ in
            model.selection.removeAll()
            selectedRecord = nil
        }
        .onChange(of: model.selectedDeviceID) { _ in
            model.selection.removeAll()
            selectedRecord = nil
        }
        .onChange(of: model.index) { newIndex in
            guard let id = selectedRecord?.id else { return }
            selectedRecord = (newIndex.records + newIndex.trashRecords).first {
                $0.id == id
            }
        }
        .onChange(of: scenePhase) { phase in
            model.setApplicationActive(phase == .active)
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
    case move, copy, tag, delete, purge, createFolder, renameFolder, deleteFolder
    case renameTag, mergeTag, deleteTag
    var id: String { rawValue }
}
