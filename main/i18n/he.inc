// Editable source of truth for the pre-shaped strings; never compiled.
// Editing REQUIRES a macOS font regeneration (swift tools/gen_fonts.swift);
// tools/check_i18n.py catches staleness via source hashes in fonts.c.
#if 0
// >>> shaped-source he
static const char *const lang_he[STR_COUNT] = {
    [STR_SETTINGS] = "הגדרות",
    [STR_WIFI] = "Wi-Fi",
    [STR_TIMEZONE] = "אזור זמן",
    [STR_BRIGHTNESS] = "בהירות",
    [STR_LED_BLINK] = "הבהוב LED",
    [STR_ROTATE] = "סיבוב 180",
    [STR_ABOUT] = "אודות",
    [STR_LANGUAGE] = "שפה",
    [STR_DONE] = "סיום",
    [STR_BACK] = "חזרה",
    [STR_CANCEL] = "ביטול",
    [STR_DEL] = "מחק",
    [STR_RUNNING] = "פועל",
    [STR_ON] = "פעיל",
    [STR_OFF] = "כבוי",
    [STR_WAITING_NTP] = "ממתין ל-NTP...",
    [STR_WIFI_SETUP] = "הגדרת Wi-Fi",
    [STR_SCANNING] = "סורק...",
    [STR_SCAN_FAILED] = "הסריקה נכשלה",
    [STR_TAP_RETRY] = "הקש לניסיון חוזר",
    [STR_SELECT_NETWORK] = "בחר רשת",
    [STR_NO_NETWORKS] = "לא נמצאו רשתות",
    [STR_NETWORK_LABEL] = "רשת:",
    [STR_ENTER_PASSWORD] = "הזן סיסמה",
    [STR_CONNECTING] = "מתחבר",
    [STR_CONNECTING_TO] = "מתחבר אל",
    [STR_CONNECTED] = "מחובר!",
    [STR_CONNECTION_FAILED] = "החיבור נכשל",
    [STR_KB_SPACE] = "רווח",
    [STR_KB_GO] = "OK",
    [STR_VERSION] = "גרסה:",
    [STR_STATUS] = "מצב:",
    [STR_READ] = "נקרא:",
    [STR_UPDATE] = "עדכן",
    [STR_RESTARTING] = "מפעיל מחדש",
    [STR_OTA_UPDATE] = "עדכון OTA",
    [STR_NTP_SERVER] = "שרת NTP",
    [STR_SERVER_LABEL] = "שרת:",
    [STR_NTP_SETTINGS] = "הגדרות NTP",
    [STR_NTP_PRESETS] = "פרופילי NTP",
    [STR_PRESETS] = "פרופילים",
    [STR_NONE] = "אין",
    [STR_SELECT_REGION] = "בחר אזור",
    [STR_DST] = "שעון קיץ",
    [STR_NO_DST] = "ללא שעון קיץ",
};
static const char *const weekdays_he[7] = {
    "יום א׳", "יום ב׳", "יום ג׳", "יום ד׳", "יום ה׳", "יום ו׳", "שבת",
};
static const char *const months_he[12] = {
    "ינו", "פבר", "מרץ", "אפר", "מאי", "יונ",
    "יול", "אוג", "ספט", "אוק", "נוב", "דצמ",
};
static const char lang_name_he[] = "עברית";
// <<< shaped-source he
#endif
