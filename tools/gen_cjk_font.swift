import AppKit
import Foundation

struct Language {
    let code: String
    let enumName: String
    let fontObject: String
    let glyphArray: String
    let fontName: String
    let glyphWidth: Int
    let fontSize16: CGFloat
    let fontSize32: CGFloat
    let generateAsciiLetters: Bool
    let generateAsciiDigits: Bool
    let nativeName: String
    let strings: [(String, String)]
    let weekdays: [String]
    let months: [String]
    let yearSuffix: String
    let daySuffix: String
    // Extra upward shift, in 16px-cell pixels, applied after line-box
    // centering so a font whose line box rounds differently from the
    // built-in 8x16 font can sit on the same baseline. Doubled for the
    // 32px cell.
    var yNudge: CGFloat = 0
    // Alpha shaping exponent for the 4-bit antialiased coverage. Blending
    // happens in gamma-encoded RGB565 on the device, which makes linear
    // coverage read thin; values below 1.0 fatten the edge ramp.
    var aaGamma: CGFloat = 0.7
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
    glyphWidth: 16,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
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
    glyphWidth: 16,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
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
    glyphWidth: 16,
    fontSize16: 13,
    fontSize32: 28,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
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
    glyphWidth: 16,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
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
        ("STR_FMT_OFF_DRIFT", "오차 %s %s 드리프트 %s"),
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

let el = Language(
    code: "el",
    enumName: "LANG_EL",
    fontObject: "font_el",
    glyphArray: "el_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 10,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: true,
    nativeName: "Ελληνικά",
    strings: [
        ("STR_SETTINGS", "Ρυθμίσεις"),
        ("STR_TIMEZONE", "Ζώνη ώρας"),
        ("STR_BRIGHTNESS", "Φωτεινότητα"),
        ("STR_LED_BLINK", "LED αναβ."),
        ("STR_ROTATE", "Στροφή 180"),
        ("STR_ABOUT", "Σχετικά"),
        ("STR_LANGUAGE", "Γλώσσα"),
        ("STR_DONE", "Τέλος"),
        ("STR_BACK", "Πίσω"),
        ("STR_CANCEL", "Άκυρο"),
        ("STR_DEL", "Διαγρ."),
        ("STR_RUNNING", "Εκτέλεση"),
        ("STR_ON", "Ενεργό"),
        ("STR_OFF", "Ανενεργό"),
        ("STR_NTS_NO", "Όχι"),
        ("STR_NTS_ATTEMPT", "Δοκιμή"),
        ("STR_NTS_REQUIRE", "Απαίτηση"),
        ("STR_WAITING_NTP", "Αναμονή NTP..."),
        ("STR_FMT_SYNCED", "Συγχρ.: %s"),
        ("STR_FMT_SYNCING", "Συγχρονισμός: %s"),
        ("STR_FMT_WAITING", "Αναμονή: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  χωρίς συγχρ."),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  πριν %s"),
        ("STR_FMT_OFF_DRIFT", "απόκλ. %s %s drift %s"),
        ("STR_WIFI_SETUP", "Ρύθμιση WiFi"),
        ("STR_SCANNING", "Σάρωση..."),
        ("STR_SCAN_FAILED", "Αποτυχία σάρωσης"),
        ("STR_TAP_RETRY", "Πατήστε ξανά"),
        ("STR_SELECT_NETWORK", "Επιλογή δικτύου"),
        ("STR_NO_NETWORKS", "Δεν βρέθηκαν δίκτυα"),
        ("STR_NETWORK_LABEL", "Δίκτυο:"),
        ("STR_ENTER_PASSWORD", "Κωδικός"),
        ("STR_CONNECTING", "Σύνδεση"),
        ("STR_CONNECTING_TO", "Σύνδεση σε"),
        ("STR_CONNECTED", "Συνδέθηκε!"),
        ("STR_CONNECTION_FAILED", "Αποτυχία σύνδεσης"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Κενό"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Έκδοση:"),
        ("STR_FIRMWARE_URL", "URL firmware:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Κατάσταση:"),
        ("STR_READ", "Ανάγν.:"),
        ("STR_UPDATE", "Ενημέρ."),
        ("STR_RESTARTING", "Επανεκκίνηση"),
        ("STR_OTA_UPDATE", "OTA ενημέρωση"),
        ("STR_NTP_SERVER", "Διακομιστής NTP"),
        ("STR_SERVER_LABEL", "Διακομιστής:"),
        ("STR_NTP_SETTINGS", "Ρυθμίσεις NTP"),
        ("STR_NTP_PRESETS", "Προφίλ NTP"),
        ("STR_PRESETS", "Προφίλ"),
        ("STR_BENCHMARK", "Δοκιμή"),
        ("STR_STOPPING", "Διακοπή"),
        ("STR_FAIL", "αποτυχία"),
        ("STR_NTP_STATS", "Στατιστικά NTP"),
        ("STR_UNSYNCED", "ασύγχρ."),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        // Latin like the other stat labels here (Stratum/Poll/Drift): the
        // Greek gloss is 90px and pushes the syncs count off the panel.
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Ηλικία: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "κανένα"),
        ("STR_SELECT_REGION", "Επιλογή περιοχής"),
    ],
    weekdays: ["Κυρ", "Δευ", "Τρι", "Τετ", "Πεμ", "Παρ", "Σαβ"],
    months: ["Ιαν","Φεβ","Μαρ","Απρ","Μαϊ","Ιουν","Ιουλ","Αυγ","Σεπ","Οκτ","Νοε","Δεκ"],
    yearSuffix: "",
    daySuffix: "",
    yNudge: 1
)

