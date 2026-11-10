// swift-tools-version:5.9
import PackageDescription

let package = Package(
  name: "BLEMIDIHostDarwin",
  platforms: [.macOS(.v12), .iOS(.v15)],
  products: [
    .library(name: "BLEMIDIHostDarwin", targets: ["BLEMIDIHostDarwin"]),
  ],
  targets: [
    .target(name: "CBLEMIDIHost", path: "Sources/CBLEMIDIHost"),
    .target(
      name: "BLEMIDIHostDarwin",
      dependencies: ["CBLEMIDIHost"],
      path: "Sources/BLEMIDIHostDarwin",
      linkerSettings: [.linkedFramework("CoreBluetooth")]
    ),
  ]
)
