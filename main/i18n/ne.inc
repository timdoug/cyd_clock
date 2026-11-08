// Editable source of truth for the pre-shaped strings; never compiled.
// Editing REQUIRES a macOS font regeneration (swift tools/gen_fonts.swift);
// tools/check_i18n.py catches staleness via source hashes in fonts.c.
#if 0
// >>> shaped-source ne
static const char *const lang_ne[STR_COUNT] = {
    [STR_SETTINGS] = "सेटिङहरू",
    [STR_WIFI] = "Wi-Fi",
    [STR_TIMEZONE] = "समय क्षेत्र",
    [STR_BRIGHTNESS] = "चमक",
    [STR_LED_BLINK] = "LED झिम्किने",
    [STR_ROTATE] = "घुमाउनु 180",
    [STR_ABOUT] = "बारेमा",
    [STR_LANGUAGE] = "भाषा",
    [STR_DONE] = "भयो",
    [STR_BACK] = "पछाडि",
    [STR_CANCEL] = "रद्द",
    [STR_DEL] = "मेट्नु",
    [STR_RUNNING] = "चलिरहेको",
    [STR_ON] = "अन",
    [STR_OFF] = "बन्द",
    [STR_WAITING_NTP] = "NTP पर्खँदै...",
    [STR_WIFI_SETUP] = "Wi-Fi सेटअप",
    [STR_SCANNING] = "खोज्दै...",
    [STR_SCAN_FAILED] = "खोजी असफल",
    [STR_TAP_RETRY] = "पुनः प्रयासका लागि छुनुहोस्",
    [STR_SELECT_NETWORK] = "नेटवर्क छान्नुहोस्",
    [STR_NO_NETWORKS] = "नेटवर्क छैन",
    [STR_NETWORK_LABEL] = "नेटवर्क:",
    [STR_ENTER_PASSWORD] = "पासवर्ड हाल्नुहोस्",
    [STR_CONNECTING] = "जडान हुँदै",
    [STR_CONNECTING_TO] = "जडान हुँदै:",
    [STR_CONNECTED] = "जडान भयो!",
    [STR_CONNECTION_FAILED] = "जडान असफल",
    [STR_KB_SPACE] = "स्पेस",
    [STR_KB_GO] = "OK",
    [STR_VERSION] = "संस्करण:",
    [STR_STATUS] = "स्थिति:",
    [STR_READ] = "पढियो:",
    [STR_UPDATE] = "अपडेट",
    [STR_RESTARTING] = "पुनः सुरु",
    [STR_OTA_UPDATE] = "OTA अपडेट",
    [STR_NTP_SERVER] = "NTP सर्भर",
    [STR_SERVER_LABEL] = "सर्भर:",
    [STR_NTP_SETTINGS] = "NTP सेटिङ",
    [STR_NTP_PRESETS] = "NTP प्रोफाइल",
    [STR_PRESETS] = "प्रोफाइल",
    [STR_NONE] = "केही छैन",
    [STR_SELECT_REGION] = "क्षेत्र छान्नुहोस्",
    [STR_DST] = "ग्रीष्म समय",
    [STR_NO_DST] = "ग्रीष्म समय छैन",
};
static const char *const weekdays_ne[7] = {
    "आइत", "सोम", "मङ्गल", "बुध", "बिहि", "शुक्र", "शनि",
};
static const char *const months_ne[12] = {
    "जनवरी", "फेब्रुअरी", "मार्च", "अप्रिल", "मे", "जुन",
    "जुलाई", "अगस्ट", "सेप्टेम्बर", "अक्टोबर", "नोभेम्बर", "डिसेम्बर",
};
static const char lang_name_ne[] = "नेपाली";
// <<< shaped-source ne
#endif
