// TraceDocument.swift: one open .grtrace file, read only.  The document owns
// a core session; its window shows the board, the caption, the timeline,
// the transport bar, the Appearance panel and the status line.

import AppKit
import QuartzCore
import RVKit

@objc(TraceDocument)
final class TraceDocument: NSDocument {
    /// nil only if the core could not create a session (then reading fails).
    let session: Session?

    override init() {
        session = try? Session()
        super.init()
    }

    nonisolated override class var autosavesInPlace: Bool { false }

    /// Read on the main thread (concurrent reading is off by default), so
    /// the main-actor session can be used here.
    nonisolated override func read(from url: URL, ofType typeName: String) throws {
        try MainActor.assumeIsolated { () throws -> Void in
            guard let session = self.session else {
                throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadUnknownError,
                              userInfo: [NSLocalizedFailureReasonErrorKey: "The viewer core is unavailable."])
            }
            do throws(CoreError) {
                try session.open(path: url.path(percentEncoded: false))
            } catch {
                throw NSError(domain: NSCocoaErrorDomain, code: NSFileReadCorruptFileError,
                              userInfo: [NSLocalizedDescriptionKey:
                                             "“\(url.lastPathComponent)” could not be opened.",
                                         NSLocalizedFailureReasonErrorKey: error.message])
            }
            // A revert (the file changed on disk) reads again: redraw.
            for controller in self.windowControllers {
                (controller as? BoardWindowController)?.reload()
            }
        }
    }

    /// The window title: the file name, extension included.
    override var displayName: String! {
        get { fileURL?.lastPathComponent ?? super.displayName }
        set { super.displayName = newValue }
    }

    override func makeWindowControllers() {
        guard let session else { return }
        let controller = BoardWindowController(session: session)
        addWindowController(controller)
        controller.reload()
        (NSApp.delegate as? AppDelegate)?.documentWillShow()
    }
}

@MainActor
final class BoardWindowController: NSWindowController, NSWindowDelegate, NSMenuItemValidation {
    let session: Session
    let boardView: BoardView
    let timelineView: TimelineView
    let panel: AppearancePanel
    private let status = NSTextField(labelWithString: "")
    private let message = NSTextField(labelWithString: "")
    let caption = NSTextField(labelWithString: "")
    let positionLabel = NSTextField(labelWithString: "")
    private let speedLabel = NSTextField(labelWithString: "")
    private let stepPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let pacingPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private var playButton: NSButton!

    /// The kind ←/→ and the step buttons step by.
    private(set) var stepKind: StepKind = .event
    /// The display link, while playing.
    private var link: CADisplayLink?
    private var lastTick: CFTimeInterval?
    private var resumeAfterScrub = false

