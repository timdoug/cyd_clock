// Editable source of truth for the pre-shaped strings; never compiled.
// Editing REQUIRES a macOS font regeneration (swift tools/gen_fonts.swift);
// tools/check_i18n.py catches staleness via source hashes in fonts.c.
#if 0
// >>> shaped-source bho
static const char *const lang_bho[STR_COUNT] = {
    [STR_WIFI] = "Wi-Fi",
    [STR_SETTINGS] = "सेटिंग",
    [STR_FAIL] = "बिफल",
    [STR_TIMEZONE] = "समय क्षेत्र",
    [STR_BRIGHTNESS] = "चमक",
    [STR_LED_BLINK] = "LED झलक",
    [STR_ROTATE] = "180 घुमाईं",
    [STR_ABOUT] = "बारे में",
    [STR_LANGUAGE] = "भाषा",
    [STR_DONE] = "भ गइल",
    [STR_BACK] = "पाछे",
    [STR_CANCEL] = "रद्द",
    [STR_DEL] = "मेटाईं",
    [STR_RUNNING] = "चलत बा",
    [STR_ON] = "चालू",
    [STR_OFF] = "बंद",
    [STR_NTS_NO] = "ना",
    [STR_NTS_ATTEMPT] = "कोसिस",
    [STR_NTS_REQUIRE] = "जरूरी",
    [STR_WAITING_NTP] = "NTP के इंतजार...",
    [STR_FMT_SYNCED] = "सिंक भइल: %s",
    [STR_FMT_SYNCING] = "सिंक होता: %s",
    [STR_FMT_WAITING] = "इंतजार: %lus",
    [STR_FMT_PEERS_NOSYNC] = "%d/%d सर्वर  poll %s  सिंक नइखे",
    [STR_FMT_PEERS_AGO] = "%d/%d सर्वर  poll %s  %s पहिले",
    [STR_WIFI_SETUP] = "Wi-Fi सेटअप",
    [STR_SCANNING] = "खोजत बा...",
    [STR_SCAN_FAILED] = "खोज बिफल",
    [STR_TAP_RETRY] = "फिर से कोसिस खातिर छूईं",
    [STR_SELECT_NETWORK] = "नेटवर्क चुनीं",
    [STR_NO_NETWORKS] = "नेटवर्क नइखे",
    [STR_NETWORK_LABEL] = "नेटवर्क:",
    [STR_ENTER_PASSWORD] = "पासवर्ड डालीं",
    [STR_CONNECTING] = "जुड़त बा",
    [STR_CONNECTING_TO] = "जुड़त बा:",
    [STR_CONNECTED] = "जुड़ गइल!",
    [STR_CONNECTION_FAILED] = "कनेक्शन बिफल",
    [STR_KB_SPACE] = "स्पेस",
    [STR_KB_GO] = "OK",
    [STR_VERSION] = "संस्करण:",
    [STR_STATUS] = "स्थिति:",
    [STR_READ] = "पढ़ल:",
    [STR_UPDATE] = "अपडेट",
    [STR_RESTARTING] = "फिर सुरू होता",
    [STR_OTA_UPDATE] = "OTA अपडेट",
    [STR_NTP_SERVER] = "NTP सर्वर",
    [STR_SERVER_LABEL] = "सर्वर:",
    [STR_NTP_SETTINGS] = "NTP सेटिंग",
    [STR_NTP_PRESETS] = "NTP विकल्प",
    [STR_PRESETS] = "विकल्प",
    [STR_BENCHMARK] = "जांच",
    [STR_STOPPING] = "रुकत बा",
    [STR_NTP_STATS] = "NTP आंकड़ा",
    [STR_UNSYNCED] = "सिंक नइखे",
    [STR_SYNCS_LABEL] = "  सिंक: ",
    [STR_NONE] = "कुछुओ ना",
    [STR_SELECT_REGION] = "क्षेत्र चुनीं",
    [STR_AGE_LABEL] = "  उमिर: ",
    [STR_DST] = "गरमी के समय",
    [STR_NO_DST] = "गरमी समय नइखे",
};
// CLDR abbreviated, except Thursday: CLDR's form overflows the 320px
// 2x date line (its short width is no shorter), so the attested
// Bhojpuri short form substitutes.
static const char *const weekdays_bho[7] = {
    "रबीबार", "सोमबार", "मंगलबार", "बुधबार", "बिफे", "शुक्रबार", "सनीचर",
};
static const char *const months_bho[12] = {
    "जनवरी", "फरवरी", "मार्च", "अप्रैल", "मई", "जून",
    "जुलाई", "अगस्त", "सितम्बर", "अक्टूबर", "नवंबर", "दिसंबर",
};
static const char lang_name_bho[] = "भोजपुरी";
// <<< shaped-source bho
#endif
