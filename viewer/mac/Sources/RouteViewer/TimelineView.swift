// TimelineView.swift: the run as a strip under the board.  Phase bands
// with their labels along the top, one bar per search (height: the
// search's size from the core; red when it found nothing), rip-up and
// failure ticks along the bottom, and the playhead.  Clicking or dragging
// scrubs: the view reports the time, the window controller seeks.
//
// Everything drawn comes from the core's timeline (rv_get_timeline); the
// bars are binned per point column when the width or the timeline changes,
// and the playhead is a separate layer, so playing never redraws the strip.
//
// M5: the selected net's events (rv_net_events) are marked in the
// selection colour: a mark in a strip under the phase bands for each, and
// its search bars drawn over in that colour.

import AppKit
import RVKit

@MainActor
final class TimelineView: NSView {
    enum Scrub { case began, moved, ended }

    private let session: Session
    /// Called while the user scrubs, with the playback time under the pointer.
    var onScrub: ((Scrub, Double) -> Void)?

    private(set) var timeline: Timeline?
    private var columns: [(size: Float, failed: Bool)] = []
    private var columnsWidth: CGFloat = -1
    private var time = 0.0
    private let playhead = NSView()
    /// The selected net's events, ascending (M5).
    private(set) var netEvents: [UInt32] = []

    static let phaseHeight: CGFloat = 15
    static let tickHeight: CGFloat = 9

