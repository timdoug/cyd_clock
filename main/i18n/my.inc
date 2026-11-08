// Editable source of truth for the pre-shaped strings; never compiled.
// Editing REQUIRES a macOS font regeneration (swift tools/gen_fonts.swift);
// tools/check_i18n.py catches staleness via source hashes in fonts.c.
#if 0
// >>> shaped-source my
static const char *const lang_my[STR_COUNT] = {
    [STR_SETTINGS] = "ဆက်တင်များ",
    [STR_WIFI] = "Wi-Fi",
    [STR_TIMEZONE] = "အချိန်ဇုန်",
    [STR_BRIGHTNESS] = "တောက်ပမှု",
    [STR_LED_BLINK] = "LED မှိတ်တုတ်",
    [STR_ROTATE] = "လှည့် 180",
    [STR_ABOUT] = "အကြောင်း",
    [STR_LANGUAGE] = "ဘာသာ",
    [STR_DONE] = "ပြီးပြီ",
    [STR_BACK] = "နောက်သို့",
    [STR_CANCEL] = "ပယ်ဖျက်",
    [STR_DEL] = "ဖျက်",
    [STR_RUNNING] = "လုပ်ဆောင်နေ",
    [STR_ON] = "ဖွင့်",
    [STR_OFF] = "ပိတ်",
    [STR_WAITING_NTP] = "NTP စောင့်နေ...",
    [STR_WIFI_SETUP] = "Wi-Fi တပ်ဆင်",
    [STR_SCANNING] = "ရှာနေသည်...",
    [STR_SCAN_FAILED] = "ရှာဖွေမှု မအောင်မြင်",
    [STR_TAP_RETRY] = "ထပ်စမ်းရန် တို့ပါ",
    [STR_SELECT_NETWORK] = "ကွန်ရက်ရွေးပါ",
    [STR_NO_NETWORKS] = "ကွန်ရက်မရှိ",
    [STR_NETWORK_LABEL] = "ကွန်ရက်:",
    [STR_ENTER_PASSWORD] = "စကားဝှက်ထည့်ပါ",
    [STR_CONNECTING] = "ချိတ်ဆက်နေ",
    [STR_CONNECTING_TO] = "ချိတ်ဆက်နေ:",
    [STR_CONNECTED] = "ချိတ်ဆက်ပြီး!",
    [STR_CONNECTION_FAILED] = "ချိတ်ဆက်မှု မအောင်မြင်",
    [STR_KB_SPACE] = "နေရာလွတ်",
    [STR_KB_GO] = "OK",
    [STR_VERSION] = "ဗားရှင်း:",
    [STR_STATUS] = "အခြေအနေ:",
    [STR_READ] = "ဖတ်ပြီး:",
    [STR_UPDATE] = "အပ်ဒိတ်",
    [STR_RESTARTING] = "ပြန်စနေ",
    [STR_OTA_UPDATE] = "OTA အပ်ဒိတ်",
    [STR_NTP_SERVER] = "NTP ဆာဗာ",
    [STR_SERVER_LABEL] = "ဆာဗာ:",
    [STR_NTP_SETTINGS] = "NTP ဆက်တင်",
    [STR_NTP_PRESETS] = "NTP ပရိုဖိုင်",
    [STR_PRESETS] = "ပရိုဖိုင်",
    [STR_NONE] = "မရှိ",
    [STR_SELECT_REGION] = "ဒေသရွေးပါ",
    [STR_DST] = "နွေရာသီအချိန်",
    [STR_NO_DST] = "နွေရာသီအချိန်မရှိ",
};
static const char *const weekdays_my[7] = {
    "တနင်္ဂနွေ", "တနင်္လာ", "အင်္ဂါ", "ဗုဒ္ဓဟူး", "ကြာသပတေး", "သောကြာ", "စနေ",
};
static const char *const months_my[12] = {
    "ဇန်", "ဖေ", "မတ်", "ဧ", "မေ", "ဇွန်",
    "ဇူ", "ဩ", "စက်", "အောက်", "နို", "ဒီ",
};
static const char lang_name_my[] = "မြန်မာ";
// <<< shaped-source my
#endif
