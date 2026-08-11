import ApplicationServices
import Foundation
import PokePodVoiceCore

final class ShortcutController {
    private(set) var isHeld = false
    private let eventSource = CGEventSource(stateID: .hidSystemState)

    var isAuthorized: Bool { AXIsProcessTrusted() }

    func requestAuthorization() {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
    }

    func pressOptionZ() throws {
        guard isAuthorized else { throw VoicePlatformError.accessibilityMissing }
        guard !isHeld else { return }

        guard post(OptionZHoldSequence.press[0]) else {
            throw VoicePlatformError.audioSinkUnavailable
        }
        guard post(OptionZHoldSequence.press[1]) else {
            _ = post(OptionZHoldSequence.release[1])
            throw VoicePlatformError.audioSinkUnavailable
        }
        isHeld = true
    }

    func releaseOptionZ() {
        guard isHeld else { return }
        for transition in OptionZHoldSequence.release {
            _ = post(transition)
        }
        isHeld = false
    }

    private func post(_ transition: KeyboardShortcutTransition) -> Bool {
        guard let event = CGEvent(
            keyboardEventSource: eventSource,
            virtualKey: CGKeyCode(transition.virtualKey),
            keyDown: transition.keyDown
        ) else { return false }
        event.flags = transition.alternateDown ? .maskAlternate : []
        event.setIntegerValueField(.keyboardEventAutorepeat, value: 0)
        event.post(tap: .cghidEventTap)
        return true
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
