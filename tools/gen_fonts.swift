// Glyph bitmap generator (macOS only). Scans the UTF-8 translation data in
// main/i18n_data.inc for the characters each font must cover and emits
// main/fonts.c: antialiased 4-bit-alpha bitmaps for the base Menlo font
// (ASCII + a sorted supplementary codepoint table) and the four
// per-language CJK fonts. Translations themselves are plain C in
// i18n_data.inc and never require this tool; rerun it only when a string
// gains a character that has no glyph yet (tools/check_i18n.py reports
// coverage portably).
import AppKit
import CoreText
import Foundation

struct FontConfig {
    let scope: String       // symbol suffix in i18n_data.inc, "" = base
    let fontObject: String  // emitted display_glyph_font_t, "" = base
    let glyphArray: String
    let fontName: String
    let glyphWidth: Int
    let fontSize16: CGFloat
    let fontSize32: CGFloat
    // Alpha shaping exponent for the 4-bit antialiased coverage. Blending
    // happens in gamma-encoded RGB565 on the device, which makes linear
    // coverage read thin; values below 1.0 fatten the edge ramp.
    var aaGamma: CGFloat = 0.7
}

// The base Menlo font carries ASCII plus every non-ASCII character used by
// the languages without their own glyph font (Latin extensions, Cyrillic,
// Greek, Vietnamese). CJK languages keep per-language fonts so shared Han
// codepoints render in the right regional face. Longest scope first so
// "zh_hant" wins over "zh".
let baseConfig = FontConfig(scope: "", fontObject: "", glyphArray: "",
                            fontName: "Menlo", glyphWidth: 8, fontSize16: 13, fontSize32: 26)
let cjkConfigs = [
    FontConfig(scope: "zh_hant", fontObject: "font_zh_hant", glyphArray: "zh_hant_glyphs",
               fontName: "PingFang TC", glyphWidth: 16, fontSize16: 13, fontSize32: 28),
    FontConfig(scope: "ja", fontObject: "font_ja", glyphArray: "ja_glyphs",
               fontName: "Hiragino Sans GB", glyphWidth: 16, fontSize16: 14, fontSize32: 29),
    FontConfig(scope: "zh", fontObject: "font_zh", glyphArray: "zh_glyphs",
               fontName: "Hiragino Sans GB", glyphWidth: 16, fontSize16: 14, fontSize32: 29),
    FontConfig(scope: "ko", fontObject: "font_ko", glyphArray: "ko_glyphs",
               fontName: "Apple SD Gothic Neo", glyphWidth: 16, fontSize16: 14, fontSize32: 29),
]

func fontHasGlyphs(_ font: NSFont, _ s: String) -> Bool {
    let utf16 = Array(s.utf16)
    var glyphs = [CGGlyph](repeating: 0, count: utf16.count)
    return CTFontGetGlyphsForCharacters(font as CTFont, utf16, &glyphs, utf16.count)
}

func isAscii(_ ch: Character) -> Bool {
    guard ch.unicodeScalars.count == 1, let scalar = ch.unicodeScalars.first else { return false }
    return scalar.value >= 0x20 && scalar.value <= 0x7e
}

func codepoint(_ ch: Character) -> UInt32 {
    let scalars = Array(String(ch).precomposedStringWithCanonicalMapping.unicodeScalars)
    guard scalars.count == 1 else {
        fatalError("character \(ch) has no single NFC codepoint")
    }
    guard scalars[0].value <= 0xFFFF else {
        fatalError("codepoint above the BMP: \(ch)")
    }
    return scalars[0].value
}

// Collect the non-ASCII characters of every string literal in
// i18n_data.inc, bucketed by which font must render them. A declaration
// whose symbol ends in _<scope> (lang_ja, weekdays_ja, date_year_ja, ...)
// belongs to that CJK font; everything else is base-font text.
func collectChars(_ source: String) -> [String: Set<Character>] {
    var buckets: [String: Set<Character>] = ["": []]
    for cfg in cjkConfigs { buckets[cfg.scope] = [] }

    var scope = ""
    var inBlock = false
    for line in source.split(separator: "\n", omittingEmptySubsequences: false) {
        if !inBlock {
            scope = ""
            // declaration line: pick scope from the symbol name
            if let m = line.range(of: #"(lang|weekdays|months|lang_name|date_year|date_day)_[a-z_]+"#,
                                  options: .regularExpression) {
                let sym = String(line[m])
                for cfg in cjkConfigs where sym.hasSuffix("_" + cfg.scope) {
                    scope = cfg.scope
                    break
                }
            }
            if line.contains("= {") { inBlock = true }
        }
        // collect string literal contents on this line
        var rest = Substring(line)
        // line comments never contain translation text
        if let c = rest.range(of: "//") { rest = rest[..<c.lowerBound] }
        while let q1 = rest.firstIndex(of: "\"") {
            var i = rest.index(after: q1)
            var lit = ""
            while i < rest.endIndex, rest[i] != "\"" {
                if rest[i] == "\\" { i = rest.index(after: i) }  // skip escape target
                else { lit.append(rest[i]) }
                i = rest.index(after: i)
            }
            for ch in lit where !isAscii(ch) {
                buckets[scope]!.insert(ch)
            }
            if i >= rest.endIndex { break }
            rest = rest[rest.index(after: i)...]
        }
        if inBlock && line.hasPrefix("};") { inBlock = false; scope = "" }
    }
    return buckets
}

