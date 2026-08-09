import CoreBluetooth
import Foundation
import PokePodVoiceCore

enum BLECentralSignal {
    case bluetoothUnavailable(String)
    case scanning
    case connecting(String)
    case transportReady(maximumWriteLength: Int)
    case deviceInfo(BLEVoiceDeviceInfo)
    case ready(BLEVoiceDeviceInfo?)
    case sessionStarted(UInt32)
    case audio(BLEVoiceAudioFrame)
    case sessionEnded(UInt32)
    case status(UInt16)
    case sessionError(sessionId: UInt32, code: UInt16)
    case error(String)
    case disconnected(String)
}

protocol BLECentralControlling: AnyObject {
    var onSignal: ((BLECentralSignal) -> Void)? { get set }
    func start()
    func reconnect()
    func disconnect()
    func forgetAndDisconnect()
    func declareApplicationReady()
    func declareSessionReady(sessionId: UInt32)
    func reject(sessionId: UInt32, code: UInt16)
    func acknowledgeStop(sessionId: UInt32)
    func requestDeviceInfoRefresh()
}

private enum CommandWritePurpose {
    case regular
    case forgetBond
}

final class BLECentralAdapter: NSObject, BLECentralControlling {
    var onSignal: ((BLECentralSignal) -> Void)?
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var commandCharacteristic: CBCharacteristic?
    private var eventCharacteristic: CBCharacteristic?
    private var audioCharacteristic: CBCharacteristic?
    private var infoCharacteristic: CBCharacteristic?
    private var subscribedEvent = false
    private var subscribedAudio = false
    private var deviceInfo: BLEVoiceDeviceInfo?
    private var receivedFirstAudio = false
    private var shouldReconnect = true
    private var commandWritePurposes = [CommandWritePurpose]()
    private var forgetFlow = ForgetReconnectStateMachine()
    private var forgetTimeoutWorkItem: DispatchWorkItem?

    override init() {
        super.init()
        central = CBCentralManager(
            delegate: self,
            queue: .main,
            options: [CBCentralManagerOptionShowPowerAlertKey: true])
    }

    func start() {
        shouldReconnect = true
        if central.state == .poweredOn { scan() }
    }

