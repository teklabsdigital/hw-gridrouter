// Session.swift: Swift wrapper over an rv_session (core/include/rv.h).
//
// The core is single-threaded, so the wrapper is main-actor isolated: every
// call into the core happens on the main thread.  Logic belongs in the core;
// this file only translates types.

import RVCore

/// A failure reported by the core.
public struct CoreError: Error, Sendable, CustomStringConvertible {
    /// The raw `rv_status` value.
    public let status: UInt32
    public let message: String

    public var description: String { "core error \(status): \(message)" }

    // rv_status values, so clients need not import RVCore.
    public static let invalidArgument: UInt32 = RV_E_INVALID_ARGUMENT.rawValue
    public static let noMemory: UInt32 = RV_E_NO_MEMORY.rawValue
    public static let internalError: UInt32 = RV_E_INTERNAL.rawValue
    public static let io: UInt32 = RV_E_IO.rawValue
    public static let format: UInt32 = RV_E_FORMAT.rawValue
    public static let unsupported: UInt32 = RV_E_UNSUPPORTED.rawValue
}

/// File-level facts about the open trace (`rv_trace_info`).
public struct TraceFacts: Sendable {
    public let headerComplete: Bool
    public let formatMajor: UInt32
    public let formatMinor: UInt32
    /// The END chunk has been read.
    public let complete: Bool
    /// Reading stopped at a corrupt chunk header.
    public let stopped: Bool
    /// The file shrank or was rewritten; refresh fails until it is reopened.
    public let failed: Bool
    public let fileSize: UInt64
    public let chunkCount: UInt32
    public let errorCount: UInt32
    public let warningCount: UInt32
    public let runCount: UInt32
    public let netCount: UInt32
    public let searchCount: UInt32
    public let routeCount: UInt32
    public let copperCount: UInt32
    public let expandedTotal: Int64
    public let traceEntries: Int64
    /// Seconds from opening the file to its last chunk.
    public let lastTime: Double
}

@MainActor
public final class Session {
    /// The interface version RVKit was compiled against (`RV_API_VERSION`).
    public nonisolated static let headerAPIVersion: UInt32 = RV_API_VERSION

    // Owned; destroyed in deinit.  nonisolated(unsafe) because deinit is not
    // main-actor isolated; the pointer is never shared, only destroyed there.
    private nonisolated(unsafe) let handle: OpaquePointer

    public init() throws(CoreError) {
        var out: OpaquePointer?
        let status = rv_create(&out)
        guard status == RV_OK, let out else {
            throw CoreError(status: status.rawValue, message: "rv_create failed")
        }
        handle = out
    }

    deinit {
        rv_destroy(handle)
    }

    /// The interface version of the linked core library.
    public var apiVersion: UInt32 { rv_api_version() }

    /// True when the linked core matches the header RVKit was built with.
    public var isCompatible: Bool { apiVersion == Session.headerAPIVersion }

    /// Message for the most recent failure, empty after a successful call.
    public var lastError: String { String(cString: rv_last_error(handle)) }

    private func check(_ status: rv_status) throws(CoreError) {
        guard status == RV_OK else {
            throw CoreError(status: status.rawValue, message: lastError)
        }
    }

    // MARK: - Trace

    /// Open a trace file.  On failure the previously open trace stays open.
    public func open(path: String) throws(CoreError) {
        try check(rv_open(handle, path))
    }

    /// Close the open trace, if any.
    public func close() {
        _ = rv_close(handle)
    }

    /// Read chunks appended since the last open or refresh; returns how many.
    @discardableResult
    public func refresh() throws(CoreError) -> UInt32 {
        var added: UInt32 = 0
        try check(rv_refresh(handle, &added))
        return added
    }

