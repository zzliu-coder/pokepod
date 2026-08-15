import AppKit
import Combine
import CryptoKit
import Foundation
import PokePodVoiceCore
import ServiceManagement

enum VoiceAppState: String {
    case setup
    case connecting
    case ready
    case listening
    case error

    var title: String {
        switch self {
        case .setup: return "未配置"
        case .connecting: return "连接中"
        case .ready: return "就绪"
        case .listening: return "正在输入"
        case .error: return "异常"
        }
    }

    var symbol: String {
        switch self {
        case .setup: return "gear.badge.questionmark"
        case .connecting: return "antenna.radiowaves.left.and.right"
        case .ready: return "waveform.badge.mic"
        case .listening: return "waveform.circle.fill"
        case .error: return "exclamationmark.triangle"
        }
    }
}

@MainActor
final class VoiceRuntimeModel: ObservableObject {
    private static let loginItemDecisionKey = "PokePodVoice.LoginItemFirstRunDecisionMade"
    @Published private(set) var state: VoiceAppState = .setup
    @Published private(set) var detail = "正在检查运行环境"
    @Published private(set) var bluetoothReady = false
    @Published private(set) var blackHoleReady = false
    @Published private(set) var blackHoleInstallInProgress = false
    @Published private(set) var accessibilityReady = false
    @Published private(set) var connectionQuality = VoiceLinkQualityFormatter.summary(
        nil, context: .disconnected)
    @Published private(set) var deviceQueueQuality = DeviceSendQueueQualityFormatter.summary(nil)
    @Published private(set) var deviceName = "PokePod"
    @Published var launchAtLogin = false

    private let ble: any BLECentralControlling
    private let platform: MacVoicePlatform
    private var executor: VoiceActionExecutor!
    private var machine = VoiceSessionMachine()
    private var completion = VoiceSessionCompletionCoordinator()
    private var jitter = VoiceJitterBuffer()
    private var latestQuality: VoiceLinkQualitySnapshot?
    private var timer: Timer?
    private var transportReady = false
    private var firmwareReady = false
    private var isShuttingDown = false
    private var blackHolePackage: URL?
    private var blackHoleDiscoveryTask: Task<URL?, Never>?
    private var blackHoleDiscoveryStarted = false