let sk = Language(
    code: "sk",
    enumName: "LANG_SK",
    fontObject: "font_sk",
    glyphArray: "sk_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 0,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
    nativeName: "Slovenčina",
    strings: [
        ("STR_SETTINGS", "Nastavenia"),
        ("STR_TIMEZONE", "Časové pásmo"),
        ("STR_BRIGHTNESS", "Jas"),
        ("STR_LED_BLINK", "Blikanie LED"),
        ("STR_ROTATE", "Otočiť 180"),
        ("STR_ABOUT", "O zariadení"),
        ("STR_LANGUAGE", "Jazyk"),
        ("STR_DONE", "Hotovo"),
        ("STR_BACK", "Späť"),
        ("STR_CANCEL", "Zrušiť"),
        ("STR_DEL", "Zmazať"),
        ("STR_RUNNING", "Beží"),
        ("STR_ON", "Zap."),
        ("STR_OFF", "Vyp."),
        ("STR_NTS_NO", "Nie"),
        ("STR_NTS_ATTEMPT", "Skúsiť"),
        ("STR_NTS_REQUIRE", "Vyžad."),
        ("STR_WAITING_NTP", "Čaká sa na NTP..."),
        ("STR_FMT_SYNCED", "Synchroniz.: %s"),
        ("STR_FMT_SYNCING", "Synchronizuje: %s"),
        ("STR_FMT_WAITING", "Čakanie: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  bez sync."),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  pred %s"),
        ("STR_FMT_OFF_DRIFT", "odch. %s %s drift %s"),
        ("STR_WIFI_SETUP", "Nast. WiFi"),
        ("STR_SCANNING", "Skenujem..."),
        ("STR_SCAN_FAILED", "Sken zlyhal"),
        ("STR_TAP_RETRY", "Ťuknite znovu"),
        ("STR_SELECT_NETWORK", "Vyberte sieť"),
        ("STR_NO_NETWORKS", "Siete nenájdené"),
        ("STR_NETWORK_LABEL", "Sieť:"),
        ("STR_ENTER_PASSWORD", "Zadajte heslo"),
        ("STR_CONNECTING", "Pripájam"),
        ("STR_CONNECTING_TO", "Pripájam k"),
        ("STR_CONNECTED", "Pripojené!"),
        ("STR_CONNECTION_FAILED", "Pripojenie zlyhalo"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Medzera"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Verzia:"),
        ("STR_FIRMWARE_URL", "URL firmvéru:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Stav:"),
        ("STR_READ", "Čítané:"),
        ("STR_UPDATE", "Aktualizovať"),
        ("STR_RESTARTING", "Reštartujem"),
        ("STR_OTA_UPDATE", "OTA aktualizácia"),
        ("STR_NTP_SERVER", "Server NTP"),
        ("STR_SERVER_LABEL", "Server:"),
        ("STR_NTP_SETTINGS", "Nastavenia NTP"),
        ("STR_NTP_PRESETS", "Profily NTP"),
        ("STR_PRESETS", "Profily"),
        ("STR_BENCHMARK", "Test"),
        ("STR_STOPPING", "Zastavujem"),
        ("STR_FAIL", "chyba"),
        ("STR_NTP_STATS", "Štatistiky NTP"),
        ("STR_UNSYNCED", "bez sync."),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Vek: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "žiadne"),
        ("STR_SELECT_REGION", "Vyberte región"),
    ],
    weekdays: ["Ne", "Po", "Ut", "St", "Št", "Pi", "So"],
    months: ["Jan","Feb","Mar","Apr","Máj","Jún","Júl","Aug","Sep","Okt","Nov","Dec"],
    yearSuffix: "",
    daySuffix: ""
)