    /// File-level facts about the open trace.
    public func facts() throws(CoreError) -> TraceFacts {
        var i = rv_trace_info()
        try check(rv_info(handle, &i))
        return TraceFacts(
            headerComplete: i.header_complete != 0,
            formatMajor: i.version_major,
            formatMinor: i.version_minor,
            complete: i.complete != 0,
            stopped: i.stopped != 0,
            failed: i.failed != 0,
            fileSize: i.file_size,
            chunkCount: i.chunk_count,
            errorCount: i.error_count,
            warningCount: i.warning_count,
            runCount: i.run_count,
            netCount: i.net_count,
            searchCount: i.search_count,
            routeCount: i.route_count,
            copperCount: i.copper_count,
            expandedTotal: i.expanded_total,
            traceEntries: i.trace_entries,
            lastTime: i.last_time
        )
    }

    /// Router net numbers and their latest names, in ascending net number.
    public func netNames() throws(CoreError) -> [(code: Int32, name: String)] {
        let count = try facts().netCount
        var result: [(code: Int32, name: String)] = []
        result.reserveCapacity(Int(count))
        for index in 0..<count {
            var code: Int32 = 0
            var name: UnsafePointer<CChar>?
            try check(rv_net(handle, index, &code, &name))
            result.append((code: code, name: name.map { String(cString: $0) } ?? ""))
        }
        return result
    }

    // MARK: - Boards

    /// The BRD chunks of the trace (empty when none is open).
    public func boards() throws(CoreError) -> [BoardSummary] {
        var count: UInt32 = 0
        try check(rv_board_count(handle, &count))
        var result: [BoardSummary] = []
        for index in 0..<count {
            var b = rv_board_info()
            try check(rv_board(handle, index, &b))
            result.append(BoardSummary(index: index, label: b.label.map { String(cString: $0) } ?? "",
                                       layerCount: b.layer_count, footprintCount: b.footprint_count,
                                       recordCount: b.record_count))
        }
        return result
    }

    /// Draw BRD number `index`, or the latest board with nil.
    public func selectBoard(_ index: UInt32?) throws(CoreError) {
        try check(rv_board_select(handle, index ?? RV_NONE))
    }

    /// The BRD index drawn, nil if there is none.
    public func selectedBoard() throws(CoreError) -> UInt32? {
        var index: UInt32 = RV_NONE
        try check(rv_board_selected(handle, &index))
        return index == RV_NONE ? nil : index
    }

    // MARK: - Layers and object classes

    /// Layers of the board drawn, in KiCad's Appearance-panel order.
    public func layers() throws(CoreError) -> [BoardLayer] {
        var count: UInt32 = 0
        try check(rv_layer_count(handle, &count))
        var result: [BoardLayer] = []
        result.reserveCapacity(Int(count))
        for position in 0..<count {
            var l = rv_layer_info()
            try check(rv_layer(handle, position, &l))
            result.append(BoardLayer(name: l.name.map { String(cString: $0) } ?? "", index: l.index,
                                     kind: l.kind, side: l.side, rgba: l.rgba, visible: l.visible != 0,
                                     items: l.items))
        }
        return result
    }

    public func setLayerVisible(_ name: String, _ visible: Bool) throws(CoreError) {
        try check(rv_layer_set_visible(handle, name, visible ? 1 : 0))
    }

    /// The object classes (Zones, Tracks, ...) with their visibility.
    public func objectClasses() throws(CoreError) -> [ObjectClass] {
        var count: UInt32 = 0
        try check(rv_class_count(handle, &count))
        var result: [ObjectClass] = []
        for cls in 0..<count {
            var name: UnsafePointer<CChar>?
            var visible: UInt32 = 0
            try check(rv_class(handle, cls, &name, &visible))
            result.append(ObjectClass(id: cls, name: name.map { String(cString: $0) } ?? "", visible: visible != 0))
        }
        return result
    }

    public func setClassVisible(_ cls: UInt32, _ visible: Bool) throws(CoreError) {
        try check(rv_class_set_visible(handle, cls, visible ? 1 : 0))
    }

    // MARK: - Display list

    /// The current display list's generation, without copying it.
    public func displayListGeneration() throws(CoreError) -> UInt32 {
        var d = rv_display_list()
        try check(rv_get_display_list(handle, &d))
        return d.generation
    }

