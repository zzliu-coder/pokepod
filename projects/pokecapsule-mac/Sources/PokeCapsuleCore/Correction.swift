import CryptoKit
import Foundation
import Security

public protocol SecretStoring {
    func set(_ value: String, account: String) throws
    func get(account: String) throws -> String?
    func delete(account: String) throws
}

public struct KeychainStore: SecretStoring {
    private let service: String

    public init(service: String = "local.pokecapsule.mac") {
        self.service = service
    }

    public func set(_ value: String, account: String) throws {
        try delete(account: account)
        let data = Data(value.utf8)
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecValueData as String: data
        ]
        let status = SecItemAdd(query as CFDictionary, nil)
        guard status == errSecSuccess else {
            throw PokeCapsuleError.adbFailure("Keychain 写入失败：\(status)")
        }
    }

    public func get(account: String) throws -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne
        ]
        var item: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &item)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess, let data = item as? Data else {
            throw PokeCapsuleError.adbFailure("Keychain 读取失败：\(status)")
        }
        return String(decoding: data, as: UTF8.self)
    }

    public func delete(account: String) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account
        ]
        let status = SecItemDelete(query as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else {
            throw PokeCapsuleError.adbFailure("Keychain 删除失败：\(status)")
        }
    }
}

public struct CorrectionConfiguration: Equatable {
    public var endpoint: URL
    public var model: String
    public var systemPrompt: String

    public init(
        endpoint: URL,
        model: String,
        systemPrompt: String = "只校正识别错误和标点；不解释、不增删原意；无法判断时原样输出。只输出正文。"
    ) {
        self.endpoint = endpoint
        self.model = model
        self.systemPrompt = systemPrompt
    }
}

public enum CorrectionCacheKey {
    public static func fileName(
        capsuleID: UUID,
        revision: Int,
        deviceSerial: String,
        rawText: String
    ) -> String {
        let input = Data((deviceSerial + "\u{0}" + rawText).utf8)
        let digest = SHA256.hash(data: input)
            .map { String(format: "%02x", $0) }
            .joined()
        return "\(capsuleID.uuidString.lowercased())-r\(revision)-\(digest).md"
    }
}

public final class CorrectionAdapter {
    private let session: URLSession
    private let secrets: SecretStoring
    private let localKeyURL: URL?
    private let keyAccount = "correction-api-key"

    public init(
        session: URLSession = .shared,
        secrets: SecretStoring = KeychainStore(),
        localKeyURL: URL? = CorrectionAdapter.defaultLocalKeyURL()
    ) {
        self.session = session
        self.secrets = secrets
        self.localKeyURL = localKeyURL
    }

    public func saveAPIKey(_ key: String) throws {
        let clean = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if let localKeyURL {
            if clean.isEmpty {
                try? FileManager.default.removeItem(at: localKeyURL)
            } else {
                try persistLocalKey(clean, to: localKeyURL)
            }
            return
        }
        if clean.isEmpty {
            try secrets.delete(account: keyAccount)
        } else {
            try secrets.set(clean, account: keyAccount)
        }
    }

    public func importAPIKey(from file: URL) throws {
        let contents = try String(contentsOf: file, encoding: .utf8)
        guard let first = contents.split(whereSeparator: \.isNewline)
            .map({ String($0).trimmingCharacters(in: .whitespacesAndNewlines) })
            .first(where: { !$0.isEmpty }),
              first.hasPrefix("sk-") else {
            throw PokeCapsuleError.invalidAPIKeyFile
        }
        try saveAPIKey(first)
    }

    public func hasAPIKey() -> Bool {
        (try? loadAPIKey()) != nil
    }

    public func correct(text: String, configuration: CorrectionConfiguration) async throws -> String {
        guard let key = try loadAPIKey() else {
            throw PokeCapsuleError.missingAPIKey
        }
        var request = URLRequest(url: configuration.endpoint)
        request.httpMethod = "POST"
        request.timeoutInterval = 60
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")
        let body = ChatRequest(
            model: configuration.model,
            messages: [
                .init(role: "system", content: configuration.systemPrompt),
                .init(role: "user", content: text)
            ],
            temperature: 0,
            maxTokens: max(128, min(2048, text.count * 2)),
            thinking: .init(type: "disabled")
        )
        request.httpBody = try JSONEncoder().encode(body)
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            let message = String(decoding: data, as: UTF8.self)
            throw PokeCapsuleError.adbFailure("API 请求失败：\(message)")
        }
        let decoded = try JSONDecoder().decode(ChatResponse.self, from: data)
        guard let output = decoded.choices.first?.message.content
            .trimmingCharacters(in: .whitespacesAndNewlines),
              !output.isEmpty else {
            throw PokeCapsuleError.invalidCorrectionResponse
        }
        return output
    }

    private func loadAPIKey() throws -> String? {
        if let localKeyURL,
           let saved = try? String(contentsOf: localKeyURL, encoding: .utf8)
            .trimmingCharacters(in: .whitespacesAndNewlines),
           saved.hasPrefix("sk-") {
            return saved
        }
        let fallback = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Desktop/api.txt")
        if let contents = try? String(contentsOf: fallback, encoding: .utf8),
           let first = contents.split(whereSeparator: \.isNewline)
                .map({ String($0).trimmingCharacters(in: .whitespacesAndNewlines) })
                .first(where: { !$0.isEmpty }),
           first.hasPrefix("sk-") {
            if let localKeyURL {
                try? persistLocalKey(first, to: localKeyURL)
            }
            return first
        }
        if let saved = try? secrets.get(account: keyAccount),
           !saved.isEmpty {
            return saved
        }
        return nil
    }

    public static func defaultLocalKeyURL() -> URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PokeCapsule/Secrets/correction-api-key")
    }

    private func persistLocalKey(_ key: String, to url: URL) throws {
        try FileManager.default.createDirectory(
            at: url.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try Data((key + "\n").utf8).write(to: url, options: .atomic)
        try FileManager.default.setAttributes(
            [.posixPermissions: 0o600],
            ofItemAtPath: url.path)
    }
}

private struct ChatRequest: Encodable {
    struct Message: Encodable {
        let role: String
        let content: String
    }
    let model: String
    let messages: [Message]
    let temperature: Double
    let maxTokens: Int
    let thinking: Thinking

    struct Thinking: Encodable {
        let type: String
    }

    enum CodingKeys: String, CodingKey {
        case model, messages, temperature, thinking
        case maxTokens = "max_tokens"
    }
}

private struct ChatResponse: Decodable {
    struct Choice: Decodable {
        struct Message: Decodable { let content: String }
        let message: Message
    }
    let choices: [Choice]
}