func render(_ ch: String, cfg: FontConfig, pixels: Int, outputWidth: Int? = nil) -> [UInt8] {
    let outputWidth = outputWidth ?? pixels
    let renderWidth = pixels == 16 ? cfg.glyphWidth : cfg.glyphWidth * 2
    guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: renderWidth,
        pixelsHigh: pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: renderWidth * 4,
        bitsPerPixel: 32
    ) else {
        fatalError("bitmap allocation failed")
    }

    guard let ctx = NSGraphicsContext(bitmapImageRep: rep) else {
        fatalError("graphics context allocation failed")
    }

    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = ctx
    NSColor.black.setFill()
    NSRect(x: 0, y: 0, width: renderWidth, height: pixels).fill()

    let fontSize = pixels == 16 ? cfg.fontSize16 : cfg.fontSize32
    guard let font = NSFont(name: cfg.fontName, size: fontSize) else {
        // Falling back to another font would silently regenerate every
        // table with different metrics.
        fatalError("font \(cfg.fontName) is not installed")
    }
    var text = ch
    if !fontHasGlyphs(font, text) {
        // Menlo lacks some precomposed Vietnamese vowels but has every
        // combining mark; draw the decomposed sequence so CoreText
        // composes base + marks in the same face. Anything else missing
        // fails loudly rather than silently borrowing a fallback font.
        let nfd = text.decomposedStringWithCanonicalMapping
        guard fontHasGlyphs(font, nfd) else {
            fatalError("\(cfg.fontName) has no glyph for \(text)")
        }
        text = nfd
    }
    let s = text as NSString
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor.white,
    ]
    let size = s.size(withAttributes: attrs)
    let y = floor((CGFloat(pixels) - size.height) / 2.0)
    if size.width > CGFloat(renderWidth) {
        // Condense a glyph wider than its cell instead of clipping its
        // outer strokes.
        let t = NSAffineTransform()
        t.scaleX(by: CGFloat(renderWidth) / size.width, yBy: 1.0)
        t.concat()
        s.draw(at: NSPoint(x: 0, y: y), withAttributes: attrs)
    } else {
        let x = floor((CGFloat(renderWidth) - size.width) / 2.0)
        s.draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
    }
    NSGraphicsContext.restoreGraphicsState()

    guard let data = rep.bitmapData else {
        fatalError("bitmap data unavailable")
    }

    // 4-bit alpha, two pixels per byte, high nibble = left pixel. The cell
    // is filled opaque black before the white glyph is drawn, so coverage
    // is the pixel's luminance, not its alpha channel.
    let bytesPerGlyphRow = outputWidth / 2
    var out = [UInt8](repeating: 0, count: pixels * bytesPerGlyphRow)
    for row in 0..<pixels {
        for col in 0..<outputWidth {
            var level = 0
            if col < renderWidth {
                let p = row * rep.bytesPerRow + col * 4
                let lum = (CGFloat(data[p]) + CGFloat(data[p + 1]) + CGFloat(data[p + 2]))
                    / (3.0 * 255.0)
                level = min(15, Int((pow(lum, cfg.aaGamma) * 15.0).rounded()))
            }
            let idx = row * bytesPerGlyphRow + col / 2
            if col % 2 == 0 {
                out[idx] = UInt8(level << 4)
            } else {
                out[idx] |= UInt8(level)
            }
        }
    }
    return out
}

func hexRow(_ bytes: [UInt8]) -> String {
    return bytes.map { String(format: "0x%02X", $0) }.joined(separator: ",")
}

func emitCodepointTable(_ name: String, _ chars: [Character]) -> String {
    var out = "static const uint16_t \(name)[] = {\n"
    var line = "   "
    for ch in chars {
        line += String(format: " 0x%04X,", codepoint(ch))
        if line.count > 90 {
            out += line + "\n"
            line = "   "
        }
    }
    if line != "   " { out += line + "\n" }
    out += "};\n"
    return out
}

