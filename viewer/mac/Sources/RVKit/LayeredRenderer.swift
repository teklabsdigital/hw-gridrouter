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
// (renderSinglePass, which the self-tests compare against).  A later
// milestone's slot kinds (the wavefront) become more planes without changes
// here.

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

        fileprivate var items: [DisplayListRenderer.Prepared] = []
        fileprivate var dirty = true
        fileprivate var slotGeneration: UInt32?

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

    /// Slot planes cover only the bounds of their batches (the default) or
    /// the whole view.  Core Graphics anti-aliases a thin diagonal stroke a
    /// little differently at another bitmap offset, so a cropped plane can
    /// differ from a single-pass render by a few edge pixels; uncropped and
    /// with `bands` 1 the two are identical (the self-tests check both).
    public var cropsSlots = true
    /// Bands for parallel rasterising; 0 picks one per active core.
    public var bands = 0

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
                if k < slots.count { p.append(Plane(kind: .slot(slots[k].kind))) }
            }
            planes = p
            structure += 1
            boardChanged = true
            epochChanged = true
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
        var count = 0, pixels = 0
        for (i, plane) in planes.enumerated() where plane.dirty {
            pixels += renderPlane(plane, bottom: i == 0, view: view, bands: bands)
            count += 1
        }
        lastRenderSeconds = Double(DispatchTime.now().uptimeNanoseconds - start) / 1e9
        lastRenderedPlanes = count
        lastRenderedPixels = pixels
        return count
    }

    private func renderPlane(_ plane: Plane, bottom: Bool, view: ViewTransform, bands: Int) -> Int {
        plane.dirty = false
        let w = max(1, Int((view.width * view.backing).rounded()))
        let h = max(1, Int((view.height * view.backing).rounded()))
        var rect: (x0: Int, y0: Int, x1: Int, y1: Int)?
        if bottom {
            rect = (0, 0, w, h)
        } else if !plane.items.isEmpty {
            if plane.isSlot, cropsSlots {
                // Only the bounds of the slot's batches (board mm, which
                // include half the stroke widths), plus 2 pixels for
                // anti-aliasing.
                var b = plane.items[0].bbox
                for item in plane.items.dropFirst() { b = b.union(item.bbox) }
                let s = view.scale * view.backing
                let x0 = Int(((b.minX * s + view.tx * view.backing)).rounded(.down)) - 2
                let y0 = Int(((b.minY * s + view.ty * view.backing)).rounded(.down)) - 2
                let x1 = Int(((b.maxX * s + view.tx * view.backing)).rounded(.up)) + 2
                let y1 = Int(((b.maxY * s + view.ty * view.backing)).rounded(.up)) + 2
                let r = (max(0, x0), max(0, y0), min(w, x1), min(h, y1))
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
                                      originX: rect.x0, originY: rect.y0, into: buffer, bands: bands)
        plane.image = buffer.takeImage()
        plane.renders += 1
        plane.pixelRect = CGRect(x: rect.x0, y: rect.y0, width: pw, height: ph)
        plane.frame = CGRect(x: CGFloat(rect.x0) / view.backing, y: CGFloat(rect.y0) / view.backing,
                             width: CGFloat(pw) / view.backing, height: CGFloat(ph) / view.backing)
        return pw * ph
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
        DisplayListRenderer.rasterize(merged[...], background: board.background, view: view, originX: 0, originY: 0,
                                      into: out, bands: bands)
        return out.takeImage()
    }
}
