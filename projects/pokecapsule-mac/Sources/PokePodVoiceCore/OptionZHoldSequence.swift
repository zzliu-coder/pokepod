import Foundation

public struct KeyboardShortcutTransition: Equatable, Sendable {
    public let virtualKey: UInt16
    public let keyDown: Bool
    public let alternateDown: Bool
    public let leftAlternateDown: Bool

    public init(
        virtualKey: UInt16,
        keyDown: Bool,
        alternateDown: Bool,
        leftAlternateDown: Bool
    ) {
        self.virtualKey = virtualKey
        self.keyDown = keyDown
        self.alternateDown = alternateDown
        self.leftAlternateDown = leftAlternateDown
    }
}

public enum OptionZHoldSequence {
    public static let leftOptionKey: UInt16 = 58
    public static let zKey: UInt16 = 6
    // IOKit/hidsystem/IOLLEvent.h: NX_DEVICELALTKEYMASK.
    public static let leftAlternateDeviceMask: UInt64 = 0x0000_0020

    /// macOS global shortcuts observe the modifier as a physical key transition.
    /// Attaching only the alternate flag to Z produces the printable character Ω.
    public static let press: [KeyboardShortcutTransition] = [
        .init(
            virtualKey: leftOptionKey,
            keyDown: true,
            alternateDown: true,
            leftAlternateDown: true),
        .init(
            virtualKey: zKey,
            keyDown: true,
            alternateDown: true,
            leftAlternateDown: true),
    ]

    public static let release: [KeyboardShortcutTransition] = [
        .init(
            virtualKey: zKey,
            keyDown: false,
            alternateDown: true,
            leftAlternateDown: true),
        .init(
            virtualKey: leftOptionKey,
            keyDown: false,
            alternateDown: false,
            leftAlternateDown: false),
    ]
}
