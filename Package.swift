// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "LocalScribe",
    platforms: [
        .macOS(.v14),
    ],
    products: [
        .library(
            name: "LocalScribeCore",
            targets: ["CLocalScribeCore"]
        ),
        .executable(
            name: "LocalScribeApp",
            targets: ["LocalScribeApp"]
        ),
        .executable(
            name: "LocalScribeMetalProbe",
            targets: ["LocalScribeMetalProbe"]
        ),
    ],
    targets: [
        .binaryTarget(
            name: "whisper",
            path: "Vendor/whisper.xcframework"
        ),
        .target(
            name: "CLocalScribeCore",
            dependencies: [
                "whisper",
            ],
            path: "Core",
            exclude: [
                "tests",
            ],
            publicHeadersPath: "include",
            cSettings: [
                .define("LS_CORE_BUILDING"),
            ],
            cxxSettings: [
                .headerSearchPath("src"),
                .define("LOCALSCRIBE_ENABLE_WHISPER"),
            ],
            linkerSettings: [
                .linkedLibrary("sqlite3"),
            ]
        ),
        .executableTarget(
            name: "LocalScribeApp",
            dependencies: [
                "CLocalScribeCore",
            ],
            path: "macOS",
            linkerSettings: [
                .linkedFramework("AppKit"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("SwiftUI"),
                .unsafeFlags([
                    "-Xlinker", "-rpath",
                    "-Xlinker", "@loader_path",
                    "-Xlinker", "-rpath",
                    "-Xlinker", "@executable_path/../Frameworks",
                ]),
            ]
        ),
        .executableTarget(
            name: "LocalScribeMetalProbe",
            dependencies: [
                "whisper",
            ],
            path: "Tools/MetalProbe",
            cxxSettings: [
                .headerSearchPath(
                    "../../Vendor/whisper.xcframework/"
                        + "macos-arm64_x86_64/whisper.framework/Headers"
                ),
            ],
            linkerSettings: [
                .unsafeFlags([
                    "-Xlinker", "-rpath",
                    "-Xlinker", "@loader_path/../Frameworks",
                ]),
            ]
        ),
        .testTarget(
            name: "LocalScribeSwiftTests",
            dependencies: [
                "LocalScribeApp",
            ],
            path: "Tests/Swift"
        ),
    ],
    swiftLanguageModes: [.v6],
    cxxLanguageStandard: .cxx20
)
