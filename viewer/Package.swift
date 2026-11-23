// swift-tools-version: 6.0
//
// Visual Gridrouter, macOS client.  No package dependencies: system
// frameworks and the C++ standard library only.
//
//   RVCore      the shared C++17 core (core/), exposed through core/include/rv.h
//   RVKit       Swift wrapper over rv.h; used by the app and later by rvrender
//   RouteViewer the AppKit app (wrapped into a .app by mac/bundle.sh)

import PackageDescription

let package = Package(
    name: "VisualGridrouter",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "RouteViewer", targets: ["RouteViewer"]),
    ],
    targets: [
        .target(
            name: "RVCore",
            path: "core",
            sources: ["src"],
            publicHeadersPath: "include"
        ),
        .target(
            name: "RVKit",
            dependencies: ["RVCore"],
            path: "mac/Sources/RVKit"
        ),
        .executableTarget(
            name: "RouteViewer",
            dependencies: ["RVKit"],
            path: "mac/Sources/RouteViewer",
            linkerSettings: [.linkedFramework("AppKit")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
