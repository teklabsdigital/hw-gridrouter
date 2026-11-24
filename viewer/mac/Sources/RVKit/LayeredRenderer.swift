// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// LayeredRenderer.swift: the board and the overlay as a stack of cached
// bitmaps ("planes"), so that playback redraws only what changed.
//
// The core's overlay slots cut the order axis (see rv_get_overlay in rv.h).
// Between two slots the board's batches form a static slab; each slot holds
// overlay batches.  Planes, bottom to top:
//
//     slab 0 (with the background), slot 0, slab 1, slot 1, ..., slab n
//
// A slab is redrawn only when the board list or the camera changes; a slot
// only when its generation (or the overlay's key epoch) or the camera
// changes, and only over the bounds of its batches.  Stacking the planes
// gives the same picture as drawing every batch in order in one pass
// (renderSinglePass, which the self-tests compare against).
//
// A wavefront slot (M4) holds no batches: its plane is the core's
// wavefront image for that order (update(wavefront:)), one pixel per grid
// cell, placed over its board rectangle and scaled with nearest-neighbour
// sampling (`nearest`).  It is never rasterised: a camera change only moves
// its frame, a new generation of the image only swaps its contents.

import CoreGraphics
import Dispatch
import Foundation

@MainActor
public final class LayeredRenderer {
    public final class Plane {
        public enum Kind: Equatable, Sendable {
            case slab(Int)
            case slot(OverlaySlot.Kind)
        }

        public let kind: Kind
        /// nil when the plane draws nothing.
        public fileprivate(set) var image: CGImage?
        /// Where the image goes, in view points (y down).
        public fileprivate(set) var frame: CGRect = .zero
        /// The same in pixels of the view (points x backing).
        public fileprivate(set) var pixelRect: CGRect = .zero
        /// Changes whenever `image` or `frame` changes.
        public fileprivate(set) var version = 0
        /// Times this plane was rasterised.
        public fileprivate(set) var renders = 0
        /// The image is a grid-resolution wavefront: scale it to `frame`
        /// with nearest-neighbour sampling (frame and pixelRect fractional).
        public var nearest: Bool { kind == .slot(.wavefront) }

        fileprivate var items: [DisplayListRenderer.Prepared] = []
        fileprivate var dirty = true
        fileprivate var slotGeneration: UInt32?
        fileprivate var wave: WavefrontImage?
        fileprivate var orderBegin: UInt32 = 0

        fileprivate init(kind: Kind) {
            self.kind = kind
        }

        public var isSlot: Bool {
            if case .slot = kind { return true }
            return false
        }
    }

    /// Bottom to top.
    public private(set) var planes: [Plane] = []
    /// Changes when planes are added or removed: the client rebuilds its layers.
    public private(set) var structure = 0

    private let board = DisplayListRenderer()
    private let overlay = DisplayListRenderer(keepsUnusedPaths: false)
    private var slots: [OverlaySlot] = []
    private var shape: [UInt64] = []
    private var boardGeneration: UInt32?
    private var boardGeometry: UInt32?
    private var overlayGeneration: UInt32?
    private var overlayEpoch: UInt32?
    private var boardChanged = true
    private var epochChanged = true
    private var needsLayout = true
    private var view: ViewTransform?

    /// Duration of the last render(), seconds, and what it did.
    public private(set) var lastRenderSeconds = 0.0
    public private(set) var lastRenderedPlanes = 0
    public private(set) var lastRenderedPixels = 0

    /// Planes other than the bottom slab cover only the bounds of their
    /// batches, and a plane that is one solid rectangle is a stretched
    /// 1 x 1 image (the default); or every plane covers the whole view.
    /// (The name is M4's, when only slot planes were cropped.)  Core Graphics anti-aliases a thin diagonal stroke a
    /// little differently at another bitmap offset, so a cropped plane can
    /// differ from a single-pass render by a few edge pixels; uncropped and
    /// with `bands` 1 the two are identical (the self-tests check both).
    public var cropsSlots = true
    /// Bands for parallel rasterising; 0 picks one per active core.
    public var bands = 0
    /// M6 (carry-over from M5): when > 0, a slot plane's crop is widened to
    /// multiples of this many view pixels and drawn in one bitmap context
    /// (not in bands), so that its context sits at a view-anchored place
    /// whatever the crop: the same batches then give the same pixels when
    /// the crop changes.  Slabs (redrawn only when the board or the camera
    /// changes) keep their bands.  0 (the default): M5's crops and bands.
    /// See README "Bands" for the measurements.
    public var cropGrid = 0

