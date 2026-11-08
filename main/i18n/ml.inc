// Editable source of truth for the pre-shaped strings; never compiled.
// Editing REQUIRES a macOS font regeneration (swift tools/gen_fonts.swift);
// tools/check_i18n.py catches staleness via source hashes in fonts.c.
#if 0
// >>> shaped-source ml
static const char *const lang_ml[STR_COUNT] = {
    [STR_SETTINGS] = "ക്രമീകരണങ്ങൾ",
    [STR_WIFI] = "Wi-Fi",
    [STR_TIMEZONE] = "സമയ മേഖല",
    [STR_BRIGHTNESS] = "തെളിച്ചം",
    [STR_LED_BLINK] = "LED മിന്നൽ",
    [STR_ROTATE] = "തിരിക്കുക 180",
    [STR_ABOUT] = "വിവരം",
    [STR_LANGUAGE] = "ഭാഷ",
    [STR_DONE] = "കഴിഞ്ഞു",
    [STR_BACK] = "പിന്നോട്ട്",
    [STR_CANCEL] = "റദ്ദ്",
    [STR_DEL] = "മായ്ക്കു",
    [STR_RUNNING] = "പ്രവർത്തിക്കുന്നു",
    [STR_ON] = "ഓൺ",
    [STR_OFF] = "ഓഫ്",
    [STR_WAITING_NTP] = "NTP കാത്തിരിപ്പ്...",
    [STR_WIFI_SETUP] = "Wi-Fi സജ്ജീകരണം",
    [STR_SCANNING] = "തിരയുന്നു...",
    [STR_SCAN_FAILED] = "തിരച്ചിൽ പരാജയം",
    [STR_TAP_RETRY] = "വീണ്ടും ശ്രമിക്കാൻ തൊടുക",
    [STR_SELECT_NETWORK] = "നെറ്റ്‌വർക്ക് തിരഞ്ഞെടുക്കുക",
    [STR_NO_NETWORKS] = "നെറ്റ്‌വർക്ക് ഇല്ല",
    [STR_NETWORK_LABEL] = "നെറ്റ്‌വർക്ക്:",
    [STR_ENTER_PASSWORD] = "പാസ്‌വേഡ് നൽകുക",
    [STR_CONNECTING] = "ബന്ധിപ്പിക്കുന്നു",
    [STR_CONNECTING_TO] = "ബന്ധിപ്പിക്കുന്നു:",
    [STR_CONNECTED] = "ബന്ധിപ്പിച്ചു!",
    [STR_CONNECTION_FAILED] = "കണക്ഷൻ പരാജയം",
    [STR_KB_SPACE] = "സ്പേസ്",
    [STR_KB_GO] = "OK",
    [STR_VERSION] = "പതിപ്പ്:",
    [STR_STATUS] = "നില:",
    [STR_READ] = "വായിച്ചു:",
    [STR_UPDATE] = "അപ്ഡേറ്റ്",
    [STR_RESTARTING] = "പുനരാരംഭം",
    [STR_OTA_UPDATE] = "OTA അപ്ഡേറ്റ്",
    [STR_NTP_SERVER] = "NTP സെർവർ",
    [STR_SERVER_LABEL] = "സെർവർ:",
    [STR_NTP_SETTINGS] = "NTP ക്രമീകരണം",
    [STR_NTP_PRESETS] = "NTP പ്രൊഫൈൽ",
    [STR_PRESETS] = "പ്രൊഫൈൽ",
    [STR_NONE] = "ഒന്നുമില്ല",
    [STR_SELECT_REGION] = "പ്രദേശം തിരഞ്ഞെടുക്കുക",
    [STR_DST] = "വേനൽ സമയം",
    [STR_NO_DST] = "വേനൽ സമയം ഇല്ല",
};
static const char *const weekdays_ml[7] = {
    "ഞായർ", "തിങ്കൾ", "ചൊവ്വ", "ബുധൻ", "വ്യാഴം", "വെള്ളി", "ശനി",
};
static const char *const months_ml[12] = {
    "ജനു", "ഫെബ്രു", "മാർ", "ഏപ്രി", "മേയ്", "ജൂൺ",
    "ജൂലൈ", "ഓഗ", "സെപ്റ്റം", "ഒക്ടോ", "നവം", "ഡിസം",
};
static const char lang_name_ml[] = "മലയാളം";
// <<< shaped-source ml
#endif
