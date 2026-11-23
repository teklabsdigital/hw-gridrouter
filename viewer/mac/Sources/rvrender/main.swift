// rvrender: draw a trace's board and router overlay to a PNG without a
// window, compare two PNGs, time redraws, or check playback.  Uses RVKit's
// LayeredRenderer, the same code the app draws with.
//
//   rvrender FILE OUT.png [--size WxH] [--board N] [--layers a,b,c | --all-layers]
//                         [--hide-classes i,j] [--fit | --window x0,y0,x1,y1]
//                         [--at FRACTION | --event N [--into F]] [--pacing PACING] [--select NET]
//   rvrender FILE OUTDIR --frames N [same options]     N frames evenly through the run
//   rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
//   rvrender --bench FILE [--size WxH] [--frames N] [--board N]
//   rvrender --bench-playback FILE [--size WxH] [--speed S] [--frames N] [--bands B] [--pacing PACING]
//                                  [--select NET] [--crop-grid G]
//   rvrender --crops FILE [--size WxH] [--at F] [--frames TRIALS]
//                                  how much a slot plane changes when only its crop does (M6)
//   rvrender --bench-live SRC DIR [--size WxH] [--rate BYTES_PER_S] [--bands B]
//                                  SRC written into DIR while followed (M6)
//   rvrender --selftest FILE [--size WxH]
//   rvrender --repeat FILE [--size WxH] [--at FRACTION]   how repeatable the banded rasteriser is
//
// Without --at or --event the position is the end of the run (as after
// opening a trace in the app).  --into F: F (0...1) of the way through event
// N, e.g. part of a search's wavefront revealed (below 0.8) or settled.
// PACING (how long each event lasts): fit[:SECONDS] (the default, 180 s),
// nets, net:NET (one router net), skip-failed, real[:SLOWDOWN] (default 10).
// --select NET: select the net of that name (KiCad-style highlight), as a
// click on it in the app does.  Exit status 0 on success (and for
// --compare, images within tolerance), 1 on failure, 2 for bad usage.

import CoreGraphics
import Foundation
import ImageIO
import RVKit

func fail(_ message: String, code: Int32 = 1) -> Never {
    FileHandle.standardError.write(Data("rvrender: \(message)\n".utf8))
    exit(code)
}

func usage() -> Never {
    fail("""
    usage: rvrender FILE OUT.png [--size WxH] [--board N] [--layers a,b,c | --all-layers]
                                 [--hide-classes i,j] [--fit | --window x0,y0,x1,y1]
                                 [--at FRACTION | --event N [--into F]] [--pacing PACING]
                                 [--select NET]
           rvrender FILE OUTDIR --frames N [options as above]
           rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
           rvrender --bench FILE [--size WxH] [--frames N] [--board N]
           rvrender --bench-playback FILE [--size WxH] [--speed S] [--frames N] [--bands B] [--pacing PACING]
                                          [--select NET]
           rvrender --bench-live SRC DIR [--size WxH] [--rate BYTES_PER_S] [--bands B]
           rvrender --crops FILE [--size WxH] [--at F] [--frames TRIALS]
           (--bench-playback also takes --crop-grid G)
           rvrender --selftest FILE [--size WxH]
           rvrender --repeat FILE [--size WxH] [--at FRACTION]
    PACING: fit[:SECONDS], nets, net:NET, skip-failed, real[:SLOWDOWN]
    """, code: 2)
}

struct Options {
    var positional: [String] = []
    var width = 1600, height = 1000
    var sizeGiven = false
    var board: UInt32?
    var layers: [String]?
    var allLayers = false
    var hideClasses: [UInt32] = []
    var window: (Double, Double, Double, Double)?
    var tolerance = 48
    var maxFraction = 0.002
    var frames = 60
    var framesGiven = false
    var profile = false
    var at: Double?
    var event: UInt32?
    var speed = 8.0
    var bands = 0
    var into: Double?
    var pacing: Pacing?
    var select: String?
    /// --bench-live: bytes per second.
    var rate = 5_000_000.0
    /// LayeredRenderer.cropGrid (M6 carry-over measurements).
    var cropGrid = 0
}

/// "fit", "fit:60", "nets", "net:4", "skip-failed", "real", "real:10" (as rvdump).
func parsePacing(_ spec: String) -> Pacing? {
    let parts = spec.split(separator: ":", maxSplits: 1, omittingEmptySubsequences: false).map(String.init)
    let name = parts[0]
    let value = parts.count > 1 ? Double(parts[1]) : nil
    if parts.count > 1 && value == nil { return nil }
    var p = Pacing()
    switch name {
    case "fit":
        p.mode = .fit
        if let value { p.fitSeconds = value }
    case "nets" where parts.count == 1: p.mode = .netsOnly
    case "net":
        guard let value, let n = Int32(exactly: value) else { return nil }
        p.mode = .oneNet
        p.net = n
    case "skip-failed" where parts.count == 1: p.mode = .skipFailed
    case "real":
        p.mode = .realTime
        if let value { p.slowdown = value }
    default: return nil
    }
    return p
}

func parseOptions(_ args: ArraySlice<String>) -> Options {
    var o = Options()
    var it = args.makeIterator()
    func value(_ name: String) -> String {
        guard let v = it.next() else { usage() }
        return v
    }
    func numbers(_ s: String, _ sep: Character) -> [Double] {
        s.split(separator: sep).map { Double($0) ?? .nan }
    }
    while let a = it.next() {
        switch a {
        case "--size":
            let v = value(a).split(separator: "x").compactMap { Int($0) }
            guard v.count == 2, v[0] > 0, v[1] > 0, v[0] <= 16384, v[1] <= 16384 else { usage() }
            o.width = v[0]
            o.height = v[1]
            o.sizeGiven = true
        case "--board":
            guard let n = UInt32(value(a)) else { usage() }
            o.board = n
        case "--layers": o.layers = value(a).split(separator: ",").map(String.init)
        case "--all-layers": o.allLayers = true
        case "--hide-classes": o.hideClasses = value(a).split(separator: ",").compactMap { UInt32($0) }
        case "--fit": o.window = nil
        case "--window":
            let v = numbers(value(a), ",")
            guard v.count == 4, v.allSatisfy(\.isFinite), v[2] > v[0], v[3] > v[1] else { usage() }
            o.window = (v[0], v[1], v[2], v[3])
        case "--tolerance":
            guard let t = Int(value(a)), t >= 0, t <= 255 else { usage() }
            o.tolerance = t
        case "--max-fraction":
            guard let f = Double(value(a)), f >= 0, f <= 1 else { usage() }
            o.maxFraction = f
        case "--profile": o.profile = true
        case "--frames":
            guard let n = Int(value(a)), n > 0 else { usage() }
            o.frames = n
            o.framesGiven = true
        case "--at":
            guard let f = Double(value(a)), f >= 0, f <= 1 else { usage() }
            o.at = f
        case "--event":
            guard let n = UInt32(value(a)) else { usage() }
            o.event = n
        case "--speed":
            guard let s = Double(value(a)), s > 0 else { usage() }
            o.speed = s
        case "--bands":
            guard let n = Int(value(a)), n >= 0 else { usage() }
            o.bands = n
        case "--into":
            guard let f = Double(value(a)), f >= 0, f <= 1 else { usage() }
            o.into = f
        case "--pacing":
            guard let p = parsePacing(value(a)) else { usage() }
            o.pacing = p
        case "--crop-grid":
            guard let g = Int(value(a)), g >= 0, g <= 4096 else { usage() }
            o.cropGrid = g
        case "--rate":
            guard let r = Double(value(a)), r > 0 else { usage() }
            o.rate = r
        case "--select":
            let v = value(a)
            guard !v.isEmpty else { usage() }
            o.select = v
        default:
            if a.hasPrefix("--") { usage() }
            o.positional.append(a)
        }
    }
    if o.at != nil && o.event != nil { usage() }
    if o.into != nil && o.event == nil { usage() }
    return o
}

