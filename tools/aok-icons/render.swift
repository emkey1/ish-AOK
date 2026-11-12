// Render each SVG in argv[1] to a 1024x1024 opaque PNG in argv[2] using WebKit.
import AppKit
import WebKit

let inDir = URL(fileURLWithPath: CommandLine.arguments[1])
let outDir = URL(fileURLWithPath: CommandLine.arguments[2])
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)
let files = try! FileManager.default.contentsOfDirectory(at: inDir, includingPropertiesForKeys: nil)
    .filter { $0.pathExtension == "svg" }.sorted { $0.path < $1.path }

class Renderer: NSObject, WKNavigationDelegate {
    var queue: [URL]
    let web: WKWebView
    init(_ q: [URL]) {
        queue = q
        let cfg = WKWebViewConfiguration()
        web = WKWebView(frame: NSRect(x: 0, y: 0, width: 1024, height: 1024), configuration: cfg)
        super.init()
        web.navigationDelegate = self
    }
    func next() {
        guard let f = queue.first else { exit(0) }
        let svg = try! String(contentsOf: f, encoding: .utf8)
        let html = "<html><head><style>html,body{margin:0;padding:0;background:#000}svg{display:block}</style></head><body>\(svg)</body></html>"
        web.loadHTMLString(html, baseURL: nil)
    }
    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            let cfg = WKSnapshotConfiguration()
            cfg.rect = NSRect(x: 0, y: 0, width: 1024, height: 1024)
            cfg.snapshotWidth = 1024
            webView.takeSnapshot(with: cfg) { img, err in
                let f = self.queue.removeFirst()
                guard let img = img, let cg = img.cgImage(forProposedRect: nil, context: nil, hints: nil) else {
                    print("FAIL", f.lastPathComponent, err as Any); exit(1)
                }
                // Redraw into an opaque 1024 RGB context (iOS icons must not carry alpha).
                let ctx = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8, bytesPerRow: 0,
                                    space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
                ctx.interpolationQuality = .high
                ctx.draw(cg, in: CGRect(x: 0, y: 0, width: 1024, height: 1024))
                let rep = NSBitmapImageRep(cgImage: ctx.makeImage()!)
                let png = rep.representation(using: .png, properties: [:])!
                let out = self.outDir.appendingPathComponent(f.deletingPathExtension().lastPathComponent + ".png")
                try! png.write(to: out)
                print("ok", out.lastPathComponent)
                self.next()
            }
        }
    }
    var outDir: URL { return CommandLine.arguments.count > 2 ? URL(fileURLWithPath: CommandLine.arguments[2]) : URL(fileURLWithPath: ".") }
}

let app = NSApplication.shared
let r = Renderer(files)
r.next()
app.run()
