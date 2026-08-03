import PokeCapsuleCore
import SwiftUI

struct TextPanel: View {
    let title: String
    let text: String
    var actionTitle: String?
    var action: () -> Void

    init(title: String, text: String, actionTitle: String? = nil,
         action: @escaping () -> Void = {}) {
        self.title = title
        self.text = text
        self.actionTitle = actionTitle
        self.action = action
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                if let actionTitle {
                    Button(actionTitle, action: action).buttonStyle(.borderless)
                }
            }
            Text(text)
                .font(.title3)
                .lineSpacing(5)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(20)
        .background(Color.pokeSurface, in: RoundedRectangle(cornerRadius: 14))
    }
}

struct PlaybackRow: View {
    @EnvironmentObject private var model: AppModel
    let record: CapsuleRecord

    var body: some View {
        HStack(spacing: 14) {
            Button { model.play(record) } label: {
                Image(systemName: "play.fill").frame(width: 22, height: 22)
            }
            .buttonStyle(.borderedProminent)
            .accessibilityLabel("播放录音")
            HStack(spacing: 3) {
                ForEach([11, 18, 26, 15, 22, 30, 17, 12], id: \.self) { height in
                    Capsule()
                        .fill(Color.secondary.opacity(0.55))
                        .frame(width: 3, height: CGFloat(height))
                }
            }
            Text(duration(record.processing?.durationMs))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
            Spacer()
        }
        .padding(.vertical, 4)
    }

    private func duration(_ milliseconds: Int?) -> String {
        guard let milliseconds else { return "时长未知" }
        return String(format: "%d:%02d", milliseconds / 60_000,
                      (milliseconds / 1_000) % 60)
    }
}

struct VersionText: View {
    let title: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(.secondary)
            Text(text).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

struct StatusBadge: View {
    let text: String
    let isError: Bool

    var body: some View {
        Text(text)
            .font(.caption.weight(.semibold))
            .foregroundStyle(isError ? Color.pokeError : Color.pokeAccent)
            .padding(.horizontal, 11)
            .padding(.vertical, 6)
            .background(isError ? Color.pokeErrorSoft : Color.pokeAccentSoft, in: Capsule())
    }
}

struct NoticePanel: View {
    let title: String
    let message: String
    let isError: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.headline)
            Text(message).foregroundStyle(.secondary)
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(isError ? Color.pokeErrorSoft : Color.pokeAccentSoft,
                    in: RoundedRectangle(cornerRadius: 14))
    }
}

struct PlaceholderView: View {
    let title: String
    let systemImage: String

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: systemImage).font(.system(size: 38))
            Text(title).font(.headline)
        }
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

extension Color {
    static let pokeAccent = Color(red: 49 / 255, green: 95 / 255, blue: 82 / 255)
    static let pokeAccentSoft = Color(red: 226 / 255, green: 236 / 255, blue: 231 / 255)
    static let pokeError = Color(red: 162 / 255, green: 59 / 255, blue: 50 / 255)
    static let pokeErrorSoft = Color(red: 247 / 255, green: 232 / 255, blue: 229 / 255)
    static let pokeBackground = Color(nsColor: .windowBackgroundColor)
    static let pokeSurface = Color(nsColor: .controlBackgroundColor)
    static let pokeEditor = Color(nsColor: .textBackgroundColor)
}