// MARK: - Session set-up

@MainActor
func openSession(_ path: String, _ o: Options, needBoard: Bool = true) -> Session {
    do {
        let s = try Session()
        try s.open(path: path)
        try s.selectBoard(o.board)
        if let p = o.pacing { try s.setPacing(p) }
        if let name = o.select {
            try s.selectName(name)
            if let sel = try s.selection(), sel.net == nil, sel.boardNets == 0 {
                FileHandle.standardError.write(Data("rvrender: no net is named \(name); everything is dimmed\n".utf8))
            }
        }
        if let f = o.at { try s.seek(fraction: f) }
        if let e = o.event {
            let count = try s.position().eventCount
            if e >= count { fail("\(path): no event \(e) (the run has \(count))") }
            try s.seek(event: e)
            if let f = o.into {
                let ev = try s.timeline().events[Int(e)]
                // Within the event: its start plus f of its duration (the
                // end of a zero-length event is the next event).
                if ev.duration > 0 { try s.seek(time: ev.start + min(f * ev.duration, ev.duration * 0.999999)) }
            }
        }
        if needBoard, try s.selectedBoard() == nil, try s.displayList()?.batches.isEmpty ?? true {
            fail("\(path): nothing to draw (no board and no router outline)")
        }
        let layers = try s.layers()
        if let wanted = o.layers {
            let names = Set(layers.map(\.name))
            for w in wanted where !names.contains(w) { fail("the board has no layer \(w)") }
            for l in layers { try s.setLayerVisible(l.name, wanted.contains(l.name)) }
        } else if o.allLayers {
            for l in layers { try s.setLayerVisible(l.name, true) }
        }
        for c in o.hideClasses { try s.setClassVisible(c, false) }
        try s.setViewSize(width: Double(o.width), height: Double(o.height), backing: 1)
        if let w = o.window {
            // Show board rectangle w: fit, then zoom so that it fills the view.
            try s.zoomToFit()
            let v = try s.view()
            let scale = min(Double(o.width) / (w.2 - w.0), Double(o.height) / (w.3 - w.1))
            try s.zoom(by: scale / v.scale, aboutX: 0, y: 0)
            let v2 = try s.view()
            let cx = (w.0 + w.2) / 2 * v2.scale + v2.tx, cy = (w.1 + w.3) / 2 * v2.scale + v2.ty
            try s.pan(dx: Double(o.width) / 2 - cx, dy: Double(o.height) / 2 - cy)
        } else {
            try s.zoomToFit()
        }
        return s
    } catch {
        fail("\(path): \(error)")
    }
}

/// Give the renderer the session's current lists and wavefront images (a
/// search whose arrays cannot be read shows no wavefront, as in the app).
@MainActor
func sync(_ s: Session, _ r: LayeredRenderer) throws(CoreError) {
    if let list = try s.displayList(ifChangedSince: r.boardList?.generation) { r.update(board: list) }
    if let o = try s.overlay(ifChangedSince: r.overlayList?.generation) { r.update(overlay: o) }
    r.update(wavefront: (try? s.wavefrontImages(reusing: r.wavefronts)) ?? [])
}

/// An RGBA8 sRGB bitmap context, flipped so that y goes down like a view.
func makeContext(_ width: Int, _ height: Int) -> CGContext {
    guard let space = CGColorSpace(name: CGColorSpace.sRGB),
          let ctx = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: 0,
                              space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    else { fail("cannot create a \(width) x \(height) bitmap") }
    ctx.translateBy(x: 0, y: CGFloat(height))
    ctx.scaleBy(x: 1, y: -1)
    return ctx
}

/// Write an opaque RGB PNG (the board always covers the image with its
/// background, so the alpha channel would only add bytes).
func writePNG(_ image: CGImage, to path: String) {
    let url = URL(fileURLWithPath: path)
    guard let space = CGColorSpace(name: CGColorSpace.sRGB),
          let ctx = CGContext(data: nil, width: image.width, height: image.height, bitsPerComponent: 8,
                              bytesPerRow: 0, space: space, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
    else { fail("cannot convert the image for \(path)") }
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: image.width, height: image.height))
    guard let opaque = ctx.makeImage(),
          let dest = CGImageDestinationCreateWithURL(url as CFURL, "public.png" as CFString, 1, nil)
    else { fail("cannot write \(path)") }
    CGImageDestinationAddImage(dest, opaque, nil)
    if !CGImageDestinationFinalize(dest) { fail("cannot write \(path)") }
}

/// RGBA8 pixels of a PNG, drawn into a plain context.
func loadPixels(_ path: String) -> (width: Int, height: Int, pixels: [UInt8]) {
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(src, 0, nil)
    else { fail("cannot read \(path)") }
    return pixels(of: img)
}

/// Pixels differing by more than `tolerance` in any channel, and the
/// largest difference.
func differingPixels(_ a: CGImage, _ b: CGImage, tolerance: Int) -> (Int, Int) {
    let pa = pixels(of: a), pb = pixels(of: b)
    guard pa.width == pb.width, pa.height == pb.height else { return (Int.max, 255) }
    var n = 0, worst = 0
    for i in stride(from: 0, to: pa.pixels.count, by: 4) {
        var d = 0
        for c in 0..<4 { d = max(d, abs(Int(pa.pixels[i + c]) - Int(pb.pixels[i + c]))) }
        worst = max(worst, d)
        if d > tolerance { n += 1 }
    }
    return (n, worst)
}

func pixels(of img: CGImage) -> (width: Int, height: Int, pixels: [UInt8]) {
    let w = img.width, h = img.height
    var pixels = [UInt8](repeating: 0, count: w * h * 4)
    let ok = pixels.withUnsafeMutableBytes { buf -> Bool in
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: w * 4, space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return false }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
        return true
    }
    if !ok { fail("cannot decode an image") }
    return (w, h, pixels)
}

func median(_ v: [Double]) -> Double {
    guard !v.isEmpty else { return 0 }
    let s = v.sorted()
    return s[s.count / 2]
}

func percentile(_ v: [Double], _ p: Double) -> Double {
    guard !v.isEmpty else { return 0 }
    let s = v.sorted()
    return s[min(s.count - 1, Int(Double(s.count) * p))]
}

// MARK: - Modes

