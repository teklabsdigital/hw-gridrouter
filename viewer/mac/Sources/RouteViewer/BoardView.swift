// BoardView.swift: the board canvas.  Shows the core's display list and
// overlay as a stack of Core Animation layers, one per LayeredRenderer
// plane, and turns mouse, trackpad and keys into the core's camera calls
// (the core owns the camera; this view only asks).
//
// The view hosts its own layer tree: a root layer with one sublayer per
// plane.  A playback frame re-rasterises only the planes whose
// overlay slot changed and swaps their contents; the static slabs of the
// board stay as they are until the board or the camera changes.

import AppKit
import QuartzCore
import RVKit

@MainActor
final class BoardView: NSView {
    let session: Session
    let layered = LayeredRenderer()

    /// Called with the status-line text after the cursor, zoom or pan changed.
    var onStatus: ((String) -> Void)?
    /// Called with a message to show over the board, nil for none.
    var onMessage: ((String?) -> Void)?

    private let root = CALayer()
    private var planeLayers: [CALayer] = []
    private var appliedVersions: [Int] = []
    private var structure = -1
    private var refreshScheduled = false
    private var lastDrag: NSPoint?
    private var cursor: NSPoint?

    /// Duration of the last refresh's rasterising, seconds, and planes redrawn.
    private(set) var lastRenderSeconds: Double = 0
    private(set) var lastRenderedPlanes = 0