let sl = Language(
    code: "sl",
    enumName: "LANG_SL",
    fontObject: "font_sl",
    glyphArray: "sl_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 0,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
    nativeName: "Slovenščina",
    strings: [
        ("STR_SETTINGS", "Nastavitve"),
        ("STR_TIMEZONE", "Časovni pas"),
        ("STR_BRIGHTNESS", "Svetlost"),
        ("STR_LED_BLINK", "Utrip LED"),
        ("STR_ROTATE", "Zasuk 180"),
        ("STR_ABOUT", "O napravi"),
        ("STR_LANGUAGE", "Jezik"),
        ("STR_DONE", "Končano"),
        ("STR_BACK", "Nazaj"),
        ("STR_CANCEL", "Prekliči"),
        ("STR_DEL", "Izbriši"),
        ("STR_RUNNING", "Teče"),
        ("STR_ON", "Vklop"),
        ("STR_OFF", "Izklop"),
        ("STR_NTS_NO", "Ne"),
        ("STR_NTS_ATTEMPT", "Poskusi"),
        ("STR_NTS_REQUIRE", "Zahtevaj"),
        ("STR_WAITING_NTP", "Čakanje na NTP..."),
        ("STR_FMT_SYNCED", "Sinhr.: %s"),
        ("STR_FMT_SYNCING", "Sinhroniziram: %s"),
        ("STR_FMT_WAITING", "Čakanje: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  brez sync."),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  pred %s"),
        ("STR_FMT_OFF_DRIFT", "odmik %s %s drift %s"),
        ("STR_WIFI_SETUP", "Nast. WiFi"),
        ("STR_SCANNING", "Iskanje..."),
        ("STR_SCAN_FAILED", "Iskanje ni uspelo"),
        ("STR_TAP_RETRY", "Tapnite znova"),
        ("STR_SELECT_NETWORK", "Izberite omrežje"),
        ("STR_NO_NETWORKS", "Ni omrežij"),
        ("STR_NETWORK_LABEL", "Omrežje:"),
        ("STR_ENTER_PASSWORD", "Vnesite geslo"),
        ("STR_CONNECTING", "Povezujem"),
        ("STR_CONNECTING_TO", "Povezujem z"),
        ("STR_CONNECTED", "Povezano!"),
        ("STR_CONNECTION_FAILED", "Povezava ni uspela"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Presledek"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Različica:"),
        ("STR_FIRMWARE_URL", "URL firmware:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Stanje:"),
        ("STR_READ", "Prebrano:"),
        ("STR_UPDATE", "Posodobi"),
        ("STR_RESTARTING", "Ponovni zagon"),
        ("STR_OTA_UPDATE", "OTA posodobitev"),
        ("STR_NTP_SERVER", "Strežnik NTP"),
        ("STR_SERVER_LABEL", "Strežnik:"),
        ("STR_NTP_SETTINGS", "Nastavitve NTP"),
        ("STR_NTP_PRESETS", "Profili NTP"),
        ("STR_PRESETS", "Profili"),
        ("STR_BENCHMARK", "Test"),
        ("STR_STOPPING", "Ustavljam"),
        ("STR_FAIL", "napaka"),
        ("STR_NTP_STATS", "Statistika NTP"),
        ("STR_UNSYNCED", "brez sync."),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Starost: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "brez"),
        ("STR_SELECT_REGION", "Izberite regijo"),
    ],
    weekdays: ["Ned", "Pon", "Tor", "Sre", "Čet", "Pet", "Sob"],
    months: ["Jan","Feb","Mar","Apr","Maj","Jun","Jul","Avg","Sep","Okt","Nov","Dec"],
    yearSuffix: "",
    daySuffix: ""
)