    /// A copy of the display list, or nil when its generation is `since`.
    public func displayList(ifChangedSince since: UInt32? = nil) throws(CoreError) -> DisplayList? {
        var d = rv_display_list()
        try check(rv_get_display_list(handle, &d))
        if let since, since == d.generation { return nil }
        return DisplayList(copying: d)
    }

    // MARK: - Camera

    public func setViewSize(width: Double, height: Double, backing: Double) throws(CoreError) {
        try check(rv_view_set_size(handle, width, height, backing))
    }

    public func zoomToFit() throws(CoreError) {
        try check(rv_view_fit(handle))
    }

    public func zoom(by factor: Double, aboutX x: Double, y: Double) throws(CoreError) {
        try check(rv_view_zoom(handle, factor, x, y))
    }

    public func pan(dx: Double, dy: Double) throws(CoreError) {
        try check(rv_view_pan(handle, dx, dy))
    }

    public func view() throws(CoreError) -> ViewTransform {
        var v = rv_view()
        try check(rv_view_get(handle, &v))
        return ViewTransform(scale: v.scale, tx: v.tx, ty: v.ty, width: v.width, height: v.height,
                             backing: v.backing, fitScale: v.fit_scale, zoomPercent: v.zoom_percent)
    }

    public func boardPoint(viewX: Double, viewY: Double) throws(CoreError) -> (x: Double, y: Double) {
        var x = 0.0, y = 0.0
        try check(rv_view_to_board(handle, viewX, viewY, &x, &y))
        return (x, y)
    }

    /// Status-line text for the cursor at a view point.
    public func statusText(viewX: Double, viewY: Double) throws(CoreError) -> String {
        var text: UnsafePointer<CChar>?
        try check(rv_view_status(handle, viewX, viewY, &text))
        return text.map { String(cString: $0) } ?? ""
    }

    // MARK: - Chunks

    /// The four-character tag of chunk `index` ("ROUT", ...).
    public func chunkTag(_ index: UInt32) throws(CoreError) -> String {
        var c = rv_chunk_info()
        try check(rv_chunk(handle, index, &c))
        return withUnsafeBytes(of: c.tag) { raw in
            String(decoding: raw.prefix(4), as: UTF8.self)
        }
    }

    /// One line describing chunk `index`.
    public func chunkText(_ index: UInt32) throws(CoreError) -> String {
        var text: UnsafePointer<CChar>?
        try check(rv_chunk_text(handle, index, &text))
        return text.map { String(cString: $0) } ?? ""
    }

    // MARK: - Timeline (M3)

    /// The timeline's generation (changes on open and refresh), without copying it.
    public func timelineGeneration() throws(CoreError) -> UInt32 {
        var t = rv_timeline()
        try check(rv_get_timeline(handle, &t))
        return t.generation
    }

    /// A copy of the timeline: events, ticks and phases.
    public func timeline() throws(CoreError) -> Timeline {
        var t = rv_timeline()
        try check(rv_get_timeline(handle, &t))
        var events: [TimelineEvent] = []
        events.reserveCapacity(Int(t.event_count))
        if let p = t.events {
            for e in UnsafeBufferPointer(start: p, count: Int(t.event_count)) {
                events.append(TimelineEvent(start: e.start, duration: e.duration, size: e.size,
                                            kind: TimelineEvent.Kind(rawValue: e.kind) ?? .end, flags: e.flags,
                                            chunk: e.chunk, lastChunk: e.last_chunk,
                                            phase: e.phase == RV_NONE ? nil : e.phase,
                                            board: e.board == RV_NONE ? nil : e.board))
            }
        }
        func copy(_ p: UnsafePointer<Double>?, _ n: UInt32) -> [Double] {
            guard let p, n > 0 else { return [] }
            return Array(UnsafeBufferPointer(start: p, count: Int(n)))
        }
        let rips = copy(t.rip_ticks, t.rip_tick_count)
        let failures = copy(t.failure_ticks, t.failure_tick_count)
        let total = t.total, routerSeconds = t.router_seconds, generation = t.generation
        var phases: [TimelinePhase] = []
        for index in 0..<t.phase_count {
            var p = rv_phase_info()
            try check(rv_phase(handle, index, &p))
            phases.append(TimelinePhase(start: p.start, end: p.end, label: p.label.map { String(cString: $0) } ?? "",
                                        event: p.event))
        }
        return Timeline(events: events, ripTicks: rips, failureTicks: failures, phases: phases, total: total,
                        routerSeconds: routerSeconds, generation: generation)
    }