    public init() {}

    public var background: CGColor { board.background }
    /// The static list last given, if any.
    public var boardList: DisplayList? { board.list }
    /// The overlay list last given, if any.
    public var overlayList: DisplayList? { overlay.list }
    /// Batches drawn: board and overlay.
    public var batchCount: Int { board.batchCount + overlay.batchCount }
    public var boardBatchCount: Int { board.batchCount }
    public var overlayBatchCount: Int { overlay.batchCount }
    /// The overlay slots last given.
    public var overlaySlots: [OverlaySlot] { slots }

    /// Take a new static display list (board or router outline).
    public func update(board list: DisplayList) {
        if list.generation == boardGeneration, list.geometryGeneration == boardGeometry { return }
        boardGeneration = list.generation
        boardGeometry = list.geometryGeneration
        board.update(list)
        boardChanged = true
        needsLayout = true
    }

    /// Take a new overlay list and its slots.
    public func update(overlay o: Overlay) {
        if o.list.generation == overlayGeneration, o.list.geometryGeneration == overlayEpoch { return }
        if o.list.geometryGeneration != overlayEpoch { epochChanged = true }
        overlayGeneration = o.list.generation
        overlayEpoch = o.list.geometryGeneration
        overlay.update(o.list)
        slots = o.slots.sorted { $0.orderBegin < $1.orderBegin }
        needsLayout = true
    }

    /// The wavefront images last given.
    public private(set) var wavefronts: [WavefrontImage] = []
    /// Wavefront planes whose image changed in the last render().
    public private(set) var lastWavefrontChanges = 0

    /// Take the wavefront images (Session.wavefrontImages).
    public func update(wavefront images: [WavefrontImage]) {
        let same = images.count == wavefronts.count && zip(images, wavefronts).allSatisfy {
            $0.generation == $1.generation && $0.order == $1.order && $0.routerLayer == $1.routerLayer
        }
        if same { return }
        wavefronts = images
        for p in planes where p.nearest { assignWave(p) }
    }

    private func assignWave(_ plane: Plane) {
        let w = wavefronts.first { $0.order == plane.orderBegin }
        if w?.generation != plane.wave?.generation || w?.order != plane.wave?.order || (w == nil) != (plane.wave == nil) {
            plane.wave = w
            plane.dirty = true
        }
    }

    /// Mark every plane for redrawing (after the window's colour space changed, say).
    public func invalidate() {
        for p in planes { p.dirty = true }
    }

    // MARK: - Planes

    private func layout() {
        let newShape = slots.map { UInt64($0.kind.rawValue) << 32 | UInt64($0.orderBegin) }
        if newShape != shape || planes.isEmpty {
            shape = newShape
            var p: [Plane] = []
            for k in 0...slots.count {
                p.append(Plane(kind: .slab(k)))
                if k < slots.count {
                    let plane = Plane(kind: .slot(slots[k].kind))
                    plane.orderBegin = slots[k].orderBegin
                    p.append(plane)
                }
            }
            planes = p
            structure += 1
            boardChanged = true
            epochChanged = true
            for plane in planes where plane.nearest { assignWave(plane) }
        }
        if boardChanged {
            // Slab k: the board batches below slot k and above slot k - 1.
            var slabItems = [[DisplayListRenderer.Prepared]](repeating: [], count: slots.count + 1)
            var k = 0
            for item in board.drawList {
                while k < slots.count && item.order >= slots[k].orderBegin { k += 1 }
                slabItems[k].append(item)
            }
            for (i, items) in slabItems.enumerated() {
                let plane = planes[2 * i]
                plane.items = items
                plane.dirty = true
            }
            boardChanged = false
        }
        // Slot k: the overlay batches in its order range.
        let items = overlay.drawList
        var lo = 0
        for (k, s) in slots.enumerated() {
            let plane = planes[2 * k + 1]
            while lo < items.count && items[lo].order < s.orderBegin { lo += 1 }
            var hi = lo
            while hi < items.count && items[hi].order < s.orderEnd { hi += 1 }
            if epochChanged || plane.slotGeneration != s.generation {
                plane.items = Array(items[lo..<hi])
                plane.slotGeneration = s.generation
                plane.dirty = true
            }
            lo = hi
        }
        epochChanged = false
        needsLayout = false
    }

