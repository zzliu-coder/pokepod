import AudioToolbox
import CoreAudio
import Foundation
import PokePodVoiceCore

enum VoicePlatformError: LocalizedError {
    case blackHoleMissing
    case defaultInputStillBlackHole
    case accessibilityMissing
    case coreAudio(String, OSStatus)
    case audioSinkUnavailable

    var errorDescription: String? {
        switch self {
        case .blackHoleMissing: return "未检测到 BlackHole 2ch"
        case .defaultInputStillBlackHole:
            return "默认麦克风仍是 BlackHole，正在等待原麦克风恢复"
        case .accessibilityMissing: return "PokePod Voice 尚未获得辅助功能权限"
        case let .coreAudio(operation, status): return "\(operation)失败（\(status)）"
        case .audioSinkUnavailable: return "无线音频输出尚未准备好"
        }
    }
}

struct AudioDeviceDescriptor: Equatable {
    let id: AudioDeviceID
    let uid: String
    let name: String
}

final class DefaultInputController {
    private enum Keys {
        static let recoveryNeeded = "PokePodVoice.RecoveryNeeded"
        static let originalInputUID = "PokePodVoice.OriginalInputUID"
        static let blackHoleUID = "PokePodVoice.BlackHoleUID"
    }

    private let defaults: UserDefaults
    private(set) var blackHole: AudioDeviceDescriptor?
    private var originalUID: String?
    private var userOverrodeInput = false
    private var lastRecoveryAttemptAt = -TimeInterval.infinity
    private var listener: AudioObjectPropertyListenerBlock?
    var onUserOverride: (() -> Void)?

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        blackHole = try? Self.findBlackHole()
        installDefaultInputListener()
    }

    deinit { removeDefaultInputListener() }

    var isBlackHoleAvailable: Bool {
        refreshBlackHole()
        return blackHole != nil
    }

    var hasPendingRecovery: Bool { defaults.bool(forKey: Keys.recoveryNeeded) }

    func saveCurrentInput() throws {
        let current = try Self.defaultInput()
        refreshBlackHole()
        let pendingOriginal = originalUID ?? defaults.string(forKey: Keys.originalInputUID)
        let knownBlackHole = blackHole?.uid ?? defaults.string(forKey: Keys.blackHoleUID)
        switch InputRecoveryPolicy.decideOriginalCapture(
            currentUID: current.uid,
            blackHoleUID: knownBlackHole,
            pendingOriginalUID: pendingOriginal) {
        case let .rejectKeepingRecovery(storedOriginal):
            originalUID = storedOriginal
            throw VoicePlatformError.defaultInputStillBlackHole
        case let .capture(uid):
            // A user-selected non-BlackHole input supersedes any stale marker.
            clearRecoveryMarker()
            originalUID = uid
        }
        userOverrodeInput = false
        defaults.set(originalUID, forKey: Keys.originalInputUID)
        defaults.set(true, forKey: Keys.recoveryNeeded)
        defaults.synchronize()
    }

    func switchToBlackHole() throws {
        refreshBlackHole()
        guard let blackHole else { throw VoicePlatformError.blackHoleMissing }
        defaults.set(blackHole.uid, forKey: Keys.blackHoleUID)
        try Self.setDefaultInput(blackHole.id)
    }

    func restoreIfOwned() {
        guard defaults.bool(forKey: Keys.recoveryNeeded) else { return }
        if userOverrodeInput {
            clearRecoveryMarker()
            return
        }
        let descriptors = (try? Self.devices()) ?? []
        let decision = InputRecoveryPolicy.decide(
            recoveryNeeded: true,
            currentUID: (try? Self.defaultInput())?.uid,
            blackHoleUID: defaults.string(forKey: Keys.blackHoleUID) ?? blackHole?.uid,
            originalUID: originalUID ?? defaults.string(forKey: Keys.originalInputUID),
            availableUIDs: Set(descriptors.map(\.uid)))
        switch decision {
        case .noAction, .retryLater:
            return
        case .respectCurrentSelection:
            if InputRecoveryPolicy.markerEffect(
                after: decision,
                restorationSucceeded: false) == .clear {
                clearRecoveryMarker()
            }
        case let .restoreOriginal(uid):
            guard let original = descriptors.first(where: { $0.uid == uid }) else { return }
            let restored: Bool
            do {
                try Self.setDefaultInput(original.id)
                restored = true
            } catch {
                restored = false
            }
            if InputRecoveryPolicy.markerEffect(
                after: decision,
                restorationSucceeded: restored) == .clear {
                clearRecoveryMarker()
            }
        }
    }

    func repairAfterPreviousCrash() {
        guard defaults.bool(forKey: Keys.recoveryNeeded) else { return }
        originalUID = defaults.string(forKey: Keys.originalInputUID)
        restoreIfOwned()
        lastRecoveryAttemptAt = ProcessInfo.processInfo.systemUptime
    }

    func retryPendingRecoveryIfNeeded(now: TimeInterval) {
        guard defaults.bool(forKey: Keys.recoveryNeeded),
              now - lastRecoveryAttemptAt >= 2 else { return }
        lastRecoveryAttemptAt = now
        originalUID = originalUID ?? defaults.string(forKey: Keys.originalInputUID)
        restoreIfOwned()
    }

    private func refreshBlackHole() {
        blackHole = try? Self.findBlackHole()
    }

    private func clearRecoveryMarker() {
        originalUID = nil
        userOverrodeInput = false
        defaults.removeObject(forKey: Keys.originalInputUID)
        defaults.removeObject(forKey: Keys.blackHoleUID)
        defaults.set(false, forKey: Keys.recoveryNeeded)
        defaults.synchronize()
    }

    private func installDefaultInputListener() {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let callback: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
            guard let self,
                  self.defaults.bool(forKey: Keys.recoveryNeeded),
                  let current = try? Self.defaultInput(),
                  let expected = self.defaults.string(forKey: Keys.blackHoleUID),
                  current.uid != expected else { return }
            self.userOverrodeInput = true
            self.onUserOverride?()
        }
        listener = callback
        AudioObjectAddPropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject), &address, .main, callback)
    }

    private func removeDefaultInputListener() {
        guard let listener else { return }
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        AudioObjectRemovePropertyListenerBlock(
            AudioObjectID(kAudioObjectSystemObject), &address, .main, listener)
    }

    static func findBlackHole() throws -> AudioDeviceDescriptor? {
        try devices().first {
            let normalized = $0.name.lowercased()
            return normalized.contains("blackhole") && normalized.contains("2ch")
        }
    }

    static func defaultInput() throws -> AudioDeviceDescriptor {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var device = AudioDeviceID(0)
        var size = UInt32(MemoryLayout<AudioDeviceID>.size)
        let status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &device)
        guard status == noErr else { throw VoicePlatformError.coreAudio("读取默认输入", status) }
        guard let result = try devices().first(where: { $0.id == device }) else {
            throw VoicePlatformError.audioSinkUnavailable
        }
        return result
    }

    static func devices() throws -> [AudioDeviceDescriptor] {
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size)
        guard status == noErr else { throw VoicePlatformError.coreAudio("读取音频设备数量", status) }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil, &size, &ids)
        guard status == noErr else { throw VoicePlatformError.coreAudio("读取音频设备", status) }
        return try ids.map { id in
            AudioDeviceDescriptor(
                id: id,
                uid: try stringProperty(id, selector: kAudioDevicePropertyDeviceUID),
                name: try stringProperty(id, selector: kAudioObjectPropertyName))
        }
    }

    static func setDefaultInput(_ device: AudioDeviceID) throws {
        var value = device
        var address = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDefaultInputDevice,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        let status = AudioObjectSetPropertyData(
            AudioObjectID(kAudioObjectSystemObject), &address, 0, nil,
            UInt32(MemoryLayout<AudioDeviceID>.size), &value)
        guard status == noErr else { throw VoicePlatformError.coreAudio("切换默认输入", status) }
    }

    private static func stringProperty(
        _ object: AudioObjectID,
        selector: AudioObjectPropertySelector
    ) throws -> String {
        var address = AudioObjectPropertyAddress(
            mSelector: selector,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let status = AudioObjectGetPropertyData(object, &address, 0, nil, &size, &value)
        guard status == noErr else { throw VoicePlatformError.coreAudio("读取音频设备属性", status) }
        guard let value else { throw VoicePlatformError.audioSinkUnavailable }
        return value.takeUnretainedValue() as String
    }
}