    /// The playback position and what is shown there.
    public func position() throws(CoreError) -> PlaybackPosition {
        var p = rv_position()
        try check(rv_position_get(handle, &p))
        return PlaybackPosition(time: p.time, total: p.total, fraction: p.fraction, speed: p.speed,
                                routerTime: p.router_time, event: p.event == RV_NONE ? nil : p.event,
                                eventCount: p.event_count, playing: p.playing != 0, direction: p.direction,
                                atEnd: p.at_end != 0, cellsRevealed: p.cells_revealed, copperAlive: p.copper_alive,
                                ghosts: p.ghosts, failures: p.failures, routedHidden: p.routed_hidden != 0)
    }

    // MARK: - Playback (M3)

    /// Move the clock by `seconds` of wall time (nothing while paused).
    @discardableResult
    public func advance(by seconds: Double) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_advance(handle, seconds, &changed))
        return PlaybackChange(rawValue: changed)
    }

    @discardableResult
    public func setPlaying(_ playing: Bool) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_play(handle, playing ? 1 : 0, &changed))
        return PlaybackChange(rawValue: changed)
    }

    /// +1 forward, -1 backward.
    public func setDirection(_ direction: Int32) throws(CoreError) {
        try check(rv_set_direction(handle, direction))
    }

    /// Snaps to the nearest of the core's speeds.
    public func setSpeed(_ speed: Double) throws(CoreError) {
        try check(rv_set_speed(handle, speed))
    }

    /// Move along the core's list of speeds (clamped).
    public func stepSpeed(by steps: Int32) throws(CoreError) {
        try check(rv_speed_step(handle, steps))
    }

    @discardableResult
    public func seek(time: Double) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_seek_time(handle, time, &changed))
        return PlaybackChange(rawValue: changed)
    }

    @discardableResult
    public func seek(fraction: Double) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_seek_fraction(handle, fraction, &changed))
        return PlaybackChange(rawValue: changed)
    }

    /// To the start of event `index`.
    @discardableResult
    public func seek(event index: UInt32) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_seek_event(handle, index, &changed))
        return PlaybackChange(rawValue: changed)
    }

    /// To the start (false) or the end (true).
    @discardableResult
    public func jump(toEnd: Bool) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_jump(handle, toEnd ? 1 : 0, &changed))
        return PlaybackChange(rawValue: changed)
    }

    /// To the next (direction > 0) or previous event of a kind; pauses.
    /// Empty when there is none in that direction.
    @discardableResult
    public func step(_ kind: StepKind, direction: Int32) throws(CoreError) -> PlaybackChange {
        var changed: UInt32 = 0
        try check(rv_step(handle, kind.rawValue, direction, &changed))
        return PlaybackChange(rawValue: changed)
    }

    /// The caption, position readout or speed readout for the position.
    public func text(_ which: PlaybackText) throws(CoreError) -> String {
        var text: UnsafePointer<CChar>?
        try check(rv_text(handle, which.rawValue, &text))
        return text.map { String(cString: $0) } ?? ""
    }

    // MARK: - Overlay (M3)

    /// The overlay list's generation, without copying it.
    public func overlayGeneration() throws(CoreError) -> UInt32 {
        var d = rv_display_list()
        var slots: UnsafePointer<rv_slot>?
        var count: UInt32 = 0
        try check(rv_get_overlay(handle, &d, &slots, &count))
        return d.generation
    }

    /// A copy of the overlay list and its slots, or nil when its generation is `since`.
    public func overlay(ifChangedSince since: UInt32? = nil) throws(CoreError) -> Overlay? {
        var d = rv_display_list()
        var slots: UnsafePointer<rv_slot>?
        var count: UInt32 = 0
        try check(rv_get_overlay(handle, &d, &slots, &count))
        if let since, since == d.generation { return nil }
        var result: [OverlaySlot] = []
        if let slots {
            for s in UnsafeBufferPointer(start: slots, count: Int(count)) {
                result.append(OverlaySlot(kind: OverlaySlot.Kind(rawValue: s.kind) ?? .markers,
                                          orderBegin: s.order_begin, orderEnd: s.order_end,
                                          layer: s.layer == RV_NONE ? nil : s.layer,
                                          firstBatch: Int(s.first_batch), batchCount: Int(s.batch_count),
                                          generation: s.generation,
                                          bbox: (s.bbox.0, s.bbox.1, s.bbox.2, s.bbox.3)))
            }
        }
        return Overlay(list: DisplayList(copying: d), slots: result)
    }
}