    func reconnect() {
        cancelForgetFlow()
        shouldReconnect = true
        central.stopScan()
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
        } else {
            clearConnection()
            if central.state == .poweredOn { scan() }
        }
    }

    func disconnect() {
        shouldReconnect = false
        cancelForgetFlow()
        central.stopScan()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        clearConnection()
    }

    func forgetAndDisconnect() {
        let actions = forgetFlow.begin(now: monotonicNow)
        guard !actions.isEmpty else { return }
        shouldReconnect = false
        central.stopScan()
        performForget(actions)
        scheduleForgetTimeout()
    }

    func declareApplicationReady() {
        send(BLEVoiceCommand(type: .ready, sessionId: 0))
    }

    func declareSessionReady(sessionId: UInt32) {
        guard sessionId != 0 else { return }
        send(BLEVoiceCommand(type: .ready, sessionId: sessionId))
    }

    func reject(sessionId: UInt32, code: UInt16) {
        send(BLEVoiceCommand(type: .reject, sessionId: sessionId, code: code))
    }

    func acknowledgeStop(sessionId: UInt32) {
        send(BLEVoiceCommand(type: .stopAcknowledged, sessionId: sessionId))
    }

    func requestDeviceInfoRefresh() {
        guard let peripheral, let infoCharacteristic else { return }
        peripheral.readValue(for: infoCharacteristic)
    }

    private func scan() {
        guard central.state == .poweredOn, !central.isScanning else { return }
        onSignal?(.scanning)
        central.scanForPeripherals(
            withServices: [CBUUID(string: BLEVoiceUUID.service)],
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    @discardableResult
    private func send(
        _ command: BLEVoiceCommand,
        purpose: CommandWritePurpose = .regular
    ) -> Bool {
        guard let peripheral, let characteristic = commandCharacteristic else { return false }
        commandWritePurposes.append(purpose)
        peripheral.writeValue(command.encoded(), for: characteristic, type: .withResponse)
        return true
    }

    private func scheduleForgetTimeout() {
        forgetTimeoutWorkItem?.cancel()
        let item = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.performForget(self.forgetFlow.tick(now: self.monotonicNow))
        }
        forgetTimeoutWorkItem = item
        DispatchQueue.main.asyncAfter(
            deadline: .now() + forgetFlow.writeTimeoutSeconds,
            execute: item)
    }

    private func cancelForgetFlow() {
        forgetTimeoutWorkItem?.cancel()
        forgetTimeoutWorkItem = nil
        forgetFlow.cancel()
    }

    private func performForget(_ actions: [ForgetReconnectAction]) {
        for action in actions {
            switch action {
            case .sendForget:
                _ = send(BLEVoiceCommand(
                    type: .reject,
                    sessionId: 0,
                    code: BLEVoiceRejectCode.forgetBond.rawValue),
                    purpose: .forgetBond)
            case .disconnect:
                forgetTimeoutWorkItem?.cancel()
                forgetTimeoutWorkItem = nil
                central.stopScan()
                if let peripheral {
                    central.cancelPeripheralConnection(peripheral)
                } else {
                    clearConnection()
                    performForget(forgetFlow.disconnected())
                }
            case .reconnect:
                shouldReconnect = true
                if central.state == .poweredOn { scan() }
            }
        }
    }

    private func finishDiscoveryIfPossible() {
        guard let peripheral,
              commandCharacteristic != nil,
              subscribedEvent,
              subscribedAudio else { return }
        let maximumWrite = peripheral.maximumWriteValueLength(for: .withoutResponse)
        do {
            try BLEHandshakePolicy.validate(maximumWriteValueLength: maximumWrite)
        } catch {
            send(BLEVoiceCommand(
                type: .reject, sessionId: 0,
                code: BLEVoiceRejectCode.mtuProxyTooSmall.rawValue))
            onSignal?(.error("蓝牙连接质量不足（可写 \(maximumWrite) B，需要至少 \(BLEHandshakePolicy.minimumWritePayload) B）"))
            return
        }
        if let infoCharacteristic { peripheral.readValue(for: infoCharacteristic) }
        onSignal?(.transportReady(maximumWriteLength: maximumWrite))
    }

    private func clearConnection() {
        peripheral?.delegate = nil
        peripheral = nil
        commandCharacteristic = nil
        eventCharacteristic = nil
        audioCharacteristic = nil
        infoCharacteristic = nil
        subscribedEvent = false
        subscribedAudio = false
        receivedFirstAudio = false
        deviceInfo = nil
        commandWritePurposes.removeAll(keepingCapacity: true)
    }

    private var monotonicNow: TimeInterval { ProcessInfo.processInfo.systemUptime }
}

extension BLECentralAdapter: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            if shouldReconnect { scan() }
        case .poweredOff:
            onSignal?(.bluetoothUnavailable("蓝牙已关闭"))
        case .unauthorized:
            onSignal?(.bluetoothUnavailable("PokePod Voice 没有蓝牙权限"))
        case .unsupported:
            onSignal?(.bluetoothUnavailable("这台 Mac 不支持蓝牙低功耗"))
        case .resetting:
            onSignal?(.bluetoothUnavailable("蓝牙正在重置"))
        case .unknown:
            onSignal?(.bluetoothUnavailable("正在检查蓝牙"))
        @unknown default:
            onSignal?(.bluetoothUnavailable("未知蓝牙状态"))
        }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        onSignal?(.connecting(peripheral.name ?? "PokePod"))
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        peripheral.discoverServices([CBUUID(string: BLEVoiceUUID.service)])
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        clearConnection()
        onSignal?(.disconnected(error?.localizedDescription ?? "连接失败"))
        if shouldReconnect { scan() }
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        forgetTimeoutWorkItem?.cancel()
        forgetTimeoutWorkItem = nil
        let forgetActions = forgetFlow.disconnected()
        clearConnection()
        onSignal?(.disconnected(error?.localizedDescription ?? "连接已断开"))
        if forgetActions.isEmpty {
            if shouldReconnect { scan() }
        } else {
            performForget(forgetActions)
        }
    }
}

