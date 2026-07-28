import Foundation

public struct ProcessResult: Equatable {
    public let status: Int32
    public let stdout: String
    public let stderr: String

    public var combinedOutput: String {
        [stdout, stderr].filter { !$0.isEmpty }.joined(separator: "\n")
    }
}

public protocol ProcessExecuting {
    func run(executable: URL, arguments: [String]) -> ProcessResult
}

public struct ProcessRunner: ProcessExecuting {
    public init() {}

    public func run(executable: URL, arguments: [String]) -> ProcessResult {
        let process = Process()
        let temporary = FileManager.default.temporaryDirectory
            .appendingPathComponent("PokeCapsuleProcess-\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: temporary, withIntermediateDirectories: true)
            defer { try? FileManager.default.removeItem(at: temporary) }
            let stdoutURL = temporary.appendingPathComponent("stdout")
            let stderrURL = temporary.appendingPathComponent("stderr")
            FileManager.default.createFile(atPath: stdoutURL.path, contents: nil)
            FileManager.default.createFile(atPath: stderrURL.path, contents: nil)
            let stdoutHandle = try FileHandle(forWritingTo: stdoutURL)
            let stderrHandle = try FileHandle(forWritingTo: stderrURL)
            defer {
                try? stdoutHandle.close()
                try? stderrHandle.close()
            }
            process.executableURL = executable
            process.arguments = arguments
            process.standardOutput = stdoutHandle
            process.standardError = stderrHandle
            try process.run()
            process.waitUntilExit()
            try stdoutHandle.synchronize()
            try stderrHandle.synchronize()
            return ProcessResult(
                status: process.terminationStatus,
                stdout: (try? String(contentsOf: stdoutURL, encoding: .utf8)) ?? "",
                stderr: (try? String(contentsOf: stderrURL, encoding: .utf8)) ?? ""
            )
        } catch {
            return ProcessResult(status: 126, stdout: "", stderr: error.localizedDescription)
        }
    }
}

public enum ADBLocator {
    public static func locate(
        environment: [String: String] = ProcessInfo.processInfo.environment,
        bundle: Bundle = .main,
        fileManager: FileManager = .default
    ) -> URL? {
        var candidates: [String] = []
        if let configured = UserDefaults.standard.string(forKey: "ADBPath") {
            candidates.append(configured)
        }
        if let bundled = bundle.url(forResource: "adb", withExtension: nil, subdirectory: "platform-tools") {
            candidates.append(bundled.path)
        }
        candidates += ["/opt/homebrew/bin/adb", "/usr/local/bin/adb"]
        if let path = environment["PATH"] {
            candidates += path.split(separator: ":").map { String($0) + "/adb" }
        }
        return candidates
            .map { URL(fileURLWithPath: $0) }
            .first { fileManager.isExecutableFile(atPath: $0.path) }
    }
}

public enum DeviceParser {
    public static func parse(_ output: String) -> [ADBDevice] {
        output.split(whereSeparator: \.isNewline).compactMap { line in
            let text = String(line).trimmingCharacters(in: .whitespaces)
            guard !text.isEmpty,
                  !text.hasPrefix("List of devices"),
                  !text.hasPrefix("* daemon"),
                  !text.hasPrefix("adb server") else { return nil }
            let fields = text.split(whereSeparator: \.isWhitespace).map(String.init)
            guard fields.count >= 2 else { return nil }
            var attributes: [String: String] = [:]
            for field in fields.dropFirst(2) {
                let pair = field.split(separator: ":", maxSplits: 1).map(String.init)
                if pair.count == 2 { attributes[pair[0]] = pair[1] }
            }
            return ADBDevice(
                serial: fields[0],
                state: fields[1],
                model: attributes["model"],
                product: attributes["product"],
                transportID: attributes["transport_id"]
            )
        }
    }