// MARK: - Timeline and playback types (M3)

public struct TimelineEvent: Sendable {
    public enum Kind: UInt32, Sendable {
        case session = 0, snapshot, search, commit, via, ripUp, routeBegin, routeEnd, drop, phase, board, end

        /// The core's name of the kind ("search", ...).
        public var name: String { String(cString: rv_event_kind_name(rawValue)) }
    }
    public static let found: UInt32 = 1 << 0
    public static let failed: UInt32 = 1 << 1
    public static let error: UInt32 = 1 << 2

    public let start: Double
    public let duration: Double
    /// Searches: relative size 0...1 (log scale); else 0.
    public let size: Float
    public let kind: Kind
    /// RV_EVF_* bits (`found`, `failed`, `error` above).
    public let flags: UInt32
    public let chunk: UInt32
    public let lastChunk: UInt32
    public let phase: UInt32?
    public let board: UInt32?

    public var isFound: Bool { flags & TimelineEvent.found != 0 }
    public var isFailed: Bool { flags & (TimelineEvent.failed | TimelineEvent.error) != 0 }
}

public struct TimelinePhase: Sendable {
    public let start: Double
    public let end: Double
    public let label: String
    public let event: UInt32
}

public struct Timeline: Sendable {
    public let events: [TimelineEvent]
    public let ripTicks: [Double]
    public let failureTicks: [Double]
    public let phases: [TimelinePhase]
    /// Playback seconds of the whole run at 1x.
    public let total: Double
    public let routerSeconds: Double
    public let generation: UInt32
}

public struct PlaybackPosition: Sendable {
    public let time: Double
    public let total: Double
    public let fraction: Double
    public let speed: Double
    public let routerTime: Double
    public let event: UInt32?
    public let eventCount: UInt32
    public let playing: Bool
    public let direction: Int32
    public let atEnd: Bool
    public let cellsRevealed: UInt32
    public let copperAlive: UInt32
    public let ghosts: UInt32
    public let failures: UInt32
    public let routedHidden: Bool
}

/// What a playback call changed (RV_CHANGED_*).
public struct PlaybackChange: OptionSet, Sendable {
    public let rawValue: UInt32
    public init(rawValue: UInt32) { self.rawValue = rawValue }
    /// The playhead and the texts.
    public static let position = PlaybackChange(rawValue: 1)
    /// The overlay list.
    public static let overlay = PlaybackChange(rawValue: 2)
    /// The static display list (board or outline).
    public static let board = PlaybackChange(rawValue: 4)
}

public enum StepKind: UInt32, CaseIterable, Sendable {
    case event = 0, search, commit, net, ripUp, failure, phase

    /// The core's name of the kind ("event", "search", ...).
    public var name: String { String(cString: rv_step_kind_name(rawValue)) }
}

public enum PlaybackText: UInt32, Sendable {
    case caption = 0, position = 1, speed = 2
}

/// One overlay slot (`rv_slot`): a range of the order axis holding overlay batches.
public struct OverlaySlot: Sendable {
    public enum Kind: UInt32, Sendable {
        case routed = 0, drills = 1, markers = 2
    }
    public let kind: Kind
    public let orderBegin: UInt32
    public let orderEnd: UInt32
    /// Board layer index of a routed slot.
    public let layer: UInt32?
    public let firstBatch: Int
    public let batchCount: Int
    public let generation: UInt32
    /// min x, min y, max x, max y, mm; zeros when empty.
    public let bbox: (Float, Float, Float, Float)
}