func emitGlyphArrays(_ prefix: String, _ chars: [Character], cfg: FontConfig,
                     storage: String, bytes16: String, bytes32: String,
                     out16: Int, out32: Int) -> String {
    var out = ""
    out += "\(storage)const uint8_t \(prefix)[][\(bytes16)] = {\n"
    for ch in chars {
        let hex = hexRow(render(String(ch), cfg: cfg, pixels: 16, outputWidth: out16))
        out += "    {\(hex)}, // U+\(String(format: "%04X", codepoint(ch)))\n"
    }
    out += "};\n\n"
    out += "\(storage)const uint8_t \(prefix)_2x[][\(bytes32)] = {\n"
    for ch in chars {
        let hex = hexRow(render(String(ch), cfg: cfg, pixels: 32, outputWidth: out32))
        out += "    {\(hex)}, // U+\(String(format: "%04X", codepoint(ch)))\n"
    }
    out += "};\n\n"
    return out
}

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let dataSource = try String(contentsOf: root.appendingPathComponent("main/i18n_data.inc"),
                            encoding: .utf8)
let buckets = collectChars(dataSource)

var out = """
// Generated by tools/gen_fonts.swift; do not edit by hand.
#include "fonts.h"

"""

for cfg in cjkConfigs.sorted(by: { $0.scope < $1.scope }) {
    let chars = buckets[cfg.scope]!.sorted { codepoint($0) < codepoint($1) }
    print("\(cfg.scope): \(chars.count) glyphs")
    out += "// \(cfg.scope): \(chars.count) glyphs (\(cfg.fontName))\n"
    out += emitCodepointTable("\(cfg.glyphArray)_cp", chars)
    out += emitGlyphArrays(cfg.glyphArray, chars, cfg: cfg, storage: "static ",
                           bytes16: "DISPLAY_GLYPH_BYTES", bytes32: "DISPLAY_GLYPH_2X_BYTES",
                           out16: 16, out32: 32)
    out += """
    const display_glyph_font_t \(cfg.fontObject) = {
        .codepoints = \(cfg.glyphArray)_cp,
        .glyphs = \(cfg.glyphArray),
        .glyphs_2x = \(cfg.glyphArray)_2x,
        .count = sizeof(\(cfg.glyphArray)) / sizeof(\(cfg.glyphArray)[0]),
        .glyph_width = \(cfg.glyphWidth),
    };

    """
}

var baseChars = buckets[""]!
baseChars.insert("\u{00B0}")  // degree sign, used by the rotation label
let baseSorted = baseChars.sorted { codepoint($0) < codepoint($1) }
print("base: \(baseSorted.count) supplementary glyphs")

out += "// base: ASCII plus \(baseSorted.count) supplementary glyphs (\(baseConfig.fontName))\n"
out += "const uint8_t font_base_ascii[][FONT_BASE_GLYPH_BYTES] = {\n"
for code in 0x20...0x7E {
    let ch = String(UnicodeScalar(code)!)
    let label: String
    switch code {
    case 0x20: label = "space"
    case 0x5C: label = "backslash"  // a bare one would splice the next line into this comment
    default: label = ch
    }
    let hex = hexRow(render(ch, cfg: baseConfig, pixels: 16, outputWidth: 8))
    out += "    {\(hex)}, // \(label)\n"
}
out += "};\n\n"
out += "const uint8_t font_base_ascii_2x[][FONT_BASE_GLYPH_2X_BYTES] = {\n"
for code in 0x20...0x7E {
    let ch = String(UnicodeScalar(code)!)
    let hex = hexRow(render(ch, cfg: baseConfig, pixels: 32, outputWidth: 16))
    out += "    {\(hex)},\n"
}
out += "};\n\n"
out += emitCodepointTable("font_base_ext_cp_table", baseSorted)
    .replacingOccurrences(of: "static const uint16_t font_base_ext_cp_table",
                          with: "const uint16_t font_base_ext_cp")
out += "\n"
out += emitGlyphArrays("font_base_ext", baseSorted, cfg: baseConfig, storage: "",
                       bytes16: "FONT_BASE_GLYPH_BYTES", bytes32: "FONT_BASE_GLYPH_2X_BYTES",
                       out16: 8, out32: 16)
out += "const uint16_t font_base_ext_count = sizeof(font_base_ext) / sizeof(font_base_ext[0]);\n"

try out.write(to: root.appendingPathComponent("main/fonts.c"), atomically: true, encoding: .utf8)
