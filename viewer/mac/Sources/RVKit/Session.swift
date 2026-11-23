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
