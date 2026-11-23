// SelfTestLive.swift: the self-test's live phase (M6).  The self-test's
// trace is written again, piece by piece, into a file that does not exist
// when its window opens; a clock the test drives stands in for wall time,
// so the run is the same every time.  Checked through the window's own
// paths (its poll, its clock, its scrub, its Live button):
//
//   - a missing file is waited for, then followed as it grows;
//   - following plays forward, entering at most one new search per frame
//     and showing each with its wavefront settled, unless it is more than
//     RV_FOLLOW_LAG behind (then it jumps; counted, not a failure);
//   - a poll never moves the playhead, nor the current event;
//   - the selection survives the growth;
//   - a scrub back stops following, and the position stays put while the
//     file grows; Live resumes following, at the end;
//   - at END following stops, playback pauses and polling ends;
//   - the result equals a fresh open of the same bytes: the timeline, the
//     nets table, and the board drawn at the same position and camera
//     (0 px); the layer tree equals a single-pass render (0 px);
//   - a replaced file reloads, keeping the camera and the visibility, and
//     a file that stops growing shows as stalled.

import AppKit
import RVKit

@MainActor
enum SelfTestLive {
    /// Run the phase on a copy of `source`; returns the failures.
    static func run(source path: String) -> [String] {
        var failures: [String] = []
        guard let bytes = FileManager.default.contents(atPath: path), bytes.count > 24 else {
            return ["live: cannot read \(path)"]
        }
        let fm = FileManager.default
        let dir = fm.temporaryDirectory.appendingPathComponent("visual-gridrouter-selftest-\(getpid())")
        try? fm.removeItem(at: dir)
        do {
            try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        } catch {
            return ["live: cannot make \(dir.path): \(error.localizedDescription)"]
        }
        defer { try? fm.removeItem(at: dir) }
        let url = dir.appendingPathComponent("live.grtrace")
        let filePath = url.path(percentEncoded: false)
        let t0 = DispatchTime.now().uptimeNanoseconds

        // The file does not exist yet: a window that waits for it.
        let doc: TraceDocument
        do {
            doc = try TraceDocument.openWaiting(url, noteRecent: false)
        } catch {
            return ["live: opening a file that does not exist yet: \(error.localizedDescription)"]
        }
        for controller in doc.windowControllers { controller.window?.isRestorable = false }
        doc.showWindows()
        defer { doc.close() }
        guard let c = doc.windowControllers.first as? BoardWindowController else { return ["live: no window"] }
        let s = c.session
        var now = 1000.0
        c.clock = { now }
        func fail(_ text: String) { failures.append("live: " + text) }

        if !c.isPolling { fail("not polling a file that does not exist yet") }
        c.poll()
        if (try? s.live())?.state != .waiting { fail("a missing file is not waiting: \(String(describing: try? s.live()))") }
        let waitingText = c.liveLabel.stringValue
        if waitingText.isEmpty || c.liveLabel.isHidden { fail("no live text while waiting") }

        // Write the file piece by piece; each piece, one poll and 1/4 s of
        // frames.  About 60 pieces whatever the size.
        guard fm.createFile(atPath: filePath, contents: nil), let handle = FileHandle(forWritingAtPath: filePath) else {
            return failures + ["live: cannot write \(filePath)"]
        }
        var written = 0
        func write(upTo n: Int) {
            let end = min(n, bytes.count)
            guard end > written else { return }
            handle.write(bytes.subdata(in: written..<end))
            written = end
        }
        let piece = max(4096, bytes.count / 60)
        let framesPerPoll = 4
        let frame = BoardWindowController.pollInterval / Double(framesPerPoll)

        // Following: the searches entered, each seen settled.
        var settledSeen = Set<UInt32>()
        var lastEvent: UInt32?
        var lastSkips: UInt32 = 0
        var followedFrames = 0, pollMoves = 0, spanSteps = 0, entered = 0, maxEntered = 0
        var everFollowing = false
        var worstPoll: (ms: Double, chunks: Int, poll: Int, events: Int) = (0, 0, 0, 0)

        func events() -> [TimelineEvent] { c.timelineView.timeline?.events ?? [] }
        func isTimedSearch(_ e: TimelineEvent) -> Bool { e.kind == .search && e.duration > 0 }

        func observeFrame() {
            guard let p = try? s.position(), let l = try? s.live() else { return }
            if l.following { everFollowing = true }
            if let e = p.event, p.settled { settledSeen.insert(e) }
            defer {
                lastEvent = p.event
                lastSkips = l.skips
            }
            guard l.following, l.skips == lastSkips, let from = lastEvent, let to = p.event, to > from else { return }
            followedFrames += 1
            let ev = events()
            var newSearches = 0
            for k in Int(from)..<min(Int(to), ev.count) where isTimedSearch(ev[k]) {
                if k > Int(from) { newSearches += 1 }
                if !settledSeen.contains(UInt32(k)) {
                    fail("following passed search event \(k) without showing it settled (frame to event \(to))")
                }
            }
            if to < ev.count, isTimedSearch(ev[Int(to)]) { newSearches += 1 }
            entered += newSearches
            maxEntered = max(maxEntered, newSearches)
            if newSearches > 1 { fail("following entered \(newSearches) searches in one frame (events \(from) to \(to))") }
        }

        /// One piece, one poll, a quarter of a second of frames.
        func step(write more: Bool = true) {
            if more { write(upTo: written + piece) }
            now += BoardWindowController.pollInterval
            let before = try? s.position()
            let xBefore = c.timelineView.playheadX
            let spanBefore = c.timelineView.timeline?.span
            let chunksBefore = (try? s.live().chunks) ?? 0
            let p0 = DispatchTime.now().uptimeNanoseconds
            c.poll()
            let pollMs = Double(DispatchTime.now().uptimeNanoseconds - p0) / 1e6
            if pollMs > worstPoll.ms {
                let l = try? s.live()
                worstPoll = (pollMs, Int((l?.chunks ?? 0) - min(chunksBefore, l?.chunks ?? 0)), c.pollCount,
                             Int(l?.events ?? 0))
            }
            let after = try? s.position()
            // At END the run is refitted: the event and the fraction of it stay.
            let refit = (try? s.live())?.state == .complete
            if let b = before, let a = after, b.eventCount > 0 {
                // At the end, the end of the last event becomes the start of
                // the next: the core keeps the event by stepping one ulp back.
                let moved = b.atEnd ? abs(a.time - b.time) > 1e-9 : a.time != b.time
                if a.event != b.event || (refit ? abs(a.fraction - b.fraction) > 1e-6 : moved) {
                    pollMoves += 1
                    fail("a poll moved the position: t \(b.time) event \(String(describing: b.event)) -> "
                         + "t \(a.time) event \(String(describing: a.event))")
                }
                if refit || c.timelineView.timeline?.span != spanBefore {
                    spanSteps += 1
                } else if abs(c.timelineView.playheadX - xBefore) > 0.5 {
                    fail("a poll moved the playhead from x \(xBefore) to \(c.timelineView.playheadX)")
                }
            }
            for _ in 0..<framesPerPoll {
                c.advanceClock(by: frame)
                observeFrame()
            }
        }

        // The header, then part of a chunk.
        write(upTo: 24 + 10)
        step(write: false)
        if (try? s.live())?.state == .replaced { fail("a header written alone reads as replaced") }

        // Grow to 30% following, select a net, grow to 50%.
        while written < bytes.count * 3 / 10 { step() }
        if !everFollowing && ((try? s.live().events) ?? 0) > 0 { fail("not following a growing trace") }
        var selected: Int32?
        if let info = try? s.netsInfo(), info.viewCount > 0 {
            c.inspector.selectRow(0)
            selected = (try? s.selection())?.net
        }
        while written < bytes.count / 2 { step() }
        if let selected, (try? s.selection())?.net != selected {
            fail("the selection (net \(selected)) did not survive the growth")
        }

        // Scrub back: following stops and the position stays while the file grows.
        var scrubbed = "not checked (no events yet)"
        if let p = try? s.position(), p.eventCount > 0, p.time > 0 {
            c.scrub(.began, to: p.time * 0.4)
            c.scrub(.ended, to: p.time * 0.4)
            let q = try? s.position()
            if (try? s.live().following) == true { fail("a scrub back did not stop following") }
            if q?.playing == true { fail("playback resumed after a scrub that stopped following") }
            for _ in 0..<5 { step() }
            let r = try? s.position()
            if r?.time != q?.time || r?.event != q?.event {
                fail("the position moved while not following: \(String(describing: q?.time)) -> \(String(describing: r?.time))")
            }
            // Live: following again, from the end.
            c.goLive(nil)
            let l = try? s.live(), e = try? s.position()
            // Live jumps to the newest event: not a following step.
            lastEvent = e?.event
            lastSkips = l?.skips ?? lastSkips
            if l?.following != true || e?.playing != true { fail("Live did not resume following") }
            if let e, e.time != e.total { fail("Live did not go to the end (t \(e.time) of \(e.total))") }
            scrubbed = "stopped following; stayed at t \(String(format: "%.2f", q?.time ?? 0)) over 5 polls; Live resumed"
        }

        // To the end of the file, then until following ends.
        while written < bytes.count { step() }
        handle.closeFile()
        var tail = 0
        while ((try? s.live().following) ?? false) && tail < 2000 {
            step(write: false)
            tail += 1
        }
        let end = try? s.live()
        let pe = try? s.position()
        if end?.state != .complete { fail("state \(String(describing: end?.state)) after END") }
        if end?.following == true { fail("still following after END") }
        if pe?.playing == true || c.isPlaying { fail("still playing after END") }
        if everFollowing && pe?.atEnd != true { fail("following did not end at the end (t \(pe?.time ?? -1))") }
        if c.isPolling { fail("still polling a complete trace") }
        let completeText = c.liveLabel.stringValue
        if !completeText.hasPrefix("Complete") { fail("live text at the end: \"\(completeText)\"") }

        // The same as a fresh open of the same bytes.
        failures += compareFresh(c, path: filePath)

        // Replaced: the file becomes another run (the first half of this
        // one, no END); the window reloads, keeping camera and visibility.
        let layerName = (try? s.layers())?.first?.name
        if let layerName { try? s.setLayerVisible(layerName, false) }
        c.boardView.zoom(by: 1.3, about: NSPoint(x: c.boardView.bounds.width * 0.3, y: c.boardView.bounds.height * 0.6))
        c.boardView.flushRefresh()
        let camera = try? s.view()
        let other = dir.appendingPathComponent("other.grtrace")
        let half = bytes.subdata(in: 0..<(bytes.count / 2))
        var replaced = "not checked"
        var reloadMs = 0.0
        do {
            try half.write(to: other)
            _ = try fm.replaceItemAt(url, withItemAt: other)
            now += BoardWindowController.pollInterval
            let r0 = DispatchTime.now().uptimeNanoseconds
            c.poll()
            reloadMs = Double(DispatchTime.now().uptimeNanoseconds - r0) / 1e6
            let l = try? s.live()
            let fresh = try? Session()
            try? fresh?.open(path: filePath)
            let want = (try? fresh?.timeline().events.count) ?? -1
            let have = c.timelineView.timeline?.events.count ?? -2
            if l?.state == .replaced || l?.state == .complete { fail("after replacement the state is \(String(describing: l?.state))") }
            if have != want { fail("after replacement the timeline has \(have) events, a fresh open \(want)") }
            if (try? s.view()) != camera { fail("the camera changed on reload") }
            if let layerName, (try? s.layers())?.first(where: { $0.name == layerName })?.visible != false {
                fail("layer \(layerName) visible again after the reload")
            }
            if !c.isPolling { fail("not polling the replacement") }
            // No growth for longer than RV_LIVE_STALL_SECONDS: stalled.
            for _ in 0..<24 { step(write: false) }
            let stalled = try? s.live()
            if stalled?.state != .stalled { fail("a file that stopped growing is \(String(describing: stalled?.state))") }
            if !c.liveLabel.stringValue.contains("stalled") { fail("stalled text: \"\(c.liveLabel.stringValue)\"") }
            replaced = String(format: "reloaded (%d events) in %.1f ms, camera and visibility kept; then \"%@\"",
                              have, reloadMs, c.liveLabel.stringValue)
        } catch {
            fail("replacing the file: \(error.localizedDescription)")
        }
        if let layerName { try? s.setLayerVisible(layerName, true) }

        let ms = Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
        let polls = c.pollSeconds.sorted()
        let median = polls.isEmpty ? 0 : polls[polls.count / 2] * 1000
        let worst = (polls.last ?? 0) * 1000
        print(String(format: "selftest: live: %d bytes in pieces of %d, %d polls (median %.2f ms, worst %.2f ms "
                        + "with the views), %d frames following, %d searches entered (at most %d a frame), "
                        + "%d skips, %d span steps, %d poll moves; %.0f ms",
                     bytes.count, piece, c.pollCount, median, worst, followedFrames, entered, maxEntered,
                     Int(end?.skips ?? 0), spanSteps, pollMoves, ms))
        print(String(format: "selftest: live: slowest poll while growing, with the views: %.2f ms (poll %d, %d chunks, "
                        + "%d events then)", worstPoll.ms, worstPoll.poll, worstPoll.chunks, worstPoll.events))
        print("selftest: live: waiting \"\(waitingText)\"; at END \"\(completeText)\"")
        print("selftest: live: scrub back: \(scrubbed)")
        print("selftest: live: replacement: \(replaced)")
        return failures
    }