let lv = Language(
    code: "lv",
    enumName: "LANG_LV",
    fontObject: "font_lv",
    glyphArray: "lv_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 0,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
    nativeName: "Latviešu",
    strings: [
        ("STR_SETTINGS", "Iestatījumi"),
        ("STR_TIMEZONE", "Laika josla"),
        ("STR_BRIGHTNESS", "Spilgtums"),
        ("STR_LED_BLINK", "LED mirgošana"),
        ("STR_ROTATE", "Pagriezt 180"),
        ("STR_ABOUT", "Par"),
        ("STR_LANGUAGE", "Valoda"),
        ("STR_DONE", "Gatavs"),
        ("STR_BACK", "Atpakaļ"),
        ("STR_CANCEL", "Atcelt"),
        ("STR_DEL", "Dzēst"),
        ("STR_RUNNING", "Darbojas"),
        ("STR_ON", "Iesl."),
        ("STR_OFF", "Izsl."),
        ("STR_NTS_NO", "Nē"),
        ("STR_NTS_ATTEMPT", "Mēģināt"),
        ("STR_NTS_REQUIRE", "Pieprasīt"),
        ("STR_WAITING_NTP", "Gaida NTP..."),
        ("STR_FMT_SYNCED", "Sinhr.: %s"),
        ("STR_FMT_SYNCING", "Sinhronizē: %s"),
        ("STR_FMT_WAITING", "Gaida: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  nav sync."),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  pirms %s"),
        ("STR_FMT_OFF_DRIFT", "nobīde %s %s drift %s"),
        ("STR_WIFI_SETUP", "WiFi iestat."),
        ("STR_SCANNING", "Skenē..."),
        ("STR_SCAN_FAILED", "Skenēšana neizdevās"),
        ("STR_TAP_RETRY", "Pieskarieties vēlreiz"),
        ("STR_SELECT_NETWORK", "Izvēlieties tīklu"),
        ("STR_NO_NETWORKS", "Tīkli nav atrasti"),
        ("STR_NETWORK_LABEL", "Tīkls:"),
        ("STR_ENTER_PASSWORD", "Ievadiet paroli"),
        ("STR_CONNECTING", "Savieno"),
        ("STR_CONNECTING_TO", "Savieno ar"),
        ("STR_CONNECTED", "Savienots!"),
        ("STR_CONNECTION_FAILED", "Savienojums neizdevās"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Atstarpe"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Versija:"),
        ("STR_FIRMWARE_URL", "Firmware URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Statuss:"),
        ("STR_READ", "Lasīts:"),
        ("STR_UPDATE", "Atjaunināt"),
        ("STR_RESTARTING", "Restartē"),
        ("STR_OTA_UPDATE", "OTA atjaun."),
        ("STR_NTP_SERVER", "NTP serveris"),
        ("STR_SERVER_LABEL", "Serveris:"),
        ("STR_NTP_SETTINGS", "NTP iestatījumi"),
        ("STR_NTP_PRESETS", "NTP profili"),
        ("STR_PRESETS", "Profili"),
        ("STR_BENCHMARK", "Tests"),
        ("STR_STOPPING", "Aptur"),
        ("STR_FAIL", "kļūda"),
        ("STR_NTP_STATS", "NTP statistika"),
        ("STR_UNSYNCED", "nav sync."),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Vecums: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "nav"),
        ("STR_SELECT_REGION", "Izvēlieties reģionu"),
    ],
    weekdays: ["Sv", "Pr", "Ot", "Tr", "Ce", "Pk", "Se"],
    months: ["Jan","Feb","Mar","Apr","Mai","Jūn","Jūl","Aug","Sep","Okt","Nov","Dec"],
    yearSuffix: "",
    daySuffix: ""
)

let lt = Language(
    code: "lt",
    enumName: "LANG_LT",
    fontObject: "font_lt",
    glyphArray: "lt_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 0,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
    nativeName: "Lietuvių",
    strings: [
        ("STR_SETTINGS", "Nustatymai"),
        ("STR_TIMEZONE", "Laiko juosta"),
        ("STR_BRIGHTNESS", "Ryškumas"),
        ("STR_LED_BLINK", "LED mirksėjimas"),
        ("STR_ROTATE", "Pasukti 180"),
        ("STR_ABOUT", "Apie"),
        ("STR_LANGUAGE", "Kalba"),
        ("STR_DONE", "Atlikta"),
        ("STR_BACK", "Atgal"),
        ("STR_CANCEL", "Atšaukti"),
        ("STR_DEL", "Trinti"),
        ("STR_RUNNING", "Veikia"),
        ("STR_ON", "Įjungta"),
        ("STR_OFF", "Išjungta"),
        ("STR_NTS_NO", "Ne"),
        ("STR_NTS_ATTEMPT", "Bandyti"),
        ("STR_NTS_REQUIRE", "Reikalauti"),
        ("STR_WAITING_NTP", "Laukiama NTP..."),
        ("STR_FMT_SYNCED", "Sinchr.: %s"),
        ("STR_FMT_SYNCING", "Sinchroniz.: %s"),
        ("STR_FMT_WAITING", "Laukiama: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  nėra sync."),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  prieš %s"),
        ("STR_FMT_OFF_DRIFT", "nuokr. %s %s drift %s"),
        ("STR_WIFI_SETUP", "WiFi nust."),
        ("STR_SCANNING", "Ieškoma..."),
        ("STR_SCAN_FAILED", "Paieška nepavyko"),
        ("STR_TAP_RETRY", "Bakstelėkite dar"),
        ("STR_SELECT_NETWORK", "Pasirinkite tinklą"),
        ("STR_NO_NETWORKS", "Tinklų nerasta"),
        ("STR_NETWORK_LABEL", "Tinklas:"),
        ("STR_ENTER_PASSWORD", "Įveskite slaptaž."),
        ("STR_CONNECTING", "Jungiama"),
        ("STR_CONNECTING_TO", "Jungiama prie"),
        ("STR_CONNECTED", "Prisijungta!"),
        ("STR_CONNECTION_FAILED", "Prisijungti nepavyko"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Tarpas"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Versija:"),
        ("STR_FIRMWARE_URL", "Firmware URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Būsena:"),
        ("STR_READ", "Skaityta:"),
        ("STR_UPDATE", "Atnaujinti"),
        ("STR_RESTARTING", "Paleidžiama iš naujo"),
        ("STR_OTA_UPDATE", "OTA naujinimas"),
        ("STR_NTP_SERVER", "NTP serveris"),
        ("STR_SERVER_LABEL", "Serveris:"),
        ("STR_NTP_SETTINGS", "NTP nustatymai"),
        ("STR_NTP_PRESETS", "NTP profiliai"),
        ("STR_PRESETS", "Profiliai"),
        ("STR_BENCHMARK", "Testas"),
        ("STR_STOPPING", "Stabdoma"),
        ("STR_FAIL", "klaida"),
        ("STR_NTP_STATS", "NTP statistika"),
        ("STR_UNSYNCED", "nėra sync."),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Amžius: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "nėra"),
        ("STR_SELECT_REGION", "Pasirinkite regioną"),
    ],
    weekdays: ["Sk", "Pr", "An", "Tr", "Kt", "Pn", "Št"],
    months: ["Sau","Vas","Kov","Bal","Geg","Bir","Lie","Rgp","Rgs","Spa","Lap","Grd"],
    yearSuffix: "",
    daySuffix: ""
)

