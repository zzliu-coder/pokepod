import AppKit
import Combine
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
        refreshPrerequisites()
    }

    func openBlackHoleInstaller() {
        guard let url = URL(string: "https://github.com/ExistentialAudio/BlackHole") else { return }
        NSWorkspace.shared.open(url)
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
        if !blackHoleReady { return "请先安装 BlackHole 2ch" }
        if !accessibilityReady { return "请允许辅助功能，以便按住和释放 Option-Z" }
        return "准备完成"
    }

    private var monotonicNow: TimeInterval { ProcessInfo.processInfo.systemUptime }
}
