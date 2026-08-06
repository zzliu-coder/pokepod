import Foundation
import PokeCapsuleCore

/// Owns local device identities and every device-derived filesystem path.
final class DeviceWorkspace {
    let applicationBase: URL

    init(applicationBase: URL? = nil) {
        self.applicationBase = applicationBase
            ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("PokeCapsule", isDirectory: true)
    }

    var registryURL: URL {
        applicationBase.appendingPathComponent("Devices/registry.json")
    }

    var backupURL: URL {
        applicationBase.appendingPathComponent("Backups", isDirectory: true)
    }

    func mirrorURL(deviceID: String) -> URL {
        DeviceStorageKey.mirrorURL(applicationBase: applicationBase, deviceID: deviceID)
    }

    func queueURL(deviceID: String) -> URL {
        DeviceStorageKey.queueURL(applicationBase: applicationBase, deviceID: deviceID)
    }

    func loadRegistryAndBootstrapLegacyMirrors() -> [RegisteredDevice] {
        var devices = (try? DeviceRegistryStore(file: registryURL).load()) ?? []
        let mirrors = applicationBase.appendingPathComponent("Mirrors", isDirectory: true)
        let entries = (try? FileManager.default.contentsOfDirectory(
            at: mirrors,
            includingPropertiesForKeys: [.isDirectoryKey],
            options: [.skipsHiddenFiles])) ?? []
        var changed = false
        for entry in entries {
            let isDirectory = (try? entry.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true
            guard isDirectory else { continue }
            let key = entry.lastPathComponent
            guard key != "default",
                  !devices.contains(where: { $0.deviceId == key }) else { continue }
            let name = key == "BE87E832" ? "Poke3" : "Android \(key.suffix(4))"
            devices.append(RegisteredDevice(
                deviceId: key,
                displayName: name,
                serialAliases: [key]))
            changed = true
        }
        sort(&devices)
        if changed { try? DeviceRegistryStore(file: registryURL).save(devices) }
        return devices
    }

    func register(
        identity: DeviceIdentity,
        adbDevice: ADBDevice,
        devices: inout [RegisteredDevice]
    ) -> String {
        register(
            identity: identity,
            transportAlias: adbDevice.serial,
            fallbackModel: adbDevice.model,
            devices: &devices)
    }

    func register(
        identity: DeviceIdentity,
        transportAlias: String,
        fallbackModel: String? = nil,
        devices: inout [RegisteredDevice]
    ) -> String {
        let exactIndex = devices.firstIndex { $0.deviceId == identity.deviceId }
        let aliasIndex = devices.firstIndex { $0.serialAliases.contains(transportAlias) }
        var record: RegisteredDevice
        if let exactIndex {
            record = devices.remove(at: exactIndex)
        } else if let aliasIndex {
            record = devices.remove(at: aliasIndex)
            if record.deviceId != identity.deviceId {
                migrateLocalDeviceStorage(from: record.deviceId, to: identity.deviceId)
                record.deviceId = identity.deviceId
            }
        } else {
            record = RegisteredDevice(deviceId: identity.deviceId, displayName: identity.displayName)
        }
        record.displayName = identity.displayName
        record.platform = identity.platform
        record.manufacturer = identity.manufacturer
        record.model = identity.model ?? fallbackModel
        if !record.serialAliases.contains(transportAlias) {
            record.serialAliases.append(transportAlias)
        }
        record.lastSeenAt = Date()
        devices.removeAll { $0.deviceId == record.deviceId }
        devices.append(record)
        sort(&devices)
        try? DeviceRegistryStore(file: registryURL).save(devices)
        return record.deviceId
    }

    private func migrateLocalDeviceStorage(from oldID: String, to newID: String) {
        moveIfNeeded(from: mirrorURL(deviceID: oldID), to: mirrorURL(deviceID: newID))
        moveIfNeeded(from: queueURL(deviceID: oldID), to: queueURL(deviceID: newID))
        if UserDefaults.standard.string(forKey: "LastDeviceID") == oldID {
            UserDefaults.standard.set(newID, forKey: "LastDeviceID")
        }
    }

    private func moveIfNeeded(from oldURL: URL, to newURL: URL) {
        guard oldURL != newURL else { return }
        let fileManager = FileManager.default
        guard fileManager.fileExists(atPath: oldURL.path),
              !fileManager.fileExists(atPath: newURL.path) else { return }
        try? fileManager.createDirectory(
            at: newURL.deletingLastPathComponent(),
            withIntermediateDirectories: true)
        try? fileManager.moveItem(at: oldURL, to: newURL)
    }

    private func sort(_ devices: inout [RegisteredDevice]) {
        devices.sort {
            let left = $0.lastSeenAt ?? .distantPast
            let right = $1.lastSeenAt ?? .distantPast
            if left != right { return left > right }
            return $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
        }
    }
}