let et = Language(
    code: "et",
    enumName: "LANG_ET",
    fontObject: "font_et",
    glyphArray: "et_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 0,
    fontSize16: 14,
    fontSize32: 29,
    generateAsciiLetters: false,
    generateAsciiDigits: false,
    nativeName: "Eesti",
    strings: [
        ("STR_SETTINGS", "Seaded"),
        ("STR_TIMEZONE", "Ajavöönd"),
        ("STR_BRIGHTNESS", "Heledus"),
        ("STR_LED_BLINK", "LED vilkumine"),
        ("STR_ROTATE", "Pööra 180"),
        ("STR_ABOUT", "Teave"),
        ("STR_LANGUAGE", "Keel"),
        ("STR_DONE", "Valmis"),
        ("STR_BACK", "Tagasi"),
        ("STR_CANCEL", "Tühista"),
        ("STR_DEL", "Kustuta"),
        ("STR_RUNNING", "Töötab"),
        ("STR_ON", "Sees"),
        ("STR_OFF", "Väljas"),
        ("STR_NTS_NO", "Ei"),
        ("STR_NTS_ATTEMPT", "Proovi"),
        ("STR_NTS_REQUIRE", "Nõua"),
        ("STR_WAITING_NTP", "NTP ootamine..."),
        ("STR_FMT_SYNCED", "Sünk.: %s"),
        ("STR_FMT_SYNCING", "Sünkroonib: %s"),
        ("STR_FMT_WAITING", "Ootamine: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d peers  poll %s  sync puudub"),
        ("STR_FMT_PEERS_AGO", "%d/%d peers  poll %s  %s tagasi"),
        ("STR_FMT_OFF_DRIFT", "nihe %s %s drift %s"),
        ("STR_WIFI_SETUP", "WiFi seaded"),
        ("STR_SCANNING", "Skannimine..."),
        ("STR_SCAN_FAILED", "Skannimine nurjus"),
        ("STR_TAP_RETRY", "Puuduta uuesti"),
        ("STR_SELECT_NETWORK", "Vali võrk"),
        ("STR_NO_NETWORKS", "Võrke ei leitud"),
        ("STR_NETWORK_LABEL", "Võrk:"),
        ("STR_ENTER_PASSWORD", "Sisesta parool"),
        ("STR_CONNECTING", "Ühendamine"),
        ("STR_CONNECTING_TO", "Ühendus"),
        ("STR_CONNECTED", "Ühendatud!"),
        ("STR_CONNECTION_FAILED", "Ühendus nurjus"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Tühik"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Versioon:"),
        ("STR_FIRMWARE_URL", "Firmware URL:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Olek:"),
        ("STR_READ", "Loetud:"),
        ("STR_UPDATE", "Uuenda"),
        ("STR_RESTARTING", "Taaskäivitus"),
        ("STR_OTA_UPDATE", "OTA uuendus"),
        ("STR_NTP_SERVER", "NTP server"),
        ("STR_SERVER_LABEL", "Server:"),
        ("STR_NTP_SETTINGS", "NTP seaded"),
        ("STR_NTP_PRESETS", "NTP profiilid"),
        ("STR_PRESETS", "Profiilid"),
        ("STR_BENCHMARK", "Test"),
        ("STR_STOPPING", "Peatamine"),
        ("STR_FAIL", "viga"),
        ("STR_NTP_STATS", "NTP statistika"),
        ("STR_UNSYNCED", "sync puudub"),
        ("STR_STRATUM_LABEL", "Stratum: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Sync.: "),
        ("STR_DRIFT_LABEL", "Drift: "),
        ("STR_AGE_LABEL", "  Vanus: "),
        ("STR_OFFSET_LABEL", "Offset: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Root: "),
        ("STR_FMT_ROOT_DETAIL", "%s delay, %s disp"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "puudub"),
        ("STR_SELECT_REGION", "Vali piirkond"),
    ],
    weekdays: ["P", "E", "T", "K", "N", "R", "L"],
    months: ["Jaan","Veebr","Mär","Apr","Mai","Juuni","Juuli","Aug","Sept","Okt","Nov","Dets"],
    yearSuffix: "",
    daySuffix: ""
)