    public static func state(for devices: [ADBDevice], adbExists: Bool = true) -> DeviceConnectionState {
        guard adbExists else { return .noADB }
        let ready = devices.filter { $0.state == "device" }
        if ready.count == 1 { return .connected(ready[0]) }
        if ready.count > 1 { return .multiple(ready) }
        let unauthorized = devices.filter { $0.state == "unauthorized" }.map(\.serial)
        if !unauthorized.isEmpty { return .unauthorized(unauthorized) }
        let offline = devices.filter { $0.state == "offline" }.map(\.serial)
        if !offline.isEmpty { return .offline(offline) }
        return .noDevice
    }
}

public final class ADBTransport {
    public let executable: URL
    public let serial: String
    private let runner: ProcessExecuting

    public init(executable: URL, serial: String, runner: ProcessExecuting = ProcessRunner()) {
        self.executable = executable
        self.serial = serial
        self.runner = runner
    }

    public static func discover(executable: URL, runner: ProcessExecuting = ProcessRunner()) -> [ADBDevice] {
        let result = runner.run(executable: executable, arguments: ["devices", "-l"])
        guard result.status == 0 else { return [] }
        return DeviceParser.parse(result.stdout)
    }

    public func readStayOnWhilePluggedIn() throws -> Int {
        let result = runner.run(
            executable: executable,
            arguments: ["-s", serial, "shell", "settings", "get", "global", "stay_on_while_plugged_in"]
        )
        guard result.status == 0 else {
            throw PokeCapsuleError.adbFailure(result.combinedOutput)
        }
        let value = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let parsed = Int(value), (0...7).contains(parsed) else {
            throw PokeCapsuleError.adbFailure("无法读取设备原有的保持唤醒设置：\(value)")
        }
        return parsed
    }

    public func writeStayOnWhilePluggedIn(_ value: Int) throws {
        guard (0...7).contains(value) else {
            throw PokeCapsuleError.adbFailure("拒绝写入非法的保持唤醒值")
        }
        let result = runner.run(
            executable: executable,
            arguments: [
                "-s", serial, "shell", "settings", "put", "global",
                "stay_on_while_plugged_in", String(value)
            ]
        )
        guard result.status == 0 else {
            throw PokeCapsuleError.adbFailure(result.combinedOutput)
        }
    }

    @discardableResult
    public func pull(remote: String, local: URL) throws -> ProcessResult {
        let safe = try PathPolicy.safeRemoteReadPath(remote)
        let result = runner.run(executable: executable, arguments: ["-s", serial, "pull", safe, local.path])
        guard result.status == 0 else { throw PokeCapsuleError.adbFailure(result.combinedOutput) }
        return result
    }

    @discardableResult
    public func pushCommand(local: URL, transactionID: UUID) throws -> ProcessResult {
        let fileName = "\(transactionID.uuidString.lowercased()).json"
        let remote = "\(ProtocolConstants.remoteRoot)/.commands/\(fileName)"
        let result = runner.run(executable: executable, arguments: ["-s", serial, "push", local.path, remote])
        guard result.status == 0 else { throw PokeCapsuleError.adbFailure(result.combinedOutput) }
        let broadcast = runner.run(
            executable: executable,
            arguments: [
                "-s", serial, "shell", "am", "broadcast",
                "-a", "com.zheliu.pokecapsule.PROCESS_COMMAND",
                "-n", "com.zheliu.pokecapsule/.command.CommandReceiver",
                "--es", "commandFile", fileName
            ]
        )
        guard broadcast.status == 0 else {
            throw PokeCapsuleError.adbFailure(broadcast.combinedOutput)
        }
        return result
    }

    @discardableResult
    public func pushImport(local: URL, transactionID: UUID, capsuleID: UUID) throws -> ProcessResult {
        let remote = "\(ProtocolConstants.remoteRoot)/.staging/\(transactionID.uuidString.lowercased())/\(capsuleID.uuidString.lowercased())"
        let result = runner.run(executable: executable, arguments: ["-s", serial, "push", local.path, remote])
        guard result.status == 0 else { throw PokeCapsuleError.adbFailure(result.combinedOutput) }
        return result
    }

    public func pullResponse(transactionID: UUID, to local: URL) -> Bool {
        let remote = "\(ProtocolConstants.remoteRoot)/.commands/results/\(transactionID.uuidString.lowercased()).json"
        let result = runner.run(executable: executable, arguments: ["-s", serial, "pull", remote, local.path])
        return result.status == 0
    }
}

