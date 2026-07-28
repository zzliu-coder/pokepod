import AppKit
import Foundation

final class DropView: NSView {
    var onDrop: (([URL]) -> Void)?
    private let destinationPicker = NSPopUpButton(frame: .zero, pullsDown: false)
    private let titleLabel = NSTextField(labelWithString: "把书或文件夹拖到这里")
    private let detailLabel = NSTextField(labelWithString: "支持 EPUB、PDF、CBZ；Apple Books 的 EPUB 会自动封装")
    private let statusLabel = NSTextField(labelWithString: "正在检查 Poke3 连接…")
    private let countLabel = NSTextField(labelWithString: "队列：0")

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        registerForDraggedTypes([.fileURL])
        wantsLayer = true
        layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor

        titleLabel.font = .systemFont(ofSize: 25, weight: .semibold)
        titleLabel.alignment = .center
        detailLabel.font = .systemFont(ofSize: 14)
        detailLabel.textColor = .secondaryLabelColor
        detailLabel.alignment = .center
        statusLabel.font = .systemFont(ofSize: 14, weight: .medium)
        statusLabel.alignment = .center
        statusLabel.maximumNumberOfLines = 3
        countLabel.font = .monospacedDigitSystemFont(ofSize: 14, weight: .regular)
        countLabel.alignment = .center

        destinationPicker.addItems(withTitles: [
            "收件箱 · Books/Inbox",
            "图书 · Books/Books",
            "漫画 · Books/Comics",
            "下载 · Download"
        ])
        destinationPicker.selectItem(at: 0)
        destinationPicker.font = .systemFont(ofSize: 14, weight: .medium)

        let destinationRow = NSStackView(views: [
            NSTextField(labelWithString: "传到："),
            destinationPicker
        ])
        destinationRow.orientation = .horizontal
        destinationRow.spacing = 8

        let stack = NSStackView(views: [titleLabel, detailLabel, destinationRow, statusLabel, countLabel])
        stack.orientation = .vertical
        stack.alignment = .centerX
        stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)

        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
            stack.leadingAnchor.constraint(greaterThanOrEqualTo: leadingAnchor, constant: 30),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: trailingAnchor, constant: -30),
            statusLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 480)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        layer?.backgroundColor = NSColor.controlAccentColor.withAlphaComponent(0.12).cgColor
        return .copy
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        layer?.backgroundColor = NSColor.windowBackgroundColor.cgColor
        let options: [NSPasteboard.ReadingOptionKey: Any] = [.urlReadingFileURLsOnly: true]
        guard let urls = sender.draggingPasteboard.readObjects(
            forClasses: [NSURL.self],
            options: options
        ) as? [URL], !urls.isEmpty else {
            return false
        }
        onDrop?(urls)
        return true
    }

    func showStatus(_ text: String, error: Bool = false) {
        statusLabel.stringValue = text
        statusLabel.textColor = error ? .systemRed : .labelColor
    }

    func showCount(_ pending: Int) {
        countLabel.stringValue = "队列：\(pending)"
    }

    var selectedDestination: String {
        switch destinationPicker.indexOfSelectedItem {
        case 1: return "/sdcard/Books/Books"
        case 2: return "/sdcard/Books/Comics"
        case 3: return "/sdcard/Download"
        default: return "/sdcard/Books/Inbox"
        }
    }
}

final class TransferManager: @unchecked Sendable {
    private struct QueueItem {
        let url: URL
        let destination: String
    }

    private let adb = "/opt/homebrew/bin/adb"
    private let device = "BE87E832"
    private let worker = OperationQueue()
    private let lock = NSLock()
    private var pending: [QueueItem] = []
    private var running = false

    var onStatus: ((String, Bool) -> Void)?
    var onCount: ((Int) -> Void)?

    init() {
        worker.maxConcurrentOperationCount = 1
        worker.qualityOfService = .userInitiated
    }

    func checkConnection() {
        worker.addOperation { [weak self] in
            guard let self else { return }
            let result = self.runADB(["-s", self.device, "get-state"])
            if result.code == 0 {
                _ = self.runADB(["-s", self.device, "shell", "svc", "power", "stayon", "true"])
                _ = self.runADB(["-s", self.device, "shell", "input", "keyevent", "224"])
            }
            self.reportStatus(
                result.code == 0 && result.output.trimmingCharacters(in: .whitespacesAndNewlines) == "device"
                    ? "Poke3 已连接，并会在 USB 连接期间保持唤醒。"
                    : "未检测到 Poke3 的 ADB 连接，请检查 USB 调试。",
                error: result.code != 0
            )
        }
    }

    func add(_ urls: [URL], destination: String) {
        var shouldStart = false
        var count = 0
        lock.lock()
        pending.append(contentsOf: urls.map { QueueItem(url: $0, destination: destination) })
        count = pending.count + (running ? 1 : 0)
        if !running {
            running = true
            shouldStart = true
        }
        lock.unlock()

        reportCount(count)
        reportStatus("已加入 \(urls.count) 项，正在按顺序传输。", error: false)

        if shouldStart {
            worker.addOperation { [weak self] in
                self?.drain()
            }
        }
    }

