import AppKit
import Foundation

struct Language {
    let code: String
    let enumName: String
    let fontObject: String
    let glyphArray: String
    let fontName: String
    let fontSize16: CGFloat
    let fontSize32: CGFloat
    let alphaThreshold: UInt8
    let brightnessThreshold: UInt16
    let nativeName: String
    let strings: [(String, String)]
    let weekdays: [String]
    let months: [String]
    let yearSuffix: String
    let daySuffix: String
}

let stringIds = [
    "STR_SETTINGS", "STR_TIMEZONE", "STR_BRIGHTNESS", "STR_LED_BLINK",
    "STR_ROTATE", "STR_ABOUT", "STR_LANGUAGE", "STR_DONE", "STR_BACK",
    "STR_CANCEL", "STR_DEL", "STR_RUNNING", "STR_ON", "STR_OFF",
    "STR_NTS_NO", "STR_NTS_ATTEMPT", "STR_NTS_REQUIRE",
    "STR_WAITING_NTP", "STR_FMT_SYNCED", "STR_FMT_SYNCING", "STR_FMT_WAITING",
    "STR_FMT_PEERS_NOSYNC", "STR_FMT_PEERS_AGO", "STR_FMT_OFF_DRIFT",
    "STR_WIFI_SETUP", "STR_SCANNING", "STR_SCAN_FAILED", "STR_TAP_RETRY",
    "STR_SELECT_NETWORK", "STR_NO_NETWORKS", "STR_NETWORK_LABEL",
    "STR_ENTER_PASSWORD", "STR_CONNECTING", "STR_CONNECTING_TO",
    "STR_CONNECTED", "STR_CONNECTION_FAILED", "STR_KB_SHIFT", "STR_KB_SPACE",
    "STR_KB_GO", "STR_VERSION", "STR_FIRMWARE_URL", "STR_OTA_URL",
    "STR_STATUS", "STR_READ", "STR_UPDATE", "STR_RESTARTING",
    "STR_OTA_UPDATE", "STR_NTP_SERVER", "STR_SERVER_LABEL",
    "STR_NTP_SETTINGS", "STR_NTP_PRESETS", "STR_PRESETS", "STR_BENCHMARK",
    "STR_STOPPING", "STR_FAIL", "STR_NTP_STATS", "STR_UNSYNCED",
    "STR_STRATUM_LABEL", "STR_POLL_LABEL", "STR_SYNCS_LABEL",
    "STR_DRIFT_LABEL", "STR_AGE_LABEL", "STR_OFFSET_LABEL",
    "STR_JITTER_LABEL", "STR_ROOT_LABEL", "STR_FMT_ROOT_DETAIL",
    "STR_PEER_HEADER", "STR_NONE", "STR_SELECT_REGION",
]

