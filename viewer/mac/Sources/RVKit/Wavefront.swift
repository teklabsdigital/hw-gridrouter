// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Trevor Attema, Teklabs Digital
//
// Wavefront.swift: one wavefront image of the core (rv_wavefront_image) as
// a CGImage cropped to its bounds, with where it goes on the board.
//
// The core makes the pixels (premultiplied sRGB RGBA, one per grid cell)
// and says where they go; this file only copies the non-empty rectangle
// into an image.  Drawn scaled with nearest-neighbour sampling (one cell, one
// flat square) by LayeredRenderer.

import CoreGraphics
import Foundation
import RVCore

public struct WavefrontImage: @unchecked Sendable {
    /// Router layer 0 or 1.
    public let routerLayer: UInt32
    /// Board layer index; nil if the board drawn has no such layer.
    public let layer: UInt32?
    /// Draw order: the orderBegin of its wavefront slot.
    public let order: UInt32
    /// Changes whenever a pixel does.
    public let generation: UInt32
    /// The search event's search_id, nil when nothing is shown.
    public let search: UInt32?
    /// Cells drawn.
    public let cells: UInt32
    /// The cropped pixels (row 0 = smallest board y), nil when empty.
    public let image: CGImage?
    /// Where `image` goes, in board mm (minX, minY = the corner of its first pixel).
    public let boardRect: CGRect
    /// The whole grid, board mm.
    public let gridRect: CGRect
    /// The grid size in cells.
    public let width: Int
    public let height: Int

    init(copying w: rv_wavefront_image) {
        routerLayer = w.router_layer
        layer = w.layer == RV_NONE ? nil : w.layer
        order = w.order
        generation = w.generation
        search = w.search == RV_NONE ? nil : w.search
        cells = w.cells
        width = Int(w.width)
        height = Int(w.height)
        let r = w.rect
        gridRect = CGRect(x: min(r.0, r.2), y: min(r.1, r.3), width: abs(r.2 - r.0), height: abs(r.3 - r.1))
        let b = w.bounds
        let x0 = Int(max(0, b.0)), y0 = Int(max(0, b.1))
        let x1 = Int(min(Int32(w.width), b.2)), y1 = Int(min(Int32(w.height), b.3))
        guard let pixels = w.pixels, x1 > x0, y1 > y0, w.width > 0, w.height > 0 else {
            image = nil
            boardRect = .zero
            return
        }
        let cw = x1 - x0, ch = y1 - y0
        let rowBytes = cw * 4
        let data = CFDataCreateMutable(nil, rowBytes * ch)!
        CFDataSetLength(data, rowBytes * ch)
        let dst = CFDataGetMutableBytePtr(data)!
        for y in 0..<ch {
            (dst + y * rowBytes).update(from: pixels + (y0 + y) * Int(w.stride) + x0 * 4, count: rowBytes)
        }
        image = CGDataProvider(data: data).flatMap {
            CGImage(width: cw, height: ch, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: rowBytes,
                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                    bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                    provider: $0, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        }
        // Pixel (i, j) covers [r0 + i * (r2 - r0) / width, ...]: the crop's corners.
        let fx = (r.2 - r.0) / Double(w.width), fy = (r.3 - r.1) / Double(w.height)
        let ax = r.0 + Double(x0) * fx, bx = r.0 + Double(x1) * fx
        let ay = r.1 + Double(y0) * fy, by = r.1 + Double(y1) * fy
        boardRect = CGRect(x: min(ax, bx), y: min(ay, by), width: abs(bx - ax), height: abs(by - ay))
    }
}
