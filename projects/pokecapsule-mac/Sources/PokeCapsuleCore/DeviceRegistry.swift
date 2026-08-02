import Foundation

public final class DeviceRegistryStore {
    private let file: URL
    private let fileManager: FileManager

    public init(file: URL, fileManager: FileManager = .default) {
        self.file = file
        self.fileManager = fileManager
    }

    public func load() throws -> [RegisteredDevice] {
        guard fileManager.fileExists(atPath: file.path) else { return [] }
        return try PokeJSON.decoder.decode(
            [RegisteredDevice].self,
            from: Data(contentsOf: file))
    }

    public func save(_ devices: [RegisteredDevice]) throws {
        try fileManager.createDirectory(
            at: file.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try PokeJSON.encoder.encode(devices).write(to: file, options: .atomic)
    }
}
