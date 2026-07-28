import Foundation

public enum TranscriptionSanity {
    public static let minimumAutomaticDurationMs = 2_000

    public static func issue(text: String, durationMs: Int) -> String? {
        if durationMs < minimumAutomaticDurationMs {
            return "录音不足 2 秒，已保留音频，不进行自动校对"
        }
        let characters = text.filter { !$0.isWhitespace }.count
        let limit = max(24, Int(ceil(Double(durationMs) / 1_000 * 10)))
        if characters > limit {
            return "\(max(1, Int(round(Double(durationMs) / 1_000)))) 秒音频生成 \(characters) 字，疑似转写幻觉"
        }
        return nil
    }

    public static func isPlausible(text: String, durationMs: Int) -> Bool {
        issue(text: text, durationMs: durationMs) == nil
    }
}