    /// Bring every plane up to date for `view`; returns how many were
    /// rasterised again (0 when nothing changed).
    @discardableResult
    public func render(view: ViewTransform) -> Int {
        let start = DispatchTime.now().uptimeNanoseconds
        if needsLayout || planes.isEmpty { layout() }
        if view != self.view {
            self.view = view
            for p in planes { p.dirty = true }
        }
        var count = 0, pixels = 0, waves = 0
        for (i, plane) in planes.enumerated() where plane.dirty {
            if plane.nearest {
                if placeWave(plane, view: view) { waves += 1 }
                continue
            }
            pixels += renderPlane(plane, bottom: i == 0, view: view, bands: bands)
            count += 1
        }
        lastWavefrontChanges = waves
        lastRenderSeconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
        lastRenderedPlanes = count
        lastRenderedPixels = pixels
        return count
    }

    /// Where a wavefront image goes in the view: points and (fractional)
    /// pixels, exactly over its cells.  Where a cell edge falls on a pixel
    /// centre, Core Animation (the layers) and Core Graphics (a single-pass
    /// render) may give that pixel to different cells: the two pictures can
    /// differ by one pixel at cell edges (the self-test allows that there,
    /// and only there).  Rounding the frame to whole pixels does not help:
    /// it makes such ties inside the image more frequent.
    nonisolated static func waveRects(_ w: WavefrontImage, view: ViewTransform) -> (frame: CGRect, pixels: CGRect) {
        let r = w.boardRect
        let frame = CGRect(x: r.minX * view.scale + view.tx, y: r.minY * view.scale + view.ty,
                           width: r.width * view.scale, height: r.height * view.scale)
        let b = view.backing
        return (frame, CGRect(x: frame.minX * b, y: frame.minY * b, width: frame.width * b, height: frame.height * b))
    }

    /// True if the centre of view pixel (x, y) (y down) lies within `slack`
    /// pixels of a cell edge of a wavefront image drawn at `view`: where the
    /// layers and a single-pass render may give the pixel to different cells.
    public func isWavefrontCellEdge(x: Int, y: Int, view: ViewTransform, slack: Double = 0.02) -> Bool {
        let cx = Double(x) + 0.5, cy = Double(y) + 0.5
        for w in wavefronts {
            guard let image = w.image else { continue }
            let r = LayeredRenderer.waveRects(w, view: view).pixels
            guard cx > r.minX - 1, cx < r.maxX + 1, cy > r.minY - 1, cy < r.maxY + 1 else { continue }
            let cw = r.width / Double(image.width), ch = r.height / Double(image.height)
            let u = (cx - r.minX) / cw, v = (cy - r.minY) / ch
            if abs(u - u.rounded()) * cw <= slack || abs(v - v.rounded()) * ch <= slack { return true }
        }
        return false
    }

    /// Bring a wavefront plane to its image and the camera; true if its
    /// image changed (not only its place).
    private func placeWave(_ plane: Plane, view: ViewTransform) -> Bool {
        plane.dirty = false
        let image = plane.wave?.image
        let changed = image !== plane.image
        let rects = plane.wave.map { LayeredRenderer.waveRects($0, view: view) } ?? (CGRect.zero, CGRect.zero)
        if !changed && rects.0 == plane.frame { return false }
        plane.image = image
        plane.frame = image == nil ? .zero : rects.0
        plane.pixelRect = image == nil ? .zero : rects.1
        plane.version += 1
        return changed
    }

