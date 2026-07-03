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
setvbuf(stdout, nil, _IONBF, 0)

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
    // Not CJK, but the same per-language mechanism: scripts Menlo does not
    // cover, in 10px cells (wide letters condense a touch).
    FontConfig(scope: "ka", fontObject: "font_ka", glyphArray: "ka_glyphs",
               fontName: "Helvetica", glyphWidth: 10, fontSize16: 13, fontSize32: 26),
    FontConfig(scope: "hy", fontObject: "font_hy", glyphArray: "hy_glyphs",
               fontName: "Noto Sans Armenian", glyphWidth: 10, fontSize16: 12, fontSize32: 24),
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
    var inShaped = false
    var inBlock = false
    for line in source.split(separator: "\n", omittingEmptySubsequences: false) {
        // shaped-script sources are consumed by the shaping pipeline, not
        // the plain character collector
        if line.hasPrefix("// >>> shaped-source") { inShaped = true }
        if line.hasPrefix("// <<< shaped-source") { inShaped = false; continue }
        if inShaped { continue }
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

// PackBits (Apple RLE): control < 128 copies control+1 literals; control
// > 128 repeats the next byte 257-control times.
func packBits(_ data: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    var i = 0
    while i < data.count {
        var j = i
        while j + 1 < data.count, data[j + 1] == data[j], j - i < 127 { j += 1 }
        let run = j - i + 1
        if run >= 3 {
            out.append(UInt8(257 - min(run, 128)))
            out.append(data[i])
            i += min(run, 128)
            continue
        }
        var k = i
        var lits = 0
        while k < data.count, lits < 128 {
            if k + 2 < data.count, data[k] == data[k + 1], data[k + 1] == data[k + 2] { break }
            k += 1
            lits += 1
        }
        out.append(UInt8(lits - 1))
        out.append(contentsOf: data[i..<(i + lits)])
        i += lits
    }
    return out
}

// Blob + count+1 offsets; a nil tile gets an empty span (pixel-doubled at
// runtime).
func emitCompressed2x(_ name: String, _ tiles: [[UInt8]?], storage: String) -> String {
    var blob: [UInt8] = []
    var offs: [Int] = [0]
    for t in tiles {
        if let t = t { blob += packBits(t) }
        offs.append(blob.count)
    }
    guard blob.count <= 0xFFFF else { fatalError("\(name): 2x blob exceeds u16 offsets") }
    var out = "\(storage)const uint8_t \(name)_rle[] = {\n"
    var line = "   "
    for b in blob {
        line += String(format: " 0x%02X,", b)
        if line.count > 100 { out += line + "\n"; line = "   " }
    }
    if line != "   " { out += line + "\n" }
    out += "};\n"
    out += "\(storage)const uint16_t \(name)_off[] = {\n"
    line = "   "
    for o in offs {
        line += " \(o),"
        if line.count > 100 { out += line + "\n"; line = "   " }
    }
    if line != "   " { out += line + "\n" }
    out += "};\n"
    return out
}

func emitGlyphArrays(_ prefix: String, _ chars: [Character], cfg: FontConfig,
                     storage: String, bytes16: String,
                     out16: Int, out32: Int) -> String {
    var out = ""
    out += "\(storage)const uint8_t \(prefix)[][\(bytes16)] = {\n"
    for ch in chars {
        let hex = hexRow(render(String(ch), cfg: cfg, pixels: 16, outputWidth: out16))
        out += "    {\(hex)}, // U+\(String(format: "%04X", codepoint(ch)))\n"
    }
    out += "};\n\n"
    let tiles: [[UInt8]?] = chars.map { render(String($0), cfg: cfg, pixels: 32, outputWidth: out32) }
    out += emitCompressed2x("\(prefix)_2x", tiles, storage: storage)
    out += "\n"
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
                           bytes16: "DISPLAY_GLYPH_BYTES",
                           out16: 16, out32: 32)
    out += """
    const display_glyph_font_t \(cfg.fontObject) = {
        .codepoints = \(cfg.glyphArray)_cp,
        .glyphs = \(cfg.glyphArray),
        .glyphs_2x_rle = \(cfg.glyphArray)_2x_rle,
        .glyphs_2x_off = \(cfg.glyphArray)_2x_off,
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
let baseAscii2x: [[UInt8]?] = (0x20...0x7E).map {
    render(String(UnicodeScalar($0)!), cfg: baseConfig, pixels: 32, outputWidth: 16)
}
out += emitCompressed2x("font_base_ascii_2x", baseAscii2x, storage: "")
out += "\n"
out += emitCodepointTable("font_base_ext_cp_table", baseSorted)
    .replacingOccurrences(of: "static const uint16_t font_base_ext_cp_table",
                          with: "const uint16_t font_base_ext_cp")
out += "\n"
out += emitGlyphArrays("font_base_ext", baseSorted, cfg: baseConfig, storage: "",
                       bytes16: "FONT_BASE_GLYPH_BYTES",
                       out16: 8, out32: 16)
out += "const uint16_t font_base_ext_count = sizeof(font_base_ext) / sizeof(font_base_ext[0]);\n"

try out.write(to: root.appendingPathComponent("main/fonts.c"), atomically: true, encoding: .utf8)

// ---------------------------------------------------------------------------
// Pre-shaped scripts: Arabic, Persian, Hebrew, Hindi, Bengali, Thai.
// CoreText shapes each string at generation time (joining, ligatures,
// conjuncts, mark stacking, bidi); the visual result is grouped into
// cluster glyphs (a base plus any marks that overlap it), deduplicated,
// and re-encoded as private-use codepoints (U+E000 + index) in visual
// order. The device renders these as plain left-to-right glyph runs with
// per-glyph widths - no runtime shaping or bidi. Format specifiers pass
// through as literal ASCII segments, with segment order reversed for RTL
// so snprintf-substituted values land in the visually correct place.

import CryptoKit

struct ShapedConfig {
    let scope: String
    let fontObject: String
    let glyphArray: String
    let fontName: String
    let fontSize: CGFloat
    let rtl: Bool
    var aaGamma: CGFloat = 0.7
}

let shapedConfigs = [
    ShapedConfig(scope: "ar", fontObject: "font_ar", glyphArray: "ar_glyphs",
                 fontName: "Geeza Pro", fontSize: 13, rtl: true),
    ShapedConfig(scope: "fa", fontObject: "font_fa", glyphArray: "fa_glyphs",
                 fontName: "Geeza Pro", fontSize: 13, rtl: true),
    ShapedConfig(scope: "he", fontObject: "font_he", glyphArray: "he_glyphs",
                 fontName: "Arial Hebrew", fontSize: 14, rtl: true),
    ShapedConfig(scope: "hi", fontObject: "font_hi", glyphArray: "hi_glyphs",
                 fontName: "Kohinoor Devanagari", fontSize: 12, rtl: false),
    ShapedConfig(scope: "bn", fontObject: "font_bn", glyphArray: "bn_glyphs",
                 fontName: "Kohinoor Bangla", fontSize: 12, rtl: false),
    ShapedConfig(scope: "th", fontObject: "font_th", glyphArray: "th_glyphs",
                 fontName: "Thonburi", fontSize: 11, rtl: false),
]

func cUnescape(_ lit: String) -> String {
    var out = ""
    var esc = false
    for ch in lit {
        if esc { out.append(ch); esc = false }
        else if ch == "\\" { esc = true }
        else { out.append(ch) }
    }
    return out
}

struct ShapedSource {
    let raw: String                     // marker-delimited section text (hashed)
    let strings: [(String, String)]     // (STR_id, text)
    let weekdays: [String]
    let months: [String]
    let name: String
}

func parseShapedSource(_ source: String, scope: String) -> ShapedSource {
    guard let a = source.range(of: "// >>> shaped-source \(scope)\n"),
          let b = source.range(of: "// <<< shaped-source \(scope)") else {
        fatalError("shaped-source markers for \(scope) not found in i18n_data.inc")
    }
    let section = String(source[a.upperBound..<b.lowerBound])

    func literals(_ block: String) -> [String] {
        var out: [String] = []
        var rest = Substring(block)
        while let q1 = rest.firstIndex(of: "\"") {
            var i = rest.index(after: q1)
            var lit = ""
            while i < rest.endIndex, rest[i] != "\"" {
                if rest[i] == "\\" {
                    lit.append(rest[i]); i = rest.index(after: i)
                    if i < rest.endIndex { lit.append(rest[i]); i = rest.index(after: i) }
                } else {
                    lit.append(rest[i]); i = rest.index(after: i)
                }
            }
            out.append(cUnescape(lit))
            if i >= rest.endIndex { break }
            rest = rest[rest.index(after: i)...]
        }
        return out
    }
    func block(_ name: String) -> String {
        guard let s = section.range(of: name),
              let e = section.range(of: "\n};", range: s.upperBound..<section.endIndex) else {
            fatalError("\(scope): table \(name) not found")
        }
        return String(section[s.upperBound..<e.lowerBound])
    }

    var strings: [(String, String)] = []
    let langBlock = block("lang_\(scope)[STR_COUNT] = {")
    for line in langBlock.split(separator: "\n") {
        guard let idStart = line.range(of: "[STR_"),
              let idEnd = line.range(of: "]", range: idStart.upperBound..<line.endIndex) else { continue }
        let id = "STR_" + line[idStart.upperBound..<idEnd.lowerBound]
        let lits = literals(String(line))
        guard lits.count == 1 else { fatalError("\(scope)/\(id): expected one literal") }
        strings.append((id, lits[0]))
    }
    let wk = literals(block("weekdays_\(scope)[7] = {"))
    let mo = literals(block("months_\(scope)[12] = {"))
    guard wk.count == 7, mo.count == 12 else { fatalError("\(scope): weekday/month count") }
    guard let n = section.range(of: "lang_name_\(scope)[] = ") else { fatalError("\(scope): name") }
    let nameLine = section[n.upperBound...].prefix(while: { $0 != "\n" })
    let name = literals(String(nameLine)).first ?? ""
    return ShapedSource(raw: section, strings: strings, weekdays: wk, months: mo, name: name)
}

// Split a template into text segments and verbatim format-specifier
// segments. Text segments containing non-ASCII get shaped; pure-ASCII
// ones stay ASCII.
enum Seg { case text(String), spec(String) }
func segment(_ s: String) -> [Seg] {
    var segs: [Seg] = []
    var text = ""
    let chars = Array(s)
    var i = 0
    let specifiers = Set("diuoxXfFeEgGaAcspn%")
    let modifiers = Set("0123456789.*+-# 'lhLqjzt")
    while i < chars.count {
        if chars[i] == "%" {
            var j = i + 1
            while j < chars.count, modifiers.contains(chars[j]) { j += 1 }
            if j < chars.count, specifiers.contains(chars[j]) {
                if !text.isEmpty { segs.append(.text(text)); text = "" }
                segs.append(.spec(String(chars[i...j])))
                i = j + 1
                continue
            }
        }
        text.append(chars[i])
        i += 1
    }
    if !text.isEmpty { segs.append(.text(text)) }
    return segs
}

struct ShapedFontBuilder {
    let cfg: ShapedConfig
    let font: NSFont
    var baselineY: CGFloat = 0
    var glyphs: [[UInt8]] = []      // 1x bitmaps, 16px-wide rows
    var glyphs2x: [[UInt8]] = []    // 2x bitmaps, 32px-wide rows
    var widths: [Int] = []
    var keyToIndex: [String: Int] = [:]

    init(cfg: ShapedConfig) {
        self.cfg = cfg
        guard let f = NSFont(name: cfg.fontName, size: cfg.fontSize) else {
            fatalError("font \(cfg.fontName) is not installed")
        }
        self.font = f
    }

    func makeLine(_ text: String) -> CTLine {
        let para = NSMutableParagraphStyle()
        para.baseWritingDirection = cfg.rtl ? .rightToLeft : .leftToRight
        let attr = NSAttributedString(string: text, attributes: [
            .font: font, .paragraphStyle: para,
        ])
        return CTLineCreateWithAttributedString(attr)
    }

    // One glyph as laid out by CoreText, in visual (line) coordinates.
    struct PlacedGlyph {
        let glyph: CGGlyph
        let font: CTFont
        let x: CGFloat
        let y: CGFloat
        let advance: CGFloat
    }

    func placedGlyphs(_ line: CTLine) -> [PlacedGlyph] {
        var out: [PlacedGlyph] = []
        let runs = CTLineGetGlyphRuns(line) as! [CTRun]
        for run in runs {
            let count = CTRunGetGlyphCount(run)
            if count == 0 { continue }
            var glyphs = [CGGlyph](repeating: 0, count: count)
            var positions = [CGPoint](repeating: .zero, count: count)
            var advances = [CGSize](repeating: .zero, count: count)
            CTRunGetGlyphs(run, CFRange(location: 0, length: 0), &glyphs)
            CTRunGetPositions(run, CFRange(location: 0, length: 0), &positions)
            CTRunGetAdvances(run, CFRange(location: 0, length: 0), &advances)
            let attrs = CTRunGetAttributes(run)
            let fontKey = unsafeBitCast(kCTFontAttributeName, to: UnsafeRawPointer.self)
            let runFont = unsafeBitCast(CFDictionaryGetValue(attrs, fontKey), to: CTFont.self)
            for i in 0..<count {
                out.append(PlacedGlyph(glyph: glyphs[i], font: runFont,
                                       x: positions[i].x, y: positions[i].y,
                                       advance: advances[i].width))
            }
        }
        return out.sorted { $0.x < $1.x }
    }

    // Group glyphs whose horizontal extents overlap (base + attached marks).
    func clusters(_ glyphs: [PlacedGlyph]) -> [[PlacedGlyph]] {
        var out: [[PlacedGlyph]] = []
        var cur: [PlacedGlyph] = []
        var curEnd: CGFloat = -1e9
        for g in glyphs {
            let inkEnd = g.x + max(g.advance, 0.5)
            if cur.isEmpty || g.x < curEnd - 0.25 || g.advance < 0.5 {
                cur.append(g)
            } else {
                out.append(cur)
                cur = [g]
                curEnd = -1e9
            }
            curEnd = max(curEnd, inkEnd)
        }
        if !cur.isEmpty { out.append(cur) }
        return out
    }

    // Render a cluster into a grayscale strip (top-down rows), then let
    // the caller slice it into <=16px tiles.
    func renderClusterStrip(_ cluster: [PlacedGlyph], width: Int, origin: CGFloat) -> [UInt8] {
        let ctxW = max(16, ((width + 15) / 16) * 16)
        guard let ctx = CGContext(data: nil, width: ctxW, height: 16,
                                  bitsPerComponent: 8, bytesPerRow: ctxW,
                                  space: CGColorSpaceCreateDeviceGray(),
                                  bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
            fatalError("cg context")
        }
        ctx.setFillColor(gray: 0, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: ctxW, height: 16))
        ctx.setFillColor(gray: 1, alpha: 1)
        for g in cluster {
            var glyph = g.glyph
            var pos = CGPoint(x: g.x - origin, y: g.y + baselineY)
            CTFontDrawGlyphs(g.font, &glyph, &pos, 1, ctx)
        }
        guard let data = ctx.data else { fatalError("cg data") }
        let px = data.bindMemory(to: UInt8.self, capacity: ctxW * 16)
        // CGBitmapContext memory is top-down already (row 0 = top scanline;
        // only the drawing coordinate origin is bottom-left).
        var out = [UInt8](repeating: 0, count: ctxW * 16)
        for i in 0..<(ctxW * 16) { out[i] = px[i] }
        return out
    }

    // Render the same cluster at doubled size: identical glyph ids and
    // scaled positions, so the 2x tile is an exact high-resolution twin of
    // the 1x tile's window.
    func renderClusterStrip2x(_ cluster: [PlacedGlyph], width: Int, origin: CGFloat) -> [UInt8] {
        let ctxW = max(32, ((width * 2 + 31) / 32) * 32)
        guard let ctx = CGContext(data: nil, width: ctxW, height: 32,
                                  bitsPerComponent: 8, bytesPerRow: ctxW,
                                  space: CGColorSpaceCreateDeviceGray(),
                                  bitmapInfo: CGImageAlphaInfo.none.rawValue) else {
            fatalError("cg context 2x")
        }
        ctx.setFillColor(gray: 0, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: ctxW, height: 32))
        ctx.setFillColor(gray: 1, alpha: 1)
        for g in cluster {
            let big = CTFontCreateCopyWithAttributes(g.font, CTFontGetSize(g.font) * 2, nil, nil)
            var glyph = g.glyph
            var pos = CGPoint(x: (g.x - origin) * 2, y: (g.y + baselineY) * 2)
            CTFontDrawGlyphs(big, &glyph, &pos, 1, ctx)
        }
        guard let data = ctx.data else { fatalError("cg data 2x") }
        let px = data.bindMemory(to: UInt8.self, capacity: ctxW * 32)
        var out = [UInt8](repeating: 0, count: ctxW * 32)
        for i in 0..<(ctxW * 32) { out[i] = px[i] }
        return out
    }

    // Pack one <=32px-wide window of a 2x strip into the 512-byte format.
    func packTile2x(_ strip: [UInt8], stripW: Int, from: Int, width: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 32 * 16)
        for row in 0..<32 {
            for col in 0..<32 {
                var level = 0
                if col < width, from + col < stripW {
                    let lum = CGFloat(strip[row * stripW + from + col]) / 255.0
                    level = min(15, Int((pow(lum, cfg.aaGamma) * 15.0).rounded()))
                }
                let idx = row * 16 + col / 2
                if col % 2 == 0 { out[idx] = UInt8(level << 4) }
                else { out[idx] |= UInt8(level) }
            }
        }
        return out
    }

    // Pack one <=16px-wide window of a strip into the 4bpp glyph format.
    func packTile(_ strip: [UInt8], stripW: Int, from: Int, width: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: DISPLAY_GLYPH_BYTES_SWIFT)
        for row in 0..<16 {
            for col in 0..<16 {
                var level = 0
                if col < width, from + col < stripW {
                    let lum = CGFloat(strip[row * stripW + from + col]) / 255.0
                    level = min(15, Int((pow(lum, cfg.aaGamma) * 15.0).rounded()))
                }
                let idx = row * 8 + col / 2
                if col % 2 == 0 { out[idx] = UInt8(level << 4) }
                else { out[idx] |= UInt8(level) }
            }
        }
        return out
    }

    // Shape one text run into PUA codepoints, registering new glyphs.
    mutating func shapeRun(_ text: String, context: String) -> String {
        let line = makeLine(text)
        let placed = placedGlyphs(line)
        var out = ""
        for cluster in clusters(placed) {
            // Quantize on the line's absolute positions so adjacent
            // clusters telescope: the sum of widths tracks the true line
            // width and connected scripts keep their baseline stroke.
            let xs = cluster.map { $0.x }.min() ?? 0
            let xe = cluster.map { $0.x + $0.advance }.max() ?? xs
            let qs = xs.rounded()
            let qe = xe.rounded()
            let w = max(1, Int(qe - qs))
            if w > 64 {
                fatalError("\(cfg.scope): cluster wider than 64px (\(w)) in \(context)")
            }
            let strip = renderClusterStrip(cluster, width: w, origin: qs)
            let stripW = max(16, ((w + 15) / 16) * 16)
            let strip2x = renderClusterStrip2x(cluster, width: w, origin: qs)
            let strip2xW = max(32, ((w * 2 + 31) / 32) * 32)
            // Slice wide clusters (conjunct chains) into <=16px tiles.
            var from = 0
            while from < w {
                let tileW = min(16, w - from)
                let bitmap = packTile(strip, stripW: stripW, from: from, width: tileW)
                let key = "\(tileW):" + bitmap.map { String(format: "%02X", $0) }.joined()
                let idx: Int
                if let existing = keyToIndex[key] {
                    idx = existing
                } else {
                    idx = glyphs.count
                    if idx >= 0x1000 { fatalError("\(cfg.scope): PUA space exhausted") }
                    keyToIndex[key] = idx
                    glyphs.append(bitmap)
                    glyphs2x.append(packTile2x(strip2x, stripW: strip2xW, from: from * 2, width: tileW * 2))
                    widths.append(tileW)
                }
                out.append(Character(UnicodeScalar(0xE000 + idx)!))
                from += tileW
            }
        }
        return out
    }

    mutating func shapeString(_ s: String, context: String) -> String {
        var segs = segment(s)
        if cfg.rtl { segs.reverse() }
        var out = ""
        for seg in segs {
            switch seg {
            case .spec(let sp):
                out += sp
            case .text(let t):
                if t.unicodeScalars.allSatisfy({ $0.value < 0x80 }) {
                    out += t
                } else {
                    out += shapeRun(t, context: context)
                }
            }
        }
        return out
    }
}

let DISPLAY_GLYPH_BYTES_SWIFT = 16 * 16 / 2

// UTF-8 with every non-ASCII byte as its own \xNN literal so a hex escape
// can never swallow a following letter.
func cStringExpr(_ value: String) -> String {
    var parts: [String] = []
    var ascii = ""
    func flushAscii() {
        if !ascii.isEmpty {
            let esc = ascii.replacingOccurrences(of: "\\", with: "\\\\")
                           .replacingOccurrences(of: "\"", with: "\\\"")
            parts.append("\"\(esc)\"")
            ascii = ""
        }
    }
    for ch in value {
        if ch.unicodeScalars.count == 1, let sc = ch.unicodeScalars.first,
           sc.value >= 0x20, sc.value <= 0x7e {
            ascii.append(ch)
        } else {
            flushAscii()
            for b in String(ch).utf8 {
                parts.append(String(format: "\"\\x%02X\"", b))
            }
        }
    }
    flushAscii()
    if parts.isEmpty { return "\"\"" }
    if parts.count == 1 { return parts[0] }
    return "(" + parts.joined(separator: " ") + ")"
}

func generateShapedC(_ source: String) -> String {
    var out = ""
    for cfg in shapedConfigs {
        fputs("shaping \(cfg.scope)...\n", stderr)
        let src = parseShapedSource(source, scope: cfg.scope)
        fputs("  parsed \(src.strings.count) strings\n", stderr)
        var builder = ShapedFontBuilder(cfg: cfg)

        // Common baseline for the language: center the union ink box of
        // every text run in the 16px cell.
        var minY: CGFloat = 1e9
        var maxY: CGFloat = -1e9
        var runs: [String] = []
        for (_, text) in src.strings { runs.append(text) }
        runs += src.weekdays + src.months + [src.name]
        for text in runs {
            for seg in segment(text) {
                if case .text(let t) = seg, !t.unicodeScalars.allSatisfy({ $0.value < 0x80 }) {
                    let b = CTLineGetImageBounds(builder.makeLine(t), nil)
                    if b.isNull { continue }
                    minY = min(minY, b.minY)
                    maxY = max(maxY, b.maxY)
                }
            }
        }
        let inkH = maxY - minY
        if inkH > 16 {
            fatalError("\(cfg.scope): ink height \(inkH) exceeds 16px at \(cfg.fontSize)pt - reduce fontSize")
        }
        builder.baselineY = ((16 - inkH) / 2 - minY).rounded()
        fputs("  ink \(minY)..\(maxY) baseline \(builder.baselineY)\n", stderr)

        var shapedStrings: [(String, String)] = []
        for (id, text) in src.strings {
            shapedStrings.append((id, builder.shapeString(text, context: "\(cfg.scope)/\(id)")))
        }
        let shapedWk = src.weekdays.map { builder.shapeString($0, context: "\(cfg.scope)/weekday") }
        let shapedMo = src.months.map { builder.shapeString($0, context: "\(cfg.scope)/month") }
        let shapedName = builder.shapeString(src.name, context: "\(cfg.scope)/name")

        let hash = SHA256.hash(data: Data(src.raw.utf8)).prefix(8)
            .map { String(format: "%02x", $0) }.joined()
        print("\(cfg.scope): \(builder.glyphs.count) cluster glyphs (\(cfg.fontName))")

        out += "// \(cfg.scope): \(builder.glyphs.count) shaped cluster glyphs (\(cfg.fontName))\n"
        out += "// shaped-source-hash \(cfg.scope) \(hash)\n"
        out += "static const uint16_t \(cfg.glyphArray)_cp[] = {\n"
        var lineBuf = "   "
        for i in 0..<builder.glyphs.count {
            lineBuf += String(format: " 0x%04X,", 0xE000 + i)
            if lineBuf.count > 90 { out += lineBuf + "\n"; lineBuf = "   " }
        }
        if lineBuf != "   " { out += lineBuf + "\n" }
        out += "};\n"
        out += "static const uint8_t \(cfg.glyphArray)_w[] = {\n"
        lineBuf = "   "
        for w in builder.widths {
            lineBuf += String(format: " %d,", w)
            if lineBuf.count > 90 { out += lineBuf + "\n"; lineBuf = "   " }
        }
        if lineBuf != "   " { out += lineBuf + "\n" }
        out += "};\n"
        out += "static const uint8_t \(cfg.glyphArray)[][DISPLAY_GLYPH_BYTES] = {\n"
        for g in builder.glyphs {
            out += "    {" + g.map { String(format: "0x%02X", $0) }.joined(separator: ",") + "},\n"
        }
        out += "};\n"
        // Full-resolution 2x only for glyphs the 2x paths can reach: the
        // clock date (weekdays, months) and the waiting banner. Everything
        // else pixel-doubles.
        var reachable2x = Set<Int>()
        for text in shapedWk + shapedMo + [shapedStrings.first(where: { $0.0 == "STR_WAITING_NTP" })?.1 ?? ""] {
            for sc in text.unicodeScalars where sc.value >= 0xE000 {
                reachable2x.insert(Int(sc.value) - 0xE000)
            }
        }
        let tiles2x: [[UInt8]?] = (0..<builder.glyphs.count).map {
            reachable2x.contains($0) ? builder.glyphs2x[$0] : nil
        }
        out += emitCompressed2x("\(cfg.glyphArray)_2x", tiles2x, storage: "static ")
        out += """
        const display_glyph_font_t \(cfg.fontObject) = {
            .codepoints = \(cfg.glyphArray)_cp,
            .glyphs = \(cfg.glyphArray),
            .glyphs_2x_rle = \(cfg.glyphArray)_2x_rle,
            .glyphs_2x_off = \(cfg.glyphArray)_2x_off,
            .widths = \(cfg.glyphArray)_w,
            .count = sizeof(\(cfg.glyphArray)) / sizeof(\(cfg.glyphArray)[0]),
            .glyph_width = 16,
        };

        """
        out += "const char *const lang_\(cfg.scope)_shaped[STR_COUNT] = {\n"
        for (id, text) in shapedStrings {
            out += "    [\(id)] = \(cStringExpr(text)),\n"
        }
        out += "};\n"
        out += "const char *const weekdays_\(cfg.scope)_shaped[7] = {\n"
        out += shapedWk.map { "    \(cStringExpr($0))" }.joined(separator: ",\n")
        out += "\n};\n"
        out += "const char *const months_\(cfg.scope)_shaped[12] = {\n"
        out += shapedMo.map { "    \(cStringExpr($0))" }.joined(separator: ",\n")
        out += "\n};\n"
        out += "const char lang_name_\(cfg.scope)_shaped[] = \(cStringExpr(shapedName));\n\n"
    }
    return out
}

try (String(contentsOf: root.appendingPathComponent("main/fonts.c"), encoding: .utf8)
     + generateShapedC(dataSource))
    .write(to: root.appendingPathComponent("main/fonts.c"), atomically: true, encoding: .utf8)
