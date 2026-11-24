// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// AppearancePanel.swift: KiCad-style Appearance panel.  The board picker,
// the layers (colour swatch and visibility checkbox, in the core's
// panel order) and the object classes.  Everything shown comes from the
// core; the panel only turns clicks into the core's visibility calls.

import AppKit
import RVKit

@MainActor
final class AppearancePanel: NSView {
    private let session: Session
    /// Called after a visibility change (`reloadBoard` false) or after
    /// another board was chosen (true).
    var onChange: ((_ reloadBoard: Bool) -> Void)?

    private let boardPopup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let stack = NSStackView()
    private let layerStack = NSStackView()
    private let classStack = NSStackView()

    init(session: Session) {
        self.session = session
        super.init(frame: NSRect(x: 0, y: 0, width: 240, height: 600))
        build()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    private func build() {
        let scroll = NSScrollView()
        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.borderType = .noBorder
        addSubview(scroll)

        let document = FlippedView()
        document.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = document

        for s in [stack, layerStack, classStack] {
            s.orientation = .vertical
            s.alignment = .leading
            s.translatesAutoresizingMaskIntoConstraints = false
        }
        stack.spacing = 6
        layerStack.spacing = 2
        classStack.spacing = 2
        document.addSubview(stack)

        boardPopup.target = self
        boardPopup.action = #selector(boardChosen(_:))
        boardPopup.controlSize = .small
        boardPopup.font = .systemFont(ofSize: NSFont.smallSystemFontSize)

        stack.addArrangedSubview(header("Board"))
        stack.addArrangedSubview(boardPopup)
        stack.setCustomSpacing(12, after: boardPopup)
        stack.addArrangedSubview(header("Layers"))
        stack.addArrangedSubview(layerStack)
        stack.setCustomSpacing(12, after: layerStack)
        stack.addArrangedSubview(header("Objects"))
        stack.addArrangedSubview(classStack)

        let clip = scroll.contentView
        NSLayoutConstraint.activate([
            scroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            scroll.topAnchor.constraint(equalTo: topAnchor),
            scroll.bottomAnchor.constraint(equalTo: bottomAnchor),
            document.leadingAnchor.constraint(equalTo: clip.leadingAnchor),
            document.trailingAnchor.constraint(equalTo: clip.trailingAnchor),
            document.topAnchor.constraint(equalTo: clip.topAnchor),
            stack.leadingAnchor.constraint(equalTo: document.leadingAnchor, constant: 10),
            stack.trailingAnchor.constraint(lessThanOrEqualTo: document.trailingAnchor, constant: -10),
            stack.topAnchor.constraint(equalTo: document.topAnchor, constant: 10),
            stack.bottomAnchor.constraint(equalTo: document.bottomAnchor, constant: -10),
            boardPopup.widthAnchor.constraint(equalToConstant: 210),
        ])
    }

    private func header(_ title: String) -> NSTextField {
        let label = NSTextField(labelWithString: title)
        label.font = .boldSystemFont(ofSize: NSFont.smallSystemFontSize)
        label.textColor = .secondaryLabelColor
        return label
    }

    /// Rebuild everything from the core: after opening, refreshing or
    /// choosing a board.
    func reload() {
        reloadBoards()
        reloadLayers()
        reloadClasses()
    }

    private func reloadBoards() {
        boardPopup.removeAllItems()
        let boards = (try? session.boards()) ?? []
        let selected = try? session.selectedBoard()
        // The default: the latest board at or before the playback position.
        boardPopup.addItem(withTitle: "Board at the playhead")
        boardPopup.lastItem?.tag = -1
        boardPopup.lastItem?.toolTip = "The latest board recorded at or before the playback position"
        for b in boards {
            boardPopup.addItem(withTitle: "\(b.index + 1): \(b.label.isEmpty ? "board" : b.label)")
            boardPopup.lastItem?.tag = Int(b.index)
            boardPopup.lastItem?.toolTip = "\(b.layerCount) layers, \(b.footprintCount) footprints, \(b.recordCount) records"
        }
        boardPopup.isEnabled = !boards.isEmpty
        // "At the playhead" stays chosen while the session follows the position.
        if followsLatest || selected == nil {
            boardPopup.selectItem(withTag: -1)
        } else if let selected {
            boardPopup.selectItem(withTag: Int(selected))
        }
    }

    private var followsLatest = true

    private func reloadLayers() {
        layerStack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        let layers = (try? session.layers()) ?? []
        if layers.isEmpty {
            layerStack.addArrangedSubview(note("No board"))
        }
        for layer in layers {
            let swatch = Swatch(rgba: layer.rgba)
            let box = NSButton(checkboxWithTitle: layer.name, target: self, action: #selector(layerToggled(_:)))
            box.controlSize = .small
            box.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            box.state = layer.visible ? .on : .off
            box.identifier = NSUserInterfaceItemIdentifier(layer.name)
            box.toolTip = layer.items == 1 ? "1 item" : "\(layer.items) items"
            let row = NSStackView(views: [swatch, box])
            row.orientation = .horizontal
            row.spacing = 6
            layerStack.addArrangedSubview(row)
        }
    }

    private func reloadClasses() {
        classStack.arrangedSubviews.forEach { $0.removeFromSuperview() }
        for cls in (try? session.objectClasses()) ?? [] {
            let box = NSButton(checkboxWithTitle: cls.name, target: self, action: #selector(classToggled(_:)))
            box.controlSize = .small
            box.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            box.state = cls.visible ? .on : .off
            box.tag = Int(cls.id)
            classStack.addArrangedSubview(box)
        }
    }

    private func note(_ text: String) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        label.textColor = .tertiaryLabelColor
        return label
    }

    // MARK: - Actions

    @objc private func boardChosen(_ sender: NSPopUpButton) {
        let tag = sender.selectedItem?.tag ?? -1
        followsLatest = tag < 0
        try? session.selectBoard(tag < 0 ? nil : UInt32(tag))
        reloadLayers()
        onChange?(true)
    }

    @objc private func layerToggled(_ sender: NSButton) {
        guard let name = sender.identifier?.rawValue else { return }
        try? session.setLayerVisible(name, sender.state == .on)
        onChange?(false)
    }

    @objc private func classToggled(_ sender: NSButton) {
        try? session.setClassVisible(UInt32(sender.tag), sender.state == .on)
        onChange?(false)
    }

    // MARK: - For the self-test

    var layerRowCount: Int { layerStack.arrangedSubviews.count }
    var classRowCount: Int { classStack.arrangedSubviews.count }
}

/// A layer's colour, as in KiCad's panel.
private final class Swatch: NSView {
    private let color: CGColor

    init(rgba: UInt32) {
        // Shown opaque: the panel shows the hue, not the drawing opacity.
        color = DisplayListRenderer.color(rgba | 0xFF)
        super.init(frame: NSRect(x: 0, y: 0, width: 14, height: 14))
        wantsLayer = true
        translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            widthAnchor.constraint(equalToConstant: 14),
            heightAnchor.constraint(equalToConstant: 14),
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    override var wantsUpdateLayer: Bool { true }

    override func updateLayer() {
        layer?.backgroundColor = color
        layer?.borderColor = NSColor.separatorColor.cgColor
        layer?.borderWidth = 1
        layer?.cornerRadius = 2
    }
}

final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}
