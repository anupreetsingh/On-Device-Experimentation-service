// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Experimentation",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "Experimentation", targets: ["Experimentation"]),
        .executable(name: "exp-demo", targets: ["ExpDemo"]),
    ],
    targets: [
        // libexp, compiled from the same sources CMake builds. Its public header is plain C, so
        // Swift imports it directly.
        .target(
            name: "CExp",
            path: "libexp",
            sources: ["src/libexp.cpp", "src/uds_client.cpp"],
            publicHeadersPath: "include"
        ),
        .target(name: "Experimentation", dependencies: ["CExp"], path: "swift/Experimentation"),
        .executableTarget(name: "ExpDemo", dependencies: ["Experimentation"], path: "swift/ExpDemo"),
    ],
    cxxLanguageStandard: .cxx17
)
