// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// InspectorPanel.swift: the nets table and the selected net's attempt
// history (M5).  Every row, cell, order and text comes from the core
// (rv_nets_*, rv_net_history); the panel only shows them and turns clicks
// into the core's selection and seek calls.
//
//   top:    a filter field and the nets table (click a column title to sort;
//           selecting a row selects that net)
//   middle: what is selected
//   bottom: the selected net's history (double-click a row: seek there)

import AppKit
import RVKit

@MainActor
final class InspectorPanel: NSView, NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate {
    private let session: Session
    /// Called after the panel changed the selection, with what changed.
    var onSelect: ((PlaybackChange) -> Void)?
    /// Called when a history row asks to seek to an event.
    var onSeek: ((UInt32) -> Void)?

    let filter = NSSearchField()
    let netsTable = NSTableView()
    let historyTable = NSTableView()
    private let selectionLabel = NSTextField(labelWithString: "No net selected")

    private var info: NetsInfo?
    private var history: [HistoryEntry] = []
    private var historyNet: Int32?
    private var historyGeneration: UInt32?
    /// Set while the panel moves the table's selection itself.
    private var syncing = false
    /// While playing, the table is refreshed at most this often (seconds).
    static let playingInterval = 0.25
    private var lastTableUpdate = 0.0

    /// Rows shown (for the self-test).
    var rowCount: Int { info?.viewCount ?? 0 }
    var historyCount: Int { history.count }

