// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "PokeCapsuleMac",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "PokeCapsuleCore", targets: ["PokeCapsuleCore"]),
        .executable(name: "PokeCapsule", targets: ["PokeCapsule"]),
        .library(name: "PokePodVoiceCore", targets: ["PokePodVoiceCore"]),
        .executable(name: "PokePodVoice", targets: ["PokePodVoice"])
    ],
    targets: [
        .target(
            name: "PokeCapsuleCore",
            linkerSettings: [.linkedFramework("Security")]
        ),
        .executableTarget(
            name: "PokeCapsule",
            dependencies: ["PokeCapsuleCore"],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("AVFoundation")
            ]
        ),
        .testTarget(
            name: "PokeCapsuleCoreTests",
            dependencies: ["PokeCapsuleCore"]
        ),
        .target(name: "PokePodVoiceCore"),
        .executableTarget(
            name: "PokePodVoice",
            dependencies: ["PokePodVoiceCore"],
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("ApplicationServices"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("CoreBluetooth"),
                .linkedFramework("ServiceManagement")
            ]
        ),
        .testTarget(
            name: "PokePodVoiceCoreTests",
            dependencies: ["PokePodVoiceCore"]
        )
    ],
    swiftLanguageVersions: [.v5]
)
