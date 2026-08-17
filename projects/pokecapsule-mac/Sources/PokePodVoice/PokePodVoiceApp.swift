import AppKit
import SwiftUI

@main
struct PokePodVoiceApp: App {
    private static var shortcutDiagnostic: ShortcutController?
    @StateObject private var model = VoiceRuntimeModel()

    init() {
        Self.runCommandLineDiagnosticIfRequested()
    }

    var body: some Scene {
        MenuBarExtra {
            VoiceMenuView(model: model)
        } label: {
            Label("PokePod Voice · \(model.state.title)", systemImage: model.state.symbol)
        }
        .menuBarExtraStyle(.window)
    }

    private static func runCommandLineDiagnosticIfRequested() {
        let arguments = ProcessInfo.processInfo.arguments
        if arguments.contains("--request-accessibility") {
            let shortcut = ShortcutController()
            shortcutDiagnostic = shortcut
            shortcut.requestAuthorization()
            print("accessibility request: trusted=\(shortcut.isAuthorized)")
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                shortcutDiagnostic = nil
                NSApplication.shared.terminate(nil)
            }
            return
        }

        guard arguments.contains("--verify-option-z-hold") else {
            return
        }

        let shortcut = ShortcutController()
        shortcutDiagnostic = shortcut
        print("option-z diagnostic: accessibility=\(shortcut.isAuthorized)")
        guard shortcut.isAuthorized else {
            DispatchQueue.main.async {
                NSApplication.shared.terminate(nil)
            }
            return
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
            do {
                try shortcut.pressOptionZ()
                print("option-z diagnostic: key-down")
            } catch {
                print("option-z diagnostic: failed=\(error)")
                shortcutDiagnostic = nil
                NSApplication.shared.terminate(nil)
                return
            }

            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                shortcut.releaseOptionZ()
                print("option-z diagnostic: key-up")
                shortcutDiagnostic = nil
                NSApplication.shared.terminate(nil)
            }
        }
    }
}

private struct VoiceMenuView: View {
    @ObservedObject var model: VoiceRuntimeModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(spacing: 10) {
                Image(systemName: model.state.symbol)
                    .font(.system(size: 24, weight: .semibold))
                    .foregroundStyle(stateColor)
                    .frame(width: 34, height: 34)
                    .background(stateColor.opacity(0.12), in: RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 2) {
                    Text(model.state.title).font(.headline)
                    Text(model.deviceName).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
            }

            Text(model.detail)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)

            Divider()
            readinessRow("蓝牙", ready: model.bluetoothReady, actionTitle: "重连") {
                model.reconnect()
            }
            readinessRow(
                "BlackHole 2ch",
                ready: model.blackHoleReady,
                actionTitle: model.blackHoleInstallInProgress ? "安装中…" : "安装",
                actionEnabled: !model.blackHoleInstallInProgress) {
                model.openBlackHoleInstaller()
            }
            readinessRow("辅助功能", ready: model.accessibilityReady, actionTitle: "授权") {
                model.requestAccessibility()
            }

            HStack {
                Image(systemName: "chart.bar.xaxis")
                VStack(alignment: .leading, spacing: 2) {
                    Text("Mac 接收质量")
                        .font(.caption.weight(.semibold))
                    Text(model.connectionQuality)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            HStack(alignment: .top) {
                Image(systemName: "timeline.selection")
                VStack(alignment: .leading, spacing: 2) {
                    Text("最近会话诊断")
                        .font(.caption.weight(.semibold))
                    Text(model.sessionDiagnostic)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            HStack(alignment: .top) {
                Image(systemName: "arrow.up.forward.square")
                VStack(alignment: .leading, spacing: 2) {
                    Text("设备发送队列")
                        .font(.caption.weight(.semibold))
                    Text(model.deviceQueueQuality)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Text("主机入队不代表无线空口已送达")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
            }

            Divider()
            Toggle("登录时启动", isOn: Binding(
                get: { model.launchAtLogin },
                set: { model.setLaunchAtLogin($0) }))
            HStack {
                Button("重连") { model.reconnect() }
                Button("重新配对") { model.forgetPokePod() }
                Button("恢复麦克风") { model.repairMicrophone() }
                Spacer()
                Button("退出") { NSApplication.shared.terminate(nil) }
            }
        }
        .padding(16)
        .frame(width: 340)
    }

    @ViewBuilder
    private func readinessRow(
        _ title: String,
        ready: Bool,
        actionTitle: String,
        actionEnabled: Bool = true,
        action: @escaping () -> Void
    ) -> some View {
        HStack {
            Image(systemName: ready ? "checkmark.circle.fill" : "exclamationmark.circle")
                .foregroundStyle(ready ? Color.green : Color.orange)
            Text(title)
            Spacer()
            Text(ready ? "已就绪" : "未就绪")
                .font(.caption)
                .foregroundStyle(.secondary)
            if !ready {
                Button(actionTitle, action: action)
                    .controlSize(.small)
                    .disabled(!actionEnabled)
            }
        }
    }

    private var stateColor: Color {
        switch model.state {
        case .setup: return .orange
        case .connecting: return .blue
        case .ready: return .mint
        case .listening: return .cyan
        case .error: return .red
        }
    }
}