    init(session: Session) {
        self.session = session
        boardView = BoardView(session: session)
        timelineView = TimelineView(session: session)
        panel = AppearancePanel(session: session)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1200, height: 860),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 640, height: 440)
        super.init(window: window)
        window.delegate = self
        layout(in: window)
        window.initialFirstResponder = boardView
        window.center()
        shouldCascadeWindows = true

        boardView.onStatus = { [weak self] text in self?.status.stringValue = text }
        boardView.onMessage = { [weak self] text in
            self?.message.stringValue = text ?? ""
            self?.message.isHidden = text == nil
        }
        panel.onChange = { [weak self] reloadBoard in
            guard let self else { return }
            if reloadBoard { self.boardView.reload() } else { self.boardView.displayListChanged() }
        }
        timelineView.onScrub = { [weak self] phase, t in self?.scrub(phase, to: t) }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    // MARK: - Layout

    private func layout(in window: NSWindow) {
        let content = NSView()
        let bar = NSView()
        let divider = NSBox()
        divider.boxType = .separator
        status.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        caption.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        caption.lineBreakMode = .byTruncatingTail
        caption.cell?.truncatesLastVisibleLine = true
        caption.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        message.font = .systemFont(ofSize: 15)
        message.textColor = NSColor(white: 0.75, alpha: 1)
        message.alignment = .center
        message.isHidden = true
        let transport = makeTransport()
        for v in [boardView, panel, bar, divider, status, caption, timelineView, transport, message] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
        }
        content.addSubview(boardView)
        content.addSubview(message)
        content.addSubview(caption)
        content.addSubview(timelineView)
        content.addSubview(transport)
        content.addSubview(divider)
        content.addSubview(panel)
        content.addSubview(bar)
        bar.addSubview(status)
        NSLayoutConstraint.activate([
            boardView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            boardView.topAnchor.constraint(equalTo: content.topAnchor),
            boardView.bottomAnchor.constraint(equalTo: caption.topAnchor, constant: -4),
            boardView.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            message.centerXAnchor.constraint(equalTo: boardView.centerXAnchor),
            message.centerYAnchor.constraint(equalTo: boardView.centerYAnchor),
            caption.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 8),
            caption.trailingAnchor.constraint(equalTo: divider.leadingAnchor, constant: -8),
            caption.heightAnchor.constraint(equalToConstant: 16),
            caption.bottomAnchor.constraint(equalTo: timelineView.topAnchor, constant: -3),
            timelineView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            timelineView.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            timelineView.heightAnchor.constraint(equalToConstant: 56),
            timelineView.bottomAnchor.constraint(equalTo: transport.topAnchor),
            transport.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 6),
            transport.trailingAnchor.constraint(equalTo: divider.leadingAnchor, constant: -8),
            transport.heightAnchor.constraint(equalToConstant: 30),
            transport.bottomAnchor.constraint(equalTo: bar.topAnchor),
            divider.topAnchor.constraint(equalTo: content.topAnchor),
            divider.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            divider.widthAnchor.constraint(equalToConstant: 1),
            divider.trailingAnchor.constraint(equalTo: panel.leadingAnchor),
            panel.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            panel.topAnchor.constraint(equalTo: content.topAnchor),
            panel.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            panel.widthAnchor.constraint(equalToConstant: 240),
            bar.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            bar.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            bar.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            bar.heightAnchor.constraint(equalToConstant: 22),
            status.leadingAnchor.constraint(equalTo: bar.leadingAnchor, constant: 8),
            status.trailingAnchor.constraint(lessThanOrEqualTo: bar.trailingAnchor, constant: -8),
            status.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
        ])
        window.contentView = content
    }

    private func makeTransport() -> NSView {
        func button(_ symbol: String, _ tip: String, _ action: Selector) -> NSButton {
            let image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip) ?? NSImage()
            let b = NSButton(image: image, target: self, action: action)
            b.isBordered = false
            b.imageScaling = .scaleProportionallyDown
            b.toolTip = tip
            b.translatesAutoresizingMaskIntoConstraints = false
            b.widthAnchor.constraint(equalToConstant: 26).isActive = true
            return b
        }
        playButton = button("play.fill", "Play or pause (space)", #selector(togglePlay(_:)))
        let buttons = [
            button("backward.end.fill", "Go to the start (Home)", #selector(goToStart(_:))),
            button("backward.frame.fill", "Step back (←; ⇧← by net)", #selector(stepBackward(_:))),
            playButton!,
            button("forward.frame.fill", "Step forward (→; ⇧→ by net)", #selector(stepForward(_:))),
            button("forward.end.fill", "Go to the end (End)", #selector(goToEnd(_:))),
        ]
        for kind in StepKind.allCases {
            stepPopup.addItem(withTitle: "Step by \(kind.name)")
            stepPopup.lastItem?.tag = Int(kind.rawValue)
        }
        stepPopup.controlSize = .small
        stepPopup.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        stepPopup.target = self
        stepPopup.action = #selector(stepKindChosen(_:))
        stepPopup.toolTip = "What ← and → step to"
        for mode in PacingMode.allCases {
            pacingPopup.addItem(withTitle: BoardWindowController.pacingTitle(mode))
            pacingPopup.lastItem?.tag = Int(mode.rawValue)
        }
        pacingPopup.controlSize = .small
        pacingPopup.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        pacingPopup.target = self
        pacingPopup.action = #selector(setPacingMode(_:))
        let slower = button("tortoise.fill", "Slower (⌘[)", #selector(slower(_:)))
        let faster = button("hare.fill", "Faster (⌘])", #selector(faster(_:)))
        speedLabel.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        speedLabel.alignment = .center
        speedLabel.translatesAutoresizingMaskIntoConstraints = false
        speedLabel.widthAnchor.constraint(equalToConstant: 40).isActive = true
        positionLabel.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        positionLabel.textColor = .secondaryLabelColor
        positionLabel.alignment = .right
        positionLabel.lineBreakMode = .byTruncatingHead
        positionLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let spacer = NSView()
        spacer.setContentHuggingPriority(.init(1), for: .horizontal)
        let stack = NSStackView(views: buttons + [stepPopup, slower, speedLabel, faster, pacingPopup, spacer,
                                                  positionLabel])
        stack.orientation = .horizontal
        stack.spacing = 4
        stack.setCustomSpacing(12, after: buttons.last!)
        stack.setCustomSpacing(12, after: stepPopup)
        stack.setCustomSpacing(12, after: faster)
        return stack
    }

    // MARK: - State

    /// The trace was (re)read: rebuild the panel and the timeline, fit the board.
    func reload() {
        panel.reload()
        timelineView.reload()
        boardView.reload()
        updateTransport()
    }

    /// Bring the views up to date after a playback call.
    func apply(_ changed: PlaybackChange) {
        if changed.contains(.board) { panel.reload() }
        if !changed.intersection([.board, .overlay, .wavefront]).isEmpty { boardView.refresh() }
        updateTransport()
    }

    /// Texts, the play button, the playhead and the clock.
    func updateTransport() {
        guard let p = try? session.position() else { return }
        timelineView.setTime(p.time)
        func set(_ field: NSTextField, _ text: String?) {
            if let text, field.stringValue != text { field.stringValue = text }
        }
        set(caption, try? session.text(.caption))
        set(positionLabel, try? session.text(.position))
        set(speedLabel, try? session.text(.speed))
        if let mode = try? session.pacing().mode, pacingPopup.selectedTag() != Int(mode.rawValue) {
            pacingPopup.selectItem(withTag: Int(mode.rawValue))
        }
        pacingPopup.toolTip = (try? session.text(.pacing)).map { "Pacing: \($0)" } ?? "Pacing"
        let symbol = p.playing ? "pause.fill" : "play.fill"
        if playButton.image?.accessibilityDescription != symbol {
            playButton.image = NSImage(systemSymbolName: symbol, accessibilityDescription: symbol)
        }
        if p.playing { startClock() } else { stopClock() }
    }

    var isPlaying: Bool { (try? session.position().playing) ?? false }
    var isClockRunning: Bool { link != nil && link?.isPaused == false }

    // MARK: - The clock

    private func startClock() {
        if link == nil {
            let l = boardView.displayLink(target: self, selector: #selector(tick(_:)))
            l.add(to: .main, forMode: .common)
            link = l
        }
        if link?.isPaused != false {
            lastTick = nil
            link?.isPaused = false
        }
    }

    private func stopClock() {
        link?.isPaused = true
        lastTick = nil
    }

    @objc private func tick(_ link: CADisplayLink) {
        let now = link.timestamp
        // Wall time since the last frame; a long stall plays at most 1/4 s.
        let dt = lastTick.map { min(max(now - $0, 0), 0.25) } ?? 0
        lastTick = now
        advanceClock(by: dt)
    }

    /// One frame of playback: dt seconds of wall time (the self-test calls
    /// this directly).
    func advanceClock(by dt: Double) {
        guard let changed = try? session.advance(by: dt) else { return }
        if changed.isEmpty { return }
        apply(changed)
    }

    private func scrub(_ phase: TimelineView.Scrub, to t: Double) {
        if phase == .began {
            resumeAfterScrub = isPlaying
            if resumeAfterScrub { _ = try? session.setPlaying(false) }
        }
        apply((try? session.seek(time: t)) ?? [])
        if phase == .ended, resumeAfterScrub {
            resumeAfterScrub = false
            apply((try? session.setPlaying(true)) ?? [])
        }
    }

    // MARK: - Playback menu and transport (reached through the responder chain)

    @objc func togglePlay(_ sender: Any?) {
        apply((try? session.setPlaying(!isPlaying)) ?? [])
    }

    @objc func playBackward(_ sender: Any?) {
        let backward = ((try? session.position().direction) ?? 1) < 0
        try? session.setDirection(backward ? 1 : -1)
        updateTransport()
    }

    @objc func stepForward(_ sender: Any?) { apply((try? session.step(stepKind, direction: 1)) ?? []) }
    @objc func stepBackward(_ sender: Any?) { apply((try? session.step(stepKind, direction: -1)) ?? []) }
    @objc func nextNet(_ sender: Any?) { apply((try? session.step(.net, direction: 1)) ?? []) }
    @objc func previousNet(_ sender: Any?) { apply((try? session.step(.net, direction: -1)) ?? []) }
    @objc func goToStart(_ sender: Any?) { apply((try? session.jump(toEnd: false)) ?? []) }
    @objc func goToEnd(_ sender: Any?) { apply((try? session.jump(toEnd: true)) ?? []) }

    @objc func faster(_ sender: Any?) {
        try? session.stepSpeed(by: 1)
        updateTransport()
    }

    @objc func slower(_ sender: Any?) {
        try? session.stepSpeed(by: -1)
        updateTransport()
    }

    /// Menu items carry the step kind in their tag.
    @objc func setStepKind(_ sender: Any?) {
        guard let item = sender as? NSMenuItem, let kind = StepKind(rawValue: UInt32(max(0, item.tag))) else { return }
        stepKind = kind
        stepPopup.selectItem(withTag: item.tag)
    }

    @objc private func stepKindChosen(_ sender: NSPopUpButton) {
        stepKind = StepKind(rawValue: UInt32(max(0, sender.selectedTag()))) ?? .event
    }

    // MARK: - Pacing (M4)

    static func pacingTitle(_ mode: PacingMode) -> String {
        switch mode {
        case .fit: return "Fit length"
        case .netsOnly: return "Nets only"
        case .oneNet: return "One net"
        case .skipFailed: return "Skip failed"
        case .realTime: return "Real time"
        }
    }

    /// The net "One net" would follow: the current event's (or the
    /// route_net in progress).
    var currentNet: Int32? { (try? session.position())?.net }

    /// Menu items and the transport popup carry the mode in their tag.
    @objc func setPacingMode(_ sender: Any?) {
        let tag = (sender as? NSMenuItem)?.tag ?? (sender as? NSPopUpButton)?.selectedTag() ?? 0
        guard let mode = PacingMode(rawValue: UInt32(max(0, tag))) else { return }
        if !setPacing(mode) { NSSound.beep() }
        updateTransport()
    }

    /// Change the pacing, keeping the event and the fraction of it; false
    /// if it could not be (One net with no net here).
    @discardableResult
    func setPacing(_ mode: PacingMode) -> Bool {
        guard var p = try? session.pacing() else { return false }
        if mode == .oneNet {
            guard let net = currentNet else { return false }
            p.net = net
        }
        p.mode = mode
        guard let changed = try? session.setPacing(p) else { return false }
        timelineView.reload()
        apply(changed)
        return true
    }

    /// Seek to a fraction of the run (the self-test).
    func seek(fraction: Double) {
        apply((try? session.seek(fraction: fraction)) ?? [])
    }

    func validateMenuItem(_ menuItem: NSMenuItem) -> Bool {
        let hasEvents = ((try? session.position().eventCount) ?? 0) > 0
        switch menuItem.action {
        case #selector(togglePlay(_:))?:
            menuItem.title = isPlaying ? "Pause" : "Play"
            return hasEvents
        case #selector(playBackward(_:))?:
            menuItem.state = ((try? session.position().direction) ?? 1) < 0 ? .on : .off
            return hasEvents
        case #selector(setStepKind(_:))?:
            menuItem.state = menuItem.tag == Int(stepKind.rawValue) ? .on : .off
            return true
        case #selector(setPacingMode(_:))?:
            let p = try? session.pacing()
            let on = p.map { menuItem.tag == Int($0.mode.rawValue) } ?? false
            menuItem.state = on ? .on : .off
            if menuItem.tag == Int(PacingMode.oneNet.rawValue) {
                let net = on ? p?.net : currentNet
                menuItem.title = net.map { "One Net (\($0))" } ?? "One Net"
                return hasEvents && (on || net != nil)
            }
            return hasEvents
        case #selector(stepForward(_:))?, #selector(stepBackward(_:))?, #selector(nextNet(_:))?,
             #selector(previousNet(_:))?, #selector(goToStart(_:))?, #selector(goToEnd(_:))?:
            return hasEvents
        default:
            return true
        }
    }

    // MARK: - View menu (reached through the responder chain)

    @objc func zoomIn(_ sender: Any?) { boardView.zoom(by: 1.25) }
    @objc func zoomOut(_ sender: Any?) { boardView.zoom(by: 0.8) }
    @objc func zoomToFit(_ sender: Any?) { boardView.zoomToFit() }

    func windowWillClose(_ notification: Notification) {
        link?.invalidate()
        link = nil
        // After the document has gone: show the welcome window if it was the last.
        DispatchQueue.main.async {
            (NSApp.delegate as? AppDelegate)?.showWelcomeIfIdle()
        }
    }
}
