// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// DisplayListRenderer.swift: draws the core's display list into any
// CGContext.  Shared by the app's BoardView and by rvrender.
//
// The core decides what is drawn, in which colour and order; this file only
// turns batches into Core Graphics paths.  Paths are built once per batch,
// in board millimetres, and kept while the core's geometry generation stays
// the same (visibility changes reuse them); the camera is applied as the
// context's transform, so panning and zooming never rebuild a path.  A
// batch's colour is taken from the list every time (the overlay's ghosts
// keep their key while they fade).

import CoreGraphics
import Dispatch
import Foundation

@MainActor
public final class DisplayListRenderer {
    struct Prepared {
        let path: CGPath
        let color: CGColor
        let primitive: DisplayBatch.Primitive
        let width: CGFloat
        let bbox: CGRect
        let order: UInt32
    }

    private struct CachedPath {
        let path: CGPath
        let bbox: CGRect
    }

    private var geometryGeneration: UInt32?
    private var generation: UInt32?
    private var cache: [UInt32: CachedPath] = [:]
    private let keepsUnusedPaths: Bool
    private(set) var drawList: [Prepared] = []
    public private(set) var background = CGColor(srgbRed: 0, green: 16 / 255, blue: 35 / 255, alpha: 1)
    /// The display list last given to update(), if any.
    public private(set) var list: DisplayList?
    /// Paths built by update() since the renderer was made (for tests and timing).
    public private(set) var pathsBuilt = 0

    /// `keepsUnusedPaths` false: after each update only the paths of the
    /// list's own batches stay cached (for the overlay, whose keys come and
    /// go as the position moves).
    public init(keepsUnusedPaths: Bool = true) {
        self.keepsUnusedPaths = keepsUnusedPaths
    }

    /// Number of batches that draw() will draw.
    public var batchCount: Int { drawList.count }

    /// Take a new display list.  Paths of batches already prepared for the
    /// same geometry generation are kept.
    public func update(_ list: DisplayList) {
        if generation == list.generation, geometryGeneration == list.geometryGeneration { return }
        if geometryGeneration != list.geometryGeneration {
            cache.removeAll(keepingCapacity: true)
            geometryGeneration = list.geometryGeneration
        }
        generation = list.generation
        self.list = list
        background = DisplayListRenderer.color(list.background)
        var used: [UInt32: CachedPath] = keepsUnusedPaths ? [:] : Dictionary(minimumCapacity: list.batches.count)
        drawList = list.batches.map { batch in
            let cached: CachedPath
            if let c = cache[batch.key] {
                cached = c
            } else {
                cached = CachedPath(path: DisplayListRenderer.path(for: batch, in: list),
                                    bbox: CGRect(x: CGFloat(batch.bbox.0), y: CGFloat(batch.bbox.1),
                                                 width: CGFloat(batch.bbox.2 - batch.bbox.0),
                                                 height: CGFloat(batch.bbox.3 - batch.bbox.1)))
                pathsBuilt += 1
                if keepsUnusedPaths { cache[batch.key] = cached }
            }
            if !keepsUnusedPaths { used[batch.key] = cached }
            return Prepared(path: cached.path, color: DisplayListRenderer.color(batch.rgba),
                            primitive: batch.primitive, width: CGFloat(batch.width), bbox: cached.bbox,
                            order: batch.order)
        }
        if !keepsUnusedPaths { cache = used }
    }

    /// Draw the background and the board.  `ctx` must be in view points
    /// with y down (a flipped NSView, or a bitmap flipped by the caller);
    /// `clip` (view points) limits what is drawn, nil for the whole view.
    public func draw(in ctx: CGContext, view: ViewTransform, clip: CGRect? = nil) {
        let area = clip ?? CGRect(x: 0, y: 0, width: view.width, height: view.height)
        DisplayListRenderer.draw(drawList[...], background: background, in: ctx, view: view, area: area)
    }