    init(session: Session) {
        self.session = session
        super.init(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
        // Layer-hosting: the tree below is ours, not AppKit's.  The root is
        // not flipped (y up, as AppKit's own layers): plane frames are
        // converted from the view's y-down points in apply(), and image
        // contents then show upright on screen and in render(in:) alike.
        root.backgroundColor = layered.background
        root.actions = BoardView.noActions
        layer = root
        wantsLayer = true
        registerForDraggedTypes([.fileURL])
        addTrackingArea(NSTrackingArea(rect: .zero,
                                       options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow,
                                                 .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    private static let noActions: [String: any CAAction] = [
        "contents": NSNull(), "bounds": NSNull(), "position": NSNull(), "frame": NSNull(),
        "hidden": NSNull(), "sublayers": NSNull(), "backgroundColor": NSNull(), "contentsScale": NSNull(),
    ]

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    /// The trace was (re)opened or another board was selected: take the
    /// new display list and fit it.
    func reload() {
        try? session.zoomToFit()
        refresh()
        reportStatus()
    }

    /// Visibility or the position changed: take the new lists, keep the camera.
    func displayListChanged() {
        setNeedsRefresh()
    }

    // MARK: - Drawing

    /// Refresh on the next turn of the run loop (several changes, one redraw).
    func setNeedsRefresh() {
        guard !refreshScheduled else { return }
        refreshScheduled = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.refreshScheduled = false
            self.refresh()
        }
    }

    /// Bring the camera, the lists and the planes up to date now; redraws
    /// only the planes that changed.
    func refresh() {
        guard bounds.width >= 1, bounds.height >= 1 else { return }
        let backing = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        do {
            try session.setViewSize(width: Double(bounds.width), height: Double(bounds.height), backing: Double(backing))
            if let list = try session.displayList(ifChangedSince: layered.boardList?.generation) {
                layered.update(board: list)
                updateMessage(list)
            }
            if let overlay = try session.overlay(ifChangedSince: layered.overlayList?.generation) {
                layered.update(overlay: overlay)
            }
            let view = try session.view()
            lastRenderedPlanes = layered.render(view: view)
            lastRenderSeconds = layered.lastRenderSeconds
            apply(backing: backing)
        } catch {
            return
        }
    }

    /// Hand the planes' images to their layers.
    private func apply(backing: CGFloat) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        root.backgroundColor = layered.background
        if structure != layered.structure {
            structure = layered.structure
            for l in planeLayers { l.removeFromSuperlayer() }
            planeLayers = layered.planes.map { _ in
                let l = CALayer()
                l.actions = BoardView.noActions
                l.contentsGravity = .resize
                l.magnificationFilter = .nearest
                l.minificationFilter = .nearest
                root.addSublayer(l)
                return l
            }
            appliedVersions = Array(repeating: -1, count: planeLayers.count)
        }
        for (i, plane) in layered.planes.enumerated() where appliedVersions[i] != plane.version {
            let l = planeLayers[i]
            l.contents = plane.image
            l.contentsScale = backing
            let f = plane.frame
            l.frame = CGRect(x: f.minX, y: bounds.height - f.maxY, width: f.width, height: f.height)
            l.isHidden = plane.image == nil
            appliedVersions[i] = plane.version
        }
        CATransaction.commit()
    }

    /// The layer tree as handed to Core Animation (the root's background,
    /// then each visible sublayer's contents at its frame, bottom to top),
    /// stacked into an image of the view's pixel size (for the self-test).
    /// Composited here rather than with CALayer.render(in:), which on macOS
    /// draws image contents upside down relative to the screen; a y-up
    /// bitmap context shows an image upright, as an unflipped layer does.
    func captureLayers() -> CGImage? {
        let backing = window?.backingScaleFactor ?? 2
        let w = Int((bounds.width * backing).rounded()), h = Int((bounds.height * backing).rounded())
        guard w > 0, h > 0,
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        ctx.interpolationQuality = .none
        ctx.scaleBy(x: backing, y: backing)
        if let bg = root.backgroundColor {
            ctx.setFillColor(bg)
            ctx.fill(root.bounds)
        }
        for l in root.sublayers ?? [] where !l.isHidden {
            guard let contents = l.contents else { continue }
            // Only CGImages are ever set as contents here.
            let image = contents as! CGImage
            ctx.draw(image, in: l.frame)
        }
        return ctx.makeImage()
    }

    private func updateMessage(_ list: DisplayList) {
        if list.board == nil && list.batches.isEmpty {
            onMessage?("This trace has no board yet.")
        } else {
            onMessage?(nil)
        }
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        setNeedsRefresh()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        layered.invalidate()
        setNeedsRefresh()
    }

    // MARK: - Camera

    func zoom(by factor: Double, about point: NSPoint? = nil) {
        guard factor.isFinite, factor > 0 else { return }
        let p = point ?? NSPoint(x: bounds.midX, y: bounds.midY)
        try? session.zoom(by: factor, aboutX: Double(p.x), y: Double(p.y))
        setNeedsRefresh()
        reportStatus()
    }

    func zoomToFit() {
        try? session.zoomToFit()
        setNeedsRefresh()
        reportStatus()
    }

    private func pan(_ dx: CGFloat, _ dy: CGFloat) {
        guard dx != 0 || dy != 0 else { return }
        try? session.pan(dx: Double(dx), dy: Double(dy))
        setNeedsRefresh()
        reportStatus()
    }

    func reportStatus() {
        guard let onStatus else { return }
        if let cursor, let text = try? session.statusText(viewX: Double(cursor.x), viewY: Double(cursor.y)) {
            onStatus(text)
        } else if let v = try? session.view() {
            onStatus(String(format: "zoom %.0f%%", v.zoomPercent))
        }
    }

    // MARK: - Mouse, trackpad, keys

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        lastDrag = convert(event.locationInWindow, from: nil)
        NSCursor.closedHand.set()
    }

    override func mouseDragged(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        cursor = p
        if let last = lastDrag { pan(p.x - last.x, p.y - last.y) }
        lastDrag = p
    }

    override func mouseUp(with event: NSEvent) {
        lastDrag = nil
        NSCursor.arrow.set()
    }

    override func mouseMoved(with event: NSEvent) {
        cursor = convert(event.locationInWindow, from: nil)
        reportStatus()
    }

    override func mouseExited(with event: NSEvent) {
        cursor = nil
        reportStatus()
    }

    /// Trackpad two-finger scroll pans; with Command, Option or Control it
    /// zooms.  A mouse wheel (no precise deltas) zooms, as in KiCad.
    override func scrollWheel(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        cursor = p
        let zoomModifiers: NSEvent.ModifierFlags = [.command, .option, .control]
        // In the device's own direction: wheel or fingers away from the user
        // is positive, whatever the natural-scrolling setting.
        let deviceDY = event.isDirectionInvertedFromDevice ? -event.scrollingDeltaY : event.scrollingDeltaY
        if !event.hasPreciseScrollingDeltas {
            guard deviceDY != 0 else { return }
            zoom(by: pow(1.15, Double(max(-4, min(4, deviceDY)))), about: p)
        } else if !event.modifierFlags.intersection(zoomModifiers).isEmpty {
            zoom(by: exp(Double(deviceDY) * 0.01), about: p)
        } else {
            pan(event.scrollingDeltaX, event.scrollingDeltaY)
        }
    }

    override func magnify(with event: NSEvent) {
        zoom(by: 1 + Double(event.magnification), about: convert(event.locationInWindow, from: nil))
    }

    /// Zoom keys here; ↑/↓ and ⌥ with any arrow pan.  Playback keys (space,
    /// ←/→, ⇧←/⇧→, Home, End) are the Playback menu's key equivalents;
    /// PlaybackKeys forwards them when the menu did not take them.
    override func keyDown(with event: NSEvent) {
        if PlaybackKeys.handle(event, from: self) { return }
        let plain = event.modifierFlags.intersection([.command, .option, .control]).isEmpty
        switch (plain, event.charactersIgnoringModifiers ?? "") {
        case (true, "+"), (true, "="): zoom(by: 1.25)
        case (true, "-"), (true, "_"): zoom(by: 0.8)
        case (true, "0"): zoomToFit()
        default:
            switch event.specialKey {
            case .leftArrow?: pan(60, 0)
            case .rightArrow?: pan(-60, 0)
            case .upArrow?: pan(0, 60)
            case .downArrow?: pan(0, -60)
            default: super.keyDown(with: event)
            }
        }
    }

    // MARK: - Dropping trace files

    override func draggingEntered(_ sender: any NSDraggingInfo) -> NSDragOperation {
        TraceFiles.urls(on: sender.draggingPasteboard).isEmpty ? [] : .copy
    }

    override func performDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        TraceFiles.open(TraceFiles.urls(on: sender.draggingPasteboard))
    }
}

