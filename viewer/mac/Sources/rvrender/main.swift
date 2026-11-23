// rvrender: draw a trace's board to a PNG without a window, compare two
// PNGs, or time full redraws.  Uses RVKit's DisplayListRenderer, the same
// code the app draws with.
//
//   rvrender FILE OUT.png [--size WxH] [--board N] [--layers a,b,c | --all-layers]
//                         [--hide-classes i,j] [--fit | --window x0,y0,x1,y1]
//   rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
//   rvrender --bench FILE [--size WxH] [--frames N] [--board N]
//
// Exit status 0 on success (and for --compare, images within tolerance),
// 1 on failure, 2 for bad usage.

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
           rvrender --compare GOLDEN.png NEW.png [--tolerance T] [--max-fraction F]
           rvrender --bench FILE [--size WxH] [--frames N] [--board N]
    """, code: 2)
}

struct Options {
    var positional: [String] = []
    var width = 1600, height = 1000
    var board: UInt32?
    var layers: [String]?
    var allLayers = false
    var hideClasses: [UInt32] = []
    var window: (Double, Double, Double, Double)?
    var tolerance = 48
    var maxFraction = 0.002
    var frames = 60
    var profile = false
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
        default:
            if a.hasPrefix("--") { usage() }
            o.positional.append(a)
        }
    }
    return o
}

// MARK: - Session set-up

@MainActor
func openSession(_ path: String, _ o: Options) -> Session {
    do {
        let s = try Session()
        try s.open(path: path)
        try s.selectBoard(o.board)
        if try s.selectedBoard() == nil { fail("\(path): no board to draw") }
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

// MARK: - Modes

@MainActor
func render(_ o: Options) -> Int32 {
    guard o.positional.count == 2 else { usage() }
    let s = openSession(o.positional[0], o)
    let renderer = DisplayListRenderer()
    do {
        guard let list = try s.displayList() else { fail("no display list") }
        renderer.update(list)
        // The app's path: parallel bands into one bitmap.
        guard let image = renderer.renderImage(view: try s.view()) else { fail("cannot make the image") }
        writePNG(image, to: o.positional[1])
        print("rvrender: \(o.positional[1]): \(o.width) x \(o.height), \(renderer.batchCount) batches")
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

let args = CommandLine.arguments.dropFirst()
guard let first = args.first else { usage() }
switch first {
case "--compare": exit(compare(parseOptions(args.dropFirst())))
case "--bench": exit(bench(parseOptions(args.dropFirst())))
default: exit(render(parseOptions(args)))
}
