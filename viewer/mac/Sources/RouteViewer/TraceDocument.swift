// TraceDocument.swift: one open .grtrace file, read only.  The document owns
// a core session; its window shows the board, the Appearance panel and the
// status line.

import AppKit
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
final class BoardWindowController: NSWindowController, NSWindowDelegate {
    let session: Session
    let boardView: BoardView
    let panel: AppearancePanel
    private let status = NSTextField(labelWithString: "")

    init(session: Session) {
        self.session = session
        boardView = BoardView(session: session)
        panel = AppearancePanel(session: session)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1200, height: 800),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 560, height: 360)
        super.init(window: window)
        window.delegate = self
        layout(in: window)
        window.initialFirstResponder = boardView
        window.center()
        shouldCascadeWindows = true

        boardView.onStatus = { [weak self] text in self?.status.stringValue = text }
        panel.onChange = { [weak self] reloadBoard in
            guard let self else { return }
            if reloadBoard { self.boardView.reload() } else { self.boardView.displayListChanged() }
        }
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    private func layout(in window: NSWindow) {
        let content = NSView()
        let bar = NSView()
        let divider = NSBox()
        divider.boxType = .separator
        status.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        for v in [boardView, panel, bar, divider, status] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
        }
        content.addSubview(boardView)
        content.addSubview(divider)
        content.addSubview(panel)
        content.addSubview(bar)
        bar.addSubview(status)
        NSLayoutConstraint.activate([
            boardView.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            boardView.topAnchor.constraint(equalTo: content.topAnchor),
            boardView.bottomAnchor.constraint(equalTo: bar.topAnchor),
            boardView.trailingAnchor.constraint(equalTo: divider.leadingAnchor),
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

    /// The trace was (re)read: rebuild the panel and fit the board.
    func reload() {
        panel.reload()
        boardView.reload()
    }

    // MARK: - View menu (reached through the responder chain)

    @objc func zoomIn(_ sender: Any?) { boardView.zoom(by: 1.25) }
    @objc func zoomOut(_ sender: Any?) { boardView.zoom(by: 0.8) }
    @objc func zoomToFit(_ sender: Any?) { boardView.zoomToFit() }

    func windowWillClose(_ notification: Notification) {
        // After the document has gone: show the welcome window if it was the last.
        DispatchQueue.main.async {
            (NSApp.delegate as? AppDelegate)?.showWelcomeIfIdle()
        }
    }
}
