// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// main.swift: programmatic AppKit start-up, no nib or storyboard.
//
//   RouteViewer                     the app
//   RouteViewer [FILE.grtrace ...]  the app, opening the files; a file that
//                                   does not exist yet is waited for (M6)
//   RouteViewer --selftest [FILE]   check the app (and FILE's board), print
//                                   "selftest ok" and exit; see AppDelegate

import AppKit

let arguments = CommandLine.arguments
let selfTest = arguments.contains("--selftest")
var selfTestFile: String?
if selfTest, let i = arguments.firstIndex(of: "--selftest"), i + 1 < arguments.count,
   !arguments[i + 1].hasPrefix("-") {
    selfTestFile = arguments[i + 1]
}
// M6: trace files named on the command line are opened by the app itself
// (TraceFiles.open), not by AppKit, which would refuse one that does not
// exist yet.
let commandLineFiles: [URL] = selfTest ? [] : arguments.dropFirst()
    .filter { !$0.hasPrefix("-") && $0.lowercased().hasSuffix(".grtrace") }
    .map { URL(fileURLWithPath: $0).standardizedFileURL }
if !selfTest && !commandLineFiles.isEmpty {
    UserDefaults.standard.register(defaults: ["NSTreatUnknownArgumentsAsOpen": "NO"])
}
if selfTest {
    // The file is the self-test's: AppKit must not also open it as a
    // document (that would add it to Open Recent).
    UserDefaults.standard.register(defaults: ["NSTreatUnknownArgumentsAsOpen": "NO"])
    // Nor reopen the documents of the owner's last session (window
    // restoration), which would be counted as documents left open.
    UserDefaults.standard.register(defaults: ["ApplePersistenceIgnoreState": "YES"])
    // Watchdog: a self-test that has not finished in 10 s fails instead of
    // hanging.  RV_SELFTEST_TIMEOUT=SECONDS allows longer for a large board
    // (the dongle's gesture check rasterises in one band: about 8 s).
    let timeout = ProcessInfo.processInfo.environment["RV_SELFTEST_TIMEOUT"].flatMap(Double.init)
        .map { max(1, min($0, 600)) } ?? 10
    DispatchQueue.global().asyncAfter(deadline: .now() + timeout) {
        FileHandle.standardError.write(Data("selftest FAILED: timed out\n".utf8))
        exit(2)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate(selfTest: selfTest, file: selfTestFile, open: commandLineFiles)
app.delegate = delegate  // weak reference; the global keeps it alive
app.run()