    /// Draw `items` over `area` (view points): the background first, or with
    /// nil a transparent clear (with `clear` false: over what is there).
    nonisolated static func draw(_ items: ArraySlice<Prepared>, background: CGColor?, clear: Bool = true,
                                 in ctx: CGContext, view: ViewTransform, area: CGRect) {
        ctx.saveGState()
        if let background {
            ctx.setFillColor(background)
            ctx.fill(area)
        } else if clear {
            ctx.clear(area)
        }
        ctx.setShouldAntialias(true)
        ctx.setAllowsAntialiasing(true)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        let t = CGAffineTransform(a: view.scale, b: 0, c: 0, d: view.scale, tx: view.tx, ty: view.ty)
        ctx.concatenate(t)
        // The visible board rectangle, for skipping batches outside it.
        let visible = area.applying(t.inverted())
        for p in items where p.bbox.intersects(visible) {
            ctx.addPath(p.path)
            switch p.primitive {
            case .fill, .disc:
                ctx.setFillColor(p.color)
                ctx.fillPath(using: .winding)
            case .stroke, .segs, .ring:
                ctx.setStrokeColor(p.color)
                ctx.setLineWidth(p.width)
                ctx.strokePath()
            }
        }
        ctx.restoreGState()
    }

    // MARK: - Parallel rendering into a bitmap

    /// Pixel buffer reused between frames while the size stays the same.
    private var buffer: PixelBuffer?

    /// Render the whole view into a new image of view size x backing pixels,
    /// in `bands` horizontal bands drawn at the same time on separate
    /// threads (Core Graphics' cost grows with the pixel area each fill
    /// covers, so bands divide it).  `bands` 0 picks one per active core.
    public func renderImage(view: ViewTransform, bands: Int = 0) -> CGImage? {
        render(view: view, bands: bands)
        return buffer?.image()
    }

    /// renderImage without making the image: only the rasterising, into the
    /// renderer's own buffer (for timing).
    public func render(view: ViewTransform, bands: Int = 0) {
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        if buffer?.width != w || buffer?.height != h { buffer = PixelBuffer(width: w, height: h) }
        guard let buffer else { return }
        DisplayListRenderer.rasterize(drawList[...], background: background, view: view, originX: 0, originY: 0,
                                      into: buffer, bands: bands)
    }

    /// Rasterise `items` into `buffer`, whose pixel (0, 0) is view pixel
    /// (originX, originY) (view points x backing), in parallel bands.
    nonisolated static func rasterize(_ items: ArraySlice<Prepared>, background: CGColor?, clear: Bool = true,
                                      view: ViewTransform, originX: Int, originY: Int, into buffer: PixelBuffer,
                                      bands: Int) {
        let n = max(1, min(buffer.height, bands > 0 ? bands : ProcessInfo.processInfo.activeProcessorCount))
        let job = BandJob(items: items, background: background, clear: clear, view: view, buffer: buffer, bands: n,
                          originX: originX, originY: originY)
        DispatchQueue.concurrentPerform(iterations: n) { i in job.run(band: i) }
    }

    fileprivate final class BandJob: @unchecked Sendable {
        let items: ArraySlice<Prepared>
        let background: CGColor?
        let clear: Bool
        let view: ViewTransform
        let buffer: PixelBuffer
        let bands: Int
        let originX: Int
        let originY: Int

        init(items: ArraySlice<Prepared>, background: CGColor?, clear: Bool, view: ViewTransform,
             buffer: PixelBuffer, bands: Int, originX: Int, originY: Int) {
            self.items = items
            self.background = background
            self.clear = clear
            self.view = view
            self.buffer = buffer
            self.bands = bands
            self.originX = originX
            self.originY = originY
        }

        func run(band i: Int) {
            let h = buffer.height
            let y0 = h * i / bands, y1 = h * (i + 1) / bands
            guard y1 > y0, let ctx = buffer.context(rows: y0..<y1) else { return }
            // Band context: memory rows y0..<y1 of the image, y up from its
            // last row.  Map view points (y down) onto it.
            ctx.translateBy(x: 0, y: CGFloat(y1 - y0))
            ctx.scaleBy(x: 1, y: -1)
            ctx.translateBy(x: -CGFloat(originX), y: -CGFloat(y0 + originY))
            ctx.scaleBy(x: CGFloat(view.backing), y: CGFloat(view.backing))
            let area = CGRect(x: CGFloat(originX) / view.backing, y: CGFloat(originY + y0) / view.backing,
                              width: CGFloat(buffer.width) / view.backing,
                              height: CGFloat(y1 - y0) / view.backing)
            DisplayListRenderer.draw(items, background: background, clear: clear, in: ctx, view: view, area: area)
        }
    }

