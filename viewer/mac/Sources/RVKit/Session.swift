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
}
