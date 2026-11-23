// AppDelegate.swift: main menu, the window, and the --selftest mode.

import AppKit
import RVKit

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private let selfTest: Bool
    private var session: Session?
    private var window: NSWindow?

    init(selfTest: Bool) {
        self.selfTest = selfTest
        super.init()
    }

    func applicationWillFinishLaunching(_ notification: Notification) {
        // A self-test run stays out of the Dock and does not take focus.
        NSApp.setActivationPolicy(selfTest ? .accessory : .regular)
        NSApp.mainMenu = makeMainMenu()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let versionText: String
        do {
            let session = try Session()
            self.session = session
            versionText = "core API version \(session.apiVersion)"
        } catch {
            versionText = "core unavailable: \(error)"
        }

        let window = makeWindow(label: versionText)
        self.window = window
        window.makeKeyAndOrderFront(nil)

        if selfTest {
            runSelfTest()
        } else {
            NSApp.activate()
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    // MARK: - Self-test

    /// Checks the window and the core after one turn of the run loop, prints
    /// "selftest ok" and exits 0, or prints the failure and exits 1.
    private func runSelfTest() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [self] in
            var failures: [String] = []
            if let window {
                if !window.isVisible { failures.append("window is not visible") }
                if window.title != "Visual Gridrouter" { failures.append("unexpected window title") }
            } else {
                failures.append("no window")
            }
            if let session {
                if !session.isCompatible {
                    failures.append("core API \(session.apiVersion), header \(Session.headerAPIVersion)")
                }
                if !session.lastError.isEmpty {
                    failures.append("fresh session reports an error: \(session.lastError)")
                }
                // The trace reader is reachable through RVKit: a missing file
                // is an I/O error with a message, and the session stays usable.
                do throws(CoreError) {
                    try session.open(path: "/nonexistent/visual-gridrouter-selftest.grtrace")
                    failures.append("opening a missing trace succeeded")
                } catch {
                    if error.status != CoreError.io || error.message.isEmpty {
                        failures.append("unexpected open failure: \(error)")
                    }
                }
                session.close()
            } else {
                failures.append("no core session")
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

    // MARK: - Window

    private func makeWindow(label text: String) -> NSWindow {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1100, height: 720),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Visual Gridrouter"
        window.isReleasedWhenClosed = false
        window.minSize = NSSize(width: 480, height: 320)

        let background = BackgroundView()
        let label = NSTextField(labelWithString: text)
        label.font = .systemFont(ofSize: 15)
        label.textColor = NSColor(white: 0.8, alpha: 1)
        label.translatesAutoresizingMaskIntoConstraints = false
        background.addSubview(label)
        NSLayoutConstraint.activate([
            label.centerXAnchor.constraint(equalTo: background.centerXAnchor),
            label.centerYAnchor.constraint(equalTo: background.centerYAnchor),
        ])

        window.contentView = background
        window.center()
        return window
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
        // Placeholder until the trace reader exists: no action, so disabled.
        fileMenu.addItem(withTitle: "Open…", action: nil, keyEquivalent: "o")
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close",
                         action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        addSubmenu(fileMenu, to: mainMenu)

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

/// Plain view filled with KiCad's default PCB background colour.
/// Temporary: from M2 the colour comes from the core's KiCad theme.
final class BackgroundView: NSView {
    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.backgroundColor = NSColor(srgbRed: 0, green: 16 / 255, blue: 35 / 255, alpha: 1).cgColor
    }
}
