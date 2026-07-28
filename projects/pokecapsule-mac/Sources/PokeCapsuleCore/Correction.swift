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
        systemPrompt: String = "你是中文口述校对助手。保留原意和事实，只修正明显的语音识别错误、标点和分段。仅返回校对后的正文。"
    ) {
        self.endpoint = endpoint
        self.model = model
        self.systemPrompt = systemPrompt
    }
}

public final class CorrectionAdapter {
    private let session: URLSession
    private let secrets: SecretStoring
    private let keyAccount = "correction-api-key"

    public init(session: URLSession = .shared, secrets: SecretStoring = KeychainStore()) {
        self.session = session
        self.secrets = secrets
    }

    public func saveAPIKey(_ key: String) throws {
        let clean = key.trimmingCharacters(in: .whitespacesAndNewlines)
        if clean.isEmpty {
            try secrets.delete(account: keyAccount)
        } else {
            try secrets.set(clean, account: keyAccount)
        }
    }

    public func hasAPIKey() -> Bool {
        ((try? secrets.get(account: keyAccount)) ?? nil)?.isEmpty == false
    }

    public func correct(text: String, configuration: CorrectionConfiguration) async throws -> String {
        guard let key = try secrets.get(account: keyAccount), !key.isEmpty else {
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
            temperature: 0
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
}

private struct ChatRequest: Encodable {
    struct Message: Encodable {
        let role: String
        let content: String
    }
    let model: String
    let messages: [Message]
    let temperature: Double
}

private struct ChatResponse: Decodable {
    struct Choice: Decodable {
        struct Message: Decodable { let content: String }
        let message: Message
    }
    let choices: [Choice]
}