let ja = Language(
    code: "ja",
    enumName: "LANG_JA",
    fontObject: "font_ja",
    glyphArray: "ja_glyphs",
    fontName: "Hiragino Sans GB",
    fontSize16: 14,
    fontSize32: 29,
    alphaThreshold: 96,
    brightnessThreshold: 384,
    nativeName: "日本語",
    strings: [
        ("STR_SETTINGS", "設定"),
        ("STR_TIMEZONE", "タイムゾーン"),
        ("STR_BRIGHTNESS", "明るさ"),
        ("STR_LED_BLINK", "LED点滅"),
        ("STR_ROTATE", "180度回転"),
        ("STR_ABOUT", "情報"),
        ("STR_LANGUAGE", "言語"),
        ("STR_DONE", "完了"),
        ("STR_BACK", "戻る"),
        ("STR_CANCEL", "取消"),
        ("STR_DEL", "削除"),
        ("STR_RUNNING", "実行中"),
        ("STR_ON", "オン"),
        ("STR_OFF", "オフ"),
        ("STR_NTS_NO", "いいえ"),
        ("STR_NTS_ATTEMPT", "試行"),
        ("STR_NTS_REQUIRE", "必須"),
        ("STR_WAITING_NTP", "NTP待機中..."),
        ("STR_FMT_SYNCED", "同期済み: %s"),
        ("STR_FMT_SYNCING", "同期中: %s"),
        ("STR_FMT_WAITING", "待機: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  未同期"),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  %s前"),
        ("STR_FMT_OFF_DRIFT", "差 %s %s ドリフト %s"),
        ("STR_WIFI_SETUP", "WiFi設定"),
        ("STR_SCANNING", "スキャン中..."),
        ("STR_SCAN_FAILED", "スキャン失敗"),
        ("STR_TAP_RETRY", "タップで再試行"),
        ("STR_SELECT_NETWORK", "ネットワーク選択"),
        ("STR_NO_NETWORKS", "ネットワークなし"),
        ("STR_NETWORK_LABEL", "ネットワーク:"),
        ("STR_ENTER_PASSWORD", "パスワード入力"),
        ("STR_CONNECTING", "接続中"),
        ("STR_CONNECTING_TO", "接続先"),
        ("STR_CONNECTED", "接続しました!"),
        ("STR_CONNECTION_FAILED", "接続失敗"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "空白"),
        ("STR_KB_GO", "実行"),
        ("STR_VERSION", "バージョン:"),
        ("STR_FIRMWARE_URL", "ファームURL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "ステータス:"),
        ("STR_READ", "読込:"),
        ("STR_UPDATE", "更新"),
        ("STR_RESTARTING", "再起動中"),
        ("STR_OTA_UPDATE", "OTA更新"),
        ("STR_NTP_SERVER", "NTPサーバー"),
        ("STR_SERVER_LABEL", "サーバー:"),
        ("STR_NTP_SETTINGS", "NTP設定"),
        ("STR_NTP_PRESETS", "NTPプリセット"),
        ("STR_PRESETS", "プリセット"),
        ("STR_BENCHMARK", "テスト"),
        ("STR_STOPPING", "停止中"),
        ("STR_FAIL", "失敗"),
        ("STR_NTP_STATS", "NTPステータス"),
        ("STR_UNSYNCED", "未同期"),
        ("STR_STRATUM_LABEL", "階層: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  同期: "),
        ("STR_DRIFT_LABEL", "ドリフト: "),
        ("STR_AGE_LABEL", "  経過: "),
        ("STR_OFFSET_LABEL", "オフセット: "),
        ("STR_JITTER_LABEL", "  ジッタ: "),
        ("STR_ROOT_LABEL", "ルート: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "なし"),
        ("STR_SELECT_REGION", "地域を選択"),
    ],
    weekdays: ["日", "月", "火", "水", "木", "金", "土"],
    months: ["1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"],
    yearSuffix: "年",
    daySuffix: "日"
)

let zh = Language(
    code: "zh",
    enumName: "LANG_ZH",
    fontObject: "font_zh",
    glyphArray: "zh_glyphs",
    fontName: "Hiragino Sans GB",
    fontSize16: 14,
    fontSize32: 29,
    alphaThreshold: 96,
    brightnessThreshold: 384,
    nativeName: "简体中文",
    strings: [
        ("STR_SETTINGS", "设置"),
        ("STR_TIMEZONE", "时区"),
        ("STR_BRIGHTNESS", "亮度"),
        ("STR_LED_BLINK", "LED闪烁"),
        ("STR_ROTATE", "旋转180"),
        ("STR_ABOUT", "关于"),
        ("STR_LANGUAGE", "语言"),
        ("STR_DONE", "完成"),
        ("STR_BACK", "返回"),
        ("STR_CANCEL", "取消"),
        ("STR_DEL", "删除"),
        ("STR_RUNNING", "运行中"),
        ("STR_ON", "开"),
        ("STR_OFF", "关"),
        ("STR_NTS_NO", "否"),
        ("STR_NTS_ATTEMPT", "尝试"),
        ("STR_NTS_REQUIRE", "要求"),
        ("STR_WAITING_NTP", "等待NTP..."),
        ("STR_FMT_SYNCED", "已同步: %s"),
        ("STR_FMT_SYNCING", "同步中: %s"),
        ("STR_FMT_WAITING", "等待: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  未同步"),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  %s前"),
        ("STR_FMT_OFF_DRIFT", "偏差 %s %s 漂移 %s"),
        ("STR_WIFI_SETUP", "WiFi设置"),
        ("STR_SCANNING", "扫描中..."),
        ("STR_SCAN_FAILED", "扫描失败"),
        ("STR_TAP_RETRY", "点按重试"),
        ("STR_SELECT_NETWORK", "选择网络"),
        ("STR_NO_NETWORKS", "未找到网络"),
        ("STR_NETWORK_LABEL", "网络:"),
        ("STR_ENTER_PASSWORD", "输入密码"),
        ("STR_CONNECTING", "连接中"),
        ("STR_CONNECTING_TO", "连接到"),
        ("STR_CONNECTED", "已连接!"),
        ("STR_CONNECTION_FAILED", "连接失败"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "空格"),
        ("STR_KB_GO", "开始"),
        ("STR_VERSION", "版本:"),
        ("STR_FIRMWARE_URL", "固件URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "状态:"),
        ("STR_READ", "已读:"),
        ("STR_UPDATE", "更新"),
        ("STR_RESTARTING", "正在重启"),
        ("STR_OTA_UPDATE", "OTA更新"),
        ("STR_NTP_SERVER", "NTP服务器"),
        ("STR_SERVER_LABEL", "服务器:"),
        ("STR_NTP_SETTINGS", "NTP设置"),
        ("STR_NTP_PRESETS", "NTP预设"),
        ("STR_PRESETS", "预设"),
        ("STR_BENCHMARK", "基准测试"),
        ("STR_STOPPING", "停止中"),
        ("STR_FAIL", "失败"),
        ("STR_NTP_STATS", "NTP统计"),
        ("STR_UNSYNCED", "未同步"),
        ("STR_STRATUM_LABEL", "层级: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  同步: "),
        ("STR_DRIFT_LABEL", "漂移: "),
        ("STR_AGE_LABEL", "  时长: "),
        ("STR_OFFSET_LABEL", "偏移: "),
        ("STR_JITTER_LABEL", "  抖动: "),
        ("STR_ROOT_LABEL", "根: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "无"),
        ("STR_SELECT_REGION", "选择地区"),
    ],
    weekdays: ["周日", "周一", "周二", "周三", "周四", "周五", "周六"],
    months: ["1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"],
    yearSuffix: "年",
    daySuffix: "日"
)

let zhHant = Language(
    code: "zh_hant",
    enumName: "LANG_ZH_HANT",
    fontObject: "font_zh_hant",
    glyphArray: "zh_hant_glyphs",
    fontName: "PingFang TC",
    fontSize16: 13,
    fontSize32: 28,
    alphaThreshold: 120,
    brightnessThreshold: 448,
    nativeName: "繁體中文",
    strings: [
        ("STR_SETTINGS", "設定"),
        ("STR_TIMEZONE", "時區"),
        ("STR_BRIGHTNESS", "亮度"),
        ("STR_LED_BLINK", "LED閃爍"),
        ("STR_ROTATE", "旋轉180"),
        ("STR_ABOUT", "關於"),
        ("STR_LANGUAGE", "語言"),
        ("STR_DONE", "完成"),
        ("STR_BACK", "返回"),
        ("STR_CANCEL", "取消"),
        ("STR_DEL", "刪除"),
        ("STR_RUNNING", "執行中"),
        ("STR_ON", "開"),
        ("STR_OFF", "關"),
        ("STR_NTS_NO", "否"),
        ("STR_NTS_ATTEMPT", "嘗試"),
        ("STR_NTS_REQUIRE", "要求"),
        ("STR_WAITING_NTP", "等待NTP..."),
        ("STR_FMT_SYNCED", "已同步: %s"),
        ("STR_FMT_SYNCING", "同步中: %s"),
        ("STR_FMT_WAITING", "等待: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  未同步"),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  %s前"),
        ("STR_FMT_OFF_DRIFT", "偏差 %s %s 漂移 %s"),
        ("STR_WIFI_SETUP", "WiFi設定"),
        ("STR_SCANNING", "掃描中..."),
        ("STR_SCAN_FAILED", "掃描失敗"),
        ("STR_TAP_RETRY", "點按重試"),
        ("STR_SELECT_NETWORK", "選擇網路"),
        ("STR_NO_NETWORKS", "找不到網路"),
        ("STR_NETWORK_LABEL", "網路:"),
        ("STR_ENTER_PASSWORD", "輸入密碼"),
        ("STR_CONNECTING", "連線中"),
        ("STR_CONNECTING_TO", "連線到"),
        ("STR_CONNECTED", "已連線!"),
        ("STR_CONNECTION_FAILED", "連線失敗"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "空格"),
        ("STR_KB_GO", "開始"),
        ("STR_VERSION", "版本:"),
        ("STR_FIRMWARE_URL", "韌體URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "狀態:"),
        ("STR_READ", "已讀:"),
        ("STR_UPDATE", "更新"),
        ("STR_RESTARTING", "正在重新啟動"),
        ("STR_OTA_UPDATE", "OTA更新"),
        ("STR_NTP_SERVER", "NTP伺服器"),
        ("STR_SERVER_LABEL", "伺服器:"),
        ("STR_NTP_SETTINGS", "NTP設定"),
        ("STR_NTP_PRESETS", "NTP預設"),
        ("STR_PRESETS", "預設"),
        ("STR_BENCHMARK", "基準測試"),
        ("STR_STOPPING", "停止中"),
        ("STR_FAIL", "失敗"),
        ("STR_NTP_STATS", "NTP統計"),
        ("STR_UNSYNCED", "未同步"),
        ("STR_STRATUM_LABEL", "層級: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  同步: "),
        ("STR_DRIFT_LABEL", "漂移: "),
        ("STR_AGE_LABEL", "  時長: "),
        ("STR_OFFSET_LABEL", "偏移: "),
        ("STR_JITTER_LABEL", "  抖動: "),
        ("STR_ROOT_LABEL", "根: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "無"),
        ("STR_SELECT_REGION", "選擇地區"),
    ],
    weekdays: ["週日", "週一", "週二", "週三", "週四", "週五", "週六"],
    months: ["1月", "2月", "3月", "4月", "5月", "6月", "7月", "8月", "9月", "10月", "11月", "12月"],
    yearSuffix: "年",
    daySuffix: "日"
)

let ko = Language(
    code: "ko",
    enumName: "LANG_KO",
    fontObject: "font_ko",
    glyphArray: "ko_glyphs",
    fontName: "Apple SD Gothic Neo",
    fontSize16: 14,
    fontSize32: 29,
    alphaThreshold: 96,
    brightnessThreshold: 384,
    nativeName: "한국어",
    strings: [
        ("STR_SETTINGS", "설정"),
        ("STR_TIMEZONE", "시간대"),
        ("STR_BRIGHTNESS", "밝기"),
        ("STR_LED_BLINK", "LED 깜박임"),
        ("STR_ROTATE", "180도 회전"),
        ("STR_ABOUT", "정보"),
        ("STR_LANGUAGE", "언어"),
        ("STR_DONE", "완료"),
        ("STR_BACK", "뒤로"),
        ("STR_CANCEL", "취소"),
        ("STR_DEL", "삭제"),
        ("STR_RUNNING", "실행 중"),
        ("STR_ON", "켜짐"),
        ("STR_OFF", "꺼짐"),
        ("STR_NTS_NO", "아니오"),
        ("STR_NTS_ATTEMPT", "시도"),
        ("STR_NTS_REQUIRE", "필수"),
        ("STR_WAITING_NTP", "NTP 대기 중..."),
        ("STR_FMT_SYNCED", "동기화됨: %s"),
        ("STR_FMT_SYNCING", "동기화 중: %s"),
        ("STR_FMT_WAITING", "대기: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  미동기화"),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  %s 전"),
        ("STR_FMT_OFF_DRIFT", "오프셋 %s %s 드리프트 %s"),
        ("STR_WIFI_SETUP", "WiFi 설정"),
        ("STR_SCANNING", "검색 중..."),
        ("STR_SCAN_FAILED", "검색 실패"),
        ("STR_TAP_RETRY", "탭하여 재시도"),
        ("STR_SELECT_NETWORK", "네트워크 선택"),
        ("STR_NO_NETWORKS", "네트워크 없음"),
        ("STR_NETWORK_LABEL", "네트워크:"),
        ("STR_ENTER_PASSWORD", "비밀번호 입력"),
        ("STR_CONNECTING", "연결 중"),
        ("STR_CONNECTING_TO", "연결 대상"),
        ("STR_CONNECTED", "연결됨!"),
        ("STR_CONNECTION_FAILED", "연결 실패"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "공백"),
        ("STR_KB_GO", "실행"),
        ("STR_VERSION", "버전:"),
        ("STR_FIRMWARE_URL", "펌웨어 URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "상태:"),
        ("STR_READ", "읽음:"),
        ("STR_UPDATE", "업데이트"),
        ("STR_RESTARTING", "재시작 중"),
        ("STR_OTA_UPDATE", "OTA 업데이트"),
        ("STR_NTP_SERVER", "NTP 서버"),
        ("STR_SERVER_LABEL", "서버:"),
        ("STR_NTP_SETTINGS", "NTP 설정"),
        ("STR_NTP_PRESETS", "NTP 프리셋"),
        ("STR_PRESETS", "프리셋"),
        ("STR_BENCHMARK", "벤치마크"),
        ("STR_STOPPING", "중지 중"),
        ("STR_FAIL", "실패"),
        ("STR_NTP_STATS", "NTP 통계"),
        ("STR_UNSYNCED", "미동기화"),
        ("STR_STRATUM_LABEL", "계층: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  동기화: "),
        ("STR_DRIFT_LABEL", "드리프트: "),
        ("STR_AGE_LABEL", "  경과: "),
        ("STR_OFFSET_LABEL", "오프셋: "),
        ("STR_JITTER_LABEL", "  지터: "),
        ("STR_ROOT_LABEL", "루트: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "없음"),
        ("STR_SELECT_REGION", "지역 선택"),
    ],
    weekdays: ["일", "월", "화", "수", "목", "금", "토"],
    months: ["1월", "2월", "3월", "4월", "5월", "6월", "7월", "8월", "9월", "10월", "11월", "12월"],
    yearSuffix: "년",
    daySuffix: "일"
)

let languages = [ja, zh, zhHant, ko]

func isAscii(_ ch: Character) -> Bool {
    guard ch.unicodeScalars.count == 1, let scalar = ch.unicodeScalars.first else { return false }
    return scalar.value >= 0x20 && scalar.value <= 0x7e
}

func collectGlyphs(_ lang: Language) -> [String] {
    var glyphs: [String] = []
    var seen = Set<String>()
    let values = lang.strings.map { $0.1 } + lang.weekdays + lang.months
        + [lang.nativeName, lang.yearSuffix, lang.daySuffix]
    for value in values {
        for ch in value {
            if isAscii(ch) { continue }
            let s = String(ch)
            if !seen.contains(s) {
                seen.insert(s)
                glyphs.append(s)
            }
        }
    }
    if glyphs.count > 255 {
        fatalError("\(lang.code) uses \(glyphs.count) glyphs; token encoding supports 255")
    }
    return glyphs
}

func render(_ ch: String, lang: Language, pixels: Int) -> [UInt8] {
    guard let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil,
        pixelsWide: pixels,
        pixelsHigh: pixels,
        bitsPerSample: 8,
        samplesPerPixel: 4,
        hasAlpha: true,
        isPlanar: false,
        colorSpaceName: .deviceRGB,
        bytesPerRow: pixels * 4,
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
    NSRect(x: 0, y: 0, width: pixels, height: pixels).fill()

    let fontSize = pixels == 16 ? lang.fontSize16 : lang.fontSize32
    let font = NSFont(name: lang.fontName, size: fontSize) ?? NSFont.systemFont(ofSize: fontSize)
    let s = ch as NSString
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor.white,
    ]
    let size = s.size(withAttributes: attrs)
    let x = floor((CGFloat(pixels) - size.width) / 2.0)
    let y = floor((CGFloat(pixels) - size.height) / 2.0) - (pixels == 16 ? 1.0 : 2.0)
    s.draw(at: NSPoint(x: x, y: y), withAttributes: attrs)
    NSGraphicsContext.restoreGraphicsState()

    guard let data = rep.bitmapData else {
        fatalError("bitmap data unavailable")
    }

    let bytesPerGlyphRow = pixels / 8
    var out = [UInt8](repeating: 0, count: pixels * bytesPerGlyphRow)
    for row in 0..<pixels {
        var bits: UInt32 = 0
        for col in 0..<pixels {
            let p = row * rep.bytesPerRow + col * 4
            let r = data[p]
            let g = data[p + 1]
            let b = data[p + 2]
            let a = data[p + 3]
            if a > lang.alphaThreshold &&
               (UInt16(r) + UInt16(g) + UInt16(b)) > lang.brightnessThreshold {
                bits |= UInt32(1) << UInt32(pixels - 1 - col)
            }
        }
        for byte in 0..<bytesPerGlyphRow {
            let shift = UInt32((bytesPerGlyphRow - 1 - byte) * 8)
            out[row * bytesPerGlyphRow + byte] = UInt8((bits >> shift) & 0xFF)
        }
    }
    return out
}

func cEscapeAscii(_ s: String) -> String {
    var out = ""
    for scalar in s.unicodeScalars {
        switch scalar {
        case "\\":
            out += "\\\\"
        case "\"":
            out += "\\\""
        default:
            out += String(scalar)
        }
    }
    return out
}

func cStringExpr(_ value: String, glyphIndex: [String: Int]) -> String {
    var parts: [String] = []
    var ascii = ""

    func flushAscii() {
        if !ascii.isEmpty {
            parts.append("\"\(cEscapeAscii(ascii))\"")
            ascii = ""
        }
    }

    for ch in value {
        if isAscii(ch) {
            ascii.append(ch)
        } else {
            flushAscii()
            let id = glyphIndex[String(ch)]! + 0x80
            parts.append(String(format: "\"\\x1E\"\"\\x%02X\"", id))
        }
    }
    flushAscii()
    return parts.isEmpty ? "\"\"" : parts.joined(separator: " ")
}

func validate(_ lang: Language) {
    let got = Set(lang.strings.map { $0.0 })
    let expected = Set(stringIds)
    if got != expected {
        let missing = stringIds.filter { !got.contains($0) }
        let extra = lang.strings.map { $0.0 }.filter { !expected.contains($0) }
        fatalError("\(lang.code) string id mismatch; missing=\(missing) extra=\(extra)")
    }
    if lang.weekdays.count != 7 || lang.months.count != 12 {
        fatalError("\(lang.code) weekday/month count mismatch")
    }
}

func generateFontC(_ glyphsByLanguage: [(Language, [String])]) -> String {
    var out = """
    // Generated by tools/gen_cjk_font.swift; do not edit by hand.
    #include \"cjk_font.h\"

    """

    for (lang, glyphs) in glyphsByLanguage {
        out += "static const uint8_t \(lang.glyphArray)[][DISPLAY_CJK_GLYPH_BYTES] = {\n"
        for glyph in glyphs {
            let bytes = render(glyph, lang: lang, pixels: 16)
            let hex = bytes.map { String(format: "0x%02X", $0) }.joined(separator: ",")
            out += "    {\(hex)},\n"
        }
        out += "};\n\n"

        out += "static const uint8_t \(lang.glyphArray)_2x[][DISPLAY_CJK_GLYPH_2X_BYTES] = {\n"
        for glyph in glyphs {
            let bytes = render(glyph, lang: lang, pixels: 32)
            let hex = bytes.map { String(format: "0x%02X", $0) }.joined(separator: ",")
            out += "    {\(hex)},\n"
        }
        out += "};\n\n"

        out += """
        const display_cjk_font_t \(lang.fontObject) = {
            .glyphs = \(lang.glyphArray),
            .glyphs_2x = \(lang.glyphArray)_2x,
            .count = sizeof(\(lang.glyphArray)) / sizeof(\(lang.glyphArray)[0]),
        };

        """
    }
    return out
}

func generateI18nInc(_ glyphsByLanguage: [(Language, [String])]) -> String {
    var out = """
    // Generated by tools/gen_cjk_font.swift; do not edit by hand.

    """

    for (lang, glyphs) in glyphsByLanguage {
        let index = Dictionary(uniqueKeysWithValues: glyphs.enumerated().map { ($0.element, $0.offset) })
        out += "static const char *const lang_\(lang.code)[STR_COUNT] = {\n"
        for (id, text) in lang.strings {
            out += "    [\(id)] = \(cStringExpr(text, glyphIndex: index)),\n"
        }
        out += "};\n\n"

        out += "static const char *const weekdays_\(lang.code)[7] = {\n"
        out += lang.weekdays.map { "    \(cStringExpr($0, glyphIndex: index))" }.joined(separator: ",\n")
        out += "\n};\n\n"

        out += "static const char *const months_\(lang.code)[12] = {\n"
        out += lang.months.map { "    \(cStringExpr($0, glyphIndex: index))" }.joined(separator: ",\n")
        out += "\n};\n\n"

        out += "static const char lang_name_\(lang.code)[] = \(cStringExpr(lang.nativeName, glyphIndex: index));\n\n"
        out += "static const char date_year_\(lang.code)[] = \(cStringExpr(lang.yearSuffix, glyphIndex: index));\n"
        out += "static const char date_day_\(lang.code)[] = \(cStringExpr(lang.daySuffix, glyphIndex: index));\n\n"
    }
    return out
}

var glyphsByLanguage: [(Language, [String])] = []
for lang in languages {
    validate(lang)
    let glyphs = collectGlyphs(lang)
    print("\(lang.code): \(glyphs.count) glyphs")
    glyphsByLanguage.append((lang, glyphs))
}

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
try generateFontC(glyphsByLanguage)
    .write(to: root.appendingPathComponent("main/cjk_font.c"), atomically: true, encoding: .utf8)
try generateI18nInc(glyphsByLanguage)
    .write(to: root.appendingPathComponent("main/cjk_i18n.inc"), atomically: true, encoding: .utf8)