@MainActor
func render(_ o: Options) -> Int32 {
    guard o.positional.count == 2 else { usage() }
    if o.framesGiven { return frames(o) }
    let s = openSession(o.positional[0], o)
    let r = LayeredRenderer()
    do {
        try sync(s, r)
        guard let image = r.composite(view: try s.view()) else { fail("cannot make the image") }
        writePNG(image, to: o.positional[1])
        let p = try s.position()
        let at = p.event.map { "event index \($0) of \(p.eventCount) events" } ?? "no events"
        print("rvrender: \(o.positional[1]): \(o.width) x \(o.height), \(r.boardBatchCount) board + "
              + "\(r.overlayBatchCount) overlay batches, \(at)")
        if p.event != nil {
            print("  copper \(p.copperAlive), ghosts \(p.ghosts), failed pads \(p.failures)"
                  + (p.routedHidden ? " (router copper hidden: the board holds it)" : "") + "; "
                  + (try s.text(.caption)))
            let waves = r.wavefronts.filter { $0.image != nil }
            print("  pacing \(try s.text(.pacing)); wavefront: \(p.cellsRevealed) entries revealed, "
                  + "\(waves.reduce(0) { $0 + Int($1.cells) }) cells drawn in \(waves.count) images"
                  + (p.settled ? ", settled" : ""))
        }
        if let sel = try s.selection() {
            let hl = r.overlaySlots.first { $0.kind == .highlight }?.batchCount ?? 0
            print("  selected \"\(sel.name)\"" + (sel.net.map { " (router net \($0))" } ?? " (no router net)")
                  + ": \(sel.boardNets) board nets, \(sel.boardItems) board items, \(hl) highlight batches")
        }
        return 0
    } catch {
        fail("\(error)")
    }
}

/// N frames evenly spaced from the start to the end of the run.
@MainActor
func frames(_ o: Options) -> Int32 {
    let dir = o.positional[1]
    do {
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    } catch {
        fail("cannot create \(dir): \(error)")
    }
    var o2 = o
    o2.at = 0
    o2.event = nil
    let s = openSession(o.positional[0], o2, needBoard: false)
    let r = LayeredRenderer()
    do {
        let n = o.frames
        for k in 0..<n {
            let f = n == 1 ? 1.0 : Double(k) / Double(n - 1)
            let changed = try s.seek(fraction: f)
            if changed.contains(.board), o.window == nil { try s.zoomToFit() }
            try sync(s, r)
            guard let image = r.composite(view: try s.view()) else { fail("cannot make the image") }
            let path = (dir as NSString).appendingPathComponent(String(format: "frame_%04d.png", k))
            writePNG(image, to: path)
            print(String(format: "%@  %.3f  %@", (path as NSString).lastPathComponent, f, try s.text(.caption)))
        }
        return 0
    } catch {
        fail("\(error)")
    }
}

func compare(_ o: Options) -> Int32 {
    guard o.positional.count == 2 else { usage() }
    let a = loadPixels(o.positional[0]), b = loadPixels(o.positional[1])
    guard a.width == b.width, a.height == b.height else {
        print("rvrender: compare \(o.positional[1]): size \(b.width) x \(b.height), golden \(a.width) x \(a.height): FAIL")
        return 1
    }
    var differing = 0, worst = 0
    for i in stride(from: 0, to: a.pixels.count, by: 4) {
        var d = 0
        for c in 0..<4 { d = max(d, abs(Int(a.pixels[i + c]) - Int(b.pixels[i + c]))) }
        worst = max(worst, d)
        if d > o.tolerance { differing += 1 }
    }
    let total = a.width * a.height
    let fraction = Double(differing) / Double(total)
    let ok = fraction <= o.maxFraction
    print(String(format: "rvrender: compare %@: %d of %d pixels differ by more than %d (%.4f%%, limit %.4f%%), "
                    + "largest difference %d: %@",
                 (o.positional[1] as NSString).lastPathComponent, differing, total, o.tolerance, 100 * fraction,
                 100 * o.maxFraction, worst, ok ? "ok" : "FAIL"))
    return ok ? 0 : 1
}

@MainActor
func bench(_ o: Options) -> Int32 {
    guard o.positional.count == 1 else { usage() }
    let s = openSession(o.positional[0], o)
    do {
        guard let list = try s.displayList() else { fail("no display list") }
        let view = try s.view()
        let ctx = makeContext(o.width, o.height)
        var points = 0
        for b in list.batches where b.primitive == .fill || b.primitive == .stroke {
            if b.primitive == .fill {
                for p in b.first..<(b.first + b.count) {
                    let c0 = Int(list.polys[2 * p]), nc = Int(list.polys[2 * p + 1])
                    for c in c0..<(c0 + nc) { points += Int(list.contours[2 * c + 1]) }
                }
            } else {
                for c in b.first..<(b.first + b.count) { points += Int(list.contours[2 * c + 1]) }
            }
        }
        print("rvrender: bench \(o.positional[0]) at \(o.width) x \(o.height) px, \(o.frames) frames each; "
              + "\(list.batches.count) visible batches, \(points) polygon/polyline points")

        func time(_ label: String, _ frame: () -> Void) {
            frame() // warm-up, not counted
            var ms: [Double] = []
            for _ in 0..<o.frames {
                let t0 = DispatchTime.now().uptimeNanoseconds
                frame()
                ms.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6)
            }
            ms.sort()
            print("  " + label.padding(toLength: 46, withPad: " ", startingAt: 0)
                  + String(format: " median %7.2f ms   p90 %7.2f ms   worst %7.2f ms   best %7.2f ms",
                           ms[ms.count / 2], ms[min(ms.count - 1, ms.count * 9 / 10)], ms[ms.count - 1], ms[0]))
        }

        if o.profile {
            // Time each batch alone (median of 15), most expensive first.
            var rows: [(Double, String)] = []
            for (k, b) in list.batches.enumerated() {
                let path = DisplayListRenderer.path(for: b, in: list)
                let color = DisplayListRenderer.color(b.rgba)
                var ms: [Double] = []
                for _ in 0..<15 {
                    ctx.saveGState()
                    ctx.concatenate(CGAffineTransform(a: view.scale, b: 0, c: 0, d: view.scale, tx: view.tx,
                                                      ty: view.ty))
                    ctx.setLineCap(.round)
                    ctx.setLineJoin(.round)
                    let t0 = DispatchTime.now().uptimeNanoseconds
                    ctx.addPath(path)
                    if b.primitive == .fill || b.primitive == .disc {
                        ctx.setFillColor(color)
                        ctx.fillPath(using: .winding)
                    } else {
                        ctx.setStrokeColor(color)
                        ctx.setLineWidth(CGFloat(b.width))
                        ctx.strokePath()
                    }
                    ms.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6)
                    ctx.restoreGState()
                }
                ms.sort()
                rows.append((ms[ms.count / 2], "batch \(k) \(b.primitive) layer \(b.layer) class \(b.objectClass) "
                                + "items \(b.count) width \(b.width)"))
            }
            var sum = 0.0
            for (t, label) in rows.sorted(by: { $0.0 > $1.0 }) {
                sum += t
                print(String(format: "  %7.2f ms  %@", t, label))
            }
            print(String(format: "  %7.2f ms  sum", sum))
        }

        // A: immediate: build every path from the display list, then draw.
        time("A immediate (paths rebuilt every frame)") {
            let r = DisplayListRenderer()
            r.update(list)
            r.draw(in: ctx, view: view)
        }
        // B: prepared CGPaths kept between frames; the frame clears the
        // background and fills and strokes every visible batch.
        let prepared = DisplayListRenderer()
        prepared.update(list)
        time("B prepared paths (full redraw)") {
            prepared.draw(in: ctx, view: view)
        }
        // D: the same full redraw, rasterised in horizontal bands at once on
        // several threads into one bitmap.
        let pixelView = view // backing 1: points are pixels here
        for bands in [4, 8, 12, 18, 24, 36] {
            time("D parallel bands x\(bands) (full redraw)") { prepared.render(view: pixelView, bands: bands) }
        }
        time("D parallel bands x18 + CGImage copy") { _ = prepared.renderImage(view: pixelView, bands: 18) }
        // The bands must give the same pixels as one context.
        prepared.draw(in: ctx, view: view)
        if let single = ctx.makeImage(), let banded = prepared.renderImage(view: pixelView, bands: 18) {
            let (n, worst) = differingPixels(single, banded, tolerance: 0)
            print("  banded vs single-context image: \(n) pixels differ (largest channel difference \(worst))")
        }
        // C: the board rendered once into a cached image; a frame composites
        // it (what a pan does between full redraws).
        prepared.draw(in: ctx, view: view)
        guard let cached = ctx.makeImage() else { fail("cannot snapshot") }
        let rect = CGRect(x: 0, y: 0, width: o.width, height: o.height)
        time("C cached bitmap blit (pan between redraws)") {
            ctx.saveGState()
            ctx.translateBy(x: 0, y: CGFloat(o.height))
            ctx.scaleBy(x: 1, y: -1)
            ctx.draw(cached, in: rect.offsetBy(dx: 3, dy: 2))
            ctx.restoreGState()
        }
        return 0
    } catch {
        fail("\(error)")
    }
}