    // MARK: - Building paths

    public static func color(_ rgba: UInt32) -> CGColor {
        CGColor(srgbRed: CGFloat((rgba >> 24) & 0xFF) / 255, green: CGFloat((rgba >> 16) & 0xFF) / 255,
                blue: CGFloat((rgba >> 8) & 0xFF) / 255, alpha: CGFloat(rgba & 0xFF) / 255)
    }

    /// The path of one batch in board millimetres.  Fills are built so that
    /// the non-zero rule gives the core's even-odd-per-polygon result (the
    /// core orients outlines and holes oppositely).
    public static func path(for batch: DisplayBatch, in list: DisplayList) -> CGPath {
        let path = CGMutablePath()
        let xy = list.xy
        func addContour(_ c: Int, close: Bool) {
            let start = Int(list.contours[2 * c]), n = Int(list.contours[2 * c + 1])
            guard n > 0 else { return }
            path.move(to: CGPoint(x: CGFloat(xy[2 * start]), y: CGFloat(xy[2 * start + 1])))
            if n == 1 {
                // A one-point polyline (a dot): a zero-length line, which a
                // round cap draws as a disc.
                path.addLine(to: CGPoint(x: CGFloat(xy[2 * start]), y: CGFloat(xy[2 * start + 1])))
            }
            for k in 1..<max(n, 1) {
                let i = 2 * (start + k)
                path.addLine(to: CGPoint(x: CGFloat(xy[i]), y: CGFloat(xy[i + 1])))
            }
            if close { path.closeSubpath() }
        }
        let range = batch.first..<(batch.first + batch.count)
        switch batch.primitive {
        case .fill:
            for p in range {
                let c0 = Int(list.polys[2 * p]), nc = Int(list.polys[2 * p + 1])
                for c in c0..<(c0 + nc) { addContour(c, close: true) }
            }
        case .stroke:
            for c in range { addContour(c, close: false) }
        case .segs:
            let s = list.segs
            for k in range {
                path.move(to: CGPoint(x: CGFloat(s[4 * k]), y: CGFloat(s[4 * k + 1])))
                path.addLine(to: CGPoint(x: CGFloat(s[4 * k + 2]), y: CGFloat(s[4 * k + 3])))
            }
        case .disc, .ring:
            let c = list.circles
            for k in range {
                let r = CGFloat(c[3 * k + 2])
                path.addEllipse(in: CGRect(x: CGFloat(c[3 * k]) - r, y: CGFloat(c[3 * k + 1]) - r,
                                           width: 2 * r, height: 2 * r))
            }
        }
        return path
    }
}

/// Bitmap memory for PixelBuffer: anonymous mappings, with a small pool of
/// recently freed ones.
///
/// Playback makes a new bitmap for every plane it redraws.  Fresh mappings
/// cost a zero-filling page fault per page (slow at Retina sizes); malloc
/// reuses faulted pages but keeps freed blocks of these sizes cached, which
/// held several hundred megabytes.  The pool keeps at most `poolLimit` bytes
/// of freed mappings and unmaps the rest at once.  Reused memory is not
/// cleared: every user fills or clears its whole area before drawing.
enum BitmapMemory {
    /// Mappings are whole multiples of this.
    static let granule = 1 << 20
    /// Freed bytes kept for reuse.
    static let poolLimit = 96 << 20

    private struct Region {
        let pointer: UnsafeMutableRawPointer
        let size: Int
    }

    private final class Pool: @unchecked Sendable {
        let lock = NSLock()
        var regions: [Region] = []
        var bytes = 0
    }

    private static let pool = Pool()

