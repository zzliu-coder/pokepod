import ApplicationServices
import Foundation
import PokePodVoiceCore

final class ShortcutController {
    private(set) var isHeld = false

    var isAuthorized: Bool { AXIsProcessTrusted() }

    func requestAuthorization() {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
    }

    func pressOptionZ() throws {
        guard isAuthorized else { throw VoicePlatformError.accessibilityMissing }
        guard !isHeld else { return }
        guard let event = CGEvent(keyboardEventSource: nil, virtualKey: 6, keyDown: true) else {
            throw VoicePlatformError.audioSinkUnavailable
        }
        event.flags = .maskAlternate
        event.post(tap: .cghidEventTap)
        isHeld = true
    }

    func releaseOptionZ() {
        guard isHeld else { return }
        let event = CGEvent(keyboardEventSource: nil, virtualKey: 6, keyDown: false)
        event?.flags = .maskAlternate
        event?.post(tap: .cghidEventTap)
        isHeld = false
    }
}

final class MacVoicePlatform: VoicePlatformAdapter {
    let input: DefaultInputController
    let sink: BlackHoleAudioSink
    let shortcut: ShortcutController

    init(
        input: DefaultInputController = .init(),
        sink: BlackHoleAudioSink = .init(),
        shortcut: ShortcutController = .init()
    ) {
        self.input = input
        self.sink = sink
        self.shortcut = shortcut
    }

    func saveDefaultInput() throws { try input.saveCurrentInput() }
    func switchToBlackHole() throws { try input.switchToBlackHole() }
    func startAudioSink() throws {
        guard let device = input.blackHole else { throw VoicePlatformError.blackHoleMissing }
        try sink.start(deviceID: device.id)
    }
    func write(samples: [Int16]) throws { try sink.append(samples) }
    func shortcutDown() throws { try shortcut.pressOptionZ() }
    func shortcutUp() { shortcut.releaseOptionZ() }
    func stopAudioSink() { sink.stop() }
    func restoreDefaultInput() { input.restoreIfOwned() }
}