/// Playback from the start through the core clock at 60 frames a second
/// of wall time (simulated, not waited for), as the app's display link
/// drives it.  A frame's cost: the clock, the overlay copy and the
/// re-rasterising of the planes that changed (the static slabs are cached).
@MainActor
func benchPlayback(_ o: Options) -> Int32 {
    guard o.positional.count == 1 else { usage() }
    var o2 = o
    o2.at = 0
    let s = openSession(o.positional[0], o2, needBoard: false)
    let r = LayeredRenderer()
    r.bands = o.bands
    r.cropGrid = o.cropGrid
    do {
        try s.setSpeed(o.speed)
        let p0 = try s.position()
        let view = try s.view()
        try sync(s, r)
        r.render(view: view)
        // A camera change redraws every plane: time that at the start and,
        // below, at the end of the run.
        func fullRedraw() -> [Double] {
            (0..<10).map { _ in
                r.invalidate()
                let t0 = DispatchTime.now().uptimeNanoseconds
                r.render(view: view)
                return Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6
            }
        }
        let fullStart = fullRedraw()
        // With --select: the time to change the selection and show it, as a
        // click in the app does: the core's call, the overlay copy, the
        // planes, the net's history and timeline events, and the nets
        // table's info and first 40 rows of cells.  Clear, then select again.
        func timeSelect() throws(CoreError) -> [Double] {
            guard let name = o.select else { return [] }
            var ms: [Double] = []
            for k in 0..<10 {
                let t0 = DispatchTime.now().uptimeNanoseconds
                if k % 2 == 0 { try s.clearSelection() } else { try s.selectName(name) }
                try sync(s, r)
                r.render(view: view)
                if let net = try s.selection()?.net {
                    _ = try s.history(net: net)
                    _ = try s.netEvents(net: net)
                }
                let info = try s.netsInfo()
                for p in 0..<min(40, info.viewCount) {
                    for c in NetColumn.allCases { _ = try s.netCell(at: p, column: c) }
                }
                ms.append(Double(DispatchTime.now().uptimeNanoseconds - t0) / 1e6)
            }
            return ms
        }
        let selectStart = try timeSelect()
        try s.setPlaying(true)
        let dt = 1.0 / 60
        let limit = o.framesGiven ? o.frames : Int.max
        var all: [Double] = [], dynamic: [Double] = [], core: [Double] = [], raster: [Double] = []
        var pixels: [Double] = [], waveOnly: [Double] = []
        var planesRendered = 0, boardFrames = 0, frames = 0, waveFrames = 0
        var worstDynamic: (ms: Double, frame: Int, event: String, caption: String, planes: Int, pixels: Int,
                           core: Double) = (0, 0, "", "", 0, 0, 0)
        var playing = true
        while frames < limit && playing {
            // As the app's run loop does each frame: release what Core
            // Graphics autoreleased.
            try autoreleasepool { () throws -> Void in
                let t0 = DispatchTime.now().uptimeNanoseconds
                let changed = try s.advance(by: dt)
                try sync(s, r)
                let t1 = DispatchTime.now().uptimeNanoseconds
                let n = r.render(view: view)
                let t2 = DispatchTime.now().uptimeNanoseconds
                frames += 1
                let ms = Double(t2 - t0) / 1e6
                all.append(ms)
                core.append(Double(t1 - t0) / 1e6)
                // A wavefront change swaps an image (no rasterising).
                let waveChanged = r.lastWavefrontChanges > 0
                if waveChanged { waveFrames += 1 }
                if changed.contains(.board) {
                    boardFrames += 1
                } else if n > 0 || waveChanged {
                    dynamic.append(ms)
                    raster.append(Double(t2 - t1) / 1e6)
                    pixels.append(Double(r.lastRenderedPixels))
                    if n == 0 { waveOnly.append(ms) }
                    if ms > worstDynamic.ms {
                        let p = try s.position()
                        worstDynamic = (ms, frames - 1, p.event.map { "event \($0)" } ?? "no event",
                                        try s.text(.caption), n, r.lastRenderedPixels, Double(t1 - t0) / 1e6)
                    }
                }
                planesRendered += n
                playing = try s.position().playing
            }
        }
        let p = try s.position()
        print(String(format: "rvrender: playback bench %@ at %d x %d px, speed %gx, %d events, run %.1f s, pacing %@",
                     o.positional[0], o.width, o.height, try s.position().speed, p0.eventCount, p0.total,
                     try s.text(.pacing)))
        print(String(format: "  %d frames (%.1f s at 60 fps), %d dynamic (planes redrawn or a wavefront changed; "
                        + "%d planes), %d with a wavefront change (%d only that), %d with a new board, end reached: %@",
                     frames, Double(frames) / 60, dynamic.count, planesRendered, waveFrames, waveOnly.count,
                     boardFrames, p.atEnd ? "yes" : "no"))
        func row(_ label: String, _ v: [Double]) {
            print("  " + label.padding(toLength: 40, withPad: " ", startingAt: 0)
                  + String(format: " median %6.2f ms   p90 %6.2f ms   p99 %6.2f ms   worst %6.2f ms",
                           median(v), percentile(v, 0.9), percentile(v, 0.99), v.max() ?? 0))
        }
        row("every frame", all)
        row("dynamic frames (no new board)", dynamic)
        row("  of which rasterising + images", raster)
        row("  wavefront-only frames", waveOnly)
        row("  core: clock + overlay copy, all frames", core)
        print(String(format: "  pixels rasterised per dynamic frame: median %.0f (view %d)", median(pixels),
                     o.width * o.height))
        print(String(format: "  slowest dynamic frame: %.2f ms, frame %d (%@: %@), %d planes, %d pixels, core %.2f ms",
                     worstDynamic.ms, worstDynamic.frame, worstDynamic.event, worstDynamic.caption,
                     worstDynamic.planes, worstDynamic.pixels, worstDynamic.core))
        let fullEnd = fullRedraw()
        let nonEmpty = r.planes.filter { $0.image != nil }.count
        row("full redraw (pan/zoom), start of run", fullStart)
        row("full redraw (pan/zoom), end of run", fullEnd)
        if let name = o.select {
            let sel = try s.selection()
            let selectEnd = try timeSelect()
            row("select / clear \(name), start", selectStart)
            row("select / clear \(name), end", selectEnd)
            print("  selected at the end: " + (sel.map { "\"\($0.name)\", \($0.boardItems) board items" } ?? "nothing"))
        }
        // A plane's bitmap: a wavefront plane's is grid-sized, a cropped
        // plane's its crop, a solid rectangle's 1 x 1.
        let bytes = r.planes.reduce(0) { $0 + ($1.image.map { $0.width * $0.height } ?? 0) * 4 }
        print("  planes at the end: \(r.planes.count), \(nonEmpty) with content, "
              + String(format: "%.1f MB of bitmaps", Double(bytes) / 1e6))
        let sizes = r.planes.compactMap { p -> String? in
            guard let image = p.image else { return nil }
            let name: String
            switch p.kind {
            case .slab(let k): name = "slab \(k)"
            case .slot(let kind): name = "\(kind)"
            }
            return String(format: "%@ %.1f", name, Double(image.width * image.height * 4) / 1e6)
        }
        print("    MB by plane: " + sizes.joined(separator: ", "))
        return 0
    } catch {
        fail("\(error)")
    }
}