    private func renderPlane(_ plane: Plane, bottom: Bool, view: ViewTransform, bands: Int) -> Int {
        plane.dirty = false
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        var rect: (x0: Int, y0: Int, x1: Int, y1: Int)?
        if !bottom, cropsSlots, plane.items.count == 1, plane.items[0].primitive == .fill,
           solidRectangle(plane, view: view, width: w, height: h) {
            return 1
        }
        if bottom {
            rect = (0, 0, w, h)
        } else if !plane.items.isEmpty {
            if cropsSlots {
                // Only the bounds of the plane's batches (board mm, which
                // include half the stroke widths), plus 2 pixels for
                // anti-aliasing.  M5: slabs too, not only slots (M4 kept
                // every slab view-sized).
                var b = plane.items[0].bbox
                for item in plane.items.dropFirst() { b = b.union(item.bbox) }
                let s = view.scale * view.backing
                let x0 = Int(((b.minX * s + view.tx * view.backing)).rounded(.down)) - 2
                let y0 = Int(((b.minY * s + view.ty * view.backing)).rounded(.down)) - 2
                let x1 = Int(((b.maxX * s + view.tx * view.backing)).rounded(.up)) + 2
                let y1 = Int(((b.maxY * s + view.ty * view.backing)).rounded(.up)) + 2
                let r = LayeredRenderer.snap((x0, y0, x1, y1), grid: plane.isSlot ? cropGrid : 0, width: w, height: h)
                if r.2 > r.0 && r.3 > r.1 { rect = r }
            } else {
                rect = (0, 0, w, h)
            }
        }
        plane.version += 1
        guard let rect else {
            plane.image = nil
            plane.frame = .zero
            plane.pixelRect = .zero
            return 0
        }
        let pw = rect.x1 - rect.x0, ph = rect.y1 - rect.y0
        // A fresh buffer each time, handed to the image without a copy: the
        // previous image (still on screen until its layer is updated) keeps
        // its own memory, and a plane holds one bitmap, not two.
        let buffer = PixelBuffer(width: pw, height: ph)
        DisplayListRenderer.rasterize(plane.items[...], background: bottom ? board.background : nil, view: view,
                                      originX: rect.x0, originY: rect.y0, into: buffer,
                                      bands: plane.isSlot && cropGrid > 0 ? 1 : bands)
        plane.image = buffer.takeImage()
        plane.renders += 1
        plane.pixelRect = CGRect(x: rect.x0, y: rect.y0, width: pw, height: ph)
        plane.frame = CGRect(x: CGFloat(rect.x0) / view.backing, y: CGFloat(rect.y0) / view.backing,
                             width: CGFloat(pw) / view.backing, height: CGFloat(ph) / view.backing)
        return pw * ph
    }

    /// A crop rectangle (view pixels) clamped to the view; with `grid` > 0
    /// first widened to multiples of `grid`.
    nonisolated static func snap(_ r: (Int, Int, Int, Int), grid: Int, width w: Int, height h: Int)
        -> (x0: Int, y0: Int, x1: Int, y1: Int) {
        var (x0, y0, x1, y1) = r
        if grid > 0 {
            func down(_ v: Int) -> Int { v >= 0 ? v / grid * grid : -((-v + grid - 1) / grid * grid) }
            x0 = down(x0)
            y0 = down(y0)
            x1 = -down(-x1)
            y1 = -down(-y1)
        }
        return (max(0, x0), max(0, y0), min(w, x1), min(h, y1))
    }

