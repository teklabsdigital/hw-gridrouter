// LayerSnapshot.swift: the board view's layer tree rendered by Core
// Animation itself, offscreen, for the self-test.
//
// A model of the tree (drawing each layer's contents where we think Core
// Animation puts it) shares our assumptions, so it cannot catch a wrong one:
// the M3 model assumed the hosted root layer is y-up, while AppKit makes it
// geometry-flipped (y down) for a flipped view, and the router copper moved
// against the board while panning.  Here a copy of the tree (the root's
// flags and size, each plane's frame, contents, scale and filters) is
// rendered by CARenderer into a Metal texture, so Core Animation decides
// where every plane goes and which way up its contents are, as on screen.
//
// The texture's row order is calibrated once against an unflipped (y-up,
// Core Animation's documented default) tree with a layer in its lower half.

import AppKit
import Metal
import QuartzCore

@MainActor
enum LayerSnapshot {
    private static let device = MTLCreateSystemDefaultDevice()
    private static let queue = device?.makeCommandQueue()
    /// True when the texture's first row in memory is the bottom of the picture.
    private static var bottomFirst: Bool?

    private static let noActions: [String: any CAAction] = [
        "contents": NSNull(), "bounds": NSNull(), "position": NSNull(), "frame": NSNull(), "hidden": NSNull(),
        "sublayers": NSNull(), "backgroundColor": NSNull(), "contentsScale": NSNull(), "transform": NSNull(),
    ]

    /// Why snapshots are unavailable, nil when they work.
    static var unavailableReason: String? {
        if device == nil || queue == nil { return "no Metal device" }
        if calibrate() == nil { return "Core Animation's offscreen renderer gave no picture" }
        return nil
    }

    /// `root`'s tree as Core Animation draws it: `size` points at `scale`
    /// pixels per point, rows top first.  nil if it cannot be rendered.
    static func render(_ root: CALayer, size: CGSize, scale: CGFloat) -> CGImage? {
        guard let bottomFirst = calibrate() else { return nil }
        let w = Int((size.width * scale).rounded()), h = Int((size.height * scale).rounded())
        guard w > 0, h > 0 else { return nil }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let mirror = CALayer()
        mirror.actions = noActions
        mirror.anchorPoint = .zero
        mirror.bounds = CGRect(origin: .zero, size: size)
        mirror.position = .zero
        mirror.transform = CATransform3DMakeScale(scale, scale, 1)
        mirror.isGeometryFlipped = root.isGeometryFlipped
        mirror.backgroundColor = root.backgroundColor
        for l in root.sublayers ?? [] {
            let c = CALayer()
            c.actions = noActions
            c.anchorPoint = l.anchorPoint
            c.bounds = l.bounds
            c.position = l.position
            c.transform = l.transform
            c.isGeometryFlipped = l.isGeometryFlipped
            c.contents = l.contents
            c.contentsScale = l.contentsScale
            c.contentsGravity = l.contentsGravity
            c.contentsRect = l.contentsRect
            c.magnificationFilter = l.magnificationFilter
            c.minificationFilter = l.minificationFilter
            c.isHidden = l.isHidden
            c.opacity = l.opacity
            c.backgroundColor = l.backgroundColor
            mirror.addSublayer(c)
        }
        let container = CALayer()
        container.actions = noActions
        container.frame = CGRect(x: 0, y: 0, width: w, height: h)
        container.addSublayer(mirror)
        CATransaction.commit()
        CATransaction.flush()
        guard var bytes = renderRaw(container, width: w, height: h) else { return nil }
        if bottomFirst {
            let row = 4 * w
            for y in 0..<(h / 2) {
                let a = y * row, b = (h - 1 - y) * row
                for x in 0..<row { bytes.swapAt(a + x, b + x) }
            }
        }
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: 4 * w,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!,
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }

    /// The texture's row order: an unflipped tree, red in its lower half.
    private static func calibrate() -> Bool? {
        if let bottomFirst { return bottomFirst }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        let container = CALayer()
        container.actions = noActions
        container.frame = CGRect(x: 0, y: 0, width: 8, height: 8)
        container.backgroundColor = CGColor(srgbRed: 0, green: 0, blue: 1, alpha: 1)
        let lower = CALayer()
        lower.actions = noActions
        lower.frame = CGRect(x: 0, y: 0, width: 8, height: 4)
        lower.backgroundColor = CGColor(srgbRed: 1, green: 0, blue: 0, alpha: 1)
        container.addSublayer(lower)
        CATransaction.commit()
        CATransaction.flush()
        guard let bytes = renderRaw(container, width: 8, height: 8) else { return nil }
        let firstRed = bytes[0] > 200 && bytes[2] < 50
        let lastRed = bytes[4 * 8 * 7] > 200 && bytes[4 * 8 * 7 + 2] < 50
        guard firstRed != lastRed else { return nil }
        bottomFirst = firstRed
        return firstRed
    }

    /// RGBA bytes of the texture rows, in memory order.
    private static func renderRaw(_ container: CALayer, width: Int, height: Int) -> [UInt8]? {
        guard let device, let queue else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height,
                                                            mipmapped: false)
        desc.usage = [.renderTarget, .shaderRead, .shaderWrite]
        desc.storageMode = .managed
        guard let texture = device.makeTexture(descriptor: desc) else { return nil }
        let options: [AnyHashable: Any] = [
            kCARendererMetalCommandQueue as String: queue,
            kCARendererColorSpace as String: CGColorSpace(name: CGColorSpace.sRGB)!,
        ]
        let renderer = CARenderer(mtlTexture: texture, options: options)
        renderer.layer = container
        // The tree reaches the renderer's context only when the transaction
        // that attached it is committed (inside the run loop's implicit one).
        CATransaction.flush()
        renderer.bounds = CGRect(x: 0, y: 0, width: width, height: height)
        renderer.beginFrame(atTime: CACurrentMediaTime(), timeStamp: nil)
        renderer.addUpdate(renderer.bounds)
        renderer.render()
        renderer.endFrame()
        // The renderer's work is on `queue`; this buffer runs after it.
        guard let buffer = queue.makeCommandBuffer(), let blit = buffer.makeBlitCommandEncoder() else { return nil }
        blit.synchronize(resource: texture)
        blit.endEncoding()
        buffer.commit()
        buffer.waitUntilCompleted()
        var bytes = [UInt8](repeating: 0, count: width * height * 4)
        bytes.withUnsafeMutableBytes { raw in
            texture.getBytes(raw.baseAddress!, bytesPerRow: width * 4,
                             from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        }
        for i in stride(from: 0, to: bytes.count, by: 4) { bytes.swapAt(i, i + 2) } // BGRA to RGBA
        return bytes
    }
}