/// Carry-over from M5 (M6): how much a cropped slot plane changes when only
/// its crop changes, for crop grids 0 (M5) and 16 to 256 pixels, in one band
/// and in the default bands, at five positions of the run (--at: one).
@MainActor
func crops(_ o: Options) -> Int32 {
    guard o.positional.count == 1 else { usage() }
    let s = openSession(o.positional[0], o, needBoard: false)
    let trials = o.framesGiven ? o.frames : 6
    do {
        let view = try s.view()
        print("rvrender: crops \(o.positional[0]) at \(o.width) x \(o.height) px, \(trials) grown crops per slot plane")
        for bands in [1, 0] {
            // With a grid, slot planes are drawn in one context (bands do not apply).
            for grid in bands == 1 ? [0, 64, 128, 256, 512] : [0] {
                var total = (compared: 0, differing: 0, over48: 0, worst: 0)
                for f in o.at.map({ [$0] }) ?? [0.2, 0.4, 0.6, 0.8, 1.0] {
                    try s.seek(fraction: f)
                    let r = LayeredRenderer()
                    r.bands = bands
                    r.cropGrid = grid
                    try sync(s, r)
                    let m = r.cropStability(view: view, trials: trials)
                    total.compared += m.compared
                    total.differing += m.differing
                    total.over48 += m.over48
                    total.worst = max(total.worst, m.worst)
                }
                print(String(format: "  bands %@ grid %3d: %10d px compared, %7d differ (%.4f%%), %5d by more than 48, "
                                + "largest difference %d",
                             bands == 0 ? "auto" : "1   ", grid, total.compared, total.differing,
                             100 * Double(total.differing) / Double(max(1, total.compared)), total.over48, total.worst))
            }
        }
        return 0
    } catch {
        fail("\(error)")
    }
}

// MARK: - Following a run while it is written (M6)

/// SRC written again into a file in DIR at RATE bytes per second (simulated:
/// the writes are not waited for), polled every 0.25 s as the app's timer
/// does, with 15 frames of 1/60 s between polls while following.  Per poll:
/// the core's rv_poll, then the client's share (the timeline copy, the nets
/// table's info and its first 40 rows of cells, the board and overlay lists
/// and the planes).  Ends when following stops at END.
@MainActor
func benchLive(_ o: Options) -> Int32 {
    guard o.positional.count == 2 else { usage() }
    let src = o.positional[0], dir = o.positional[1]
    // Read piece by piece, so that the copy does not count in the memory peak.
    guard let input = FileHandle(forReadingAtPath: src),
          let size = (try? FileManager.default.attributesOfItem(atPath: src))?[.size] as? Int
    else { fail("cannot read \(src)") }
    let path = (dir as NSString).appendingPathComponent("bench-live-\(getpid()).grtrace")
    try? FileManager.default.removeItem(atPath: path)
    defer { try? FileManager.default.removeItem(atPath: path) }
    guard FileManager.default.createFile(atPath: path, contents: nil),
          let handle = FileHandle(forWritingAtPath: path) else { fail("cannot write \(path)") }
    let interval = 0.25
    let piece = max(1, Int(o.rate * interval))
    let r = LayeredRenderer()
    r.bands = o.bands
    do {
        let s = try Session()
        try s.open(path: path, flags: [.live, .wait])
        try s.setViewSize(width: Double(o.width), height: Double(o.height), backing: 1)
        var written = 0, polls = 0
        var now = 0.0
        var core: [Double] = [], client: [Double] = [], frames: [Double] = []
        var worst: (ms: Double, poll: Int, chunks: UInt32, bytes: Int, events: UInt32) = (0, 0, 0, 0, 0)
        var fitted = false
        var following = 0
        while true {
            if written < size {
                autoreleasepool {
                    let data = input.readData(ofLength: min(piece, size - written))
                    handle.write(data)
                    written += data.count
                }
                if written >= size { handle.closeFile() }
            }
            now += interval
            let before = try s.live()
            let t0 = DispatchTime.now().uptimeNanoseconds
            let result = try s.poll(now: now)
            let t1 = DispatchTime.now().uptimeNanoseconds
            if result.added > 0 {
                // The client's share, as the window's poll does it.
                _ = try s.timeline()
                let info = try s.netsInfo()
                for p in 0..<min(40, info.viewCount) {
                    for c in NetColumn.allCases { _ = try s.netCell(at: p, column: c) }
                }
                if !fitted, let list = try s.displayList(), list.board != nil || !list.batches.isEmpty {
                    try s.zoomToFit()
                    fitted = true
                }
                try sync(s, r)
                r.render(view: try s.view())
            }
            let t2 = DispatchTime.now().uptimeNanoseconds
            polls += 1
            let coreMs = Double(t1 - t0) / 1e6
            core.append(coreMs)
            if result.added > 0 { client.append(Double(t2 - t1) / 1e6) }
            if coreMs > worst.ms {
                let after = try s.live()
                worst = (coreMs, polls, result.added, Int(after.indexedEnd - before.indexedEnd), after.events - before.events)
            }
            // A quarter of a second of frames.
            for _ in 0..<15 {
                try autoreleasepool { () throws -> Void in
                    let f0 = DispatchTime.now().uptimeNanoseconds
                    _ = try s.advance(by: 1.0 / 60)
                    try sync(s, r)
                    r.render(view: try s.view())
                    let ms = Double(DispatchTime.now().uptimeNanoseconds - f0) / 1e6
                    if try s.live().following {
                        frames.append(ms)
                        following += 1
                    }
                }
            }
            let l = try s.live()
            if written >= size && l.state == .complete && !l.following { break }
            if polls > 100_000 { fail("did not end") }
        }
        let pe = try s.position()
        let l = try s.live()
        print(String(format: "rvrender: live bench %@: %d bytes at %.1f MB/s (%d per poll), %d polls of %.2f s, "
                        + "%u events, run %.1f s at the end",
                     src, size, o.rate / 1e6, piece, polls, interval, pe.eventCount, pe.total))
        func row(_ label: String, _ v: [Double]) {
            print("  " + label.padding(toLength: 44, withPad: " ", startingAt: 0)
                  + String(format: " median %6.2f ms   p90 %6.2f ms   p99 %6.2f ms   worst %6.2f ms   sum %7.1f ms",
                           median(v), percentile(v, 0.9), percentile(v, 0.99), v.max() ?? 0, v.reduce(0, +)))
        }
        row("refresh (rv_poll), every poll", core)
        row("client share of a poll that read chunks", client)
        row("frames while following (\(following))", frames)
        print(String(format: "  worst poll: #%d, %u chunks, %d bytes, %u events, %.2f ms", worst.poll, worst.chunks,
                     worst.bytes, worst.events, worst.ms))
        print("  following: \(l.skips) jumps ahead (more than RV_FOLLOW_LAG behind); state at the end: \(l.state)")
        var usage = rusage()
        getrusage(RUSAGE_SELF, &usage)
        print(String(format: "  peak resident memory: %.1f MB", Double(usage.ru_maxrss) / 1e6))
        return 0
    } catch {
        fail("\(error)")
    }
}

