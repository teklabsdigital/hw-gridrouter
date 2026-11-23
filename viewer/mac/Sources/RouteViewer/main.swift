// main.swift: programmatic AppKit start-up, no nib or storyboard.

import AppKit

let selfTest = CommandLine.arguments.contains("--selftest")
if selfTest {
    // Watchdog: a self-test that has not finished in 10 s fails instead of hanging.
    DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
        FileHandle.standardError.write(Data("selftest FAILED: timed out\n".utf8))
        exit(2)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate(selfTest: selfTest)
app.delegate = delegate  // weak reference; the global keeps it alive
app.run()
