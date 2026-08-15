import XCTest
@testable import PokePodVoiceCore

final class BlackHoleInstallPolicyTests: XCTestCase {
    func testDiscoveryRootsOnlyIncludeDownloadsAndPrivateInstallerCache() {
        let home = URL(fileURLWithPath: "/tmp/PokePodHome", isDirectory: true)
        let support = URL(fileURLWithPath: "/tmp/PokePodSupport", isDirectory: true)

        XCTAssertEqual(
            BlackHoleInstallPolicy.discoveryRoots(
                homeDirectory: home,
                applicationSupportDirectory: support).map(\.path),
            [
                "/tmp/PokePodHome/Downloads",
                "/tmp/PokePodSupport/PokePod Voice/Installers"
            ])
    }

    func testDiscoveryDoesNotRecurseIntoUserFolders() throws {
        let root = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString, isDirectory: true)
        let nested = root.appendingPathComponent("nested", isDirectory: true)
        try FileManager.default.createDirectory(at: nested, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }

        let nestedPackage = nested.appendingPathComponent("BlackHole2ch-0.6.1.pkg")
        try Data().write(to: nestedPackage)
        XCTAssertNil(BlackHoleInstallPolicy.discoverPackage(in: [root]))

        let directPackage = root.appendingPathComponent("BlackHole2ch-0.6.0.pkg")
        try Data().write(to: directPackage)
        XCTAssertEqual(
            BlackHoleInstallPolicy.discoverPackage(in: [root])?.resolvingSymlinksInPath(),
            directPackage.resolvingSymlinksInPath())
    }

    func testSelectsNewestBlackHolePackageAndIgnoresOtherPackages() throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }

        let older = directory.appendingPathComponent("BlackHole2ch-0.6.0.pkg")
        let newer = directory.appendingPathComponent("BlackHole2ch-0.6.1.pkg")
        let unrelated = directory.appendingPathComponent("OtherAudio.pkg")
        for url in [older, newer, unrelated] { try Data().write(to: url) }
        try FileManager.default.setAttributes(
            [.modificationDate: Date(timeIntervalSince1970: 10)],
            ofItemAtPath: older.path)
        try FileManager.default.setAttributes(
            [.modificationDate: Date(timeIntervalSince1970: 20)],
            ofItemAtPath: newer.path)

        XCTAssertEqual(
            BlackHoleInstallPolicy.selectPackage(from: [older, newer, unrelated]),
            newer)
    }

    func testOfficialReleaseAPIEndpoint() {
        XCTAssertEqual(
            BlackHoleInstallPolicy.latestReleaseAPIURL.absoluteString,
            "https://api.github.com/repos/ExistentialAudio/BlackHole/releases/latest")
    }

    func testOfficialPackageURLUsesOnlyNumericReleaseTag() {
        XCTAssertEqual(
            BlackHoleInstallPolicy.officialPackageURL(forTag: "v0.7.1")?.absoluteString,
            "https://existential.audio/downloads/BlackHole2ch-0.7.1.pkg")
        XCTAssertEqual(
            BlackHoleInstallPolicy.officialPackageURL(forTag: "0.6.0")?.absoluteString,
            "https://existential.audio/downloads/BlackHole2ch-0.6.0.pkg")
        XCTAssertNil(BlackHoleInstallPolicy.officialPackageURL(forTag: "latest"))
        XCTAssertNil(BlackHoleInstallPolicy.officialPackageURL(forTag: "v0.7.1-rc1"))
    }

    func testDownloadTrustAndDigestRulesAreClosed() throws {
        XCTAssertTrue(BlackHoleInstallPolicy.isOfficialDownloadURL(
            try XCTUnwrap(URL(string: "https://github.com/ExistentialAudio/BlackHole/releases/download/v0.6.1/BlackHole2ch.pkg"))))
        XCTAssertTrue(BlackHoleInstallPolicy.isOfficialDownloadURL(
            try XCTUnwrap(URL(string: "https://existential.audio/downloads/BlackHole2ch-0.6.1.pkg"))))
        XCTAssertFalse(BlackHoleInstallPolicy.isOfficialDownloadURL(
            try XCTUnwrap(URL(string: "https://example.invalid/BlackHole2ch.pkg"))))
        XCTAssertFalse(BlackHoleInstallPolicy.isOfficialDownloadURL(
            try XCTUnwrap(URL(string: "https://github.com/attacker/BlackHole/releases/download/v0.6.1/BlackHole2ch.pkg"))))
        XCTAssertTrue(BlackHoleInstallPolicy.isSafePackageName("BlackHole2ch-0.6.1.pkg"))
        XCTAssertFalse(BlackHoleInstallPolicy.isSafePackageName("../BlackHole2ch.pkg"))
        XCTAssertFalse(BlackHoleInstallPolicy.isSafePackageName("BlackHole2ch.pkg/evil"))

        let digest = "sha256:" + String(repeating: "a", count: 64)
        XCTAssertEqual(BlackHoleInstallPolicy.normalizedSHA256Digest(digest), String(repeating: "a", count: 64))
        XCTAssertTrue(BlackHoleInstallPolicy.isValidDigestDeclaration(nil))
        XCTAssertTrue(BlackHoleInstallPolicy.isValidDigestDeclaration(digest))
        XCTAssertFalse(BlackHoleInstallPolicy.isValidDigestDeclaration("sha256:short"))
        XCTAssertFalse(BlackHoleInstallPolicy.isValidDigestDeclaration("md5:" + String(repeating: "a", count: 32)))
    }

    func testSignatureOutputRequiresTrustedStatusAndTeam() {
        let trusted = """
        Package "BlackHole2ch.pkg":
           Status: signed by a certificate trusted by Mac OS X
           Certificate Chain:
            1. Developer ID Installer: Existential Audio Inc (Q5C99V536K)
        """
        XCTAssertTrue(BlackHoleInstallPolicy.signatureOutputIsTrusted(trusted))
        XCTAssertFalse(BlackHoleInstallPolicy.signatureOutputIsTrusted(
            trusted.replacingOccurrences(of: "Q5C99V536K", with: "WRONGTEAM")))
        XCTAssertFalse(BlackHoleInstallPolicy.signatureOutputIsTrusted(
            trusted.replacingOccurrences(of: "signed by a certificate trusted", with: "not signed")))
        XCTAssertFalse(BlackHoleInstallPolicy.signatureOutputIsTrusted(
            "Package \"Q5C99V536K.pkg\":\n Status: signed by a certificate trusted by Mac OS X"))
        XCTAssertFalse(BlackHoleInstallPolicy.signatureOutputIsTrusted("Status: signed by a certificate trusted by Mac OS X"))
    }

    func testSelectsOnlyBlackHoleTwoChannelReleaseAsset() throws {
        let assets = [
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole16ch-0.6.0.pkg",
                downloadURL: try XCTUnwrap(URL(string: "https://github.com/ExistentialAudio/BlackHole/releases/download/v0.6.0/16.pkg"))),
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole2ch-0.6.0.zip",
                downloadURL: try XCTUnwrap(URL(string: "https://github.com/ExistentialAudio/BlackHole/releases/download/v0.6.0/zip"))),
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole2ch-0.6.0.pkg",
                downloadURL: try XCTUnwrap(URL(string: "https://github.com/ExistentialAudio/BlackHole/releases/download/v0.6.0/2.pkg")),
                digest: "sha256:" + String(repeating: "a", count: 64))
        ]
        XCTAssertEqual(
            BlackHoleInstallPolicy.selectReleaseAsset(from: assets)?.name,
            "BlackHole2ch-0.6.0.pkg")
    }
}
