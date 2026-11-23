// AppDelegate.swift: main menu, the welcome window, and the --selftest mode.
// Trace windows belong to TraceDocument (NSDocument, named in Info.plist).

import AppKit
import RVKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let selfTest: Bool
    private let selfTestFile: String?
    /// A session without a trace: the version check and the theme's background.
    private var session: Session?
    private var welcome: NSWindow?
    private let recentMenu = NSMenu(title: "Open Recent")

    init(selfTest: Bool, file: String?) {
        self.selfTest = selfTest
        selfTestFile = file
        super.init()
    }

    func applicationWillFinishLaunching(_ notification: Notification) {
        // A self-test run stays out of the Dock and does not take focus.
        NSApp.setActivationPolicy(selfTest ? .accessory : .regular)
        NSApp.mainMenu = makeMainMenu()
        session = try? Session()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if selfTest {
            runSelfTest()
            return
        }
        NSApp.activate()
        // Files opened from Finder or the Dock arrive before this; show the
        // welcome window only if none did.
        DispatchQueue.main.async { [self] in showWelcomeIfIdle() }
    }

    func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool { false }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag { showWelcomeIfIdle() }
        return true
    }

    // MARK: - Welcome window

    /// Show the welcome window when no trace is open.
    func showWelcomeIfIdle() {
        guard !selfTest, NSDocumentController.shared.documents.isEmpty else { return }
        let window = welcome ?? makeWelcomeWindow()
        welcome = window
        window.makeKeyAndOrderFront(nil)
    }

    /// A trace window is about to appear: the welcome window goes.
    func documentWillShow() {
        welcome?.orderOut(nil)
    }

    private var versionText: String {
        guard let session else { return "core unavailable" }
        return session.isCompatible
            ? "core API version \(session.apiVersion)"
            : "core API \(session.apiVersion), expected \(Session.headerAPIVersion)"
    }

    /// The theme's background colour, from the core.
    private var themeBackground: CGColor {
        if let rgba = (try? session?.displayList())??.background { return DisplayListRenderer.color(rgba) }
        return DisplayListRenderer().background
    }

    private func makeWelcomeWindow() -> NSWindow {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 480),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.title = "Visual Gridrouter"
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 480, height: 320)
        window.contentView = WelcomeView(background: themeBackground, version: versionText,
                                         target: self, action: #selector(openDocument(_:)))
        window.center()
        return window
    }

    @objc func openDocument(_ sender: Any?) {
        NSDocumentController.shared.openDocument(sender)
    }

    // MARK: - Open Recent

    func menuNeedsUpdate(_ menu: NSMenu) {
        guard menu === recentMenu else { return }
        menu.removeAllItems()
        let urls = NSDocumentController.shared.recentDocumentURLs
        for url in urls {
            let item = menu.addItem(withTitle: url.lastPathComponent, action: #selector(openRecent(_:)),
                                    keyEquivalent: "")
            item.target = self
            item.representedObject = url
            item.toolTip = url.path(percentEncoded: false)
        }
        if !urls.isEmpty { menu.addItem(.separator()) }
        let clear = menu.addItem(withTitle: "Clear Menu",
                                 action: #selector(NSDocumentController.clearRecentDocuments(_:)),
                                 keyEquivalent: "")
        clear.target = NSDocumentController.shared
    }

    @objc private func openRecent(_ sender: NSMenuItem) {
        guard let url = sender.representedObject as? URL else { return }
        NSDocumentController.shared.openDocument(withContentsOf: url, display: true) { _, _, error in
            if let error {
                Task { @MainActor in NSApp.presentError(error) }
            }
        }
    }

    // MARK: - Self-test

    /// Without a file: the welcome window and the core.  With a file: open
    /// it as a document without touching Open Recent, check its window,
    /// display list, Appearance panel and an offscreen render, then exit.
    /// Prints "selftest ok" and exits 0, or prints the failures and exits 1.
    private func runSelfTest() {
        var failures: [String] = []
        var document: TraceDocument?
        if let selfTestFile {
            let url = URL(fileURLWithPath: selfTestFile)
            do {
                let doc = try NSDocumentController.shared.makeDocument(
                    withContentsOf: url, ofType: "local.visual-gridrouter.grtrace")
                if let doc = doc as? TraceDocument {
                    doc.makeWindowControllers()
                    for controller in doc.windowControllers { controller.window?.isRestorable = false }
                    doc.showWindows()
                    document = doc
                } else {
                    failures.append("the document is not a TraceDocument: \(type(of: doc))")
                }
            } catch {
                failures.append("opening \(selfTestFile): \(error.localizedDescription)")
            }
        } else {
            let window = makeWelcomeWindow()
            window.isRestorable = false
            welcome = window
            window.orderFront(nil)
        }

        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { [self] in
            failures += checkCore()
            if let document {
                failures += check(document: document)
            } else if selfTestFile == nil {
                if welcome?.isVisible != true { failures.append("welcome window is not visible") }
            }
            if failures.isEmpty {
                print("selftest ok")
                exit(0)
            }
            for failure in failures {
                FileHandle.standardError.write(Data("selftest FAILED: \(failure)\n".utf8))
            }
            exit(1)
        }
    }

    private func checkCore() -> [String] {
        guard let session else { return ["no core session"] }
        var failures: [String] = []
        if !session.isCompatible {
            failures.append("core API \(session.apiVersion), header \(Session.headerAPIVersion)")
        }
        if !session.lastError.isEmpty {
            failures.append("fresh session reports an error: \(session.lastError)")
        }
        // A missing file is an I/O error with a message; the session stays usable.
        do throws(CoreError) {
            try session.open(path: "/nonexistent/visual-gridrouter-selftest.grtrace")
            failures.append("opening a missing trace succeeded")
        } catch {
            if error.status != CoreError.io || error.message.isEmpty {
                failures.append("unexpected open failure: \(error)")
            }
        }
        session.close()
        return failures
    }

    private func check(document: TraceDocument) -> [String] {
        var failures: [String] = []
        guard let controller = document.windowControllers.first as? BoardWindowController,
              let window = controller.window
        else { return ["the document has no board window"] }
        let name = URL(fileURLWithPath: selfTestFile ?? "").lastPathComponent
        if !window.isVisible { failures.append("board window is not visible") }
        if window.title != name { failures.append("window title \"\(window.title)\", expected \"\(name)\"") }
        if controller.panel.layerRowCount == 0 { failures.append("Appearance panel lists no layers") }
        if controller.panel.classRowCount != 10 {
            failures.append("Appearance panel lists \(controller.panel.classRowCount) object classes")
        }

        let view = controller.boardView
        guard let image = view.renderImage() else { return failures + ["board view rendered nothing"] }
        let batches = view.renderer.batchCount
        if batches == 0 { failures.append("display list is empty") }
        let background = view.renderer.background
        let pixels = SelfTestPixels.rgba(image)
        let bg = SelfTestPixels.rgba(of: background)
        var painted = 0
        for i in stride(from: 0, to: pixels.count, by: 4) where !SelfTestPixels.close(pixels, i, bg, 6) {
            painted += 1
        }
        let total = image.width * image.height
        if painted == 0 { failures.append("offscreen render shows only the background") }

        // The view's own drawing path (draw(_:) into a bitmap) must give the
        // same picture, the right way up.
        var differing = -1
        if let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
            view.cacheDisplay(in: view.bounds, to: rep)
            if let cached = rep.cgImage, cached.width == image.width, cached.height == image.height {
                let other = SelfTestPixels.rgba(cached)
                differing = 0
                for i in stride(from: 0, to: pixels.count, by: 4) where !SelfTestPixels.close(pixels, i, other, i, 8) {
                    differing += 1
                }
                if differing > total / 100 {
                    failures.append("view drawing differs from the render in \(differing) of \(total) pixels")
                }
            } else {
                failures.append("could not capture the board view")
            }
        }
        print(String(format: "selftest: %@: %d batches, %dx%d px, %.1f%% painted, view capture differs in %d px, render %.2f ms",
                     name, batches, image.width, image.height, 100 * Double(painted) / Double(total), differing,
                     view.lastRenderSeconds * 1000))
        document.close()
        // The self-test's file must not have been opened a second time by
        // AppKit (as a command-line argument) nor added to Open Recent.
        let controller2 = NSDocumentController.shared
        if !controller2.documents.isEmpty {
            failures.append("\(controller2.documents.count) documents still open (argument opened by AppKit?)")
        }
        if controller2.recentDocumentURLs.contains(where: { $0.lastPathComponent == name }) {
            failures.append("\(name) is in Open Recent")
        }
        return failures
    }

    // MARK: - Menu

    private func makeMainMenu() -> NSMenu {
        let appName = "Visual Gridrouter"
        let mainMenu = NSMenu()

        let appMenu = NSMenu(title: appName)
        appMenu.addItem(withTitle: "About \(appName)",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
                        keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide \(appName)",
                        action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let hideOthers = appMenu.addItem(withTitle: "Hide Others",
                                         action: #selector(NSApplication.hideOtherApplications(_:)),
                                         keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "Show All",
                        action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit \(appName)",
                        action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        addSubmenu(appMenu, to: mainMenu)

        let fileMenu = NSMenu(title: "File")
        let open = fileMenu.addItem(withTitle: "Open…", action: #selector(openDocument(_:)), keyEquivalent: "o")
        open.target = self
        recentMenu.delegate = self
        let recent = fileMenu.addItem(withTitle: "Open Recent", action: nil, keyEquivalent: "")
        recent.submenu = recentMenu
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close",
                         action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        addSubmenu(fileMenu, to: mainMenu)

        let viewMenu = NSMenu(title: "View")
        viewMenu.addItem(withTitle: "Zoom In", action: #selector(BoardWindowController.zoomIn(_:)),
                         keyEquivalent: "=")
        viewMenu.addItem(withTitle: "Zoom Out", action: #selector(BoardWindowController.zoomOut(_:)),
                         keyEquivalent: "-")
        viewMenu.addItem(withTitle: "Zoom to Fit", action: #selector(BoardWindowController.zoomToFit(_:)),
                         keyEquivalent: "0")
        addSubmenu(viewMenu, to: mainMenu)

        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Minimize",
                           action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowMenu.addItem(withTitle: "Zoom",
                           action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        windowMenu.addItem(.separator())
        windowMenu.addItem(withTitle: "Bring All to Front",
                           action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")
        addSubmenu(windowMenu, to: mainMenu)
        NSApp.windowsMenu = windowMenu

        return mainMenu
    }

    private func addSubmenu(_ submenu: NSMenu, to menu: NSMenu) {
        let item = NSMenuItem(title: submenu.title, action: nil, keyEquivalent: "")
        item.submenu = submenu
        menu.addItem(item)
    }
}

/// The empty state: the theme's board background, a hint, an Open button;
/// trace files can be dropped on it.
final class WelcomeView: NSView {
    private let background: CGColor

    init(background: CGColor, version: String, target: AnyObject, action: Selector) {
        self.background = background
        super.init(frame: NSRect(x: 0, y: 0, width: 760, height: 480))
        wantsLayer = true
        registerForDraggedTypes([.fileURL])

        let title = NSTextField(labelWithString: "Visual Gridrouter")
        title.font = .systemFont(ofSize: 24, weight: .medium)
        title.textColor = NSColor(white: 0.9, alpha: 1)
        let hint = NSTextField(labelWithString: "Open a .grtrace file, or drop one here.")
        hint.font = .systemFont(ofSize: 14)
        hint.textColor = NSColor(white: 0.75, alpha: 1)
        let button = NSButton(title: "Open…", target: target, action: action)
        button.bezelStyle = .push
        let detail = NSTextField(labelWithString: version)
        detail.font = .systemFont(ofSize: 11)
        detail.textColor = NSColor(white: 0.55, alpha: 1)

        let stack = NSStackView(views: [title, hint, button, detail])
        stack.orientation = .vertical
        stack.spacing = 12
        stack.setCustomSpacing(24, after: button)
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.centerXAnchor.constraint(equalTo: centerXAnchor),
            stack.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.backgroundColor = background
    }

    override func draggingEntered(_ sender: any NSDraggingInfo) -> NSDragOperation {
        TraceFiles.urls(on: sender.draggingPasteboard).isEmpty ? [] : .copy
    }

    override func performDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        TraceFiles.open(TraceFiles.urls(on: sender.draggingPasteboard))
    }
}

/// Pixel helpers for the self-test: images redrawn into RGBA8 sRGB.
enum SelfTestPixels {
    static func rgba(_ image: CGImage) -> [UInt8] {
        let w = image.width, h = image.height
        var out = [UInt8](repeating: 0, count: w * h * 4)
        out.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(data: raw.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                      bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return }
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        return out
    }

    static func rgba(of color: CGColor) -> [UInt8] {
        let srgb = color.converted(to: CGColorSpace(name: CGColorSpace.sRGB)!, intent: .defaultIntent, options: nil)
        let c = srgb?.components ?? [0, 0, 0, 1]
        return (0..<4).map { UInt8(max(0, min(255, (c.count > $0 ? c[$0] : 1) * 255)).rounded()) }
    }

    static func close(_ a: [UInt8], _ i: Int, _ b: [UInt8], _ tolerance: Int) -> Bool {
        (0..<3).allSatisfy { abs(Int(a[i + $0]) - Int(b[$0])) <= tolerance }
    }

    static func close(_ a: [UInt8], _ i: Int, _ b: [UInt8], _ j: Int, _ tolerance: Int) -> Bool {
        (0..<3).allSatisfy { abs(Int(a[i + $0]) - Int(b[j + $0])) <= tolerance }
    }
}
