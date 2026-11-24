// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// TraceDocument.swift: one open .grtrace file, read only.  The document owns
// a core session; its window shows the inspector (nets table and history,
// M5), the board, the caption, the timeline, the transport bar, the
// Appearance panel and the status line.

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
                // M6: a trace still being written is followed (live), and a
                // file with no header yet is waited for.
                try session.open(path: url.path(percentEncoded: false), flags: [.live, .wait])
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

    /// M6: the file grows while it is followed; the window's poll reads it.
    /// NSDocument must not offer to revert, nor warn that the file changed
    /// on disk (the document is read only and never saved).
    nonisolated override func presentedItemDidChange() {}

    /// The poll keeps the date NSDocument compares in step with the file.
    func noteFileChanged() {
        guard let path = fileURL?.path(percentEncoded: false),
              let date = (try? FileManager.default.attributesOfItem(atPath: path))?[.modificationDate] as? Date
        else { return }
        if fileModificationDate != date { fileModificationDate = date }
    }

    /// M6: open a trace whose file does not exist yet (the recorder has not
    /// created it): a document that waits for it.  `noteRecent` false for
    /// the self-test.
    @discardableResult
    static func openWaiting(_ url: URL, noteRecent: Bool = true) throws -> TraceDocument {
        let doc = TraceDocument()
        doc.fileURL = url
        doc.fileType = TraceDocument.typeName
        try doc.read(from: url, ofType: TraceDocument.typeName)
        NSDocumentController.shared.addDocument(doc)
        doc.makeWindowControllers()
        if noteRecent { NSDocumentController.shared.noteNewRecentDocumentURL(url) }
        return doc
    }

    static let typeName = "local.visual-gridrouter.grtrace"

    /// The window title: the file name, extension included.
    override var displayName: String! {
        get { fileURL?.lastPathComponent ?? super.displayName }
        set { super.displayName = newValue }
    }

    override func makeWindowControllers() {
        guard let session, windowControllers.isEmpty else { return }
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
    let inspector: InspectorPanel
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

    // M6: following a trace while it is written.
    /// The core's live text ("Live: growing, ..."); hidden for a trace that
    /// was complete when it was opened.
    let liveLabel = NSTextField(labelWithString: "")
    private var liveButton: NSButton!
    /// Polls the file every `pollInterval` seconds while it is not complete.
    private var pollTimer: Timer?
    static let pollInterval: TimeInterval = 0.25
    /// The clock handed to the core's poll (the self-test drives its own).
    var clock: () -> Double = { ProcessInfo.processInfo.systemUptime }
    /// The trace was not complete when it was (re)opened: show the live text.
    private var showsLive = false
    /// Polls made, and the time they took in the core and in the views (seconds).
    private(set) var pollCount = 0
    private(set) var pollSeconds: [Double] = []

    init(session: Session) {
        self.session = session
        boardView = BoardView(session: session)
        timelineView = TimelineView(session: session)
        panel = AppearancePanel(session: session)
        inspector = InspectorPanel(session: session)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1480, height: 880),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 960, height: 520)
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
        boardView.onSelect = { [weak self] changed in self?.selectionChanged(changed) }
        inspector.onSelect = { [weak self] changed in self?.selectionChanged(changed) }
        inspector.onSeek = { [weak self] event in
            guard let self else { return }
            self.apply((try? self.session.seek(event: event)) ?? [])
        }
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
        let leftDivider = NSBox()
        leftDivider.boxType = .separator
        status.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        caption.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        caption.lineBreakMode = .byTruncatingTail
        caption.cell?.truncatesLastVisibleLine = true
        caption.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        liveLabel.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        liveLabel.textColor = .secondaryLabelColor
        liveLabel.alignment = .right
        liveLabel.lineBreakMode = .byTruncatingTail
        liveLabel.isHidden = true
        liveLabel.translatesAutoresizingMaskIntoConstraints = false
        liveLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        message.font = .systemFont(ofSize: 15)
        message.textColor = NSColor(white: 0.75, alpha: 1)
        message.alignment = .center
        message.isHidden = true
        let transport = makeTransport()
        for v in [boardView, panel, bar, divider, status, caption, timelineView, transport, message, inspector,
                  leftDivider] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
        }
        content.addSubview(inspector)
        content.addSubview(leftDivider)
        let left = leftDivider.trailingAnchor
        content.addSubview(boardView)
        content.addSubview(message)
        content.addSubview(caption)
        content.addSubview(timelineView)
        content.addSubview(transport)
        content.addSubview(divider)
        content.addSubview(panel)
        content.addSubview(bar)
        bar.addSubview(status)
        bar.addSubview(liveLabel)
        NSLayoutConstraint.activate([
            liveLabel.trailingAnchor.constraint(equalTo: bar.trailingAnchor, constant: -8),
            liveLabel.centerYAnchor.constraint(equalTo: bar.centerYAnchor),
            liveLabel.leadingAnchor.constraint(greaterThanOrEqualTo: status.trailingAnchor, constant: 12),
            inspector.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            inspector.topAnchor.constraint(equalTo: content.topAnchor),
            inspector.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            inspector.widthAnchor.constraint(equalToConstant: 340),
            leftDivider.leadingAnchor.constraint(equalTo: inspector.trailingAnchor),
            leftDivider.topAnchor.constraint(equalTo: content.topAnchor),
            leftDivider.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            leftDivider.widthAnchor.constraint(equalToConstant: 1),
            boardView.leadingAnchor.constraint(equalTo: left),
            boardView.topAnchor.constraint(equalTo: content.topAnchor),
            boardView.bottomAnchor.constraint(equalTo: caption.topAnchor, constant: -4),
            boardView.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            message.centerXAnchor.constraint(equalTo: boardView.centerXAnchor),
            message.centerYAnchor.constraint(equalTo: boardView.centerYAnchor),
            caption.leadingAnchor.constraint(equalTo: left, constant: 8),
            caption.trailingAnchor.constraint(equalTo: divider.leadingAnchor, constant: -8),
            caption.heightAnchor.constraint(equalToConstant: 16),
            caption.bottomAnchor.constraint(equalTo: timelineView.topAnchor, constant: -3),
            timelineView.leadingAnchor.constraint(equalTo: left),
            timelineView.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            timelineView.heightAnchor.constraint(equalToConstant: 56),
            timelineView.bottomAnchor.constraint(equalTo: transport.topAnchor),
            transport.leadingAnchor.constraint(equalTo: left, constant: 6),
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
            bar.leadingAnchor.constraint(equalTo: left),
            bar.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
            bar.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            bar.heightAnchor.constraint(equalToConstant: 22),
            status.leadingAnchor.constraint(equalTo: bar.leadingAnchor, constant: 8),
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
        // M6: back to following a trace that is being written.
        liveButton = NSButton(title: "Live", target: self, action: #selector(goLive(_:)))
        liveButton.controlSize = .small
        liveButton.bezelStyle = .push
        liveButton.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
        liveButton.toolTip = "Follow the run as it is written (L or End)"
        liveButton.isHidden = true
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
        let stack = NSStackView(views: buttons + [liveButton, stepPopup, slower, speedLabel, faster, pacingPopup,
                                                  spacer, positionLabel])
        stack.detachesHiddenViews = true
        stack.orientation = .horizontal
        stack.spacing = 4
        stack.setCustomSpacing(12, after: buttons.last!)
        stack.setCustomSpacing(12, after: liveButton)
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
        inspector.reload()
        updateNetMarks()
        updateTransport()
        startLive()
    }

    // MARK: - Following a trace while it is written (M6)

    /// After an open or a reload: poll while the trace is not complete.
    private func startLive() {
        let info = try? session.live()
        showsLive = info?.needsPolling ?? false
        if showsLive { startPolling() } else { stopPolling() }
        updateLive()
    }

    private func startPolling() {
        guard pollTimer == nil else { return }
        let timer = Timer(timeInterval: BoardWindowController.pollInterval, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.poll() }
        }
        RunLoop.main.add(timer, forMode: .common)
        pollTimer = timer
    }

    private func stopPolling() {
        pollTimer?.invalidate()
        pollTimer = nil
    }

    var isPolling: Bool { pollTimer != nil }

    /// Read what the recorder appended and bring every view up to date (the
    /// timer calls this; the self-test too, with its own clock).
    func poll() {
        let t0 = DispatchTime.now().uptimeNanoseconds
        defer {
            pollCount += 1
            pollSeconds.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e9)
            updateLive()
        }
        let result: (added: UInt32, change: PlaybackChange)
        do throws(CoreError) {
            result = try session.poll(now: clock())
        } catch {
            // The file is another run now: read it from the start, keeping
            // the camera and the visibility (the core keeps them).
            if error.status == CoreError.replaced { reloadReplaced() }
            return
        }
        if result.added > 0 {
            (document as? TraceDocument)?.noteFileChanged()
            grew(result.change)
        } else if !result.change.isEmpty {
            apply(result.change)
        }
    }

    /// New chunks: the timeline grows (its events keep their places), the
    /// board and the overlay take the new lists, the nets table its rows;
    /// the position and the camera stay.
    private func grew(_ changed: PlaybackChange) {
        if changed.contains(.timeline) {
            timelineView.reload()
            updateNetMarks()
        }
        if changed.contains(.board) { panel.reload() }
        boardView.refresh()
        updateTransport()
        inspector.update(force: true)
    }

    /// The file was replaced by another run: open it again in place.
    private func reloadReplaced() {
        stopPolling()
        do throws(CoreError) {
            try session.reload()
        } catch {
            boardView.onMessage?("The trace could not be read again: \(error.message)")
        }
        panel.reload()
        timelineView.reload()
        boardView.refresh()
        boardView.reportStatus()
        inspector.reload()
        updateNetMarks()
        updateTransport()
        startLive()
    }

    /// The live text, the Live button; stop polling once complete.
    func updateLive() {
        guard let info = try? session.live() else { return }
        let text = showsLive ? ((try? session.liveText()) ?? "") : ""
        if liveLabel.stringValue != text { liveLabel.stringValue = text }
        liveLabel.isHidden = text.isEmpty
        liveLabel.textColor = info.state == .replaced || info.state == .failed || info.state == .stopped
            ? .systemRed : (info.following ? .labelColor : .secondaryLabelColor)
        let growing = info.live && (info.state == .growing || info.state == .stalled || info.state == .waiting)
        if liveButton.isHidden == growing { liveButton.isHidden = !growing }
        liveButton.state = info.following ? .on : .off
        liveButton.isEnabled = !info.following
        if !info.needsPolling && info.state != .replaced { stopPolling() }
    }

    @objc func goLive(_ sender: Any?) {
        apply((try? session.setFollowing(true)) ?? [])
        updateLive()
    }

    /// Bring the views up to date after a playback call.
    func apply(_ changed: PlaybackChange) {
        if changed.contains(.board) { panel.reload() }
        if changed.contains(.timeline) { timelineView.reload() }
        if !changed.intersection([.board, .overlay, .wavefront]).isEmpty { boardView.refresh() }
        updateTransport()
        inspector.update(playing: isPlaying)
        if showsLive && (changed.contains(.live) || changed.contains(.position)) { updateLive() }
    }

    /// The selection changed (a click on the board, a row of the table).
    func selectionChanged(_ changed: PlaybackChange) {
        apply(changed)
        inspector.update(force: true)
        updateNetMarks()
        boardView.reportStatus()
    }

    /// The timeline marks of the selected net's events.
    private func updateNetMarks() {
        let net = (try? session.selection())?.net
        timelineView.setNetEvents(net.flatMap { try? session.netEvents(net: $0) } ?? [])
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

    /// The timeline view's scrubbing (the self-test calls this too).  M6: a
    /// scrub stops following (the core's rule), and playback does not resume
    /// after it if it was following: the position stays where it was put.
    func scrub(_ phase: TimelineView.Scrub, to t: Double) {
        if phase == .began {
            let following = (try? session.live().following) ?? false
            resumeAfterScrub = isPlaying && !following
            if isPlaying { _ = try? session.setPlaying(false) }
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

    /// The net "One net" follows: the selected router net (M5), else the
    /// current event's (or the route_net in progress).
    var currentNet: Int32? { (try? session.selection())?.net ?? (try? session.position())?.net }

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
        case #selector(goLive(_:))?:
            guard let info = try? session.live() else { return false }
            menuItem.state = info.following ? .on : .off
            return info.live && (info.state == .growing || info.state == .stalled || info.state == .waiting)
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
        stopPolling()
        // After the document has gone: show the welcome window if it was the last.
        DispatchQueue.main.async {
            (NSApp.delegate as? AppDelegate)?.showWelcomeIfIdle()
        }
    }
}
