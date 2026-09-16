import AppKit
import Foundation

// Reproducible geometric artwork; run on macOS to refresh the checked-in icons.
let destination = URL(fileURLWithPath: CommandLine.arguments[1])
let iconset = destination.appendingPathComponent("ThunderLink.iconset")
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
func png(_ size: Int) -> Data {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size,
        bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
        colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    let scale = CGFloat(size) / 1024
    let transform = NSAffineTransform(); transform.scale(by: scale); transform.concat()
    let background = NSBezierPath(roundedRect: NSRect(x: 80, y: 80, width: 864, height: 864), xRadius: 200, yRadius: 200)
    NSGradient(starting: NSColor(calibratedRed: 0.12, green: 0.20, blue: 0.34, alpha: 1),
               ending: NSColor(calibratedRed: 0.04, green: 0.06, blue: 0.11, alpha: 1))!.draw(in: background, angle: -90)
    let screen = NSBezierPath(roundedRect: NSRect(x: 228, y: 310, width: 568, height: 384), xRadius: 42, yRadius: 42)
    NSColor(calibratedRed: 0.34, green: 0.57, blue: 1, alpha: 1).setStroke()
    screen.lineWidth = 32; screen.stroke()
    let bolt = NSBezierPath()
    bolt.move(to: NSPoint(x: 574, y: 790)); bolt.line(to: NSPoint(x: 392, y: 496))
    bolt.line(to: NSPoint(x: 507, y: 496)); bolt.line(to: NSPoint(x: 450, y: 240))
    bolt.line(to: NSPoint(x: 661, y: 557)); bolt.line(to: NSPoint(x: 539, y: 557)); bolt.close()
    NSColor(calibratedRed: 0.81, green: 0.89, blue: 1, alpha: 1).setFill(); bolt.fill()
    NSGraphicsContext.restoreGraphicsState()
    return bitmap.representation(using: .png, properties: [:])!
}
for size in [16, 32, 128, 256, 512] {
    try png(size).write(to: iconset.appendingPathComponent("icon_\(size)x\(size).png"))
    try png(size * 2).write(to: iconset.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
let image = png(256)
var ico = Data([0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 32, 0])
for value in [UInt32(image.count), UInt32(22)] {
    var little = value.littleEndian
    withUnsafeBytes(of: &little) { ico.append(contentsOf: $0) }
}
ico.append(image)
try ico.write(to: destination.appendingPathComponent("ThunderLink.ico"))
