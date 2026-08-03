import CryptoKit
import Foundation

public enum DeviceStorageKey {
    public static func fileComponent(_ value: String) -> String {
        let clean = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if let uuid = UUID(uuidString: clean) {
            return uuid.uuidString.lowercased()
        }
        if !clean.isEmpty,
           clean.count <= 128,
           clean.first != ".",
           clean.unicodeScalars.allSatisfy({ scalar in
               CharacterSet.alphanumerics.contains(scalar)
                   || scalar.value == 45 || scalar.value == 95
           }) {
            return clean
        }
        guard !clean.isEmpty else { return "default" }
        let digest = SHA256.hash(data: Data(clean.utf8))
            .map { String(format: "%02x", $0) }
            .joined()
        return "device-\(digest)"
    }

    public static func mirrorURL(applicationBase: URL, deviceID: String) -> URL {
        applicationBase
            .appendingPathComponent("Mirrors", isDirectory: true)
            .appendingPathComponent(fileComponent(deviceID), isDirectory: true)
    }

    public static func queueURL(applicationBase: URL, deviceID: String) -> URL {
        applicationBase
            .appendingPathComponent("Queues", isDirectory: true)
            .appendingPathComponent("\(fileComponent(deviceID)).json")
    }
}
