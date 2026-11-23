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

    /// Files named on the command line (M6), opened at launch.
    private let commandLineFiles: [URL]

    init(selfTest: Bool, file: String?, open files: [URL] = []) {
        self.selfTest = selfTest
        selfTestFile = file
        commandLineFiles = files
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
        if !commandLineFiles.isEmpty { TraceFiles.open(commandLineFiles) }
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
        TraceFiles.open([url])
    }

    /// Finder, the Dock and `open -a`: through TraceFiles, so that a file
    /// still being written, or not there yet, is followed (M6).
    func application(_ application: NSApplication, open urls: [URL]) {
        guard !selfTest else { return }
        TraceFiles.open(urls)
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
        if controller.panel.classRowCount != 12 {
            failures.append("Appearance panel lists \(controller.panel.classRowCount) object classes")
        }

        let view = controller.boardView
        // A full redraw of every plane, timed.
        view.layered.invalidate()
        view.refresh()
        let fullRender = view.lastRenderSeconds
        let layered = view.layered
        let batches = layered.batchCount
        if batches == 0 { failures.append("display list is empty") }
        // The comparisons below rasterise in one band: Core Graphics drawing
        // on several threads at once (the default, for speed) is not exactly
        // repeatable for thin strokes (checkGestures measures it), and these
        // checks are about where the planes go, which bands do not change.
        layered.bands = 1
        layered.invalidate()

        /// The layer tree as Core Animation draws it (LayerSnapshot) against
        /// a single-pass render of every batch: the planes must add up to
        /// the same picture, in the same places, the right way up; no pixel
        /// may be off by more than 48.
        if let reason = LayerSnapshot.unavailableReason {
            failures.append("cannot render the layer tree offscreen: \(reason)")
        }
        func compareLayers(_ label: String) -> (painted: Double, differing: Int, total: Int) {
            view.refresh()
            guard let vt = try? controller.session.view(), let single = layered.renderSinglePass(view: vt),
                  let captured = view.snapshotLayers()
            else {
                failures.append("\(label): could not render or capture the board view")
                return (0, -1, 0)
            }
            guard captured.width == single.width, captured.height == single.height else {
                failures.append("\(label): capture \(captured.width)x\(captured.height), render \(single.width)x\(single.height)")
                return (0, -1, 0)
            }
            // RV_SELFTEST_DUMP=DIR keeps both pictures.
            if let dir = ProcessInfo.processInfo.environment["RV_SELFTEST_DUMP"] {
                for (image, suffix) in [(single, "single"), (captured, "layers")] {
                    let url = URL(fileURLWithPath: dir).appendingPathComponent("\(label)_\(suffix).png")
                    try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?.write(to: url)
                }
            }
            let a = SelfTestPixels.rgba(single), b = SelfTestPixels.rgba(captured)
            let bg = SelfTestPixels.rgba(of: layered.background)
            var painted = 0
            for i in stride(from: 0, to: a.count, by: 4) where !SelfTestPixels.close(a, i, bg, 6) { painted += 1 }
            let d = SelfTestPixels.differing(a, b, width: single.width) {
                layered.isWavefrontCellEdge(x: $0, y: $1, view: vt)
            }
            let total = single.width * single.height
            if painted == 0 { failures.append("\(label): the render shows only the background") }
            if d.strict > 0 {
                failures.append("\(label): the layers on screen differ from a single-pass render in \(d.strict) of \(total) pixels")
            }
            if d.cellEdge > 0 { print("selftest: \(label): \(d.cellEdge) px at wavefront cell edges a pixel apart") }
            return (100 * Double(painted) / Double(total), d.strict, total)
        }
        let first = compareLayers("opened")
        print(String(format: "selftest: %@: %d batches (%d board, %d overlay) in %d planes, %d px, %.1f%% painted, "
                        + "layers differ from single pass in %d px, render %.2f ms",
                     name, batches, layered.boardBatchCount, layered.overlayBatchCount, layered.planes.count,
                     first.total, first.painted, first.differing, fullRender * 1000))

        let playback = checkPlayback(controller, compareLayers)
        failures += playback
        // M6: the same trace again, written while the window follows it.
        if let selfTestFile { failures += SelfTestLive.run(source: selfTestFile) }
        document.close()
        // The self-test's file must not have been opened a second time by
        // AppKit (as a command-line argument) nor added to Open Recent.
        let controller2 = NSDocumentController.shared
        if !controller2.documents.isEmpty {
            failures.append("\(controller2.documents.count) documents still open (argument opened by AppKit?): "
                            + controller2.documents.map { "\($0.fileURL?.path ?? "?") same \($0 === document)" }
                                .joined(separator: ", "))
        }
        // By path: the owner's own Open Recent may hold another file of the same name.
        let ownPath = URL(fileURLWithPath: selfTestFile ?? "").standardizedFileURL.resolvingSymlinksInPath().path
        if controller2.recentDocumentURLs.contains(where: {
            $0.standardizedFileURL.resolvingSymlinksInPath().path == ownPath
        }) {
            failures.append("\(name) is in Open Recent")
        }
        return failures
    }

    /// Play the run from the start through the window's own clock path
    /// (accelerated: large frames), then seek, step and compare the layers
    /// with a single-pass render in mid-run.
    private func checkPlayback(_ c: BoardWindowController,
                               _ compareLayers: (String) -> (painted: Double, differing: Int, total: Int))
        -> [String] {
        var failures: [String] = []
        guard c.timelineView.eventCount > 0 else {
            print("selftest: no routing events: playback not checked")
            return failures
        }
        let s = c.session
        c.goToStart(nil)
        guard let p0 = try? s.position() else { return ["no position"] }
        if p0.time != 0 { failures.append("Go to Start left t = \(p0.time)") }
        if c.timelineView.playheadX > 2 { failures.append("playhead at x \(c.timelineView.playheadX) at the start") }
        c.togglePlay(nil)
        if !c.isPlaying { failures.append("Play did not start playing") }
        if !c.isClockRunning { failures.append("the display link is not running while playing") }
        let frames = 60
        let dt = p0.total / max(p0.speed, 1e-9) / Double(frames - 10)
        var overlays = Set<UInt32>(), frame = 0
        func renders() -> Int { c.boardView.layered.planes.reduce(0) { $0 + $1.renders } }
        let rendersBefore = renders()
        while c.isPlaying && frame < frames {
            c.advanceClock(by: dt)
            if let g = c.boardView.layered.overlayList?.generation { overlays.insert(g) }
            frame += 1
        }
        let planesRedrawn = renders() - rendersBefore
        guard let pe = try? s.position() else { return failures + ["no position"] }
        if !pe.atEnd || c.isPlaying { failures.append("playing did not stop at the end (t = \(pe.time))") }
        if c.isClockRunning { failures.append("the display link still runs after the end") }
        let copper = (try? s.facts().copperCount) ?? 0
        if copper > 0 && overlays.count < 2 { failures.append("the overlay never changed while playing") }
        if c.caption.stringValue.isEmpty || c.positionLabel.stringValue.isEmpty {
            failures.append("caption or position readout is empty")
        }
        let atEnd = c.timelineView.playheadX
        if atEnd < c.timelineView.bounds.width - 3 { failures.append("playhead at x \(atEnd) at the end") }

        // Mid-run: the router copper is drawn by the overlay.
        c.seek(fraction: 0.5)
        let mid = compareLayers("mid-run")
        let pm = try? s.position()
        c.setStepKind(nil)
        c.stepForward(nil)
        let ps = try? s.position()
        if ps?.time ?? 0 <= pm?.time ?? 0 || ps?.playing == true || ps?.fraction != 0 {
            failures.append("Step Forward did not move to the next event start, paused")
        }
        print(String(format: "selftest: played %d frames: %d overlay lists, %d planes redrawn; mid-run: %d copper, "
                        + "%d ghosts, layers differ in %d px; caption \"%@\"",
                     frame, overlays.count, planesRedrawn, pm?.copperAlive ?? 0, pm?.ghosts ?? 0, mid.differing,
                     c.caption.stringValue))
        // M4: a search in the second half of the run, part revealed (router
        // copper, ghosts and a wavefront over the board), then settled.
        let midSearch = seekMidSearch(c)
        let waves = c.boardView.layered.wavefronts
        let shownWaves = waves.filter { $0.image != nil }
        let midWave = compareLayers("mid-search")
        if let e = midSearch {
            let event = c.timelineView.timeline?.events[Int(e)]
            print(String(format: "selftest: mid-search (event %u, %@): %d wavefront images, %d drawn, %u cells; "
                            + "layers differ in %d px",
                         e, event?.isFound == true ? "found" : "failed", waves.count, shownWaves.count,
                         shownWaves.reduce(UInt32(0)) { $0 + $1.cells }, midWave.differing))
            if let event, event.size > 0, shownWaves.isEmpty, (try? s.position().cellsRevealed) ?? 0 > 0 {
                failures.append("mid-search: cells are revealed but no wavefront image is drawn")
            }
            if let event {
                _ = try? s.seek(time: event.start + event.duration * 0.95)
                c.apply([.position, .overlay, .wavefront])
                let settled = compareLayers("search settled")
                if (try? s.position().settled) != true { failures.append("95% into a search is not settled") }
                print("selftest: search settled: layers differ in \(settled.differing) px")
            }
        } else {
            print("selftest: no search with a duration in the second half: wavefront not checked")
        }
        // Pans and zooms through the view's own event handlers, mid-run
        // (router copper over the board), in a search when there is one.
        if midSearch == nil { c.seek(fraction: 0.5) } else { _ = seekMidSearch(c) }
        // M5: select a net through the nets table; the gestures below then
        // run with it highlighted.
        failures += checkSelection(c, compareLayers)
        let g0 = DispatchTime.now().uptimeNanoseconds
        failures += checkGestures(c.boardView, session: s)
        print(String(format: "selftest: gestures took %.0f ms",
                     Double(DispatchTime.now().uptimeNanoseconds - g0) / 1e6))
        failures += checkClicks(c)
        failures += checkPacing(c)
        return failures
    }

    /// M5: a net selected through the nets table (the search's net at the
    /// position, else the first row): the table, the history, the timeline
    /// marks and the highlight, the layers against a single-pass render; and
    /// "one net" pacing following the selection.  Leaves the net selected.
    private func checkSelection(_ c: BoardWindowController,
                                _ compareLayers: (String) -> (painted: Double, differing: Int, total: Int))
        -> [String] {
        var failures: [String] = []
        let s = c.session
        guard let info = try? s.netsInfo(), info.viewCount > 0 else {
            print("selftest: selection: the nets table is empty: not checked")
            return failures
        }
        if c.inspector.rowCount != info.viewCount {
            failures.append("nets table shows \(c.inspector.rowCount) rows, the core has \(info.viewCount)")
        }
        let want = (try? s.position())?.net
        var position = 0
        if let want {
            for p in 0..<info.viewCount where (try? s.netRow(at: p))?.net == want {
                position = p
                break
            }
        }
        guard let row = try? s.netRow(at: position) else { return failures + ["nets table row \(position)"] }
        // Where the time of a first selection goes, step by step (another
        // net than the one selected below, first selected here).
        var parts: [String] = []
        func timed(_ label: String, _ f: () -> Void) {
            let t = DispatchTime.now().uptimeNanoseconds
            f()
            parts.append(String(format: "%@ %.1f", label, Double(DispatchTime.now().uptimeNanoseconds - t) / 1e6))
        }
        if let other = try? s.netRow(at: (position + 1) % info.viewCount) {
            timed("core") { _ = try? s.selectNet(other.net) }
            timed("planes") { c.boardView.refresh() }
            timed("history") { _ = try? s.history(net: other.net) }
            timed("events") { _ = try? s.netEvents(net: other.net) }
            timed("panel") { c.inspector.update(force: true) }
            timed("all") { c.selectionChanged([.selection, .overlay]) }
            timed("display") { c.window?.displayIfNeeded() }
            _ = try? s.clearSelection()
            c.selectionChanged([.selection, .overlay])
            c.window?.displayIfNeeded()
        }
        let t0 = DispatchTime.now().uptimeNanoseconds
        c.inspector.selectRow(position)
        c.window?.displayIfNeeded()
        let selectMs = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
        let sel = try? s.selection()
        if sel?.net != row.net { failures.append("selecting row \(position) (net \(row.net)) selected \(String(describing: sel))") }
        if c.inspector.netsTable.selectedRow != (try? s.netsInfo())?.selectedPosition ?? -1 {
            failures.append("the nets table's selected row is \(c.inspector.netsTable.selectedRow)")
        }
        if row.session != nil && c.inspector.historyCount == 0 { failures.append("no history for net \(row.net)") }
        if row.session != nil && c.timelineView.netEvents.isEmpty { failures.append("no timeline marks for net \(row.net)") }
        let slots = c.boardView.layered.overlaySlots
        let dim = slots.first { $0.kind == .dim }?.batchCount ?? 0
        let hl = slots.first { $0.kind == .highlight }?.batchCount ?? 0
        if dim != 1 { failures.append("selected: \(dim) dim batches") }
        let shown = compareLayers("selected")
        // One net pacing follows the selection.
        if c.setPacing(.oneNet) {
            if (try? s.pacing())?.net != row.net { failures.append("one net pacing does not follow the selection") }
            _ = c.setPacing(.fit)
        } else {
            failures.append("one net pacing could not be set with net \(row.net) selected")
        }
        print(String(format: "selftest: selected %@ (row %d of %d): %d history lines, %d timeline marks, "
                        + "%d highlight batches; layers differ in %d px; select %.1f ms",
                     row.name, position, info.viewCount, c.inspector.historyCount, c.timelineView.netEvents.count,
                     hl, shown.differing, selectMs))
        print("selftest: a first selection of another net, ms: " + parts.joined(separator: ", "))
        return failures
    }

    /// M5: clicks and drags through the board view's own mouse handlers: a
    /// click on a net selects it, a click on empty space clears, a drag that
    /// starts on a net pans and selects nothing.
    private func checkClicks(_ c: BoardWindowController) -> [String] {
        var failures: [String] = []
        let view = c.boardView, s = c.session
        guard let window = view.window else { return ["the board view has no window"] }
        view.zoomToFit()
        view.flushRefresh()
        func send(_ type: NSEvent.EventType, _ p: NSPoint) {
            guard let e = NSEvent.mouseEvent(with: type, location: view.convert(p, to: nil), modifierFlags: [],
                                             timestamp: ProcessInfo.processInfo.systemUptime,
                                             windowNumber: window.windowNumber, context: nil, eventNumber: 0,
                                             clickCount: 1, pressure: 1)
            else { failures.append("could not make a mouse event"); return }
            switch type {
            case .leftMouseDown: view.mouseDown(with: e)
            case .leftMouseDragged: view.mouseDragged(with: e)
            default: view.mouseUp(with: e)
            }
        }
        // A point on a net and a point on nothing, from a grid over the view.
        var onNet: (NSPoint, Hit)?, empty: NSPoint?
        let w = view.bounds.width, h = view.bounds.height
        for gy in 1..<24 {
            for gx in 1..<36 {
                let p = NSPoint(x: w * CGFloat(gx) / 36, y: h * CGFloat(gy) / 24)
                guard let hits = try? s.hitTest(viewX: Double(p.x), viewY: Double(p.y)) else { continue }
                if hits.isEmpty, empty == nil { empty = p }
                if onNet == nil, let top = hits.first(where: { $0.net != nil || !$0.netName.isEmpty }),
                   top.netName != ((try? s.selection())?.name ?? "") {
                    onNet = (p, top)
                }
            }
        }
        guard let (p, hit) = onNet else {
            print("selftest: clicks: no net under any grid point: not checked")
            return failures
        }
        // A press with a small wobble (under the slop) is still a click.
        send(.leftMouseDown, p)
        send(.leftMouseDragged, NSPoint(x: p.x + 1, y: p.y + 1))
        send(.leftMouseUp, NSPoint(x: p.x + 1, y: p.y + 1))
        let sel = try? s.selection()
        if let n = hit.net, sel?.net != n {
            failures.append("a click on \(hit.text) selected \(String(describing: sel))")
        } else if hit.net == nil, sel?.name != hit.netName {
            failures.append("a click on \(hit.text) selected \(String(describing: sel))")
        }
        if c.inspector.netsTable.selectedRow != (try? s.netsInfo())?.selectedPosition ?? -1 {
            failures.append("after a click the nets table's selected row is \(c.inspector.netsTable.selectedRow)")
        }
        // A click on empty space clears.
        var cleared = "not checked (no empty point)"
        if let e = empty {
            send(.leftMouseDown, e)
            send(.leftMouseUp, e)
            if (try? s.selection()) != nil { failures.append("a click on empty space did not clear the selection") }
            cleared = "cleared"
        }
        // A drag from the net: pans by the drag, selects nothing.
        let before = try? s.view()
        send(.leftMouseDown, p)
        send(.leftMouseDragged, NSPoint(x: p.x + 2, y: p.y))
        send(.leftMouseDragged, NSPoint(x: p.x + 20, y: p.y + 10))
        send(.leftMouseUp, NSPoint(x: p.x + 20, y: p.y + 10))
        let after = try? s.view()
        if let before, let after, abs(after.tx - before.tx - 20) > 1e-9 || abs(after.ty - before.ty - 10) > 1e-9 {
            failures.append("a drag of (20, 10) moved the camera by (\(after.tx - before.tx), \(after.ty - before.ty))")
        }
        if empty != nil, (try? s.selection()) != nil { failures.append("a drag selected a net") }
        // The hover text names what is under the pointer.
        let status = (try? s.hoverStatus(viewX: Double(p.x + 20), viewY: Double(p.y + 10))) ?? ""
        if !status.contains(hit.text) { failures.append("hover status \"\(status)\" does not name \(hit.text)") }
        view.zoomToFit()
        view.flushRefresh()
        print("selftest: clicks: a click on \(hit.text) selected it; empty space \(cleared); a drag panned (20, 10) "
              + "and selected nothing")
        _ = try? s.clearSelection()
        c.selectionChanged([.selection, .overlay])
        return failures
    }

    /// Seek 40% into the first search with a duration that starts in the
    /// second half of the run (the reveal), or nil if there is none.
    private func seekMidSearch(_ c: BoardWindowController) -> UInt32? {
        guard let tl = c.timelineView.timeline else { return nil }
        let candidates = tl.events.indices.filter {
            tl.events[$0].kind == .search && tl.events[$0].duration > 0
        }
        guard let i = candidates.first(where: { tl.events[$0].start >= tl.total * 0.5 }) ?? candidates.last
        else { return nil }
        let e = tl.events[i]
        c.apply((try? c.session.seek(time: e.start + e.duration * 0.4)) ?? [])
        return UInt32(i)
    }

    /// Play the run to the end through the window's clock in every pacing
    /// mode (One net: the net of the search seekMidSearch finds), counting
    /// the frames in which a wavefront image changed; back to fit length.
    private func checkPacing(_ c: BoardWindowController) -> [String] {
        var failures: [String] = []
        let s = c.session
        for mode in PacingMode.allCases {
            if mode == .oneNet {
                // The net of a search in the second half (the playhead is
                // at the end, where there is none).
                let events = c.timelineView.timeline?.events ?? []
                let searches = events.indices.filter { events[$0].kind == .search && events[$0].net != nil }
                guard let i = searches.first(where: { $0 >= events.count / 2 }) ?? searches.last else {
                    if c.setPacing(.oneNet) { failures.append("pacing one net: set with no net at the playhead") }
                    print("selftest: pacing one net: no search with a net: not played")
                    continue
                }
                c.apply((try? s.seek(event: UInt32(i))) ?? [])
            }
            guard c.setPacing(mode), let p0 = try? s.pacing(), p0.mode == mode else {
                failures.append("pacing \(mode.name): could not be set")
                continue
            }
            if c.timelineView.timeline?.total != (try? s.position().total) {
                failures.append("pacing \(mode.name): the timeline was not reloaded")
            }
            let t0 = DispatchTime.now().uptimeNanoseconds
            c.goToStart(nil)
            guard let start = try? s.position() else { return failures + ["no position"] }
            c.togglePlay(nil)
            // Large frames: the dongle must fit in the 10 s watchdog.
            let frames = 16
            let dt = start.total / max(start.speed, 1e-9) / Double(frames - 4)
            var frame = 0, waveFrames = 0
            while c.isPlaying && frame < frames {
                c.advanceClock(by: dt)
                if c.boardView.layered.lastWavefrontChanges > 0 { waveFrames += 1 }
                frame += 1
            }
            let end = try? s.position()
            if end?.atEnd != true || c.isPlaying {
                failures.append("pacing \(mode.name): playing did not stop at the end (t = \(end?.time ?? -1))")
                _ = try? s.setPlaying(false)
            }
            let text = (try? s.text(.pacing)) ?? ""
            let ms = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
            print(String(format: "selftest: pacing %@ (%@): total %.1f s, played %d frames (%.0f ms), "
                            + "%d with a wavefront change",
                         mode.name, text, start.total, frame, ms, waveFrames))
        }
        if !c.setPacing(.fit) { failures.append("pacing: could not go back to fit length") }
        c.updateTransport()
        return failures
    }

    /// Many small pans and zooms through the board view's own handlers
    /// (mouse drags, trackpad scrolls, the zoom that pinch and the wheel
    /// call), at backing scales 2 and 1.  After every step, the refresh the
    /// next turn of the run loop would make runs, and the layer tree as Core
    /// Animation draws it (LayerSnapshot) must equal a single-pass render
    /// at that camera: no pixel off by more than 48.
    private func checkGestures(_ view: BoardView, session: Session) -> [String] {
        var failures: [String] = []
        guard let window = view.window else { return ["the board view has no window"] }
        let w = view.bounds.width, h = view.bounds.height
        enum Step {
            case drag(CGFloat, CGFloat)
            case scroll(Int32, Int32)
            case zoom(Double, CGFloat, CGFloat) // factor, anchor as a fraction of the view
            case fit
        }
        var steps: [Step] = [.fit]
        for _ in 0..<6 { steps.append(.drag(0, 3.25)) }          // drag down
        for _ in 0..<4 { steps.append(.drag(0, -5.5)) }          // and up
        for _ in 0..<3 { steps.append(.drag(2.75, 0)) }          // right
        for _ in 0..<3 { steps.append(.drag(-4.5, 0.5)) }        // left
        steps += [.drag(1.5, -0.75), .drag(0.25, 0.25)]
        for _ in 0..<3 { steps.append(.zoom(1.7, 0.3, 0.7)) }    // in, about an off-centre point
        for _ in 0..<3 { steps.append(.drag(0, 7.25)) }
        for _ in 0..<2 { steps.append(.zoom(2.5, 0.8, 0.2)) }    // far in
        steps += [.drag(-3.5, -2.25), .scroll(-3, 2), .scroll(2, 0), .scroll(0, -5)]
        for _ in 0..<4 { steps.append(.zoom(0.45, 0.15, 0.85)) } // far out
        steps += [.drag(0, 11), .zoom(1.1, 0.5, 0.5), .drag(0, -1.5), .fit, .drag(0, 20), .drag(0, -40)]

        var lastDrag = NSPoint(x: w / 2, y: h / 2)
        func send(_ type: NSEvent.EventType, _ p: NSPoint) {
            guard let e = NSEvent.mouseEvent(with: type, location: view.convert(p, to: nil), modifierFlags: [],
                                             timestamp: ProcessInfo.processInfo.systemUptime,
                                             windowNumber: window.windowNumber, context: nil, eventNumber: 0,
                                             clickCount: 1, pressure: 1)
            else { failures.append("could not make a mouse event"); return }
            switch type {
            case .leftMouseDown: view.mouseDown(with: e)
            case .leftMouseDragged: view.mouseDragged(with: e)
            default: view.mouseUp(with: e)
            }
        }
        for backing: CGFloat in [2, 1] {
            view.backingOverride = backing
            view.flushRefresh()
            var compared = 0, worst = 0, worstStep = -1, failing = 0, failingVertical = 0, cellEdges = 0
            let verticalDrags = steps.filter {
                if case let .drag(dx, dy) = $0 { return dx == 0 && dy != 0 }
                return false
            }.count
            var before: ViewTransform? = nil
            send(.leftMouseDown, lastDrag)
            for (k, step) in steps.enumerated() {
                switch step {
                case let .drag(dx, dy):
                    lastDrag = NSPoint(x: lastDrag.x + dx, y: lastDrag.y + dy)
                    send(.leftMouseDragged, lastDrag)
                case let .scroll(dx, dy):
                    if let cg = CGEvent(scrollWheelEvent2Source: nil, units: .pixel, wheelCount: 2, wheel1: dy,
                                        wheel2: dx, wheel3: 0),
                       let e = NSEvent(cgEvent: cg) {
                        view.scrollWheel(with: e)
                    }
                case let .zoom(f, ax, ay):
                    view.zoom(by: f, about: NSPoint(x: w * ax, y: h * ay))
                case .fit:
                    view.zoomToFit()
                }
                view.flushRefresh()
                guard let vt = try? session.view(), let single = view.layered.renderSinglePass(view: vt),
                      let shown = view.snapshotLayers(), shown.width == single.width, shown.height == single.height
                else {
                    failures.append("gestures at backing \(backing), step \(k): could not render or capture")
                    break
                }
                let a = SelfTestPixels.rgba(single), b = SelfTestPixels.rgba(shown)
                let d = SelfTestPixels.differing(a, b, width: single.width) {
                    view.layered.isWavefrontCellEdge(x: $0, y: $1, view: vt)
                }
                let differing = d.strict
                cellEdges += d.cellEdge
                compared += 1
                // A drag moves the board with the pointer, by the same points.
                if case let .drag(dx, dy) = step, let before {
                    if abs(vt.tx - before.tx - Double(dx)) > 1e-9 || abs(vt.ty - before.ty - Double(dy)) > 1e-9 {
                        failures.append("gestures: a drag of (\(dx), \(dy)) moved the camera by "
                                        + "(\(vt.tx - before.tx), \(vt.ty - before.ty))")
                    }
                }
                before = vt
                if differing > 0 {
                    failing += 1
                    if case let .drag(dx, dy) = step, dx == 0, dy != 0 { failingVertical += 1 }
                }
                if differing > worst {
                    worst = differing
                    worstStep = k
                    if let dir = ProcessInfo.processInfo.environment["RV_SELFTEST_DUMP"] {
                        for (image, suffix) in [(single, "single"), (shown, "layers")] {
                            let url = URL(fileURLWithPath: dir)
                                .appendingPathComponent("gesture_x\(Int(backing))_step\(k)_\(suffix).png")
                            try? NSBitmapImageRep(cgImage: image).representation(using: .png, properties: [:])?
                                .write(to: url)
                        }
                    }
                }
            }
            send(.leftMouseUp, lastDrag)
            print(String(format: "selftest: gestures at backing %.0f: %d steps compared (%d vertical drags), "
                            + "%d differ (%d vertical drags), worst %d px off (step %d); "
                            + "%d px at wavefront cell edges a pixel apart in all",
                         backing, compared, verticalDrags, failing, failingVertical, worst, worstStep, cellEdges))
            if worst > 0 {
                failures.append("gestures at backing \(Int(backing)): the layers on screen differ from a single-pass "
                                + "render after \(failing) of \(compared) steps (\(failingVertical) of "
                                + "\(verticalDrags) vertical drags), at worst in \(worst) px (step \(worstStep))")
            }
        }
        // How repeatable the parallel rasteriser is (information only):
        // the same picture drawn twice in the default bands, zoomed in.
        view.backingOverride = nil
        view.zoom(by: 4, about: NSPoint(x: w * 0.4, y: h * 0.6))
        view.flushRefresh()
        let savedBands = view.layered.bands
        view.layered.bands = 0
        var worstRepeat = 0
        if let vt = try? session.view() {
            for _ in 0..<4 {
                guard let x = view.layered.renderSinglePass(view: vt), let y = view.layered.renderSinglePass(view: vt)
                else { break }
                let a = SelfTestPixels.rgba(x), b = SelfTestPixels.rgba(y)
                var n = 0
                for i in stride(from: 0, to: a.count, by: 4) where !SelfTestPixels.close(a, i, b, i, 48) { n += 1 }
                worstRepeat = max(worstRepeat, n)
            }
        }
        view.layered.bands = savedBands
        print("selftest: the parallel rasteriser drawing the same picture twice: up to \(worstRepeat) px off by more than 48")
        view.flushRefresh()
        view.zoomToFit()
        view.flushRefresh()
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
        addSubmenu(makePlaybackMenu(), to: mainMenu)

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

    /// The Playback menu.  Its items go to the key window's
    /// BoardWindowController through the responder chain; the keys without
    /// modifiers (space, arrows, Home, End) are the transport's shortcuts.
    private func makePlaybackMenu() -> NSMenu {
        let menu = NSMenu(title: "Playback")
        func key(_ special: NSEvent.SpecialKey) -> String { String(Character(special.unicodeScalar)) }
        func add(_ title: String, _ action: Selector, _ key: String,
                 _ modifiers: NSEvent.ModifierFlags = []) {
            let item = menu.addItem(withTitle: title, action: action, keyEquivalent: key)
            item.keyEquivalentModifierMask = modifiers
        }
        add("Play", #selector(BoardWindowController.togglePlay(_:)), " ")
        add("Play Backward", #selector(BoardWindowController.playBackward(_:)), "")
        menu.addItem(.separator())
        add("Step Forward", #selector(BoardWindowController.stepForward(_:)), key(.rightArrow))
        add("Step Backward", #selector(BoardWindowController.stepBackward(_:)), key(.leftArrow))
        add("Next Net", #selector(BoardWindowController.nextNet(_:)), key(.rightArrow), [.shift])
        add("Previous Net", #selector(BoardWindowController.previousNet(_:)), key(.leftArrow), [.shift])
        let stepBy = NSMenu(title: "Step By")
        for kind in StepKind.allCases {
            let item = stepBy.addItem(withTitle: kind.name.prefix(1).uppercased() + kind.name.dropFirst(),
                                      action: #selector(BoardWindowController.setStepKind(_:)), keyEquivalent: "")
            item.tag = Int(kind.rawValue)
        }
        let stepByItem = menu.addItem(withTitle: "Step By", action: nil, keyEquivalent: "")
        stepByItem.submenu = stepBy
        menu.addItem(.separator())
        add("Go to Start", #selector(BoardWindowController.goToStart(_:)), key(.home))
        add("Go to End", #selector(BoardWindowController.goToEnd(_:)), key(.end))
        // M6: follow a run that is being written (End does the same then).
        add("Follow Live", #selector(BoardWindowController.goLive(_:)), "l")
        menu.addItem(.separator())
        add("Faster", #selector(BoardWindowController.faster(_:)), "]", [.command])
        add("Slower", #selector(BoardWindowController.slower(_:)), "[", [.command])
        // M4: how long each event lasts.  "One Net" follows the net of the
        // event at the playhead.
        let pacing = NSMenu(title: "Pacing")
        let titles: [PacingMode: String] = [.fit: "Fit Length", .netsOnly: "Nets Only", .oneNet: "One Net",
                                            .skipFailed: "Skip Failed", .realTime: "Real Time"]
        for mode in PacingMode.allCases {
            let item = pacing.addItem(withTitle: titles[mode] ?? mode.name,
                                      action: #selector(BoardWindowController.setPacingMode(_:)), keyEquivalent: "")
            item.tag = Int(mode.rawValue)
        }
        let pacingItem = menu.addItem(withTitle: "Pacing", action: nil, keyEquivalent: "")
        pacingItem.submenu = pacing
        return menu
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

    /// Pixels of `shown` more than 48 off `single` (both `width` wide, rows
    /// from the top).  A pixel whose centre lies on a wavefront cell edge
    /// (`cellEdge`, LayeredRenderer.isWavefrontCellEdge) and that equals a
    /// pixel of `single` one pixel away is counted apart, as a tie that Core
    /// Animation and Core Graphics break differently; every other differing
    /// pixel counts.
    static func differing(_ single: [UInt8], _ shown: [UInt8], width: Int, cellEdge: (Int, Int) -> Bool)
        -> (strict: Int, cellEdge: Int) {
        let height = single.count / (4 * max(1, width))
        var strict = 0, edge = 0
        // The pixels off by more than 48 in a channel, quickly.
        var off: [Int] = []
        single.withUnsafeBufferPointer { a in
            shown.withUnsafeBufferPointer { b in
                var i = 0
                let n = min(a.count, b.count)
                while i + 3 < n {
                    if abs(Int(a[i]) - Int(b[i])) > 48 || abs(Int(a[i + 1]) - Int(b[i + 1])) > 48
                        || abs(Int(a[i + 2]) - Int(b[i + 2])) > 48 {
                        off.append(i)
                    }
                    i += 4
                }
            }
        }
        for i in off {
            let x = (i / 4) % width, y = (i / 4) / width
            var tie = false
            if cellEdge(x, y) {
                search: for dy in -1...1 {
                    for dx in -1...1 where (dx, dy) != (0, 0) {
                        let nx = x + dx, ny = y + dy
                        guard nx >= 0, ny >= 0, nx < width, ny < height else { continue }
                        if close(single, (ny * width + nx) * 4, shown, i, 48) { tie = true; break search }
                    }
                }
            }
            if tie { edge += 1 } else { strict += 1 }
        }
        return (strict, edge)
    }
}