extension BLECentralAdapter: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { onSignal?(.error(error.localizedDescription)); return }
        guard let service = peripheral.services?.first(where: {
            $0.uuid == CBUUID(string: BLEVoiceUUID.service)
        }) else {
            onSignal?(.error("PokePod 缺少 BLE Voice v1 服务"))
            return
        }
        peripheral.discoverCharacteristics([
            CBUUID(string: BLEVoiceUUID.deviceInfo),
            CBUUID(string: BLEVoiceUUID.command),
            CBUUID(string: BLEVoiceUUID.event),
            CBUUID(string: BLEVoiceUUID.audio)
        ], for: service)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error { onSignal?(.error(error.localizedDescription)); return }
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid.uuidString.uppercased() {
            case BLEVoiceUUID.deviceInfo: infoCharacteristic = characteristic
            case BLEVoiceUUID.command: commandCharacteristic = characteristic
            case BLEVoiceUUID.event:
                eventCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            case BLEVoiceUUID.audio:
                audioCharacteristic = characteristic
                peripheral.setNotifyValue(true, for: characteristic)
            default: break
            }
        }
        guard commandCharacteristic != nil, eventCharacteristic != nil, audioCharacteristic != nil else {
            onSignal?(.error("PokePod BLE Voice v1 特征不完整"))
            return
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error { onSignal?(.error(error.localizedDescription)); return }
        if characteristic.uuid == CBUUID(string: BLEVoiceUUID.event) {
            subscribedEvent = characteristic.isNotifying
        } else if characteristic.uuid == CBUUID(string: BLEVoiceUUID.audio) {
            subscribedAudio = characteristic.isNotifying
        }
        finishDiscoveryIfPossible()
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error { onSignal?(.error(error.localizedDescription)); return }
        guard let value = characteristic.value else { return }
        do {
            if characteristic.uuid == CBUUID(string: BLEVoiceUUID.deviceInfo) {
                let info = try BLEVoiceDeviceInfo.decode(value)
                deviceInfo = info
                onSignal?(.deviceInfo(info))
            } else if characteristic.uuid == CBUUID(string: BLEVoiceUUID.event) {
                let event = try BLEVoiceEvent.decode(value)
                switch event.type {
                case .sessionStart: onSignal?(.sessionStarted(event.sessionId))
                case .sessionEnd: onSignal?(.sessionEnded(event.sessionId))
                case .status:
                    onSignal?(.status(event.code))
                    do {
                        if try BLEHandshakePolicy.validate(statusCode: event.code) {
                            onSignal?(.ready(deviceInfo))
                        }
                    } catch {
                        onSignal?(.error("PokePod 报告 ATT MTU 小于 185"))
                    }
                case .error:
                    if event.sessionId == 0 {
                        onSignal?(.error("PokePod 连接错误码 \(event.code)"))
                    } else {
                        onSignal?(.sessionError(
                            sessionId: event.sessionId,
                            code: event.code))
                    }
                }
            } else if characteristic.uuid == CBUUID(string: BLEVoiceUUID.audio) {
                let frame = try BLEVoiceAudioFrame.decode(value)
                if !receivedFirstAudio {
                    receivedFirstAudio = true
                    try BLEHandshakePolicy.validateFirstAudioNotification(length: value.count)
                }
                onSignal?(.audio(frame))
            }
        } catch {
            onSignal?(.error("BLE 数据帧无效：\(error.localizedDescription)"))
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        let purpose = commandWritePurposes.isEmpty
            ? CommandWritePurpose.regular
            : commandWritePurposes.removeFirst()
        if let error { onSignal?(.error("发送命令失败：\(error.localizedDescription)")) }
        guard characteristic.uuid == CBUUID(string: BLEVoiceUUID.command),
              purpose == .forgetBond,
              forgetFlow.isAwaitingWrite else { return }
        forgetTimeoutWorkItem?.cancel()
        forgetTimeoutWorkItem = nil
        // A callback (success or error) proves CoreBluetooth finished this
        // with-response write. Disconnect only now; timeout covers no callback.
        performForget(forgetFlow.writeCompleted())
    }
}