    init(session: Session) {
        self.session = session
        super.init(frame: NSRect(x: 0, y: 0, width: 800, height: 56))
        wantsLayer = true
        layerContentsRedrawPolicy = .onSetNeedsDisplay
        playhead.wantsLayer = true
        playhead.layer?.backgroundColor = NSColor.white.cgColor
        playhead.layer?.actions = ["position": NSNull(), "bounds": NSNull(), "frame": NSNull()]
        addSubview(playhead)
        toolTip = "Click or drag to move the playhead"
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var isFlipped: Bool { true }
    override var isOpaque: Bool { true }

    /// Events on the strip (for the self-test).
    var eventCount: Int { timeline?.events.count ?? 0 }
    /// The playhead's x, points (for the self-test).
    var playheadX: CGFloat { playhead.frame.midX }

    /// The timeline changed (open, refresh): take it again.
    func reload() {
        timeline = try? session.timeline()
        columnsWidth = -1
        needsDisplay = true
        placePlayhead()
    }

    /// Mark the selected net's events (empty: none selected).
    func setNetEvents(_ events: [UInt32]) {
        guard events != netEvents else { return }
        netEvents = events
        needsDisplay = true
    }

    /// Move the playhead to playback time `t`.
    func setTime(_ t: Double) {
        guard t != time else { return }
        time = t
        placePlayhead()
    }

    private func x(of t: Double) -> CGFloat {
        guard let tl = timeline, tl.total > 0 else { return 0 }
        return CGFloat(t / tl.total) * bounds.width
    }

    private func placePlayhead() {
        let x = min(max(x(of: time), 1), max(1, bounds.width - 1))
        playhead.frame = NSRect(x: (x - 1).rounded(), y: 0, width: 2, height: bounds.height)
    }

    override func layout() {
        super.layout()
        placePlayhead()
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        needsDisplay = true
        placePlayhead()
    }

    // MARK: - Drawing

    private static let background = NSColor(srgbRed: 0.09, green: 0.10, blue: 0.12, alpha: 1)
    private static let phaseFill = [NSColor(white: 0.20, alpha: 1), NSColor(white: 0.26, alpha: 1)]
    private static let found = NSColor(srgbRed: 0.36, green: 0.72, blue: 0.48, alpha: 1)
    private static let notFound = NSColor(srgbRed: 0.90, green: 0.36, blue: 0.32, alpha: 1)
    private static let rip = NSColor(srgbRed: 0.98, green: 0.65, blue: 0.20, alpha: 1)
    private static let failure = NSColor(srgbRed: 1.0, green: 0.25, blue: 0.25, alpha: 1)
    private static let selected = NSColor(srgbRed: 0.35, green: 0.80, blue: 1.0, alpha: 1)
    static let markHeight: CGFloat = 3

    private func binColumns() {
        let n = max(1, Int(bounds.width.rounded(.up)))
        columns = Array(repeating: (0, false), count: n)
        columnsWidth = bounds.width
        guard let tl = timeline, tl.total > 0 else { return }
        for e in tl.events where e.kind == .search {
            let c = min(n - 1, max(0, Int(CGFloat(e.start / tl.total) * bounds.width)))
            // The largest search of the column; red if any found nothing.
            columns[c].size = max(columns[c].size, e.size)
            if !e.isFound { columns[c].failed = true }
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        TimelineView.background.setFill()
        bounds.fill()
        guard let tl = timeline, tl.total > 0, !tl.events.isEmpty else {
            let text = NSAttributedString(string: "No routing events in this trace",
                                          attributes: [.font: NSFont.systemFont(ofSize: 11),
                                                       .foregroundColor: NSColor(white: 0.6, alpha: 1)])
            text.draw(at: NSPoint(x: 8, y: (bounds.height - text.size().height) / 2))
            return
        }
        if columnsWidth != bounds.width { binColumns() }
        let w = bounds.width, h = bounds.height

        // Phase bands with their labels.
        let labelAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 10), .foregroundColor: NSColor(white: 0.85, alpha: 1),
        ]
        for (i, ph) in tl.phases.enumerated() {
            let x0 = x(of: ph.start), x1 = x(of: ph.end)
            let band = NSRect(x: x0, y: 0, width: max(1, x1 - x0), height: TimelineView.phaseHeight)
            TimelineView.phaseFill[i % 2].setFill()
            band.fill()
            if band.width > 24 {
                NSGraphicsContext.saveGraphicsState()
                band.insetBy(dx: 3, dy: 0).clip()
                NSAttributedString(string: ph.label, attributes: labelAttrs).draw(at: NSPoint(x: x0 + 4, y: 1))
                NSGraphicsContext.restoreGraphicsState()
            }
        }

        // Search bars.
        let top = TimelineView.phaseHeight + 3 + TimelineView.markHeight, bottom = h - TimelineView.tickHeight - 2
        let barArea = max(1, bottom - top)
        for (c, col) in columns.enumerated() where col.size > 0 || col.failed {
            let bh = 2 + CGFloat(col.size) * (barArea - 2)
            (col.failed ? TimelineView.notFound : TimelineView.found).setFill()
            NSRect(x: CGFloat(c), y: bottom - bh, width: 1, height: bh).fill()
        }

        // Rip-up and failure ticks.
        TimelineView.rip.setFill()
        for t in tl.ripTicks {
            NSRect(x: x(of: t).rounded(.down), y: h - 6, width: 1, height: 6).fill()
        }
        TimelineView.failure.setFill()
        for t in tl.failureTicks {
            NSRect(x: x(of: t).rounded(.down), y: h - TimelineView.tickHeight, width: 1,
                   height: TimelineView.tickHeight).fill()
        }
        NSColor(white: 0.3, alpha: 1).setFill()
        NSRect(x: 0, y: bottom + 1, width: w, height: 1).fill()

        // The selected net's events: a mark each under the phase bands, and
        // its search bars over the others.
        if !netEvents.isEmpty {
            TimelineView.selected.setFill()
            let n = tl.events.count
            for i in netEvents where Int(i) < n {
                let e = tl.events[Int(i)]
                let x0 = x(of: e.start).rounded(.down), x1 = x(of: e.start + e.duration)
                NSRect(x: x0, y: TimelineView.phaseHeight + 1, width: max(1, x1 - x0),
                       height: TimelineView.markHeight).fill()
                if e.kind == .search {
                    let bh = 2 + CGFloat(e.size) * (barArea - 2)
                    NSRect(x: x0, y: bottom - bh, width: 1, height: bh).fill()
                }
            }
        }
    }

    // MARK: - Scrubbing

    private func time(at event: NSEvent) -> Double {
        guard let tl = timeline, bounds.width > 0 else { return 0 }
        let p = convert(event.locationInWindow, from: nil)
        return Double(min(max(p.x / bounds.width, 0), 1)) * tl.total
    }

    override func mouseDown(with event: NSEvent) {
        guard timeline?.events.isEmpty == false else { return }
        onScrub?(.began, time(at: event))
    }

    override func mouseDragged(with event: NSEvent) {
        guard timeline?.events.isEmpty == false else { return }
        onScrub?(.moved, time(at: event))
    }

    override func mouseUp(with event: NSEvent) {
        guard timeline?.events.isEmpty == false else { return }
        onScrub?(.ended, time(at: event))
    }

    override func keyDown(with event: NSEvent) {
        if !PlaybackKeys.handle(event, from: self) { super.keyDown(with: event) }
    }
}