public final class StayAwakeSession {
    private let transport: ADBTransport
    private var originalValue: Int?
    private var changed = false

    public init(transport: ADBTransport) {
        self.transport = transport
    }

    public func begin() throws {
        guard originalValue == nil else { return }
        let original = try transport.readStayOnWhilePluggedIn()
        originalValue = original
        let withUSB = original | 2
        if withUSB != original {
            try transport.writeStayOnWhilePluggedIn(withUSB)
            changed = true
        }
    }

    public func restore() throws {
        guard let originalValue else { return }
        defer {
            self.originalValue = nil
            changed = false
        }
        if changed {
            try transport.writeStayOnWhilePluggedIn(originalValue)
        }
    }
}

public final class DeviceCommandClient {
    private let transport: ADBTransport
    private let fileManager: FileManager
    private let waitInterval: TimeInterval
    private let timeout: TimeInterval

    public init(
        transport: ADBTransport,
        fileManager: FileManager = .default,
        waitInterval: TimeInterval = 0.5,
        timeout: TimeInterval = 20
    ) {
        self.transport = transport
        self.fileManager = fileManager
        self.waitInterval = waitInterval
        self.timeout = timeout
    }

    public func submit(_ command: DeviceCommand, waitForResponse: Bool = true) throws -> DeviceCommandResponse? {
        let temp = fileManager.temporaryDirectory
            .appendingPathComponent("PokeCapsuleCommand-\(command.transactionId.uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: temp, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: temp) }
        let commandURL = temp.appendingPathComponent("command.json")
        try PokeJSON.encoder.encode(command).write(to: commandURL, options: .atomic)
        try transport.pushCommand(local: commandURL, transactionID: command.transactionId)
        guard waitForResponse else { return nil }

        let responseURL = temp.appendingPathComponent("response.json")
        let deadline = Date().addingTimeInterval(timeout)
        repeat {
            if transport.pullResponse(transactionID: command.transactionId, to: responseURL),
               let data = try? Data(contentsOf: responseURL),
               let response = try? PokeJSON.decoder.decode(DeviceCommandResponse.self, from: data) {
                guard response.success else {
                    throw PokeCapsuleError.maintenanceRejected(response.message ?? "设备拒绝了操作")
                }
                return response
            }
            Thread.sleep(forTimeInterval: waitInterval)
        } while Date() < deadline
        throw PokeCapsuleError.commandTimedOut
    }

    public func performMaintenance(_ commands: [DeviceCommand]) throws {
        let maintenanceID = UUID()
        let stayAwake = StayAwakeSession(transport: transport)
        try stayAwake.begin()
        var firstError: Error?
        do {
            _ = try submit(DeviceCommand(
                operation: "beginMaintenance",
                maintenanceId: maintenanceID))
            for var command in commands {
                command.maintenanceId = maintenanceID
                _ = try submit(command)
            }
        } catch {
            firstError = error
        }
        do {
            _ = try submit(DeviceCommand(
                operation: "endMaintenance",
                maintenanceId: maintenanceID))
        } catch {
            if firstError == nil { firstError = error }
        }
        do {
            try stayAwake.restore()
        } catch {
            if firstError == nil { firstError = error }
        }
        if let firstError { throw firstError }
    }

    public func importCapsule(
        _ package: CapsulePackage,
        destination: String,
        existing: CapsuleRecord?
    ) throws -> Bool {
        let destination = try PathPolicy.validatedRelativeFolder(destination)
        if let existing {
            let existingManifest = try FileDigest.manifest(of: existing.localDirectory)
            if existingManifest == package.manifest { return false }
            throw PokeCapsuleError.uuidConflict(package.metadata.id)
        }

        let transactionID = UUID()
        let maintenanceID = UUID()
        let stagedPath = ".staging/\(transactionID.uuidString.lowercased())/\(package.metadata.id.uuidString.lowercased())"
        let stayAwake = StayAwakeSession(transport: transport)
        try stayAwake.begin()
        var firstError: Error?
        do {
            _ = try submit(DeviceCommand(
                operation: "beginMaintenance",
                maintenanceId: maintenanceID))
            try transport.pushImport(
                local: package.directory,
                transactionID: transactionID,
                capsuleID: package.metadata.id
            )
            _ = try submit(DeviceCommand(
                transactionId: transactionID,
                operation: "commitImport",
                maintenanceId: maintenanceID,
                capsuleIds: [package.metadata.id],
                destination: destination,
                stagedPath: stagedPath
            ))
        } catch {
            firstError = error
        }
        do {
            _ = try submit(DeviceCommand(
                operation: "endMaintenance",
                maintenanceId: maintenanceID))
        } catch {
            if firstError == nil { firstError = error }
        }
        do {
            try stayAwake.restore()
        } catch {
            if firstError == nil { firstError = error }
        }
        if let firstError { throw firstError }
        return true
    }

    public func commitCorrection(
        text: String,
        capsuleID: UUID,
        expectedRevision: Int
    ) throws {
        let transactionID = UUID()
        let maintenanceID = UUID()
        let temporary = fileManager.temporaryDirectory
            .appendingPathComponent("PokeCapsuleCorrection-\(transactionID.uuidString)", isDirectory: true)
        let capsuleDirectory = temporary.appendingPathComponent(
            capsuleID.uuidString.lowercased(), isDirectory: true)
        try fileManager.createDirectory(at: capsuleDirectory, withIntermediateDirectories: true)
        defer { try? fileManager.removeItem(at: temporary) }
        try Data(text.utf8).write(
            to: capsuleDirectory.appendingPathComponent("polished.md"),
            options: .atomic)

        let stagedPath = ".staging/\(transactionID.uuidString.lowercased())/\(capsuleID.uuidString.lowercased())"
        let stayAwake = StayAwakeSession(transport: transport)
        try stayAwake.begin()
        var firstError: Error?
        do {
            _ = try submit(DeviceCommand(
                operation: "beginMaintenance",
                maintenanceId: maintenanceID))
            try transport.pushImport(
                local: capsuleDirectory,
                transactionID: transactionID,
                capsuleID: capsuleID)
            _ = try submit(DeviceCommand(
                transactionId: transactionID,
                operation: "commitCorrection",
                maintenanceId: maintenanceID,
                capsuleIds: [capsuleID],
                stagedPath: stagedPath,
                expectedRevision: expectedRevision))
        } catch {
            firstError = error
        }
        do {
            _ = try submit(DeviceCommand(
                operation: "endMaintenance",
                maintenanceId: maintenanceID))
        } catch {
            if firstError == nil { firstError = error }
        }
        do {
            try stayAwake.restore()
        } catch {
            if firstError == nil { firstError = error }
        }
        if let firstError { throw firstError }
    }
}

public final class MirrorSynchronizer {
    private let fileManager: FileManager

    public init(fileManager: FileManager = .default) {
        self.fileManager = fileManager
    }

    public func refresh(using transport: ADBTransport, mirror: URL) throws -> CapsuleIndex {
        let parent = mirror.deletingLastPathComponent()
        let staging = parent.appendingPathComponent(".mirror-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: staging, withIntermediateDirectories: true)
        do {
            try transport.pull(remote: ProtocolConstants.remoteRoot + "/.", local: staging)
            if fileManager.fileExists(atPath: mirror.path) {
                let backup = parent.appendingPathComponent(".old-\(UUID().uuidString)", isDirectory: true)
                try fileManager.moveItem(at: mirror, to: backup)
                do {
                    try fileManager.moveItem(at: staging, to: mirror)
                    try? fileManager.removeItem(at: backup)
                } catch {
                    try? fileManager.moveItem(at: backup, to: mirror)
                    throw error
                }
            } else {
                try fileManager.moveItem(at: staging, to: mirror)
            }
            return CapsuleScanner(fileManager: fileManager).scan(root: mirror)
        } catch {
            try? fileManager.removeItem(at: staging)
            throw error
        }
    }
}