final class BlackHoleAudioSink {
    private let lock = NSLock()
    private var pending = [Int16]()
    private var readOffset = 0
    private var audioUnit: AudioUnit?

    func start(deviceID: AudioDeviceID) throws {
        stop()
        var description = AudioComponentDescription(
            componentType: kAudioUnitType_Output,
            componentSubType: kAudioUnitSubType_HALOutput,
            componentManufacturer: kAudioUnitManufacturer_Apple,
            componentFlags: 0,
            componentFlagsMask: 0)
        guard let component = AudioComponentFindNext(nil, &description) else {
            throw VoicePlatformError.audioSinkUnavailable
        }
        var unit: AudioUnit?
        var status = AudioComponentInstanceNew(component, &unit)
        guard status == noErr, let unit else {
            throw VoicePlatformError.coreAudio("创建 BlackHole 输出", status)
        }
        audioUnit = unit
        var device = deviceID
        status = AudioUnitSetProperty(
            unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
            &device, UInt32(MemoryLayout<AudioDeviceID>.size))
        try check(status, "选择 BlackHole")

        var format = AudioStreamBasicDescription(
            mSampleRate: 16_000,
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
            mBytesPerPacket: 2,
            mFramesPerPacket: 1,
            mBytesPerFrame: 2,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 16,
            mReserved: 0)
        status = AudioUnitSetProperty(
            unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
            &format, UInt32(MemoryLayout<AudioStreamBasicDescription>.size))
        try check(status, "设置 16 kHz 音频格式")

        var callback = AURenderCallbackStruct(
            inputProc: { refCon, _, _, _, frameCount, ioData in
                guard let ioData else { return noErr }
                let sink = Unmanaged<BlackHoleAudioSink>.fromOpaque(refCon).takeUnretainedValue()
                sink.render(frameCount: Int(frameCount), buffers: ioData)
                return noErr
            },
            inputProcRefCon: Unmanaged.passUnretained(self).toOpaque())
        status = AudioUnitSetProperty(
            unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
            &callback, UInt32(MemoryLayout<AURenderCallbackStruct>.size))
        try check(status, "注册音频回调")
        try check(AudioUnitInitialize(unit), "初始化 BlackHole 输出")
        try check(AudioOutputUnitStart(unit), "启动 BlackHole 输出")
    }

