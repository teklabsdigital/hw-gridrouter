// swift-tools-version: 6.0
//
// Visual Gridrouter, macOS client.  No package dependencies: system
// frameworks and the C++ standard library only.
//
//   RVCore      the shared C++17 core (core/), exposed through core/include/rv.h
//   RVKit       Swift wrapper over rv.h and the Core Graphics display-list
//               renderer; used by the app and by rvrender
//   RouteViewer the AppKit app (wrapped into a .app by mac/bundle.sh)
//   rvrender    headless renderer: board to PNG, PNG comparison, timing

import PackageDescription

let package = Package(
    name: "VisualGridrouter",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "RouteViewer", targets: ["RouteViewer"]),
        .executable(name: "rvrender", targets: ["rvrender"]),
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
            path: "mac/Sources/RVKit",
            linkerSettings: [.linkedFramework("CoreGraphics")]
        ),
        .executableTarget(
            name: "RouteViewer",
            dependencies: ["RVKit"],
            path: "mac/Sources/RouteViewer",
            linkerSettings: [.linkedFramework("AppKit")]
        ),
        .executableTarget(
            name: "rvrender",
            dependencies: ["RVKit"],
            path: "mac/Sources/rvrender",
            linkerSettings: [.linkedFramework("CoreGraphics"), .linkedFramework("ImageIO")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