    init(session: Session) {
        self.session = session
        super.init(frame: NSRect(x: 0, y: 0, width: 320, height: 600))
        build()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) is not used")
    }

    private static func textCell(right: Bool) -> NSTextFieldCell {
        let cell = NSTextFieldCell(textCell: "")
        cell.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        cell.alignment = right ? .right : .left
        cell.lineBreakMode = .byTruncatingTail
        cell.isEditable = false
        return cell
    }

    private func build() {
        filter.placeholderString = "Filter nets"
        filter.controlSize = .small
        filter.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        filter.sendsSearchStringImmediately = true
        filter.delegate = self
        filter.target = self
        filter.action = #selector(filterChanged(_:))

        let small = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        for column in NetColumn.allCases {
            let c = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("\(column.rawValue)"))
            c.title = column.title
            c.headerCell.font = small
            c.headerCell.alignment = column.rightAligned ? .right : .left
            c.width = column == .name ? 150 : (column == .state ? 72 : 56)
            c.minWidth = 30
            c.sortDescriptorPrototype = NSSortDescriptor(key: "\(column.rawValue)", ascending: true)
            c.dataCell = InspectorPanel.textCell(right: column.rightAligned)
            netsTable.addTableColumn(c)
        }
        netsTable.dataSource = self
        netsTable.delegate = self
        netsTable.rowHeight = 16
        netsTable.usesAlternatingRowBackgroundColors = true
        netsTable.allowsMultipleSelection = false
        netsTable.allowsEmptySelection = true
        netsTable.columnAutoresizingStyle = .noColumnAutoresizing
        netsTable.style = .plain
        netsTable.sortDescriptors = [NSSortDescriptor(key: "\(NetColumn.name.rawValue)", ascending: true)]

        let h = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("history"))
        h.title = "History"
        h.headerCell.font = small
        h.width = 600
        h.dataCell = InspectorPanel.textCell(right: false)
        historyTable.addTableColumn(h)
        historyTable.headerView = nil
        historyTable.dataSource = self
        historyTable.delegate = self
        historyTable.rowHeight = 16
        historyTable.style = .plain
        historyTable.columnAutoresizingStyle = .noColumnAutoresizing
        historyTable.target = self
        historyTable.doubleAction = #selector(historyDoubleClicked(_:))

        selectionLabel.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
        selectionLabel.lineBreakMode = .byTruncatingTail
        selectionLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let netsScroll = NSScrollView()
        netsScroll.documentView = netsTable
        netsScroll.hasVerticalScroller = true
        netsScroll.hasHorizontalScroller = true
        let historyScroll = NSScrollView()
        historyScroll.documentView = historyTable
        historyScroll.hasVerticalScroller = true
        historyScroll.hasHorizontalScroller = true
        let hint = NSTextField(labelWithString: "Double-click a line to go to its event")
        hint.font = .systemFont(ofSize: NSFont.smallSystemFontSize - 1)
        hint.textColor = .secondaryLabelColor

        for v in [filter, netsScroll, selectionLabel, historyScroll, hint] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            addSubview(v)
        }
        NSLayoutConstraint.activate([
            filter.topAnchor.constraint(equalTo: topAnchor, constant: 8),
            filter.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            filter.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            netsScroll.topAnchor.constraint(equalTo: filter.bottomAnchor, constant: 6),
            netsScroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            netsScroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            netsScroll.heightAnchor.constraint(equalTo: heightAnchor, multiplier: 0.48),
            selectionLabel.topAnchor.constraint(equalTo: netsScroll.bottomAnchor, constant: 6),
            selectionLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            selectionLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            historyScroll.topAnchor.constraint(equalTo: selectionLabel.bottomAnchor, constant: 4),
            historyScroll.leadingAnchor.constraint(equalTo: leadingAnchor),
            historyScroll.trailingAnchor.constraint(equalTo: trailingAnchor),
            hint.topAnchor.constraint(equalTo: historyScroll.bottomAnchor, constant: 2),
            hint.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 8),
            hint.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -4),
        ])
    }

    // MARK: - Updating

    /// The trace was (re)read: everything again.
    func reload() {
        info = nil
        historyNet = nil
        historyGeneration = nil
        update(force: true)
    }

    /// Bring the table, the selection line and the history to the session.
    /// While playing (force false) the table is refreshed at most every
    /// `playingInterval` seconds; the selection always at once.
    func update(force: Bool = false, playing: Bool = false) {
        let now = ProcessInfo.processInfo.systemUptime
        let sel = try? session.selection()
        updateSelection(sel)
        if !force && playing && now - lastTableUpdate < InspectorPanel.playingInterval { return }
        lastTableUpdate = now
        guard let fresh = try? session.netsInfo() else { return }
        let reload = force || info?.generation != fresh.generation || info?.viewCount != fresh.viewCount
        info = fresh
        if reload { netsTable.reloadData() }
        syncTableSelection()
    }

    private func updateSelection(_ sel: NetSelection?) {
        let text: String
        if let sel {
            let name = sel.name.isEmpty ? "unnamed net" : sel.name
            let router = sel.net.map { "router net \($0)" } ?? "no router net"
            text = "\(name) · \(router) · \(sel.boardItems) board items"
        } else {
            text = "No net selected · click a net on the board or in the table"
        }
        if selectionLabel.stringValue != text { selectionLabel.stringValue = text }
        // The history depends on the net and on the trace (refresh), not on the position.
        let net = sel?.net
        let generation = try? session.timelineGeneration()
        if net != historyNet || generation != historyGeneration {
            historyNet = net
            historyGeneration = generation
            history = net.flatMap { try? session.history(net: $0) } ?? []
            historyTable.reloadData()
        }
    }

    /// Show the selected net's row as the table's selection.
    private func syncTableSelection() {
        let want = info?.selectedPosition
        let have = netsTable.selectedRow >= 0 ? netsTable.selectedRow : nil
        guard want != have else { return }
        syncing = true
        if let want {
            netsTable.selectRowIndexes(IndexSet(integer: want), byExtendingSelection: false)
            netsTable.scrollRowToVisible(want)
        } else {
            netsTable.deselectAll(nil)
        }
        syncing = false
    }

    // MARK: - Actions

    @objc private func filterChanged(_ sender: Any?) {
        applyView()
    }

    private func applyView() {
        let d = netsTable.sortDescriptors.first
        let column = d.flatMap { UInt32($0.key ?? "") }.flatMap(NetColumn.init(rawValue:)) ?? .name
        try? session.setNetsView(column: column, descending: !(d?.ascending ?? true), filter: filter.stringValue)
        update(force: true)
    }

    @objc private func historyDoubleClicked(_ sender: Any?) {
        let row = historyTable.clickedRow
        guard row >= 0, row < history.count, let event = history[row].event else { return }
        onSeek?(event)
    }

    /// Select the net of the table's row `position` (the self-test calls this too).
    func selectRow(_ position: Int) {
        guard let row = try? session.netRow(at: position),
              let changed = try? session.selectNet(row.net) else { return }
        update(force: true)
        onSelect?(changed)
    }

    // MARK: - Table data

    func numberOfRows(in tableView: NSTableView) -> Int {
        tableView === netsTable ? (info?.viewCount ?? 0) : history.count
    }

    // Cell-based (NSTextFieldCell) rather than view-based: a view-based
    // table of 18 columns made selecting a net take some 30 ms of AppKit
    // layout on the dongle; cells draw the same text for a fraction of it.
    func tableView(_ tableView: NSTableView, objectValueFor tableColumn: NSTableColumn?, row: Int) -> Any? {
        if tableView === netsTable {
            let column = tableColumn.flatMap { UInt32($0.identifier.rawValue) }.flatMap(NetColumn.init(rawValue:))
                ?? .name
            return (try? session.netCell(at: row, column: column)) ?? ""
        }
        guard row < history.count else { return "" }
        let e = history[row]
        return String(repeating: "    ", count: e.depth) + e.text
    }

    func tableView(_ tableView: NSTableView, willDisplayCell cell: Any, for tableColumn: NSTableColumn?, row: Int) {
        guard tableView === historyTable, row < history.count, let cell = cell as? NSTextFieldCell else { return }
        let e = history[row]
        cell.textColor = e.event == nil ? .secondaryLabelColor
            : (e.flags & (TimelineEvent.failed | TimelineEvent.error) != 0 ? .systemRed : .labelColor)
    }

    func tableView(_ tableView: NSTableView, sortDescriptorsDidChange oldDescriptors: [NSSortDescriptor]) {
        guard tableView === netsTable else { return }
        applyView()
    }

    func tableViewSelectionDidChange(_ notification: Notification) {
        guard !syncing, (notification.object as? NSTableView) === netsTable else { return }
        let row = netsTable.selectedRow
        if row < 0 {
            if let changed = try? session.clearSelection() {
                update(force: true)
                onSelect?(changed)
            }
        } else {
            selectRow(row)
        }
    }
}
