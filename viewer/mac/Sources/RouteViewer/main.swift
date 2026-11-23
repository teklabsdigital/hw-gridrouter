// main.swift: programmatic AppKit start-up, no nib or storyboard.
//
//   RouteViewer                     the app
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
if selfTest {
    // The file is the self-test's: AppKit must not also open it as a
    // document (that would add it to Open Recent).
    UserDefaults.standard.register(defaults: ["NSTreatUnknownArgumentsAsOpen": "NO"])
    // Watchdog: a self-test that has not finished in 10 s fails instead of hanging.
    DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
        FileHandle.standardError.write(Data("selftest FAILED: timed out\n".utf8))
        exit(2)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate(selfTest: selfTest, file: selfTestFile)
app.delegate = delegate  // weak reference; the global keeps it alive
app.run()