// MARK: - Self-test

/// Plays the run from start to end through the core clock (accelerated:
/// large time steps), seeks to positions in both directions, renders at
/// each, compares the layered picture with a single-pass render, checks
/// that seeking to the same position from either side gives the same
/// picture, and checks the router copper against the ROUT chunk.
@MainActor
func selfTest(_ o: Options) -> Int32 {
    guard o.positional.count == 1 else { usage() }
    var o2 = o
    if !o.sizeGiven {
        o2.width = 800
        o2.height = 500
    }
    let file = o.positional[0]
    let s = openSession(file, o2, needBoard: false)
    // r: as the app draws (cropped slot planes, parallel bands); strict:
    // whole-view planes in one band, which must match a single-pass render
    // exactly.  Both are updated incrementally all the way.
    let r = LayeredRenderer()
    let strict = LayeredRenderer()
    strict.cropsSlots = false
    strict.bands = 1
    var failures: [String] = []
    func check(_ ok: Bool, _ what: @autoclosure () -> String) {
        if !ok { failures.append(what()) }
    }
    do {
        let tl = try s.timeline()
        let view0 = try s.view()
        func picture() throws(CoreError) -> CGImage {
            try sync(s, r)
            guard let image = r.composite(view: try s.view()) else { fail("cannot make the image") }
            return image
        }
        var compared = 0, worstLoose = 0
        func compareSinglePass(_ label: String) throws(CoreError) {
            compared += 1
            let view = try s.view()
            try sync(s, strict)
            guard let exact = strict.composite(view: view), let exactSingle = strict.renderSinglePass(view: view)
            else { fail("cannot make the image") }
            let (n0, worst0) = differingPixels(exact, exactSingle, tolerance: 3)
            check(n0 == 0, "\(label): whole-view planes and the single-pass picture differ in \(n0) pixels "
                  + "(largest \(worst0))")
            dumpMismatch(n0, exact, exactSingle, "strict_\(compared)")
            // The app's configuration: edge pixels of thin strokes may differ
            // (see LayeredRenderer.cropsSlots); nothing more.
            let layered = try picture()
            guard let single = r.renderSinglePass(view: view) else { fail("cannot make the image") }
            let (n, worst) = differingPixels(layered, single, tolerance: 48)
            let limit = layered.width * layered.height / 2000
            worstLoose = max(worstLoose, n)
            check(n <= limit, "\(label): layered and single-pass pictures differ by more than 48 in \(n) pixels "
                  + "(limit \(limit), largest \(worst))")
            dumpMismatch(n > limit ? n : 0, layered, single, "app_\(compared)")
        }
        // RVRENDER_SELFTEST_DUMP=DIR keeps both pictures of a mismatch and a
        // picture of the differing pixels.
        func dumpMismatch(_ n: Int, _ layered: CGImage, _ single: CGImage, _ name: String) {
            if n != 0, let dir = ProcessInfo.processInfo.environment["RVRENDER_SELFTEST_DUMP"] {
                let stem = (dir as NSString).appendingPathComponent("mismatch_\(name)")
                writePNG(layered, to: stem + "_layered.png")
                writePNG(single, to: stem + "_single.png")
                let pa = pixels(of: layered), pb = pixels(of: single)
                var x0 = Int.max, y0 = Int.max, x1 = -1, y1 = -1
                var diff = [UInt8](repeating: 0, count: pa.pixels.count)
                for i in stride(from: 0, to: pa.pixels.count, by: 4) {
                    var d = 0
                    for c in 0..<4 { d = max(d, abs(Int(pa.pixels[i + c]) - Int(pb.pixels[i + c]))) }
                    diff[i + 3] = 255
                    for c in 0..<3 { diff[i + c] = pa.pixels[i + c] / 4 }
                    if d > 3 {
                        let x = (i / 4) % pa.width, y = (i / 4) / pa.width
                        x0 = min(x0, x)
                        y0 = min(y0, y)
                        x1 = max(x1, x)
                        y1 = max(y1, y)
                        diff[i] = 255
                        diff[i + 1] = 255
                        diff[i + 2] = 255
                    }
                }
                diff.withUnsafeMutableBytes { raw in
                    if let ctx = CGContext(data: raw.baseAddress, width: pa.width, height: pa.height,
                                           bitsPerComponent: 8, bytesPerRow: pa.width * 4,
                                           space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                           bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
                       let img = ctx.makeImage() {
                        writePNG(img, to: stem + "_diff.png")
                    }
                }
                print("selftest: mismatch \(name) differs within x \(x0)...\(x1), y \(y0)...\(y1)")
            }
        }
        print("selftest: \(file): \(tl.events.count) events, run \(String(format: "%.1f", tl.total)) s, "
              + "\(tl.phases.count) phases, \(tl.ripTicks.count) rip-ups, \(tl.failureTicks.count) failure ticks")

        // 1. Play from the start to the end in about 200 accelerated steps.
        try s.jump(toEnd: false)
        try s.setSpeed(64)
        let speed = try s.position().speed
        try s.setPlaying(true)
        let dt = max(tl.total / speed / 200, 1e-3)
        var last = -1.0, steps = 0, overlayChanges = 0, boardChanges = 0, maxGhosts: UInt32 = 0
        var maxCopper: UInt32 = 0, renders = 0
        while try s.position().playing, steps < 100_000 {
            let changed = try s.advance(by: dt)
            let p = try s.position()
            check(p.time >= last, "playing went backwards: \(p.time) after \(last)")
            last = p.time
            if changed.contains(.overlay) { overlayChanges += 1 }
            if changed.contains(.board) { boardChanges += 1 }
            maxGhosts = max(maxGhosts, p.ghosts)
            maxCopper = max(maxCopper, p.copperAlive)
            try sync(s, r)
            renders += r.render(view: view0)
            try sync(s, strict)
            strict.render(view: view0)
            if steps % 40 == 20 { try compareSinglePass(String(format: "playing, t = %.2f s", p.time)) }
            steps += 1
        }
        let pend = try s.position()
        check(tl.events.isEmpty || pend.atEnd, "playing stopped before the end (t = \(pend.time) of \(pend.total))")
        check(!pend.playing, "still playing at the end")
        if !tl.events.isEmpty {
            check(overlayChanges > 0 || maxCopper == 0, "the overlay never changed while playing")
        }
        if !tl.ripTicks.isEmpty && steps >= 100 {
            check(maxGhosts > 0, "no ghost seen while playing through \(tl.ripTicks.count) rip-ups")
        }
        print("selftest: played \(steps) steps: \(overlayChanges) overlay changes, \(boardChanges) board changes, "
              + "\(renders) planes redrawn, most copper \(maxCopper), most ghosts \(maxGhosts)")

        // 2. Seek in both directions; the same position from either side
        // gives the same picture.
        if !tl.events.isEmpty {
            var seen: [Double: CGImage] = [:]
            let fractions = [0.9, 0.5, 0.1, 0.5, 0.25, 0.75, 0.25, 1.0, 0.0, 0.75, 0.6, 0.4]
            var matches = 0
            for f in fractions {
                try s.seek(fraction: f)
                // The strict renderer: one band, so the pictures are reproducible.
                try sync(s, strict)
                guard let image = strict.composite(view: try s.view()) else { fail("cannot make the image") }
                if let before = seen[f] {
                    let (n, worst) = differingPixels(before, image, tolerance: 0)
                    check(n == 0, "seek to \(f) from the other side: \(n) pixels differ (largest \(worst))")
                    matches += 1
                } else {
                    seen[f] = image
                }
                try compareSinglePass("seek \(f)")
            }
            // Stepping lands on event starts and pauses.
            try s.jump(toEnd: false)
            let c1 = try s.step(.commit, direction: 1)
            if !c1.isEmpty {
                let p = try s.position()
                check(p.fraction == 0 && !p.playing, "step to a commit: fraction \(p.fraction), playing \(p.playing)")
            }
            print("selftest: \(fractions.count) seeks, \(matches) revisited from the other side")
        }

        // 3. The router copper against the ROUT chunk: at the event that
        // applies the last ROUT, the router vias drawn are ROUT's vias.
        let facts = try s.facts()
        var rout: UInt32?
        for i in 0..<facts.chunkCount {
            if try s.chunkTag(i) == "ROUT" { rout = i }
        }
        if let rout, !tl.events.isEmpty {
            let text = try s.chunkText(rout)
            let numbers = text.split(whereSeparator: { !$0.isNumber }).compactMap { Int($0) }
            // "routes(): N segments, M vias"
            let routSegs = numbers.count >= 2 ? numbers[numbers.count - 2] : -1
            let routVias = numbers.last ?? -1
            guard var e = tl.events.firstIndex(where: { $0.lastChunk >= rout }) else { fail("no event applies ROUT") }
            try s.seek(event: UInt32(e))
            var p = try s.position()
            // When that event brings a board that holds the routing (it
            // hides the router copper), compare at the event before it: an
            // event that changes no copper shows the same router copper.
            let copperKinds: [TimelineEvent.Kind] = [.commit, .via, .ripUp, .session]
            if p.routedHidden, e > 0, !copperKinds.contains(tl.events[e].kind) {
                e -= 1
                try s.seek(event: UInt32(e))
                p = try s.position()
            }
            try sync(s, r)
            guard let overlay = r.overlayList else { fail("no overlay") }
            var vias = Set<String>(), routedBatches = 0
            for b in overlay.batches where b.objectClass == DisplayBatch.routedClass {
                let sub = b.order & 0xFF
                if sub == 0x38 { routedBatches += 1 }
                if sub == 0x3A && b.primitive == .disc {
                    for k in b.first..<(b.first + b.count) {
                        vias.insert(String(format: "%.3f,%.3f", overlay.circles[3 * k], overlay.circles[3 * k + 1]))
                    }
                }
            }
            if p.routedHidden {
                print("selftest: ROUT at event \(e): router copper hidden by the board there; not compared")
            } else {
                check(vias.count == routVias, "ROUT has \(routVias) vias, the overlay draws \(vias.count) at event \(e)")
                check(routSegs <= 0 || routedBatches > 0, "ROUT has \(routSegs) segments, the overlay draws no copper")
                print("selftest: ROUT (\(text)) at event \(e): \(p.copperAlive) copper items alive, "
                      + "\(vias.count) router vias drawn, \(routedBatches) copper batches")
            }
            try compareSinglePass("ROUT event \(e)")
        }

        // 4. Wavefronts (M4): in searches, revealing and settled, the
        // layered picture against a single-pass one.
        let searches = tl.events.indices.filter { tl.events[$0].kind == .search && tl.events[$0].duration > 0 }
        var waveChecks = 0, mostCells = 0
        for i in [searches.first, searches.dropFirst(searches.count / 2).first, searches.last].compactMap({ $0 }) {
            let e = tl.events[i]
            for f in [0.3, 0.79, 0.95] {
                try s.seek(time: e.start + f * e.duration)
                try sync(s, r)
                let p = try s.position()
                check(p.settled == (f >= 0.8), "search event \(i) at \(f): settled \(p.settled)")
                mostCells = max(mostCells, r.wavefronts.reduce(0) { $0 + Int($1.cells) })
                try compareSinglePass("search event \(i) at \(f)")
                waveChecks += 1
            }
        }
        print("selftest: \(waveChecks) positions in searches compared, most wavefront cells drawn \(mostCells)")

        // 5. Every pacing mode plays to the end (One net: the net of the
        // middle search), then back to fit length.
        if !tl.events.isEmpty {
            let netSearch = searches.dropFirst(searches.count / 2).first(where: { tl.events[$0].net != nil })
            var line: [String] = []
            for mode in PacingMode.allCases {
                var pacing = Pacing(mode: mode)
                if mode == .oneNet {
                    guard let i = netSearch, let net = tl.events[i].net else {
                        line.append("one net: no search with a net")
                        continue
                    }
                    pacing.net = net
                }
                try s.setPacing(pacing)
                let t = try s.timeline()
                try s.jump(toEnd: false)
                try s.setSpeed(64)
                try s.setPlaying(true)
                let speed = try s.position().speed
                let dt = max(t.total / speed / 100, 1e-3)
                var n = 0, waves = 0
                while try s.position().playing, n < 100_000 {
                    if try s.advance(by: dt).contains(.wavefront) { waves += 1 }
                    n += 1
                }
                let pe = try s.position()
                check(pe.atEnd && !pe.playing, "pacing \(mode.name): playing stopped at t = \(pe.time) of \(pe.total)")
                check(abs(pe.total - t.total) < 1e-9, "pacing \(mode.name): total \(pe.total), timeline \(t.total)")
                line.append(String(format: "%@ %.1f s, %d steps, %d wavefront changes", try s.text(.pacing), t.total,
                                   n, waves))
            }
            try s.setPacing(Pacing())
            print("selftest: pacing modes played to the end: " + line.joined(separator: "; "))
        }

        // 7. The selection (M5): highlighted pictures against single-pass
        // ones, mid-search, at a rip-up (ghosts) and at the end; a click on
        // what a hit test reports selects its net; clearing restores the
        // picture exactly.
        if let i = searches.dropFirst(searches.count / 2).first(where: { tl.events[$0].net != nil }),
           let net = tl.events[i].net {
            let e = tl.events[i]
            try s.seek(time: e.start + 0.5 * e.duration)
            try sync(s, strict)
            guard let before = strict.composite(view: try s.view()) else { fail("cannot make the image") }
            let changed = try s.selectNet(net)
            check(changed.contains(.selection) && changed.contains(.overlay),
                  "selecting net \(net) changed \(changed.rawValue)")
            try compareSinglePass("selected net \(net), search event \(i)")
            var positions = 1
            if let rip = tl.events.indices.first(where: { $0 > i && tl.events[$0].kind == .ripUp }) {
                try s.seek(event: UInt32(rip))
                try compareSinglePass("selected net \(net), rip-up event \(rip)")
                positions += 1
            }
            try s.seek(event: UInt32(i))
            try s.seek(time: e.start + 0.5 * e.duration)
            try s.clearSelection()
            try sync(s, strict)
            guard let after = strict.composite(view: try s.view()) else { fail("cannot make the image") }
            let (n, worst) = differingPixels(before, after, tolerance: 0)
            check(n == 0, "selection cleared: \(n) pixels differ from before it (largest \(worst))")
            // Clicks at the points of a coarse grid: the topmost hit with a
            // net decides the selection.
            try s.jump(toEnd: true)
            let v = try s.view()
            var clicks = 0, selected = 0
            for gy in 1..<8 {
                for gx in 1..<12 {
                    let x = v.width * Double(gx) / 12, y = v.height * Double(gy) / 8
                    let hits = try s.hitTest(viewX: x, viewY: y)
                    try s.click(viewX: x, viewY: y)
                    let sel = try s.selection()
                    clicks += 1
                    if let top = hits.first(where: { $0.net != nil || !$0.netName.isEmpty }) {
                        selected += 1
                        if let n = top.net {
                            check(sel?.net == n, "click at (\(x), \(y)) on \(top.text): selected \(String(describing: sel))")
                        } else {
                            check(sel?.name == top.netName, "click at (\(x), \(y)) on \(top.text): selected \(String(describing: sel))")
                        }
                    } else {
                        check(sel == nil, "click at (\(x), \(y)) on nothing: selected \(String(describing: sel))")
                    }
                }
            }
            if let sel = try s.selection() {
                try compareSinglePass("selected \(sel.name) at the end")
                positions += 1
            }
            try s.clearSelection()
            print("selftest: selection: net \(net) at \(positions) positions compared; \(clicks) clicks, "
                  + "\(selected) on a net")
        }

        // 6. The end: the default position after opening.
        try s.jump(toEnd: true)
        let pe = try s.position()
        try compareSinglePass("end")
        print("selftest: end: copper \(pe.copperAlive), hidden by the board: \(pe.routedHidden ? "yes" : "no"), "
              + "failed pads \(pe.failures)")
        print("selftest: \(compared) positions compared with single-pass renders; cropped planes: at most "
              + "\(worstLoose) pixels off by more than 48")
    } catch {
        failures.append("\(error)")
    }
    if failures.isEmpty {
        print("selftest ok")
        return 0
    }
    for f in failures { FileHandle.standardError.write(Data("selftest FAILED: \(f)\n".utf8)) }
    return 1
}

/// How repeatable the banded rasteriser is (M5 investigation): the same
/// single-pass picture drawn 8 times with the default bands and with one,
/// then the default bands, 2 and 3 bands against one band, with where the
/// differing pixels lie.
@MainActor
func repeatTest(_ o: Options) -> Int32 {
    guard o.positional.count == 1 else { usage() }
    let s = openSession(o.positional[0], o)
    let r = LayeredRenderer()
    do {
        try sync(s, r)
        try s.zoom(by: 4, aboutX: Double(o.width) * 0.4, y: Double(o.height) * 0.6)
        let v = try s.view()
        for (label, bands) in [("default bands", 0), ("1 band", 1)] {
            r.bands = bands
            var worst = 0, worstAny = 0
            guard let first = r.renderSinglePass(view: v) else { fail("no image") }
            for _ in 0..<8 {
                guard let x = r.renderSinglePass(view: v) else { fail("no image") }
                worst = max(worst, differingPixels(first, x, tolerance: 48).0)
                worstAny = max(worstAny, differingPixels(first, x, tolerance: 0).0)
            }
            print("\(label), drawn 9 times: up to \(worst) px off by more than 48, \(worstAny) px differ at all")
        }
        r.bands = 1
        guard let one = r.renderSinglePass(view: v) else { fail("no image") }
        for nb in [0, 2, 3] {
            r.bands = nb
            guard let x = r.renderSinglePass(view: v) else { fail("no image") }
            let pa = pixels(of: x), pb = pixels(of: one)
            var rows: [Int: Int] = [:], worst = 0
            for i in stride(from: 0, to: pa.pixels.count, by: 4) {
                var d = 0
                for c in 0..<4 { d = max(d, abs(Int(pa.pixels[i + c]) - Int(pb.pixels[i + c]))) }
                worst = max(worst, d)
                if d > 0 { rows[(i / 4) / pa.width, default: 0] += 1 }
            }
            let n = nb == 0 ? ProcessInfo.processInfo.activeProcessorCount : nb
            let seams = (1..<n).map { pa.height * $0 / n }
            let near = rows.filter { kv in seams.contains { abs($0 - kv.key) <= 3 } }.values.reduce(0, +)
            print("\(n) bands against 1: \(rows.values.reduce(0, +)) px differ (largest \(worst)) in \(rows.count) "
                  + "rows; \(near) px within 3 rows of a seam")
        }
        return 0
    } catch {
        fail("\(error)")
    }
}

let args = CommandLine.arguments.dropFirst()
guard let first = args.first else { usage() }
switch first {
case "--compare": exit(compare(parseOptions(args.dropFirst())))
case "--bench": exit(bench(parseOptions(args.dropFirst())))
case "--bench-playback": exit(benchPlayback(parseOptions(args.dropFirst())))
case "--bench-live": exit(benchLive(parseOptions(args.dropFirst())))
case "--crops": exit(crops(parseOptions(args.dropFirst())))
case "--selftest": exit(selfTest(parseOptions(args.dropFirst())))
case "--repeat": exit(repeatTest(parseOptions(args.dropFirst())))
default: exit(render(parseOptions(args)))
}