    init(
        ble: any BLECentralControlling = BLECentralAdapter(),
        platform: MacVoicePlatform = .init()
    ) {
        self.ble = ble
        self.platform = platform
        executor = VoiceActionExecutor(platform: platform) { [weak self] message in
            Task { @MainActor in self?.showError(message) }
        }
        platform.input.onUserOverride = { [weak self] in
            Task { @MainActor in self?.userChangedInput() }
        }
        ble.onSignal = { [weak self] signal in
            Task { @MainActor in self?.handle(signal) }
        }
        platform.input.repairAfterPreviousCrash()
        launchAtLogin = SMAppService.mainApp.status == .enabled
        startBlackHoleDiscovery()
        refreshPrerequisites()
        timer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification,
            object: nil,
            queue: .main
        ) { [weak self] _ in
            Task { @MainActor in self?.shutdown() }
        }
        ble.start()
    }

    func reconnect() {
        safeAbort(reason: "手动重新连接", report: false)
        firmwareReady = false
        transportReady = false
        state = .connecting
        detail = "正在重新连接 PokePod"
        ble.reconnect()
    }

    func repairMicrophone() {
        safeAbort(reason: "手动恢复麦克风", report: false)
        refreshPrerequisites()
        if platform.input.hasPendingRecovery {
            state = .error
            detail = "原麦克风暂不可用；恢复记录已保留，将继续自动重试"
        } else {
            applyPresentationUpdate(VoiceSessionPresentationPolicy.recovery(
                environmentReady: firmwareReady && prerequisitesReady,
                detail: "默认麦克风已检查并恢复"))
        }
    }

    func requestAccessibility() {
        platform.shortcut.requestAuthorization()
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
            NSWorkspace.shared.open(url)
        }
        refreshPrerequisites()
    }

    func openBlackHoleInstaller() {
        guard !blackHoleInstallInProgress else { return }
        blackHoleInstallInProgress = true
        detail = "正在准备 BlackHole 2ch 安装包"
        Task { @MainActor [weak self] in
            guard let self else { return }
            defer { blackHoleInstallInProgress = false }
            do {
                let package = try await self.obtainBlackHolePackage()
                self.openSystemInstaller(package)
            } catch {
                self.showRecoverable("BlackHole 自动下载失败：\(error.localizedDescription)")
            }
        }
    }

    func forgetPokePod() {
        safeAbort(reason: "已断开当前 PokePod", report: false, rejectCode: nil)
        firmwareReady = false
        transportReady = false
        ble.forgetAndDisconnect()
        state = .connecting
        detail = "正在清除配对；写入确认后会自动重新连接"
    }

    func setLaunchAtLogin(_ enabled: Bool) {
        // A manual choice is final for automatic onboarding, even when macOS
        // requires the user to finish approval in System Settings.
        UserDefaults.standard.set(true, forKey: Self.loginItemDecisionKey)
        do {
            if enabled { try SMAppService.mainApp.register() }
            else { try SMAppService.mainApp.unregister() }
            launchAtLogin = enabled
        } catch {
            launchAtLogin = SMAppService.mainApp.status == .enabled
            showRecoverable("登录启动设置失败：\(error.localizedDescription)。可在菜单中再次切换。")
        }
    }

    func shutdown() {
        guard !isShuttingDown else { return }
        isShuttingDown = true
        timer?.invalidate()
        blackHoleDiscoveryTask?.cancel()
        blackHoleDiscoveryTask = nil
        safeAbort(reason: "应用退出", report: false, rejectCode: nil)
        ble.disconnect()
    }

    private func handle(_ signal: BLECentralSignal) {
        guard !isShuttingDown else { return }
        switch signal {
        case let .bluetoothUnavailable(message):
            safeAbort(reason: message, report: false, rejectCode: nil)
            bluetoothReady = false
            transportReady = false
            firmwareReady = false
            state = .setup
            detail = message
        case .scanning:
            bluetoothReady = true
            firmwareReady = false
            transportReady = false
            state = prerequisitesReady ? .connecting : .setup
            detail = prerequisitesReady ? "正在查找 PokePod" : prerequisiteMessage
            updateQuality(context: .disconnected)
        case let .connecting(name):
            deviceName = name
            state = prerequisitesReady ? .connecting : .setup
            detail = prerequisitesReady ? "正在建立安全连接" : prerequisiteMessage
        case .transportReady:
            transportReady = true
            updateQuality(context: .recent)
            if prerequisitesReady {
                ble.declareApplicationReady()
                state = .connecting
                detail = "蓝牙已订阅，等待 PokePod 就绪"
            } else {
                ble.reject(
                    sessionId: 0,
                    code: blackHoleReady
                        ? BLEVoiceRejectCode.accessibilityMissing.rawValue
                        : BLEVoiceRejectCode.blackHoleMissing.rawValue)
                state = .setup
                detail = prerequisiteMessage
            }
        case let .deviceInfo(info):
            deviceName = info.deviceId
            deviceQueueQuality = DeviceSendQueueQualityFormatter.summary(info)
        case let .ready(info):
            firmwareReady = true
            if let info {
                deviceName = info.deviceId
                deviceQueueQuality = DeviceSendQueueQualityFormatter.summary(info)
            }
            updateQuality(context: .recent)
            state = prerequisitesReady ? .ready : .setup
            detail = prerequisitesReady ? "按住 PokePod 的微信语音输入开始说话" : prerequisiteMessage
            configureFirstRunLoginItemIfNeeded()
        case let .sessionStarted(sessionId):
            guard prerequisitesReady, firmwareReady else {
                ble.reject(sessionId: sessionId, code: BLEVoiceRejectCode.applicationNotReady.rawValue)
                showError("运行环境尚未就绪")
                return
            }
            guard VoiceSessionStartCoordinator.start(
                sessionId: sessionId,
                now: monotonicNow,
                machine: &machine,
                executor: executor,
                prepareAudioStream: { [weak self] in self?.jitter.reset(sessionId: $0) },
                sendSessionReady: { [ble] in ble.declareSessionReady(sessionId: $0) },
                reject: { [ble] in ble.reject(
                    sessionId: $0,
                    code: BLEVoiceRejectCode.platformSetupFailed.rawValue) }) else { return }
            latestQuality = jitter.qualitySnapshot
            updateQuality(context: .active)
            state = .listening
            detail = "正在准备 120 ms 音频缓冲"
        case let .audio(frame):
            do {
                for output in try jitter.ingest(frame) {
                    let actions = machine.receive(
                        sessionId: frame.sessionId,
                        samples: output.samples,
                        now: monotonicNow)
                    guard executor.execute(actions) else {
                        safeAbort(
                            reason: "音频输出失败",
                            report: false,
                            rejectCode: BLEVoiceRejectCode.audioOutputFailed.rawValue)
                        return
                    }
                }
                latestQuality = jitter.qualitySnapshot
                updateQuality(context: .active)
                if case .streaming = machine.phase { detail = "正在向微信输入法传送语音" }
            } catch {
                safeAbort(
                    reason: "音频序列异常：\(error.localizedDescription)",
                    rejectCode: BLEVoiceRejectCode.audioSequenceInvalid.rawValue)
            }
        case let .sessionEnded(sessionId):
            switch completion.beginEnding(
                sessionId: sessionId,
                now: monotonicNow,
                machine: &machine,
                executor: executor) {
            case .accepted:
                detail = "正在排空尾部音频"
            case let .failed(failedSessionId):
                ble.reject(
                    sessionId: failedSessionId,
                    code: BLEVoiceRejectCode.audioOutputFailed.rawValue)
                applyCompletionPresentation(.aborted(
                    failedSessionId,
                    .executionFailure))
            case .ignored:
                break
            }
        case let .status(code):
            if code != 1 { detail = "PokePod 状态码 \(code)" }
        case let .sessionError(sessionId, code):
            guard VoiceSessionSignalPolicy.remoteErrorTargetsActive(
                reportedSessionId: sessionId,
                activeSessionId: machine.activeSessionId) else { return }
            safeAbort(reason: "PokePod 会话错误码 \(code)")
            if DeviceInfoRefreshPolicy.shouldRefresh(after: .sessionFailed) {
                ble.requestDeviceInfoRefresh()
            }
        case let .error(message):
            safeAbort(reason: message)
        case let .disconnected(message):
            safeAbort(reason: message, report: false, rejectCode: nil)
            updateQuality(context: .disconnected)
            firmwareReady = false
            transportReady = false
            state = prerequisitesReady ? .connecting : .setup
            detail = prerequisitesReady ? "连接中断，正在自动重连" : prerequisiteMessage
        }
    }

    private func tick() {
        let outcome = completion.tick(
            now: monotonicNow,
            machine: &machine,
            executor: executor,
            acknowledgeStop: { [ble] in ble.acknowledgeStop(sessionId: $0) })
        switch outcome {
        case .completed:
            applyCompletionPresentation(outcome)
            if DeviceInfoRefreshPolicy.shouldRefresh(after: .sessionCompleted) {
                ble.requestDeviceInfoRefresh()
            }
        case let .aborted(sessionId, reason):
            ble.reject(
                sessionId: sessionId,
                code: reason == .executionFailure
                    ? BLEVoiceRejectCode.audioOutputFailed.rawValue
                    : BLEVoiceRejectCode.sessionAborted.rawValue)
            applyCompletionPresentation(outcome)
            return
        case .none:
            break
        }
        if machine.isIdle {
            let recoveryWasPending = platform.input.hasPendingRecovery
            platform.input.retryPendingRecoveryIfNeeded(now: monotonicNow)
            if platform.input.hasPendingRecovery {
                state = .error
                detail = "原麦克风暂不可用；恢复记录已保留，将继续自动重试"
            } else if recoveryWasPending {
                state = firmwareReady && prerequisitesReady ? .ready : .setup
                detail = "原麦克风已恢复，可以继续使用"
            }
        }
        if state == .setup {
            let priorReady = prerequisitesReady
            refreshPrerequisites()
            if !priorReady, prerequisitesReady, transportReady {
                ble.declareApplicationReady()
                state = .connecting
                detail = "运行环境已就绪，等待 PokePod 确认"
            }
        }
    }

    private func applyCompletionPresentation(_ outcome: VoiceSessionCompletionOutcome) {
        if outcome != .none { updateQuality(context: .recent) }
        applyPresentationUpdate(VoiceSessionPresentationPolicy.completion(
            outcome,
            environmentReady: firmwareReady && prerequisitesReady))
    }

    private func applyPresentationUpdate(_ update: VoiceSessionPresentationUpdate) {
        switch update.state {
        case .unchanged:
            return
        case .ready:
            state = .ready
        case .setup:
            state = .setup
        }
        if let message = update.detail { detail = message }
    }

    private func userChangedInput() {
        safeAbort(reason: "检测到你手动切换了麦克风，已停止本次输入并保留你的选择")
    }

    private func safeAbort(
        reason: String,
        report: Bool = true,
        rejectCode: UInt16? = BLEVoiceRejectCode.sessionAborted.rawValue
    ) {
        latestQuality = jitter.qualitySnapshot ?? latestQuality
        updateQuality(context: .recent)
        let wirePolicy: VoiceSessionAbortWirePolicy
        if let rejectCode { wirePolicy = .reject(code: rejectCode) }
        else { wirePolicy = .silent }
        _ = VoiceSessionAbortCoordinator.abort(
            reason: reason,
            report: false,
            wirePolicy: wirePolicy,
            machine: &machine,
            completion: &completion,
            executor: executor,
            reject: { [ble] in ble.reject(sessionId: $0, code: $1) })
        if report { showError(reason) }
    }

    private func updateQuality(context: VoiceLinkQualityContext) {
        connectionQuality = VoiceLinkQualityFormatter.summary(
            latestQuality,
            context: context)
    }

    private func showError(_ message: String) {
        state = .error
        detail = message
    }

    private func showRecoverable(_ message: String) {
        state = prerequisitesReady && firmwareReady ? .ready : .setup
        detail = message
    }

    private func configureFirstRunLoginItemIfNeeded() {
        let serviceStatus = SMAppService.mainApp.status
        let decision = LoginItemFirstRunPolicy.decide(
            hasRecordedDecision: UserDefaults.standard.bool(forKey: Self.loginItemDecisionKey),
            prerequisitesReady: prerequisitesReady,
            firmwareReady: firmwareReady,
            serviceAlreadyConfigured: serviceStatus != .notRegistered)
        guard decision != .noAction else { return }

        // Persist before invoking ServiceManagement so a failure or crash can
        // never turn into repeated automatic re-enablement.
        UserDefaults.standard.set(true, forKey: Self.loginItemDecisionKey)
        switch decision {
        case .noAction:
            return
        case .markExistingConfiguration:
            launchAtLogin = serviceStatus == .enabled
            if serviceStatus == .requiresApproval {
                detail = "语音已就绪；登录启动等待你在系统设置中批准。"
            }
        case .registerOnceAndMark:
            do {
                try SMAppService.mainApp.register()
                launchAtLogin = SMAppService.mainApp.status == .enabled
                if !launchAtLogin {
                    detail = "语音已就绪；登录启动等待你在系统设置中批准。"
                }
            } catch {
                launchAtLogin = false
                showRecoverable("语音已就绪；登录启动未开启：\(error.localizedDescription)。可在菜单中手动开启。")
            }
        }
    }

    private func refreshPrerequisites() {
        blackHoleReady = platform.input.isBlackHoleAvailable
        accessibilityReady = platform.shortcut.isAuthorized
        bluetoothReady = bluetoothReady || transportReady || firmwareReady
        if !prerequisitesReady, state != .listening {
            state = .setup
            detail = prerequisiteMessage
        }
    }

    private var prerequisitesReady: Bool {
        bluetoothReady && blackHoleReady && accessibilityReady
    }

    private var prerequisiteMessage: String {
        if !bluetoothReady { return "请打开蓝牙并允许 PokePod Voice 使用蓝牙" }
        if !blackHoleReady {
            if blackHoleDiscoveryTask != nil { return "正在检查 BlackHole 安装包" }
            return blackHolePackage == nil
                ? "未找到安装包；点击“安装”会后台下载并打开系统安装器"
                : "已找到 BlackHole 安装包，点击“安装”完成系统安装"
        }
        if !accessibilityReady { return "请在辅助功能中打开 PokePod Voice，返回后会自动刷新" }
        return "准备完成"
    }

    private func installerDiscoveryRoots() -> [URL] {
        let home = URL(fileURLWithPath: NSHomeDirectory(), isDirectory: true)
        let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask)[0]
        return BlackHoleInstallPolicy.discoveryRoots(
            homeDirectory: home,
            applicationSupportDirectory: appSupport)
    }

    private func startBlackHoleDiscovery(force: Bool = false) {
        if !force, blackHoleDiscoveryStarted { return }
        guard blackHoleDiscoveryTask == nil else { return }
        blackHoleDiscoveryStarted = true
        let roots = installerDiscoveryRoots()
        let task = Task.detached(priority: .utility) {
            BlackHoleInstallPolicy.discoverPackage(in: roots)
        }
        blackHoleDiscoveryTask = task
        Task { @MainActor [weak self] in
            let package = await task.value
            guard let self, !self.isShuttingDown else { return }
            self.blackHolePackage = package
            self.blackHoleDiscoveryTask = nil
            self.refreshPrerequisites()
        }
    }

    private func rescanBlackHolePackage() async -> URL? {
        startBlackHoleDiscovery(force: true)
        guard let task = blackHoleDiscoveryTask else { return blackHolePackage }
        let package = await task.value
        guard !isShuttingDown else { return nil }
        blackHolePackage = package
        blackHoleDiscoveryTask = nil
        refreshPrerequisites()
        return package
    }

    private func obtainBlackHolePackage() async throws -> URL {
        if let local = await rescanBlackHolePackage() {
            try await verifyBlackHolePackage(local)
            return local
        }

        var request = URLRequest(url: BlackHoleInstallPolicy.latestReleaseAPIURL)
        request.setValue("PokePodVoice/1.0", forHTTPHeaderField: "User-Agent")
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw BlackHoleDownloadError.releaseLookupFailed
        }
        let release = try JSONDecoder().decode(GitHubRelease.self, from: data)
        let candidates = release.assets.compactMap { asset -> BlackHoleInstallPolicy.ReleaseAsset? in
            guard let url = URL(string: asset.browserDownloadURL) else { return nil }
            return .init(name: asset.name, downloadURL: url, digest: asset.digest)
        }
        let asset: BlackHoleInstallPolicy.ReleaseAsset
        if let githubAsset = BlackHoleInstallPolicy.selectReleaseAsset(from: candidates) {
            asset = githubAsset
        } else if let officialURL = BlackHoleInstallPolicy.officialPackageURL(forTag: release.tagName) {
            asset = .init(
                name: officialURL.lastPathComponent,
                downloadURL: officialURL,
                digest: nil)
        } else {
            throw BlackHoleDownloadError.twoChannelAssetMissing
        }
        guard BlackHoleInstallPolicy.isOfficialDownloadURL(asset.downloadURL),
              BlackHoleInstallPolicy.isSafePackageName(asset.name) else {
            throw BlackHoleDownloadError.untrustedDownloadSource
        }

        var downloadRequest = URLRequest(url: asset.downloadURL)
        downloadRequest.setValue("PokePodVoice/1.0", forHTTPHeaderField: "User-Agent")
        let (temporaryURL, downloadResponse) = try await URLSession.shared.download(for: downloadRequest)
        guard let http = downloadResponse as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw BlackHoleDownloadError.downloadFailed
        }
        let directory = FileManager.default.urls(
            for: .applicationSupportDirectory,
            in: .userDomainMask)[0]
            .appendingPathComponent("PokePod Voice/Installers", isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let destination = directory.appendingPathComponent(asset.name)
        try? FileManager.default.removeItem(at: destination)
        try FileManager.default.moveItem(at: temporaryURL, to: destination)

        if let digest = asset.digest {
            guard let expected = BlackHoleInstallPolicy.normalizedSHA256Digest(digest) else {
                try? FileManager.default.removeItem(at: destination)
                throw BlackHoleDownloadError.invalidDigest
            }
            let actual = try await Task.detached(priority: .utility) {
                try Self.sha256Hex(at: destination)
            }.value
            guard actual == expected else {
                try? FileManager.default.removeItem(at: destination)
                throw BlackHoleDownloadError.digestMismatch
            }
        }
        try await verifyBlackHolePackage(destination)
        return destination
    }

    private func verifyBlackHolePackage(_ package: URL) async throws {
        let roots = installerDiscoveryRoots()
        guard BlackHoleInstallPolicy.isPackagePathAllowed(package, within: roots) else {
            throw BlackHoleDownloadError.untrustedPackageLocation
        }
        let expectedPath = package.resolvingSymlinksInPath().standardizedFileURL.path
        let trusted = await Task.detached(priority: .utility) {
            Self.checkPackageSignature(at: package)
        }.value
        guard package.resolvingSymlinksInPath().standardizedFileURL.path == expectedPath else {
            throw BlackHoleDownloadError.packagePathChanged
        }
        guard trusted else { throw BlackHoleDownloadError.signatureVerificationFailed }
    }

    private nonisolated static func checkPackageSignature(at package: URL) -> Bool {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/pkgutil")
        process.arguments = ["--check-signature", package.path]
        let output = Pipe()
        let error = Pipe()
        process.standardOutput = output
        process.standardError = error
        do {
            try process.run()
        } catch {
            return false
        }
        let deadline = Date().addingTimeInterval(10)
        while process.isRunning {
            if Date() >= deadline {
                process.terminate()
                return false
            }
            Thread.sleep(forTimeInterval: 0.01)
        }
        let stdout = output.fileHandleForReading.readDataToEndOfFile()
        let stderr = error.fileHandleForReading.readDataToEndOfFile()
        guard process.terminationStatus == 0 else { return false }
        let combined = String(data: stdout + stderr, encoding: .utf8) ?? ""
        return BlackHoleInstallPolicy.signatureOutputIsTrusted(combined)
    }

    private nonisolated static func sha256Hex(at file: URL) throws -> String {
        SHA256.hash(data: try Data(contentsOf: file))
            .map { String(format: "%02x", $0) }.joined()
    }

    private func openSystemInstaller(_ package: URL) {
        let installer = URL(fileURLWithPath: "/System/Library/CoreServices/Installer.app")
        NSWorkspace.shared.open(
            [package],
            withApplicationAt: installer,
            configuration: NSWorkspace.OpenConfiguration()) { [weak self] _, error in
                guard let error else { return }
                Task { @MainActor in
                    self?.showRecoverable("无法打开系统安装器：\(error.localizedDescription)")
                }
            }
        detail = "已打开系统安装器；确认管理员授权后会自动刷新 BlackHole 状态"
    }

    private struct GitHubRelease: Decodable {
        let tagName: String
        let assets: [GitHubAsset]

        enum CodingKeys: String, CodingKey {
            case tagName = "tag_name"
            case assets
        }
    }

    private struct GitHubAsset: Decodable {
        let name: String
        let browserDownloadURL: String
        let digest: String?

        enum CodingKeys: String, CodingKey {
            case name
            case browserDownloadURL = "browser_download_url"
            case digest
        }
    }

    private enum BlackHoleDownloadError: LocalizedError {
        case releaseLookupFailed
        case twoChannelAssetMissing
        case downloadFailed
        case digestMismatch
        case invalidDigest
        case untrustedDownloadSource
        case signatureVerificationFailed
        case untrustedPackageLocation
        case packagePathChanged

        var errorDescription: String? {
            switch self {
            case .releaseLookupFailed: return "无法读取官方最新版本信息"
            case .twoChannelAssetMissing: return "官方版本没有找到 BlackHole 2ch 安装包"
            case .downloadFailed: return "官方安装包下载失败"
            case .digestMismatch: return "安装包校验失败，已删除不完整文件"
            case .invalidDigest: return "官方安装包摘要格式无效，已拒绝安装"
            case .untrustedDownloadSource: return "安装包来源或文件名不受信任，已拒绝安装"
            case .signatureVerificationFailed: return "安装包签名或开发者身份校验失败，已拒绝安装"
            case .untrustedPackageLocation: return "安装包位置不受信任，已拒绝打开"
            case .packagePathChanged: return "安装包路径在校验期间发生变化，已拒绝打开"
            }
        }
    }

    private var monotonicNow: TimeInterval { ProcessInfo.processInfo.systemUptime }
}