/// The playback keys, for views that get them before the menu does.
@MainActor
enum PlaybackKeys {
    /// Sends the Playback menu's action for `event`, if it is a playback
    /// key; true when it was one.
    static func handle(_ event: NSEvent, from sender: Any) -> Bool {
        let mods = event.modifierFlags.intersection([.command, .option, .control, .shift])
        let action: Selector?
        switch (event.specialKey, mods) {
        case (.leftArrow?, []): action = #selector(BoardWindowController.stepBackward(_:))
        case (.rightArrow?, []): action = #selector(BoardWindowController.stepForward(_:))
        case (.leftArrow?, [.shift]): action = #selector(BoardWindowController.previousNet(_:))
        case (.rightArrow?, [.shift]): action = #selector(BoardWindowController.nextNet(_:))
        case (.home?, []): action = #selector(BoardWindowController.goToStart(_:))
        case (.end?, []): action = #selector(BoardWindowController.goToEnd(_:))
        default:
            action = mods.isEmpty && event.charactersIgnoringModifiers == " "
                ? #selector(BoardWindowController.togglePlay(_:)) : nil
        }
        guard let action else { return false }
        return NSApp.sendAction(action, to: nil, from: sender)
    }
}

/// Opening .grtrace files dropped on a window.
@MainActor
enum TraceFiles {
    static func urls(on pasteboard: NSPasteboard) -> [URL] {
        let urls = pasteboard.readObjects(forClasses: [NSURL.self],
                                          options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        return urls.filter { $0.pathExtension.lowercased() == "grtrace" }
    }

    @discardableResult
    static func open(_ urls: [URL]) -> Bool {
        for url in urls {
            NSDocumentController.shared.openDocument(withContentsOf: url, display: true) { _, _, error in
                if let error {
                    Task { @MainActor in NSApp.presentError(error) }
                }
            }
        }
        return !urls.isEmpty
    }
}