    /// The live window against a fresh session on the same file: the
    /// timeline, the nets table, and pictures at the same position and camera.
    private static func compareFresh(_ c: BoardWindowController, path: String) -> [String] {
        var failures: [String] = []
        func fail(_ text: String) { failures.append("live: " + text) }
        let s = c.session
        guard let fresh = try? Session() else { return ["live: no fresh session"] }
        do throws(CoreError) {
            try fresh.open(path: path)
        } catch {
            return ["live: fresh open: \(error)"]
        }
        // Timeline.
        if let a = try? s.timeline(), let b = try? fresh.timeline() {
            if a.events.count != b.events.count || a.total != b.total || a.ripTicks != b.ripTicks
                || a.failureTicks != b.failureTicks || a.phases.count != b.phases.count {
                fail("timeline: \(a.events.count) events, total \(a.total); fresh \(b.events.count), \(b.total)")
            } else {
                for (k, (x, y)) in zip(a.events, b.events).enumerated()
                where x.start != y.start || x.duration != y.duration || x.kind != y.kind || x.chunk != y.chunk
                    || x.lastChunk != y.lastChunk || x.size != y.size || x.flags != y.flags || x.net != y.net {
                    fail("timeline event \(k) differs from a fresh open")
                    break
                }
            }
        } else {
            fail("no timeline to compare")
        }
        // Nets table, every cell (the live window's view: same sort and filter).
        _ = try? s.clearSelection()
        try? s.setNetsView(column: .name, descending: false, filter: "")
        try? fresh.setNetsView(column: .name, descending: false, filter: "")
        let ia = try? s.netsInfo(), ib = try? fresh.netsInfo()
        if ia?.viewCount != ib?.viewCount || ia?.rowCount != ib?.rowCount {
            fail("nets table: \(ia?.viewCount ?? -1) rows, fresh \(ib?.viewCount ?? -1)")
        } else if let n = ia?.viewCount {
            outer: for p in 0..<n {
                for col in NetColumn.allCases where (try? s.netCell(at: p, column: col)) != (try? fresh.netCell(at: p, column: col)) {
                    fail("nets table row \(p) column \(col.title) differs from a fresh open")
                    break outer
                }
            }
        }
        c.inspector.update(force: true)
        // Pictures: the same camera, at the middle of a search in the second
        // half, then at the end.
        let view = c.boardView
        view.zoomToFit()
        view.flushRefresh()
        let saved = view.layered.bands
        view.layered.bands = 1
        view.layered.invalidate()
        defer {
            view.layered.bands = saved
            view.layered.invalidate()
        }
        let freshRenderer = LayeredRenderer()
        freshRenderer.bands = 1
        let events = c.timelineView.timeline?.events ?? []
        let mid = events.indices.first { events[$0].kind == .search && events[$0].duration > 0
            && events[$0].start >= (c.timelineView.timeline?.total ?? 0) / 2 }
        var times: [Double] = []
        if let mid { times.append(events[mid].start + events[mid].duration * 0.4) }
        times.append((try? s.position().total) ?? 0)
        var report: [String] = []
        for t in times {
            c.apply((try? s.seek(time: t)) ?? [])
            view.refresh()
            guard let vt = try? s.view() else { fail("no view"); continue }
            do throws(CoreError) {
                try fresh.setViewSize(width: vt.width, height: vt.height, backing: vt.backing)
                try fresh.zoomToFit()
                try fresh.seek(time: t)
                if let list = try fresh.displayList() { freshRenderer.update(board: list) }
                if let o = try fresh.overlay() { freshRenderer.update(overlay: o) }
                freshRenderer.update(wavefront: try fresh.wavefrontImages())
            } catch {
                fail("fresh session at t \(t): \(error)")
                continue
            }
            if (try? fresh.view()) != vt { fail("the fresh session's camera differs") }
            guard let live = view.layered.renderSinglePass(view: vt), let other = freshRenderer.renderSinglePass(view: vt),
                  let shown = view.snapshotLayers(), live.width == other.width, live.height == other.height,
                  shown.width == live.width, shown.height == live.height
            else {
                fail("could not render at t \(t)")
                continue
            }
            let a = SelfTestPixels.rgba(live), b = SelfTestPixels.rgba(other), d = SelfTestPixels.rgba(shown)
            var differ = 0
            for i in stride(from: 0, to: a.count, by: 4) where !SelfTestPixels.close(a, i, b, i, 0) { differ += 1 }
            let layers = SelfTestPixels.differing(a, d, width: live.width) {
                view.layered.isWavefrontCellEdge(x: $0, y: $1, view: vt)
            }
            if differ > 0 { fail("at t \(t) the board differs from a fresh open's in \(differ) px") }
            if layers.strict > 0 { fail("at t \(t) the layers differ from a single-pass render in \(layers.strict) px") }
            report.append(String(format: "t %.2f: %d px from a fresh open, layers %d px", t, differ, layers.strict))
        }
        print("selftest: live: equal to a fresh open: timeline \(events.count) events, nets table "
              + "\(ia?.viewCount ?? 0) rows; " + report.joined(separator: "; "))
        return failures
    }
}