    /// Memory of at least `size` bytes, and its actual size; nil if mapping failed.
    static func take(_ size: Int) -> (UnsafeMutableRawPointer, Int)? {
        let want = (max(1, size) + granule - 1) / granule * granule
        pool.lock.lock()
        // The smallest pooled region that fits and wastes at most half of itself.
        var best: Int?
        for (i, r) in pool.regions.enumerated() where r.size >= want && r.size <= 2 * want {
            if best == nil || r.size < pool.regions[best!].size { best = i }
        }
        if let best {
            let r = pool.regions.remove(at: best)
            pool.bytes -= r.size
            pool.lock.unlock()
            return (r.pointer, r.size)
        }
        pool.lock.unlock()
        let p = mmap(nil, want, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
        guard let p, p != MAP_FAILED else { return nil }
        return (p, want)
    }

    /// Give memory from take() back (any thread).
    static func give(_ pointer: UnsafeMutableRawPointer, _ size: Int) {
        pool.lock.lock()
        var unmap: [Region] = []
        if size <= poolLimit {
            pool.regions.append(Region(pointer: pointer, size: size))
            pool.bytes += size
            // Over the limit: unmap the oldest.
            while pool.bytes > poolLimit {
                let r = pool.regions.removeFirst()
                pool.bytes -= r.size
                unmap.append(r)
            }
        } else {
            unmap.append(Region(pointer: pointer, size: size))
        }
        pool.lock.unlock()
        for r in unmap { munmap(r.pointer, r.size) }
    }

    /// Bytes held in the pool now.
    static var pooledBytes: Int {
        pool.lock.lock()
        defer { pool.lock.unlock() }
        return pool.bytes
    }
}

/// An RGBA8 premultiplied sRGB pixel buffer that band contexts share.  Its
/// memory comes from BitmapMemory and is not cleared.
final class PixelBuffer: @unchecked Sendable {
    let width: Int
    let height: Int
    let bytesPerRow: Int
    private let data: UnsafeMutableRawPointer
    /// Bytes of `data`: at least bytesPerRow x height.
    private let size: Int
    private let mapped: Bool
    private let space = CGColorSpace(name: CGColorSpace.sRGB)!

    init(width: Int, height: Int) {
        self.width = width
        self.height = height
        bytesPerRow = width * 4
        if let (p, n) = BitmapMemory.take(bytesPerRow * height) {
            data = p
            size = n
            mapped = true
        } else {
            size = max(1, bytesPerRow * height)
            data = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 64)
            mapped = false
        }
    }

    /// False once takeImage() handed the memory to an image.
    private var ownsData = true

    deinit {
        guard ownsData else { return }
        if mapped { BitmapMemory.give(data, size) } else { data.deallocate() }
    }

    /// An image over the buffer's own memory, without copying it.  The
    /// image owns the memory from then on (it is freed when the image is),
    /// so the buffer must not be drawn into or used again.
    func takeImage() -> CGImage? {
        guard ownsData else { return nil }
        let provider: CGDataProvider?
        let used = bytesPerRow * height
        if mapped {
            // The info pointer carries the mapping's full size.
            let info = UnsafeMutableRawPointer(bitPattern: size)
            provider = CGDataProvider(dataInfo: info, data: data, size: used, releaseData: { info, memory, _ in
                BitmapMemory.give(UnsafeMutableRawPointer(mutating: memory), Int(bitPattern: info))
            })
        } else {
            provider = CGDataProvider(dataInfo: nil, data: data, size: used, releaseData: { _, memory, _ in
                UnsafeMutableRawPointer(mutating: memory).deallocate()
            })
        }
        guard let provider else { return nil }
        ownsData = false
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: bytesPerRow, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    /// A context over image rows `rows` (row 0 is the top of the image).
    func context(rows: Range<Int>) -> CGContext? {
        CGContext(data: data + rows.lowerBound * bytesPerRow, width: width, height: rows.count,
                  bitsPerComponent: 8, bytesPerRow: bytesPerRow, space: space,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    }

    /// An image of the buffer as it is now (copied, so the buffer can be
    /// drawn into again at once).
    func image() -> CGImage? {
        guard let cf = CFDataCreate(nil, data.assumingMemoryBound(to: UInt8.self), bytesPerRow * height),
              let provider = CGDataProvider(data: cf)
        else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: bytesPerRow, space: space,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}