    /// Carry-over measurement (M6): how much a cropped plane's pixels change
    /// when only its crop changes (the same batches drawn over a larger
    /// rectangle, as when a slot gains copper elsewhere): every non-wavefront
    /// slot plane with batches, drawn at its own crop and at `trials` crops
    /// grown by 0 to 48 pixels on each side, with `cropGrid` and `bands` as
    /// set; the overlap compared.  Pixels compared, differing at all, by
    /// more than 48, and the largest channel difference.
    public func cropStability(view: ViewTransform, trials: Int, seed: UInt64 = 1)
        -> (compared: Int, differing: Int, over48: Int, worst: Int) {
        render(view: view)
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        var rng = seed
        func next(_ n: Int) -> Int {
            rng = rng &* 6364136223846793005 &+ 1442695040888963407
            return Int((rng >> 33) % UInt64(n))
        }
        var compared = 0, differing = 0, over48 = 0, worst = 0
        for plane in planes where !plane.nearest && !plane.items.isEmpty && plane.isSlot {
            if plane.items.count == 1, plane.items[0].primitive == .fill, plane.items[0].path.isRect(nil) { continue }
            var b = plane.items[0].bbox
            for item in plane.items.dropFirst() { b = b.union(item.bbox) }
            let s = view.scale * view.backing
            let base = (Int((b.minX * s + view.tx * view.backing).rounded(.down)) - 2,
                        Int((b.minY * s + view.ty * view.backing).rounded(.down)) - 2,
                        Int((b.maxX * s + view.tx * view.backing).rounded(.up)) + 2,
                        Int((b.maxY * s + view.ty * view.backing).rounded(.up)) + 2)
            func draw(_ r: (Int, Int, Int, Int)) -> (rect: (x0: Int, y0: Int, x1: Int, y1: Int), px: [UInt8])? {
                let c = LayeredRenderer.snap(r, grid: cropGrid, width: w, height: h)
                guard c.x1 > c.x0, c.y1 > c.y0 else { return nil }
                let buffer = PixelBuffer(width: c.x1 - c.x0, height: c.y1 - c.y0)
                DisplayListRenderer.rasterize(plane.items[...], background: nil, view: view, originX: c.x0,
                                              originY: c.y0, into: buffer, bands: cropGrid > 0 ? 1 : bands)
                guard let image = buffer.takeImage(), let data = image.dataProvider?.data,
                      let p = CFDataGetBytePtr(data) else { return nil }
                let stride = image.bytesPerRow
                var px = [UInt8](repeating: 0, count: (c.x1 - c.x0) * (c.y1 - c.y0) * 4)
                for y in 0..<(c.y1 - c.y0) {
                    for x in 0..<((c.x1 - c.x0) * 4) { px[(y * (c.x1 - c.x0)) * 4 + x] = p[y * stride + x] }
                }
                return (c, px)
            }
            guard let a = draw(base) else { continue }
            for _ in 0..<trials {
                let grown = (base.0 - next(49), base.1 - next(49), base.2 + next(49), base.3 + next(49))
                guard let g = draw(grown) else { continue }
                let x0 = max(a.rect.x0, g.rect.x0), y0 = max(a.rect.y0, g.rect.y0)
                let x1 = min(a.rect.x1, g.rect.x1), y1 = min(a.rect.y1, g.rect.y1)
                guard x1 > x0, y1 > y0 else { continue }
                let aw = a.rect.x1 - a.rect.x0, gw = g.rect.x1 - g.rect.x0
                for y in y0..<y1 {
                    for x in x0..<x1 {
                        let i = ((y - a.rect.y0) * aw + (x - a.rect.x0)) * 4
                        let j = ((y - g.rect.y0) * gw + (x - g.rect.x0)) * 4
                        var d = 0
                        for k in 0..<4 { d = max(d, abs(Int(a.px[i + k]) - Int(g.px[j + k]))) }
                        compared += 1
                        if d > 0 { differing += 1 }
                        if d > 48 { over48 += 1 }
                        worst = max(worst, d)
                    }
                }
            }
        }
        return (compared, differing, over48, worst)
    }