let vi = Language(
    code: "vi",
    enumName: "LANG_VI",
    fontObject: "font_vi",
    glyphArray: "vi_glyphs",
    fontName: "Helvetica Neue",
    glyphWidth: 8,
    fontSize16: 12,
    fontSize32: 27,
    generateAsciiLetters: true,
    generateAsciiDigits: true,
    nativeName: "Tiếng Việt",
    strings: [
        ("STR_SETTINGS", "Cài đặt"),
        ("STR_TIMEZONE", "Múi giờ"),
        ("STR_BRIGHTNESS", "Độ sáng"),
        ("STR_LED_BLINK", "LED nhấp nháy"),
        ("STR_ROTATE", "Xoay 180"),
        ("STR_ABOUT", "Giới thiệu"),
        ("STR_LANGUAGE", "Ngôn ngữ"),
        ("STR_DONE", "Xong"),
        ("STR_BACK", "Quay lại"),
        ("STR_CANCEL", "Hủy"),
        ("STR_DEL", "Xóa"),
        ("STR_RUNNING", "Đang chạy"),
        ("STR_ON", "Bật"),
        ("STR_OFF", "Tắt"),
        ("STR_NTS_NO", "Không"),
        ("STR_NTS_ATTEMPT", "Thử"),
        ("STR_NTS_REQUIRE", "Bắt buộc"),
        ("STR_WAITING_NTP", "Đang chờ NTP..."),
        ("STR_FMT_SYNCED", "Đã đồng bộ: %s"),
        ("STR_FMT_SYNCING", "Đang đồng bộ: %s"),
        ("STR_FMT_WAITING", "Đang chờ: %lus"),
        ("STR_FMT_PEERS_NOSYNC", "%d/%d nguồn  poll %s  chưa đồng bộ"),
        ("STR_FMT_PEERS_AGO", "%d/%d nguồn  poll %s  %s trước"),
        ("STR_FMT_OFF_DRIFT", "lệch %s %s trôi %s"),
        ("STR_WIFI_SETUP", "Cài đặt WiFi"),
        ("STR_SCANNING", "Đang quét..."),
        ("STR_SCAN_FAILED", "Quét thất bại"),
        ("STR_TAP_RETRY", "Chạm để thử lại"),
        ("STR_SELECT_NETWORK", "Chọn mạng"),
        ("STR_NO_NETWORKS", "Không tìm thấy mạng"),
        ("STR_NETWORK_LABEL", "Mạng:"),
        ("STR_ENTER_PASSWORD", "Nhập mật khẩu"),
        ("STR_CONNECTING", "Đang kết nối"),
        ("STR_CONNECTING_TO", "Kết nối tới"),
        ("STR_CONNECTED", "Đã kết nối!"),
        ("STR_CONNECTION_FAILED", "Kết nối thất bại"),
        ("STR_KB_SHIFT", "Shift"),
        ("STR_KB_SPACE", "Dấu cách"),
        ("STR_KB_GO", "OK"),
        ("STR_VERSION", "Phiên bản:"),
        ("STR_FIRMWARE_URL", "URL firmware:"),
        ("STR_OTA_URL", "OTA URL"),
        ("STR_STATUS", "Trạng thái:"),
        ("STR_READ", "Đã đọc:"),
        ("STR_UPDATE", "Cập nhật"),
        ("STR_RESTARTING", "Đang khởi động lại"),
        ("STR_OTA_UPDATE", "Cập nhật OTA"),
        ("STR_NTP_SERVER", "Máy chủ NTP"),
        ("STR_SERVER_LABEL", "Máy chủ:"),
        ("STR_NTP_SETTINGS", "Cài đặt NTP"),
        ("STR_NTP_PRESETS", "Mẫu NTP"),
        ("STR_PRESETS", "Mẫu"),
        ("STR_BENCHMARK", "Kiểm tra"),
        ("STR_STOPPING", "Đang dừng"),
        ("STR_FAIL", "lỗi"),
        ("STR_NTP_STATS", "Thống kê NTP"),
        ("STR_UNSYNCED", "chưa ĐB"),
        ("STR_STRATUM_LABEL", "Tầng: "),
        ("STR_POLL_LABEL", "  Poll: "),
        ("STR_SYNCS_LABEL", "  Lần: "),
        ("STR_DRIFT_LABEL", "Trôi: "),
        ("STR_AGE_LABEL", "  Tuổi: "),
        ("STR_OFFSET_LABEL", "Lệch: "),
        ("STR_JITTER_LABEL", "  Jitter: "),
        ("STR_ROOT_LABEL", "Gốc: "),
        ("STR_FMT_ROOT_DETAIL", "%s trễ, %s phân tán"),
        ("STR_PEER_HEADER", "Peer            R  Offset  Delay Jitter"),
        ("STR_NONE", "không có"),
        ("STR_SELECT_REGION", "Chọn khu vực"),
    ],
    weekdays: ["CN", "T2", "T3", "T4", "T5", "T6", "T7"],
    months: ["thg 1","thg 2","thg 3","thg 4","thg 5","thg 6","thg 7","thg 8","thg 9","thg 10","thg 11","thg 12"],
    yearSuffix: "",
    daySuffix: ""
)

