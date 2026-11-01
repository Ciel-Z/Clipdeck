// swift-tools-version: 6.0

import PackageDescription

let package = Package(
    name: "ClipDeck",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ClipDeck", targets: ["ClipDeck"])
    ],
    dependencies: [
        .package(url: "https://github.com/groue/GRDB.swift.git", from: "7.10.0"),
        .package(url: "https://github.com/jedisct1/swift-sodium.git", from: "0.9.1")
    ],
    targets: [
        .executableTarget(
            name: "ClipDeck",
            dependencies: [
                .product(name: "GRDB", package: "GRDB.swift"),
                .product(name: "Clibsodium", package: "swift-sodium")
            ],
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