    /// A plane that is one filled axis-aligned rectangle of one colour (the
    /// selection's dim rectangle, M5) becomes a 1 x 1 image stretched over
    /// the rectangle, its edges rounded to whole pixels, instead of a
    /// view-sized bitmap.  It can differ from a single-pass render only in
    /// the anti-aliased edge pixels (the dim rectangle's edges lie 2 mm
    /// outside everything drawn, where it changes nothing).  False if the
    /// item is not such a rectangle.
    private func solidRectangle(_ plane: Plane, view: ViewTransform, width w: Int, height h: Int) -> Bool {
        let item = plane.items[0]
        var r = CGRect.zero
        guard item.path.isRect(&r) else { return false }
        let s = view.scale * view.backing
        let x0 = max(0, min(w, Int((r.minX * s + view.tx * view.backing).rounded())))
        let y0 = max(0, min(h, Int((r.minY * s + view.ty * view.backing).rounded())))
        let x1 = max(0, min(w, Int((r.maxX * s + view.tx * view.backing).rounded())))
        let y1 = max(0, min(h, Int((r.maxY * s + view.ty * view.backing).rounded())))
        plane.version += 1
        guard x1 > x0, y1 > y0 else {
            plane.image = nil
            plane.frame = .zero
            plane.pixelRect = .zero
            return true
        }
        let buffer = PixelBuffer(width: 1, height: 1)
        if let ctx = buffer.context(rows: 0..<1) {
            ctx.clear(CGRect(x: 0, y: 0, width: 1, height: 1))
            ctx.setFillColor(item.color)
            ctx.fill(CGRect(x: 0, y: 0, width: 1, height: 1))
        }
        plane.image = buffer.takeImage()
        plane.renders += 1
        plane.pixelRect = CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0)
        plane.frame = CGRect(x: CGFloat(x0) / view.backing, y: CGFloat(y0) / view.backing,
                             width: CGFloat(x1 - x0) / view.backing, height: CGFloat(y1 - y0) / view.backing)
        return true
    }

    // MARK: - Whole images (rvrender, self-tests)

    /// The planes stacked into one opaque image of the view's pixel size.
    public func composite(view: ViewTransform) -> CGImage? {
        render(view: view)
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        let out = PixelBuffer(width: w, height: h)
        guard let ctx = out.context(rows: 0..<h) else { return nil }
        ctx.clear(CGRect(x: 0, y: 0, width: w, height: h))
        ctx.interpolationQuality = .none
        for plane in planes {
            guard let image = plane.image else { continue }
            let r = plane.pixelRect
            // Bitmap contexts are y up: row 0 of the image is the top.
            ctx.draw(image, in: CGRect(x: r.minX, y: CGFloat(h) - r.maxY, width: r.width, height: r.height))
        }
        return out.takeImage()
    }

    /// Every batch, board and overlay, merged by order and drawn in one
    /// pass over the background: what the planes must add up to.
    public func renderSinglePass(view: ViewTransform) -> CGImage? {
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        let a = board.drawList, b = overlay.drawList
        var merged: [DisplayListRenderer.Prepared] = []
        merged.reserveCapacity(a.count + b.count)
        var i = 0, j = 0
        while i < a.count || j < b.count {
            if j >= b.count || (i < a.count && a[i].order <= b[j].order) {
                merged.append(a[i])
                i += 1
            } else {
                merged.append(b[j])
                j += 1
            }
        }
        let out = PixelBuffer(width: w, height: h)
        // The wavefront images go between the batches, at their order.
        let waves = wavefronts.filter { $0.image != nil }.sorted { $0.order < $1.order }
        var start = 0
        for (k, wave) in waves.enumerated() {
            var end = start
            while end < merged.count && merged[end].order < wave.order { end += 1 }
            DisplayListRenderer.rasterize(merged[start..<end], background: k == 0 ? board.background : nil,
                                          clear: k == 0, view: view, originX: 0, originY: 0, into: out, bands: bands)
            if let image = wave.image, let ctx = out.context(rows: 0..<h) {
                ctx.interpolationQuality = .none
                let r = LayeredRenderer.waveRects(wave, view: view).pixels
                ctx.draw(image, in: CGRect(x: r.minX, y: CGFloat(h) - r.maxY, width: r.width, height: r.height))
            }
            start = end
        }
        DisplayListRenderer.rasterize(merged[start...], background: waves.isEmpty ? board.background : nil,
                                      clear: waves.isEmpty, view: view, originX: 0, originY: 0, into: out, bands: bands)
        return out.takeImage()
    }
}