let languages = [ja, zh, zhHant, ko, el, sk, sl, lv, lt, et, vi]

func isAscii(_ ch: Character) -> Bool {
    guard ch.unicodeScalars.count == 1, let scalar = ch.unicodeScalars.first else { return false }
    return scalar.value >= 0x20 && scalar.value <= 0x7e
}

func isAsciiLetter(_ ch: Character) -> Bool {
    guard ch.unicodeScalars.count == 1, let scalar = ch.unicodeScalars.first else { return false }
    return (scalar.value >= 0x41 && scalar.value <= 0x5a)
        || (scalar.value >= 0x61 && scalar.value <= 0x7a)
}

func isAsciiDigit(_ ch: Character) -> Bool {
    guard ch.unicodeScalars.count == 1, let scalar = ch.unicodeScalars.first else { return false }
    return scalar.value >= 0x30 && scalar.value <= 0x39
}

func shouldGenerateAscii(_ ch: Character, lang: Language) -> Bool {
    return (lang.generateAsciiLetters && isAsciiLetter(ch))
        || (lang.generateAsciiDigits && isAsciiDigit(ch))
}

func printfSpanEnd(_ chars: [Character], from start: Int) -> Int {
    var index = start + 1
    if index < chars.count && chars[index] == "%" {
        return index + 1
    }

    let specifiers = Set("diuoxXfFeEgGaAcspn@")
    let modifiers = Set("0123456789.*+-# 'lhLqjzt")
    while index < chars.count {
        if specifiers.contains(chars[index]) {
            return index + 1
        }
        if !modifiers.contains(chars[index]) {
            break
        }
        index += 1
    }

    return start + 1
}

func collectGlyphs(_ lang: Language) -> [String] {
    if lang.glyphWidth == 0 { return [] }
    var glyphs: [String] = []
    var seen = Set<String>()
    let values = lang.strings.map { $0.1 } + lang.weekdays + lang.months
        + [lang.nativeName, lang.yearSuffix, lang.daySuffix]
    for value in values {
        let chars = Array(value)
        var index = 0
        while index < chars.count {
            let ch = chars[index]
            if ch == "%" {
                index = printfSpanEnd(chars, from: index)
                continue
            }
            if isAscii(ch) && !shouldGenerateAscii(ch, lang: lang) {
                index += 1
                continue
            }
            let s = String(ch)
            if !seen.contains(s) {
                seen.insert(s)
                glyphs.append(s)
            }
            index += 1
        }
    }
    if lang.generateAsciiDigits {
        for digit in "0123456789" {
            let s = String(digit)
            if !seen.contains(s) {
                seen.insert(s)
                glyphs.append(s)
            }
        }
    }
    // Token payloads are emitted as one byte of 0x80 + index, so only 128
    // glyphs are addressable before the payload overflows the \xNN escape.
    if glyphs.count > 128 {
        fatalError("\(lang.code) uses \(glyphs.count) glyphs; token encoding supports 128")
    }
    return glyphs
}

let latinByteMap: [Character: UInt8] = [
    "á": 0xE1, "ä": 0xE4, "é": 0xE9, "í": 0xED, "ó": 0xF3, "ú": 0xFA,
    "ą": 0x80, "č": 0x86, "š": 0x89, "ť": 0x8B, "ž": 0x8A,
    "Č": 0x8F, "Š": 0xD5, "Ť": 0xD6,
    "ā": 0xD7, "ē": 0xD8, "ģ": 0xD9, "ī": 0xDB, "ļ": 0xDD,
    "ū": 0xDE, "ė": 0xDF, "Į": 0xF0, "ų": 0xF2, "Ü": 0xDC,
    "õ": 0xF5, "ö": 0xF6, "ü": 0xFC,
]

