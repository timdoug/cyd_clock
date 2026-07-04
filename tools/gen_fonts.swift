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

// Coverage -> 4-bit level: a contrast stretch kills the faint halo below
// 15% coverage and saturates above 85% (sharper edges); in between the
// level is linear coverage. No gamma shaping: the device blends the
// palette in linear light, which is correct for any fg/bg pair.
func quantizeCoverage(_ lum: CGFloat) -> Int {
    let t = min(1.0, max(0.0, (lum - 0.15) / 0.70))
    return min(15, Int((t * 15.0).rounded()))
}

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
// 2x bitmaps are only reachable from the clock date and the waiting
// banner, so alongside the full character buckets we track the subset
// used by weekday/month/date-suffix tables and STR_WAITING_NTP; every
// other glyph pixel-doubles at 2x (which nothing renders).
var dateBuckets: [String: Set<Character>] = ["": Set("0123456789 .,-")]

func collectChars(_ source: String) -> [String: Set<Character>] {
    var buckets: [String: Set<Character>] = ["": []]
    for cfg in cjkConfigs { buckets[cfg.scope] = []; dateBuckets[cfg.scope] = [] }

    var scope = ""
    var inShaped = false
    var inBlock = false
    var inDateTable = false
    for line in source.split(separator: "\n", omittingEmptySubsequences: false) {
        // shaped-script sources are consumed by the shaping pipeline, not
        // the plain character collector
        if line.hasPrefix("// >>> shaped-source") { inShaped = true }
        if line.hasPrefix("// <<< shaped-source") { inShaped = false; continue }
        if inShaped { continue }
        if !inBlock {
            scope = ""
            inDateTable = false
            // declaration line: pick scope from the symbol name
            if let m = line.range(of: #"(lang|weekdays|months|lang_name|date_year|date_day)_[a-z_]+"#,
                                  options: .regularExpression) {
                let sym = String(line[m])
                for cfg in cjkConfigs where sym.hasSuffix("_" + cfg.scope) {
                    scope = cfg.scope
                    break
                }
                if sym.hasPrefix("weekdays_") || sym.hasPrefix("months_")
                    || sym.hasPrefix("date_year_") || sym.hasPrefix("date_day_") {
                    inDateTable = true
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
            for ch in lit {
                if !isAscii(ch) { buckets[scope]!.insert(ch) }
                if inDateTable || line.contains("[STR_WAITING_NTP]") {
                    dateBuckets[scope]!.insert(ch)
                }
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
    // No hinting exists on macOS, but snapping glyph positions to whole
    // pixels keeps 1px stems from smearing across two columns at these
    // sizes.
    ctx.cgContext.setShouldSubpixelPositionFonts(false)
    ctx.cgContext.setShouldSubpixelQuantizeFonts(true)
    NSColor.black.setFill()
    NSRect(x: 0, y: 0, width: renderWidth, height: pixels).fill()

    let fontSize = pixels == 16 ? cfg.fontSize16 : cfg.fontSize32
    guard var font = NSFont(name: cfg.fontName, size: fontSize) else {
        // Falling back to another font would silently regenerate every
        // table with different metrics.
        fatalError("font \(cfg.fontName) is not installed")
    }
    var text = ch
    if !fontHasGlyphs(font, text) {
        // Menlo lacks some precomposed Vietnamese vowels but has every
        // combining mark; draw the decomposed sequence so CoreText
        // composes base + marks in the same face. A handful of Cyrillic
        // letters (Bashkir qa with stroke) are absent outright, but the
        // metric sibling Monaco has them; anything else missing fails
        // loudly rather than silently borrowing an arbitrary fallback.
        let nfd = text.decomposedStringWithCanonicalMapping
        if fontHasGlyphs(font, nfd) {
            text = nfd
        } else if cfg.fontName == "Menlo",
                  let monaco = NSFont(name: "Monaco", size: fontSize),
                  fontHasGlyphs(monaco, text) {
            font = monaco
        } else {
            fatalError("\(cfg.fontName) has no glyph for \(text)")
        }
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
                level = quantizeCoverage(lum)
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

func packCodepoints(_ chars: [Character]) -> Int {
    return blobAppend(u16le(chars.map { Int(codepoint($0)) }), align: 2)
}

// Raw-DEFLATE encoder restricted to stored and fixed-Huffman blocks
// (equivalent to zlib's Z_FIXED strategy). Greedy LZ77 with the output
// history as the window; on tiny per-glyph inputs, dynamic Huffman
// tables are pure overhead, and a fixed-only decoder is ~110 lines with
// no table state. Measured 3.4x vs PackBits' 2.7x on the glyph corpus.
struct BitWriter {
    var bytes: [UInt8] = []
    var bit = 0
    // DEFLATE packs bits LSB-first into bytes.
    mutating func putBits(_ value: Int, _ n: Int) {
        for k in 0..<n {
            if bit == 0 { bytes.append(0) }
            if (value >> k) & 1 == 1 { bytes[bytes.count - 1] |= UInt8(1 << bit) }
            bit = (bit + 1) & 7
        }
    }
    // Huffman codes are emitted MSB of the code first.
    mutating func putCode(_ code: Int, _ n: Int) {
        for k in stride(from: n - 1, through: 0, by: -1) {
            putBits((code >> k) & 1, 1)
        }
    }
}

func putFixedLitLen(_ bw: inout BitWriter, _ sym: Int) {
    switch sym {
    case 0...143:   bw.putCode(0x30 + sym, 8)
    case 144...255: bw.putCode(0x190 + sym - 144, 9)
    case 256...279: bw.putCode(sym - 256, 7)
    default:        bw.putCode(0xC0 + sym - 280, 8)
    }
}

let lenBase = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
               35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
let lenExtra = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0]
let distBase = [1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                257, 385, 513, 769]
let distExtra = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8]

func packBits(_ data: [UInt8]) -> [UInt8] {
    var bw = BitWriter()
    bw.putBits(1, 1)  // BFINAL
    bw.putBits(1, 2)  // BTYPE = 01, fixed Huffman
    var i = 0
    let n = data.count
    while i < n {
        var bestLen = 0
        var bestDist = 0
        let start = max(0, i - 32768)
        var j = start
        while j < i {
            var l = 0
            while l < 258, i + l < n, data[j + l] == data[i + l] { l += 1 }
            if l > bestLen { bestLen = l; bestDist = i - j }
            j += 1
        }
        if bestLen >= 3 {
            var ls = 28
            while lenBase[ls] > bestLen || (ls < 28 && lenBase[ls + 1] <= bestLen) { ls -= 1 }
            while ls < 28, lenBase[ls + 1] <= bestLen { ls += 1 }
            putFixedLitLen(&bw, 257 + ls)
            bw.putBits(bestLen - lenBase[ls], lenExtra[ls])
            var ds = distBase.count - 1
            while distBase[ds] > bestDist { ds -= 1 }
            bw.putCode(ds, 5)
            bw.putBits(bestDist - distBase[ds], distExtra[ds])
            i += bestLen
        } else {
            putFixedLitLen(&bw, Int(data[i]))
            i += 1
        }
    }
    putFixedLitLen(&bw, 256)  // end of block
    // fall back to a stored block when incompressible
    if bw.bytes.count >= n + 5 {
        var out: [UInt8] = [0x01]  // BFINAL, BTYPE = 00
        out.append(UInt8(n & 0xFF)); out.append(UInt8((n >> 8) & 0xFF))
        out.append(UInt8(~n & 0xFF)); out.append(UInt8((~n >> 8) & 0xFF))
        out += data
        return out
    }
    return bw.bytes
}

// All bulk data accumulates into fonts.bin, embedded in the firmware via
// EMBED_FILES; the generated C holds only fonts_blob + offset address
// constants. The C hex-literal representation cost ~5 bytes of source per
// data byte and 36k lines of compile input.
var fontsBlob: [UInt8] = []

func blobAppend(_ bytes: [UInt8], align: Int = 1) -> Int {
    while fontsBlob.count % align != 0 { fontsBlob.append(0) }
    let off = fontsBlob.count
    fontsBlob += bytes
    return off
}

func u16le(_ vals: [Int]) -> [UInt8] {
    var out: [UInt8] = []
    for v in vals {
        out.append(UInt8(v & 0xFF))
        out.append(UInt8((v >> 8) & 0xFF))
    }
    return out
}

// Pack tiles into the blob; returns (rle offset, offsets-array offset).
// A nil tile gets an empty span (pixel-doubled at runtime).
func packTiles(_ name: String, _ tiles: [[UInt8]?]) -> (Int, Int) {
    var blob: [UInt8] = []
    var offs: [Int] = [0]
    for t in tiles {
        if let t = t { blob += packBits(t) }
        offs.append(blob.count)
    }
    guard blob.count <= 0xFFFF else { fatalError("\(name): blob exceeds u16 offsets") }
    let rleOff = blobAppend(blob)
    let offOff = blobAppend(u16le(offs), align: 2)
    return (rleOff, offOff)
}

func blobPtr(_ off: Int) -> String { "fonts_blob + \(off)" }
func blobPtr16(_ off: Int) -> String { "FB16(\(off))" }

// Pack initializer entries onto shared lines instead of one per line.
func chunkLines(_ items: [String], width: Int = 96) -> String {
    var lines: [String] = []
    var cur = ""
    for item in items {
        let piece = item + ","
        if !cur.isEmpty && cur.count + piece.count + 1 > width {
            lines.append(cur)
            cur = ""
        }
        cur += (cur.isEmpty ? "    " : " ") + piece
    }
    if !cur.isEmpty { lines.append(cur) }
    return lines.joined(separator: "\n")
}

// Pack a font's 1x and 2x tiles; returns (rle1x, off1x, rle2x, off2x).
// 2x tiles are emitted only for date-reachable characters.
func packGlyphArrays(_ prefix: String, _ chars: [Character], cfg: FontConfig,
                     out16: Int, out32: Int, reach2x: Set<Character>) -> (Int, Int, Int, Int) {
    let tiles1x: [[UInt8]?] = chars.map { render(String($0), cfg: cfg, pixels: 16, outputWidth: out16) }
    let (r1, o1) = packTiles(prefix, tiles1x)
    let tiles: [[UInt8]?] = chars.map {
        reach2x.contains($0) ? render(String($0), cfg: cfg, pixels: 32, outputWidth: out32) : nil
    }
    let (r2, o2) = packTiles("\(prefix)_2x", tiles)
    return (r1, o1, r2, o2)
}

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let dataSource = try String(contentsOf: root.appendingPathComponent("main/i18n_data.inc"),
                            encoding: .utf8)
let buckets = collectChars(dataSource)

var out = """
// Generated by tools/gen_fonts.swift; do not edit by hand.
// Bulk glyph and string data lives in fonts.bin (embedded into the app
// image via EMBED_FILES); everything here is offsets into that blob.
#include "fonts.h"

#ifndef FONTS_BLOB_HOST
extern const uint8_t fonts_blob[] __asm__("_binary_fonts_bin_start");
#else
extern const uint8_t fonts_blob[];  // host harness provides the blob
#endif

// Pointer-into-blob shorthands: FB is a string, FB16 a uint16 table.
#define FB(off)   ((const char *)(fonts_blob + (off)))
#define FB16(off) ((const uint16_t *)(const void *)(fonts_blob + (off)))

"""

for cfg in cjkConfigs.sorted(by: { $0.scope < $1.scope }) {
    let chars = buckets[cfg.scope]!.sorted { codepoint($0) < codepoint($1) }
    print("\(cfg.scope): \(chars.count) glyphs")
    out += "// \(cfg.scope): \(chars.count) glyphs (\(cfg.fontName))\n"
    let cpOff = packCodepoints(chars)
    let (r1, o1, r2, o2) = packGlyphArrays(cfg.glyphArray, chars, cfg: cfg,
                                           out16: 16, out32: 32,
                                           reach2x: dateBuckets[cfg.scope] ?? [])
    out += """
    const display_glyph_font_t \(cfg.fontObject) = {
        .codepoints = \(blobPtr16(cpOff)),
        .glyphs_rle = \(blobPtr(r1)),
        .glyphs_off = \(blobPtr16(o1)),
        .glyphs_2x_rle = \(blobPtr(r2)),
        .glyphs_2x_off = \(blobPtr16(o2)),
        .count = \(chars.count),
        .glyph_width = \(cfg.glyphWidth),
    };

    """
}

var baseChars = buckets[""]!
baseChars.insert("\u{00B0}")  // degree sign, used by the rotation label
let baseSorted = baseChars.sorted { codepoint($0) < codepoint($1) }
print("base: \(baseSorted.count) supplementary glyphs")

out += "// base: ASCII plus \(baseSorted.count) supplementary glyphs (\(baseConfig.fontName))\n"
let baseAscii1x: [[UInt8]?] = (0x20...0x7E).map {
    render(String(UnicodeScalar($0)!), cfg: baseConfig, pixels: 16, outputWidth: 8)
}
let (bar1, bao1) = packTiles("font_base_ascii", baseAscii1x)
let baseReach = dateBuckets[""] ?? []
let baseAscii2x: [[UInt8]?] = (0x20...0x7E).map {
    baseReach.contains(Character(UnicodeScalar($0)!))
        ? render(String(UnicodeScalar($0)!), cfg: baseConfig, pixels: 32, outputWidth: 16)
        : nil
}
let (bar2, bao2) = packTiles("font_base_ascii_2x", baseAscii2x)
let bExtCp = packCodepoints(baseSorted)
let (ber1, beo1, ber2, beo2) = packGlyphArrays("font_base_ext", baseSorted, cfg: baseConfig,
                                               out16: 8, out32: 16, reach2x: baseReach)
out += """
const uint8_t *const font_base_ascii_rle = \(blobPtr(bar1));
const uint16_t *const font_base_ascii_off = \(blobPtr16(bao1));
const uint8_t *const font_base_ascii_2x_rle = \(blobPtr(bar2));
const uint16_t *const font_base_ascii_2x_off = \(blobPtr16(bao2));
const uint16_t *const font_base_ext_cp = \(blobPtr16(bExtCp));
const uint8_t *const font_base_ext_rle = \(blobPtr(ber1));
const uint16_t *const font_base_ext_off = \(blobPtr16(beo1));
const uint8_t *const font_base_ext_2x_rle = \(blobPtr(ber2));
const uint16_t *const font_base_ext_2x_off = \(blobPtr16(beo2));
const uint16_t font_base_ext_count = \(baseSorted.count);

"""

try out.write(to: root.appendingPathComponent("main/fonts.c"), atomically: true, encoding: .utf8)

// ---------------------------------------------------------------------------
// Picker order: native names sorted by Unicode root collation (via ICU's
// en tailoring, which adds nothing to root) - the same order OS language
// lists use. Emitted so the hand-maintained array cannot drift.
var pickerNames: [(String, String)] = []  // (enum, native name)
var scanShaped = false
var pendingScope = ""
for line in dataSource.split(separator: "\n", omittingEmptySubsequences: false) {
    if let r = line.range(of: #"lang_name_([a-z_]+)\[\] = ""#, options: .regularExpression) {
        let frag = String(line[r])
        let code = frag.replacingOccurrences(of: "lang_name_", with: "")
            .replacingOccurrences(of: "[] = \"", with: "")
        guard let q1 = line.range(of: "\"")?.upperBound else { continue }
        var name = ""
        var i = q1
        while i < line.endIndex, line[i] != "\"" {
            if line[i] == "\\" { i = line.index(after: i) }
            if i < line.endIndex { name.append(line[i]); i = line.index(after: i) }
        }
        pickerNames.append((code.uppercased(), name))
    }
    _ = scanShaped; _ = pendingScope
}
let collator = Locale(identifier: "en")
let sortedNames = pickerNames.sorted {
    $0.1.compare($1.1, options: [], range: nil, locale: collator) == .orderedAscending
}
var orderOut = """
// Generated by tools/gen_fonts.swift; do not edit by hand.
// Native names sorted by Unicode root collation, as OS language pickers do.
static const lang_t lang_order[LANG_COUNT] = {

"""
for (en, name) in sortedNames {
    // ASCII-only comment: keep Latin-script names, else the code
    let ascii = name.unicodeScalars.allSatisfy { $0.value < 0x80 }
    orderOut += "    LANG_\(en),\(ascii ? "  // " + name : "")\n"
}
orderOut += "};\n"
try orderOut.write(to: root.appendingPathComponent("main/lang_order.inc"), atomically: true, encoding: .utf8)
print("picker order: \(sortedNames.count) languages")

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
    // Characters the C side composes at runtime (outside the pre-shaped
    // strings), emitted at their real codepoints: Persian digits for the
    // Jalali date.
    var extraChars: String = ""
}

let shapedConfigs = [
    ShapedConfig(scope: "ar", fontObject: "font_ar", glyphArray: "ar_glyphs",
                 fontName: "Geeza Pro", fontSize: 13, rtl: true),
    ShapedConfig(scope: "fa", fontObject: "font_fa", glyphArray: "fa_glyphs",
                 fontName: "Geeza Pro", fontSize: 13, rtl: true,
                 extraChars: "\u{06F0}\u{06F1}\u{06F2}\u{06F3}\u{06F4}\u{06F5}\u{06F6}\u{06F7}\u{06F8}\u{06F9}"),
    ShapedConfig(scope: "he", fontObject: "font_he", glyphArray: "he_glyphs",
                 fontName: "Arial Hebrew", fontSize: 14, rtl: true),
    ShapedConfig(scope: "hi", fontObject: "font_hi", glyphArray: "hi_glyphs",
                 fontName: "Kohinoor Devanagari", fontSize: 12, rtl: false),
    ShapedConfig(scope: "bn", fontObject: "font_bn", glyphArray: "bn_glyphs",
                 fontName: "Kohinoor Bangla", fontSize: 12, rtl: false),
    ShapedConfig(scope: "th", fontObject: "font_th", glyphArray: "th_glyphs",
                 fontName: "Thonburi", fontSize: 11, rtl: false),
    ShapedConfig(scope: "ur", fontObject: "font_ur", glyphArray: "ur_glyphs",
                 fontName: "Geeza Pro", fontSize: 12, rtl: true),
    ShapedConfig(scope: "mr", fontObject: "font_mr", glyphArray: "mr_glyphs",
                 fontName: "Kohinoor Devanagari", fontSize: 12, rtl: false),
    ShapedConfig(scope: "ta", fontObject: "font_ta", glyphArray: "ta_glyphs",
                 fontName: "Tamil Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "te", fontObject: "font_te", glyphArray: "te_glyphs",
                 fontName: "Kohinoor Telugu", fontSize: 10, rtl: false),
    ShapedConfig(scope: "pa", fontObject: "font_pa", glyphArray: "pa_glyphs",
                 fontName: "Gurmukhi Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "gu", fontObject: "font_gu", glyphArray: "gu_glyphs",
                 fontName: "Kohinoor Gujarati", fontSize: 12, rtl: false),
    ShapedConfig(scope: "kn", fontObject: "font_kn", glyphArray: "kn_glyphs",
                 fontName: "Kannada Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "ml", fontObject: "font_ml", glyphArray: "ml_glyphs",
                 fontName: "Malayalam Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "ne", fontObject: "font_ne", glyphArray: "ne_glyphs",
                 fontName: "Kohinoor Devanagari", fontSize: 12, rtl: false),
    ShapedConfig(scope: "si", fontObject: "font_si", glyphArray: "si_glyphs",
                 fontName: "Sinhala Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "km", fontObject: "font_km", glyphArray: "km_glyphs",
                 fontName: "Khmer Sangam MN", fontSize: 9.5, rtl: false),
    ShapedConfig(scope: "lo", fontObject: "font_lo", glyphArray: "lo_glyphs",
                 fontName: "Lao Sangam MN", fontSize: 12, rtl: false),
    ShapedConfig(scope: "my", fontObject: "font_my", glyphArray: "my_glyphs",
                 fontName: "Myanmar Sangam MN", fontSize: 10, rtl: false),
    ShapedConfig(scope: "am", fontObject: "font_am", glyphArray: "am_glyphs",
                 fontName: "Kefa", fontSize: 12, rtl: false),
    ShapedConfig(scope: "kok", fontObject: "font_kok", glyphArray: "kok_glyphs",
                 fontName: "Kohinoor Devanagari", fontSize: 12, rtl: false),
    ShapedConfig(scope: "as", fontObject: "font_as", glyphArray: "as_glyphs",
                 fontName: "Kohinoor Bangla", fontSize: 12, rtl: false),
    ShapedConfig(scope: "ti", fontObject: "font_ti", glyphArray: "ti_glyphs",
                 fontName: "Kefa", fontSize: 12, rtl: false),
    // Latin with combining tone marks (no NFC precomposition), so these
    // two go through the shaping pipeline rather than the base font.
    ShapedConfig(scope: "yo", fontObject: "font_yo", glyphArray: "yo_glyphs",
                 fontName: "Menlo", fontSize: 12, rtl: false),
    ShapedConfig(scope: "pcm", fontObject: "font_pcm", glyphArray: "pcm_glyphs",
                 fontName: "Menlo", fontSize: 12, rtl: false),
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
        ctx.setShouldSubpixelPositionFonts(false)
        ctx.setShouldSubpixelQuantizeFonts(true)
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
        ctx.setShouldSubpixelPositionFonts(false)
        ctx.setShouldSubpixelQuantizeFonts(true)
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
                    level = quantizeCoverage(lum)
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
                    level = quantizeCoverage(lum)
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

        // Runtime-composed characters (Persian digits): standalone glyph
        // entries at their real codepoints, never deduped into the PUA
        // tile namespace.
        var realCps: [(UInt32, Int)] = []  // (codepoint, glyph index)
        for ch in cfg.extraChars {
            let line = builder.makeLine(String(ch))
            let placed = builder.placedGlyphs(line)
            guard !placed.isEmpty else { fatalError("\(cfg.scope): no glyph for extra char") }
            let xs = placed.map { $0.x }.min()!
            let xe = placed.map { $0.x + $0.advance }.max()!
            let w = max(1, min(16, Int((xe - xs).rounded())))
            let strip = builder.renderClusterStrip(placed, width: w, origin: xs.rounded())
            let strip2x = builder.renderClusterStrip2x(placed, width: w, origin: xs.rounded())
            let idx = builder.glyphs.count
            builder.glyphs.append(builder.packTile(strip, stripW: max(16, ((w + 15) / 16) * 16), from: 0, width: w))
            builder.glyphs2x.append(builder.packTile2x(strip2x, stripW: max(32, ((w * 2 + 31) / 32) * 32), from: 0, width: w * 2))
            builder.widths.append(w)
            realCps.append((codepoint(ch), idx))
        }

        let hash = SHA256.hash(data: Data(src.raw.utf8)).prefix(8)
            .map { String(format: "%02x", $0) }.joined()
        print("\(cfg.scope): \(builder.glyphs.count) cluster glyphs (\(cfg.fontName))")

        // The codepoint table must be sorted for the runtime binary
        // search; extra chars carry real codepoints below the PUA block,
        // so emit through a sort permutation.
        var cpOf = (0..<builder.glyphs.count).map { UInt32(0xE000 + $0) }
        for (cp, idx) in realCps { cpOf[idx] = cp }
        let perm = (0..<cpOf.count).sorted { cpOf[$0] < cpOf[$1] }

        out += "// \(cfg.scope): \(builder.glyphs.count) shaped cluster glyphs (\(cfg.fontName))\n"
        out += "// shaped-source-hash \(cfg.scope) \(hash)\n"
        let cpOff = blobAppend(u16le(perm.map { Int(cpOf[$0]) }), align: 2)
        let wOff = blobAppend(perm.map { UInt8(builder.widths[$0]) })
        let (r1, o1) = packTiles(cfg.glyphArray, perm.map { Optional(builder.glyphs[$0]) })
        // Full-resolution 2x only for glyphs the 2x paths can reach: the
        // clock date (weekdays, months) and the waiting banner. Everything
        // else pixel-doubles.
        var reachable2x = Set<Int>()
        for text in shapedWk + shapedMo + [shapedStrings.first(where: { $0.0 == "STR_WAITING_NTP" })?.1 ?? ""] {
            for sc in text.unicodeScalars where sc.value >= 0xE000 {
                reachable2x.insert(Int(sc.value) - 0xE000)
            }
        }
        for (_, idx) in realCps { reachable2x.insert(idx) }  // date digits
        let tiles2x: [[UInt8]?] = perm.map {
            reachable2x.contains($0) ? builder.glyphs2x[$0] : nil
        }
        let (r2, o2) = packTiles("\(cfg.glyphArray)_2x", tiles2x)
        out += """
        const display_glyph_font_t \(cfg.fontObject) = {
            .codepoints = \(blobPtr16(cpOff)),
            .glyphs_rle = \(blobPtr(r1)),
            .glyphs_off = \(blobPtr16(o1)),
            .glyphs_2x_rle = \(blobPtr(r2)),
            .glyphs_2x_off = \(blobPtr16(o2)),
            .widths = \(blobPtr(wOff)),
            .count = \(builder.glyphs.count),
            .glyph_width = 16,
        };

        """
        func strOff(_ text: String) -> Int {
            return blobAppend(Array(text.utf8) + [0])
        }
        out += "const char *const lang_\(cfg.scope)_shaped[STR_COUNT] = {\n"
        out += chunkLines(shapedStrings.map { "[\($0.0)] = FB(\(strOff($0.1)))" })
        out += "\n};\n"
        out += "const char *const weekdays_\(cfg.scope)_shaped[7] = {\n"
        out += chunkLines(shapedWk.map { "FB(\(strOff($0)))" })
        out += "\n};\n"
        out += "const char *const months_\(cfg.scope)_shaped[12] = {\n"
        out += chunkLines(shapedMo.map { "FB(\(strOff($0)))" })
        out += "\n};\n"
        out += "const char lang_name_\(cfg.scope)_shaped[] = \(cStringExpr(shapedName));\n\n"
    }
    return out
}

try (String(contentsOf: root.appendingPathComponent("main/fonts.c"), encoding: .utf8)
     + generateShapedC(dataSource))
    .write(to: root.appendingPathComponent("main/fonts.c"), atomically: true, encoding: .utf8)
try Data(fontsBlob).write(to: root.appendingPathComponent("main/fonts.bin"))
print("fonts.bin: \(fontsBlob.count) bytes")
