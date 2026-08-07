import AppKit
import PokeCapsuleCore
import SwiftUI

struct SettingsView: View {
    @AppStorage("CorrectionEndpoint") private var endpoint = "https://api.deepseek.com/chat/completions"
    @AppStorage("CorrectionModel") private var model = "deepseek-v4-flash"
    @AppStorage("CorrectionPrompt") private var prompt = "只校正识别错误和标点；不解释、不增删原意；无法判断时原样输出。只输出正文。"
    @AppStorage("ADBPath") private var adbPath = ""
    @AppStorage("AutomaticCorrectionEnabled") private var automaticCorrection = false
    @State private var apiKey = ""
    @State private var message = ""

    var body: some View {
        Form {
            Section("电脑连接") {
                TextField("ADB 路径", text: $adbPath)
                Text("每台设备拥有独立镜像、备份和离线操作队列。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("PokePod 有线语音输入") {
                Button("打开听写设置") { openDictationSettings() }
                Text("按住设备按键时会保持 Option-Z 按下并开始听写，松手时释放并结束；无需给 PokeCapsule 键盘监听或辅助功能权限。听写麦克风请选择 TinyUSB UAC1（PokeCapsule，48 kHz）。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("手动校对") {
                TextField("API 地址", text: $endpoint)
                TextField("模型", text: $model)
                TextField("校对提示词", text: $prompt)
                SecureField("API 密钥", text: $apiKey)
                HStack {
                    Button("保存密钥") { saveKey() }
                    Button("从桌面 api.txt 导入") { importKey() }
                    Text(message).foregroundStyle(.secondary)
                }
                Text("DeepSeek 只在点击校对按钮时调用，并关闭思考。密钥保存在当前 Mac 的受限本地文件中。")
                    .font(.caption).foregroundStyle(.secondary)
                Toggle("自动校对新转写", isOn: $automaticCorrection)
                Text("默认关闭。开启后，Mac 每次同步只处理一条新转写，原始转写永远保留。")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("存储与备份") {
                Text("原始录音永久保留；删除先进入设备回收站。Mac 离线修改会进入当前设备的独立队列。")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .padding(20)
        .frame(width: 620, height: 560)
    }

    private func saveKey() {
        do {
            try CorrectionAdapter().saveAPIKey(apiKey)
            apiKey = ""
            message = "密钥已保存"
        } catch { message = error.localizedDescription }
    }

    private func openDictationSettings() {
        guard let url = URL(string:
            "x-apple.systempreferences:com.apple.Keyboard-Settings.extension?Dictation") else { return }
        NSWorkspace.shared.open(url)
    }

    private func importKey() {
        do {
            let file = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Desktop/api.txt")
            try CorrectionAdapter().importAPIKey(from: file)
            apiKey = ""
            message = "已导入并保存"
        } catch { message = error.localizedDescription }
    }
}