    private func drain() {
        while true {
            let next: QueueItem?
            let remaining: Int
            lock.lock()
            if pending.isEmpty {
                running = false
                next = nil
                remaining = 0
            } else {
                next = pending.removeFirst()
                remaining = pending.count + 1
            }
            lock.unlock()

            guard let next else {
                reportCount(0)
                reportStatus("队列已完成。可以继续拖入。", error: false)
                return
            }

            reportCount(remaining)
            _ = runADB(["-s", device, "shell", "mkdir", "-p", next.destination])
            _ = runADB(["-s", device, "shell", "input", "keyevent", "224"])
            reportStatus("正在传输：\(next.url.lastPathComponent)", error: false)

            do {
                let prepared = try prepareSource(next.url)
                let result = runADB(["-s", device, "push", prepared.url.path, next.destination + "/"])
                if let cleanup = prepared.cleanup {
                    try? FileManager.default.removeItem(at: cleanup)
                }
                if result.code == 0 {
                    reportStatus("完成：\(next.url.lastPathComponent)", error: false)
                } else {
                    let message = result.output.trimmingCharacters(in: .whitespacesAndNewlines)
                    reportStatus("失败：\(next.url.lastPathComponent)\n\(message)", error: true)
                }
            } catch {
                reportStatus("封装失败：\(next.url.lastPathComponent)\n\(error.localizedDescription)", error: true)
            }
        }
    }

    private func prepareSource(_ source: URL) throws -> (url: URL, cleanup: URL?) {
        let values = try source.resourceValues(forKeys: [.isDirectoryKey])
        guard values.isDirectory == true, source.pathExtension.lowercased() == "epub" else {
            return (source, nil)
        }

        let mimetype = source.appendingPathComponent("mimetype")
        let container = source.appendingPathComponent("META-INF/container.xml")
        guard FileManager.default.fileExists(atPath: mimetype.path),
              FileManager.default.fileExists(atPath: container.path) else {
            throw NSError(
                domain: "Poke3Transfer",
                code: 1,
                userInfo: [NSLocalizedDescriptionKey: "这个 EPUB 文件夹缺少 mimetype 或 META-INF/container.xml"]
            )
        }

        let temporaryRoot = FileManager.default.temporaryDirectory
            .appendingPathComponent("Poke3Transfer-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: temporaryRoot, withIntermediateDirectories: true)
        let output = temporaryRoot.appendingPathComponent(source.lastPathComponent)

        let first = runProcess(
            "/usr/bin/zip",
            arguments: ["-X0", output.path, "mimetype"],
            currentDirectory: source
        )
        guard first.code == 0 else {
            try? FileManager.default.removeItem(at: temporaryRoot)
            throw NSError(
                domain: "Poke3Transfer",
                code: 2,
                userInfo: [NSLocalizedDescriptionKey: first.output]
            )
        }

        let rest = runProcess(
            "/usr/bin/zip",
            arguments: [
                "-Xr9D", output.path, ".",
                "-x", "mimetype", "*.DS_Store", "__MACOSX/*"
            ],
            currentDirectory: source
        )
        guard rest.code == 0 else {
            try? FileManager.default.removeItem(at: temporaryRoot)
            throw NSError(
                domain: "Poke3Transfer",
                code: 3,
                userInfo: [NSLocalizedDescriptionKey: rest.output]
            )
        }

        let validation = runProcess(
            "/usr/bin/unzip",
            arguments: ["-tqq", output.path],
            currentDirectory: nil
        )
        guard validation.code == 0 else {
            try? FileManager.default.removeItem(at: temporaryRoot)
            throw NSError(
                domain: "Poke3Transfer",
                code: 4,
                userInfo: [NSLocalizedDescriptionKey: "生成的 EPUB 未通过完整性校验"]
            )
        }

        return (output, temporaryRoot)
    }

    private func runADB(_ arguments: [String]) -> (code: Int32, output: String) {
        guard FileManager.default.isExecutableFile(atPath: adb) else {
            return (127, "Mac 上没有找到 ADB")
        }

        return runProcess(adb, arguments: arguments, currentDirectory: nil)
    }

    private func runProcess(
        _ executable: String,
        arguments: [String],
        currentDirectory: URL?
    ) -> (code: Int32, output: String) {
        let process = Process()
        let pipe = Pipe()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.currentDirectoryURL = currentDirectory
        process.standardOutput = pipe
        process.standardError = pipe

        do {
            try process.run()
            process.waitUntilExit()
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            return (process.terminationStatus, String(decoding: data, as: UTF8.self))
        } catch {
            return (126, error.localizedDescription)
        }
    }

    private func reportStatus(_ text: String, error: Bool) {
        DispatchQueue.main.async { [weak self] in
            self?.onStatus?(text, error)
        }
    }

    private func reportCount(_ count: Int) {
        DispatchQueue.main.async { [weak self] in
            self?.onCount?(count)
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow!
    private let dropView = DropView(frame: NSRect(x: 0, y: 0, width: 580, height: 380))
    private let transfer = TransferManager()

    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 580, height: 380),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Poke3 传书"
        window.center()
        window.contentView = dropView
        window.makeKeyAndOrderFront(nil)

        dropView.onDrop = { [weak self] urls in
            guard let self else { return }
            self.transfer.add(urls, destination: self.dropView.selectedDestination)
        }
        transfer.onStatus = { [weak dropView] text, isError in
            dropView?.showStatus(text, error: isError)
        }
        transfer.onCount = { [weak dropView] count in
            dropView?.showCount(count)
        }

        NSApp.activate(ignoringOtherApps: true)
        transfer.checkConnection()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func application(_ sender: NSApplication, openFiles filenames: [String]) {
        let urls = filenames.map { URL(fileURLWithPath: $0) }
        transfer.add(urls, destination: dropView.selectedDestination)
        sender.reply(toOpenOrPrint: .success)
    }
}

@main
struct Poke3TransferApp {
    @MainActor
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.run()
    }
}