    func append(_ samples: [Int16]) throws {
        guard audioUnit != nil else { throw VoicePlatformError.audioSinkUnavailable }
        lock.lock()
        if readOffset > 8_192 {
            pending.removeFirst(readOffset)
            readOffset = 0
        }
        pending.append(contentsOf: samples)
        lock.unlock()
    }

    func stop() {
        if let audioUnit {
            AudioOutputUnitStop(audioUnit)
            AudioUnitUninitialize(audioUnit)
            AudioComponentInstanceDispose(audioUnit)
        }
        audioUnit = nil
        lock.lock()
        pending.removeAll(keepingCapacity: false)
        readOffset = 0
        lock.unlock()
    }

    private func render(frameCount: Int, buffers: UnsafeMutablePointer<AudioBufferList>) {
        let list = UnsafeMutableAudioBufferListPointer(buffers)
        lock.lock()
        let available = max(0, pending.count - readOffset)
        let count = min(frameCount, available)
        for buffer in list {
            guard let data = buffer.mData else { continue }
            let capacity = Int(buffer.mDataByteSize) / MemoryLayout<Int16>.size
            let output = data.bindMemory(to: Int16.self, capacity: capacity)
            let copyCount = min(count, capacity)
            if copyCount > 0 {
                pending.withUnsafeBufferPointer { source in
                    output.update(from: source.baseAddress!.advanced(by: readOffset), count: copyCount)
                }
            }
            if copyCount < capacity {
                output.advanced(by: copyCount).initialize(repeating: 0, count: capacity - copyCount)
            }
        }
        readOffset += count
        lock.unlock()
    }

    private func check(_ status: OSStatus, _ operation: String) throws {
        guard status == noErr else {
            stop()
            throw VoicePlatformError.coreAudio(operation, status)
        }
    }
}
