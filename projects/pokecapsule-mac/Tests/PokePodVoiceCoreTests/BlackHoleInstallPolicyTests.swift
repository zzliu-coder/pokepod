import XCTest
@testable import PokePodVoiceCore

final class BlackHoleInstallPolicyTests: XCTestCase {
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

    func testOfficialFallbackUsesReleasesPage() {
        XCTAssertEqual(
            BlackHoleInstallPolicy.officialReleasesURL.absoluteString,
            "https://github.com/ExistentialAudio/BlackHole/releases/latest")
        XCTAssertEqual(
            BlackHoleInstallPolicy.latestReleaseAPIURL.absoluteString,
            "https://api.github.com/repos/ExistentialAudio/BlackHole/releases/latest")
    }

    func testSelectsOnlyBlackHoleTwoChannelReleaseAsset() throws {
        let assets = [
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole16ch-0.6.0.pkg",
                downloadURL: try XCTUnwrap(URL(string: "https://example.invalid/16.pkg"))),
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole2ch-0.6.0.zip",
                downloadURL: try XCTUnwrap(URL(string: "https://example.invalid/zip"))),
            BlackHoleInstallPolicy.ReleaseAsset(
                name: "BlackHole2ch-0.6.0.pkg",
                downloadURL: try XCTUnwrap(URL(string: "https://example.invalid/2.pkg")),
                digest: "sha256:abc")
        ]
        XCTAssertEqual(
            BlackHoleInstallPolicy.selectReleaseAsset(from: assets)?.name,
            "BlackHole2ch-0.6.0.pkg")
    }
}
