// DisplayListRenderer.swift: draws the core's display list into any
// CGContext.  Shared by the app's BoardView and by rvrender.
//
// The core decides what is drawn, in which colour and order; this file only
// turns batches into Core Graphics paths.  Paths are built once per batch,
// in board millimetres, and kept while the core's geometry generation stays
// the same (visibility changes reuse them); the camera is applied as the
// context's transform, so panning and zooming never rebuild a path.

import CoreGraphics
import Dispatch
import Foundation

@MainActor
public final class DisplayListRenderer {
    fileprivate struct Prepared {
        let path: CGPath
        let color: CGColor
        let primitive: DisplayBatch.Primitive
        let width: CGFloat
        let bbox: CGRect
    }

    private var geometryGeneration: UInt32?
    private var generation: UInt32?
    private var cache: [UInt32: Prepared] = [:]
    private var drawList: [Prepared] = []
    public private(set) var background = CGColor(srgbRed: 0, green: 16 / 255, blue: 35 / 255, alpha: 1)
    /// The display list last given to update(), if any.
    public private(set) var list: DisplayList?

    public init() {}

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
        drawList = list.batches.map { batch in
            if let p = cache[batch.key] { return p }
            let p = Prepared(path: DisplayListRenderer.path(for: batch, in: list),
                             color: DisplayListRenderer.color(batch.rgba), primitive: batch.primitive,
                             width: CGFloat(batch.width),
                             bbox: CGRect(x: CGFloat(batch.bbox.0), y: CGFloat(batch.bbox.1),
                                          width: CGFloat(batch.bbox.2 - batch.bbox.0),
                                          height: CGFloat(batch.bbox.3 - batch.bbox.1)))
            cache[batch.key] = p
            return p
        }
    }

    /// Draw the background and the board.  `ctx` must be in view points
    /// with y down (a flipped NSView, or a bitmap flipped by the caller);
    /// `clip` (view points) limits what is drawn, nil for the whole view.
    public func draw(in ctx: CGContext, view: ViewTransform, clip: CGRect? = nil) {
        let area = clip ?? CGRect(x: 0, y: 0, width: view.width, height: view.height)
        DisplayListRenderer.draw(drawList, background: background, in: ctx, view: view, area: area)
    }

    private nonisolated static func draw(_ items: [Prepared], background: CGColor, in ctx: CGContext,
                                         view: ViewTransform, area: CGRect) {
        ctx.saveGState()
        ctx.setFillColor(background)
        ctx.fill(area)
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
        let n = max(1, min(h, bands > 0 ? bands : ProcessInfo.processInfo.activeProcessorCount))
        let job = BandJob(items: drawList, background: background, view: view, buffer: buffer, bands: n)
        DispatchQueue.concurrentPerform(iterations: n) { i in job.run(band: i) }
    }

    fileprivate final class BandJob: @unchecked Sendable {
        let items: [Prepared]
        let background: CGColor
        let view: ViewTransform
        let buffer: PixelBuffer
        let bands: Int

        init(items: [Prepared], background: CGColor, view: ViewTransform, buffer: PixelBuffer, bands: Int) {
            self.items = items
            self.background = background
            self.view = view
            self.buffer = buffer
            self.bands = bands
        }

        func run(band i: Int) {
            let h = buffer.height
            let y0 = h * i / bands, y1 = h * (i + 1) / bands
            guard y1 > y0, let ctx = buffer.context(rows: y0..<y1) else { return }
            // Band context: memory rows y0..<y1 of the image, y up from its
            // last row.  Map view points (y down) onto it.
            ctx.translateBy(x: 0, y: CGFloat(y1 - y0))
            ctx.scaleBy(x: 1, y: -1)
            ctx.translateBy(x: 0, y: -CGFloat(y0))
            ctx.scaleBy(x: CGFloat(view.backing), y: CGFloat(view.backing))
            let area = CGRect(x: 0, y: CGFloat(y0) / view.backing, width: view.width,
                              height: CGFloat(y1 - y0) / view.backing)
            DisplayListRenderer.draw(items, background: background, in: ctx, view: view, area: area)
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

/// An RGBA8 premultiplied sRGB pixel buffer that band contexts share.
final class PixelBuffer: @unchecked Sendable {
    let width: Int
    let height: Int
    let bytesPerRow: Int
    private let data: UnsafeMutableRawPointer
    private let space = CGColorSpace(name: CGColorSpace.sRGB)!

    init(width: Int, height: Int) {
        self.width = width
        self.height = height
        bytesPerRow = width * 4
        data = UnsafeMutableRawPointer.allocate(byteCount: bytesPerRow * height, alignment: 64)
    }

    deinit { data.deallocate() }

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
