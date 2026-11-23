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
}
