// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "PokeCapsuleMac",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "PokeCapsuleCore", targets: ["PokeCapsuleCore"]),
        .executable(name: "PokeCapsule", targets: ["PokeCapsule"])
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
        )
    ],
    swiftLanguageVersions: [.v5]
)
