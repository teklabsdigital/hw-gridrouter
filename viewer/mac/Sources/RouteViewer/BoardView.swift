// BoardView.swift: the board canvas.  Draws the core's display list through
// RVKit's DisplayListRenderer and turns mouse, trackpad and keys into the
// core's camera calls (the core owns the camera; this view only asks).

import AppKit
import RVKit

@MainActor
final class BoardView: NSView {
    let session: Session
    let renderer = DisplayListRenderer()

    /// Called with the status-line text after the cursor, zoom or pan changed.
    var onStatus: ((String) -> Void)?

    private var listGeneration: UInt32?
    private var lastDrag: NSPoint?
    private var cursor: NSPoint?
    private let message = NSTextField(labelWithString: "")

    /// Duration of the last render (rasterising and making the image), seconds.
    private(set) var lastRenderSeconds: Double = 0

    init(session: Session) {
        self.session = session
        super.init(frame: NSRect(x: 0, y: 0, width: 800, height: 600))
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        registerForDraggedTypes([.fileURL])
        addTrackingArea(NSTrackingArea(rect: .zero,
                                       options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow,
                                                 .inVisibleRect],
                                       owner: self, userInfo: nil))
        message.font = .systemFont(ofSize: 15)
        message.textColor = NSColor(white: 0.75, alpha: 1)
        message.alignment = .center
        message.isHidden = true
        message.translatesAutoresizingMaskIntoConstraints = false
        addSubview(message)
        NSLayoutConstraint.activate([
            message.centerXAnchor.constraint(equalTo: centerXAnchor),
            message.centerYAnchor.constraint(equalTo: centerYAnchor),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    /// The trace was (re)opened or another board was selected: take the
    /// new display list and fit it.
    func reload() {
        listGeneration = nil
        try? session.zoomToFit()
        needsDisplay = true
        reportStatus()
    }

    /// Visibility changed: take the new list, keep the camera.
    func displayListChanged() {
        needsDisplay = true
    }

    // MARK: - Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        if let image = renderImage() {
            // The image's first row is the top of the view; CGContext.draw
            // puts it at the top in y-up coordinates, so undo the flip.
            ctx.saveGState()
            ctx.translateBy(x: 0, y: bounds.height)
            ctx.scaleBy(x: 1, y: -1)
            ctx.interpolationQuality = .none
            ctx.draw(image, in: CGRect(origin: .zero, size: bounds.size))
            ctx.restoreGState()
        } else {
            ctx.setFillColor(renderer.background)
            ctx.fill(bounds)
        }
    }

    /// Bring the core's camera and display list up to date and render the
    /// whole view at the window's backing scale.
    func renderImage() -> CGImage? {
        guard bounds.width >= 1, bounds.height >= 1 else { return nil }
        let backing = window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2
        do {
            try session.setViewSize(width: Double(bounds.width), height: Double(bounds.height), backing: Double(backing))
            if let list = try session.displayList(ifChangedSince: listGeneration) {
                renderer.update(list)
                listGeneration = list.generation
                updateMessage(list)
            }
            let view = try session.view()
            let start = DispatchTime.now().uptimeNanoseconds
            let image = renderer.renderImage(view: view)
            lastRenderSeconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
            return image
        } catch {
            return nil
        }
    }

    private func updateMessage(_ list: DisplayList) {
        if list.board == nil {
            message.stringValue = "This trace has no board yet."
            message.isHidden = false
        } else {
            message.isHidden = true
        }
    }

    // MARK: - Camera

    func zoom(by factor: Double, about point: NSPoint? = nil) {
        guard factor.isFinite, factor > 0 else { return }
        let p = point ?? NSPoint(x: bounds.midX, y: bounds.midY)
        try? session.zoom(by: factor, aboutX: Double(p.x), y: Double(p.y))
        needsDisplay = true
        reportStatus()
    }

    func zoomToFit() {
        try? session.zoomToFit()
        needsDisplay = true
        reportStatus()
    }

    private func pan(_ dx: CGFloat, _ dy: CGFloat) {
        guard dx != 0 || dy != 0 else { return }
        try? session.pan(dx: Double(dx), dy: Double(dy))
        needsDisplay = true
        reportStatus()
    }

    private func reportStatus() {
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

    override func keyDown(with event: NSEvent) {
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

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        needsDisplay = true
    }

    // MARK: - Dropping trace files

    override func draggingEntered(_ sender: any NSDraggingInfo) -> NSDragOperation {
        TraceFiles.urls(on: sender.draggingPasteboard).isEmpty ? [] : .copy
    }

    override func performDragOperation(_ sender: any NSDraggingInfo) -> Bool {
        TraceFiles.open(TraceFiles.urls(on: sender.draggingPasteboard))
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