/// A copy of the overlay list and its slots.
public struct Overlay: Sendable {
    public let list: DisplayList
    public let slots: [OverlaySlot]
}

// MARK: - Value types

public struct BoardSummary: Sendable {
    public let index: UInt32
    public let label: String
    public let layerCount: UInt32
    public let footprintCount: UInt32
    public let recordCount: UInt32
}

public struct BoardLayer: Sendable {
    public let name: String
    /// Position in the BRD layer table.
    public let index: UInt32
    public let kind: UInt32
    public let side: UInt32
    /// Theme colour, 0xRRGGBBAA.
    public let rgba: UInt32
    public let visible: Bool
    public let items: UInt32

    public var isCopper: Bool { kind == RV_LAYER_COPPER }
}

public struct ObjectClass: Sendable {
    public let id: UInt32
    public let name: String
    public let visible: Bool
}

/// The camera: view = board_mm * scale + (tx, ty), in points, y down.
public struct ViewTransform: Sendable, Equatable {
    public let scale: Double
    public let tx: Double
    public let ty: Double
    public let width: Double
    public let height: Double
    public let backing: Double
    public let fitScale: Double
    public let zoomPercent: Double
}

/// One batch of the display list (`rv_batch`).
public struct DisplayBatch: Sendable {
    public enum Primitive: UInt32, Sendable {
        case fill = 1, stroke = 2, segs = 3, disc = 4, ring = 5
    }
    public let primitive: Primitive
    public let objectClass: UInt32
    public let layer: UInt32
    public let order: UInt32
    public let rgba: UInt32
    public let key: UInt32
    public let width: Float
    public let first: Int
    public let count: Int
    /// min x, min y, max x, max y, mm.
    public let bbox: (Float, Float, Float, Float)

    /// The object class of router copper and ghosts (RV_OBJ_ROUTED).
    public static let routedClass: UInt32 = RV_OBJ_ROUTED.rawValue
}

/// A copy of the core's display list (see `rv_display_list` in rv.h).
public struct DisplayList: Sendable {
    public let batches: [DisplayBatch]
    public let xy: [Float]
    public let contours: [UInt32]
    public let polys: [UInt32]
    public let segs: [Float]
    public let circles: [Float]
    public let background: UInt32
    public let board: UInt32?
    public let extent: (Float, Float, Float, Float)?
    public let geometryGeneration: UInt32
    public let generation: UInt32

    init(copying d: rv_display_list) {
        func copy<T>(_ p: UnsafePointer<T>?, _ n: Int) -> [T] {
            guard let p, n > 0 else { return [] }
            return Array(UnsafeBufferPointer(start: p, count: n))
        }
        let raw = copy(d.batches, Int(d.batch_count))
        batches = raw.compactMap { b in
            guard let prim = DisplayBatch.Primitive(rawValue: b.prim) else { return nil }
            return DisplayBatch(primitive: prim, objectClass: b.cls, layer: b.layer, order: b.order, rgba: b.rgba,
                                key: b.key, width: b.width, first: Int(b.first), count: Int(b.count),
                                bbox: (b.bbox.0, b.bbox.1, b.bbox.2, b.bbox.3))
        }
        xy = copy(d.xy, Int(d.point_count) * 2)
        contours = copy(d.contours, Int(d.contour_count) * 2)
        polys = copy(d.polys, Int(d.poly_count) * 2)
        segs = copy(d.segs, Int(d.seg_count) * 4)
        circles = copy(d.circles, Int(d.circle_count) * 3)
        background = d.background
        board = d.board == RV_NONE ? nil : d.board
        extent = d.has_extent != 0 ? (d.extent.0, d.extent.1, d.extent.2, d.extent.3) : nil
        geometryGeneration = d.geometry_generation
        generation = d.generation
    }
}
