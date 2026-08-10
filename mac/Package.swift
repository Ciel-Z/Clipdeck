// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "ClipDeck",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ClipDeck", targets: ["ClipDeck"])
    ],
    dependencies: [
        .package(url: "https://github.com/groue/GRDB.swift.git", from: "7.10.0")
    ],
    targets: [
        .executableTarget(
            name: "ClipDeck",
            dependencies: [.product(name: "GRDB", package: "GRDB.swift")],
            path: "Sources/ClipDeck",
            resources: [.process("Resources")]
        ),
        .testTarget(
            name: "ClipDeckTests",
            dependencies: ["ClipDeck"],
            path: "Tests/ClipDeckTests"
        )
    ]
)