func render(_ ch: String, lang: Language, pixels: Int) -> [UInt8] {
    let outputWidth = pixels
    let renderWidth = pixels == 16 ? lang.glyphWidth : lang.glyphWidth * 2
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

    let fontSize = pixels == 16 ? lang.fontSize16 : lang.fontSize32
    guard let font = NSFont(name: lang.fontName, size: fontSize) else {
        // Falling back to another font would silently regenerate every
        // table with different metrics.
        fatalError("font \(lang.fontName) is not installed")
    }
    let s = ch as NSString
    let attrs: [NSAttributedString.Key: Any] = [
        .font: font,
        .foregroundColor: NSColor.white,
    ]
    let size = s.size(withAttributes: attrs)
    let y = floor((CGFloat(pixels) - size.height) / 2.0)
        + (pixels == 16 ? lang.yNudge : lang.yNudge * 2)
    if size.width > CGFloat(renderWidth) {
        // Condense a glyph wider than its cell instead of clipping its
        // outer strokes (Helvetica Neue W/M/m overflow the 8px cell).
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
                level = min(15, Int((pow(lum, lang.aaGamma) * 15.0).rounded()))
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

func cStringExpr(_ value: String, lang: Language, glyphIndex: [String: Int]) -> String {
    var parts: [String] = []
    var ascii = ""

    func flushAscii() {
        if !ascii.isEmpty {
            parts.append("\"\(cEscapeAscii(ascii))\"")
            ascii = ""
        }
    }

    let chars = Array(value)
    var index = 0
    while index < chars.count {
        let ch = chars[index]
        if ch == "%" {
            let end = printfSpanEnd(chars, from: index)
            ascii += String(chars[index..<end])
            index = end
            continue
        }
        if isAscii(ch) && !shouldGenerateAscii(ch, lang: lang) {
            ascii.append(ch)
        } else if lang.glyphWidth == 0 {
            flushAscii()
            guard let byte = latinByteMap[ch] else {
                fatalError("\(lang.code) has no byte mapping for \(ch)")
            }
            parts.append(String(format: "\"\\x%02X\"", byte))
        } else {
            flushAscii()
            let id = glyphIndex[String(ch)]! + 0x80
            parts.append(String(format: "\"\\x1E\"\"\\x%02X\"", id))
        }
        index += 1
    }
    flushAscii()
    if parts.isEmpty { return "\"\"" }
    if parts.count == 1 { return parts[0] }
    // Parenthesize concatenations so hex escapes cannot swallow a following
    // letter and clang's -Wstring-concatenation stays quiet in initializer
    // lists.
    return "(" + parts.joined(separator: " ") + ")"
}

func validate(_ lang: Language) {
    let got = Set(lang.strings.map { $0.0 })
    if lang.strings.count != got.count {
        // A duplicated pair would emit two designated initializers for the
        // same slot and silently keep only the last one.
        fatalError("\(lang.code) has duplicate string ids")
    }
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
        if lang.glyphWidth == 0 { continue }
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
            .glyph_width = \(lang.glyphWidth),
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
            out += "    [\(id)] = \(cStringExpr(text, lang: lang, glyphIndex: index)),\n"
        }
        out += "};\n\n"

        out += "static const char *const weekdays_\(lang.code)[7] = {\n"
        out += lang.weekdays.map { "    \(cStringExpr($0, lang: lang, glyphIndex: index))" }.joined(separator: ",\n")
        out += "\n};\n\n"

        out += "static const char *const months_\(lang.code)[12] = {\n"
        out += lang.months.map { "    \(cStringExpr($0, lang: lang, glyphIndex: index))" }.joined(separator: ",\n")
        out += "\n};\n\n"

        out += "static const char lang_name_\(lang.code)[] = \(cStringExpr(lang.nativeName, lang: lang, glyphIndex: index));\n\n"
        if !lang.yearSuffix.isEmpty || !lang.daySuffix.isEmpty {
            out += "static const char date_year_\(lang.code)[] = \(cStringExpr(lang.yearSuffix, lang: lang, glyphIndex: index));\n"
            out += "static const char date_day_\(lang.code)[] = \(cStringExpr(lang.daySuffix, lang: lang, glyphIndex: index));\n\n"
        }

        if lang.generateAsciiDigits {
            out += "static const char *const digits_\(lang.code)[10] = {\n"
            out += (0...9).map {
                "    \(cStringExpr(String($0), lang: lang, glyphIndex: index))"
            }.joined(separator: ",\n")
            out += "\n};\n\n"
        }
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
