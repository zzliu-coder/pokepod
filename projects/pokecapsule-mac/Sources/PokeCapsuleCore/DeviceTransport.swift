import Foundation

public struct DeviceTransportSessionToken: Equatable, Sendable {
    public let value: String

    public init(_ value: String) {
        self.value = value
    }
}

public protocol DeviceTransport: AnyObject {
    var deviceIdentifier: String { get }

    func readDeviceIdentity() throws -> DeviceIdentity?
    func metadataFingerprint() throws -> String
    func fetchLibrary(to local: URL) throws
    func submitCommandFile(local: URL, transactionID: UUID) throws
    func stageImport(local: URL, transactionID: UUID, capsuleID: UUID) throws
    func fetchCommandResult(transactionID: UUID, to local: URL) throws -> Bool
    func beginHostSession() throws -> DeviceTransportSessionToken?
    func endHostSession(_ token: DeviceTransportSessionToken?) throws
}

public extension DeviceTransport {
    func beginHostSession() throws -> DeviceTransportSessionToken? { nil }
    func endHostSession(_ token: DeviceTransportSessionToken?) throws {}
}
