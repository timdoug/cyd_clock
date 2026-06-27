#ifndef I18N_H
#define I18N_H

#include <stddef.h>

// UI localization. A single string table indexed by (language, string id).
// The active language is loaded from NVS at boot and switched at runtime from
// the settings Language picker. Translations are ASCII source: accented
// letters are written as Latin-1 \xNN escapes and rendered by the extended
// font (see display.c font_high). Universal technical tokens (NTP, NTS, WiFi,
// IPv6, RSSI, MAC, units) and NTP tooling column names commonly left as-is
// (poll, offset, jitter, delay/disp in the compact stats UI) are deliberately
// kept canonical; surrounding status text is localized.

typedef enum {
    LANG_EN,
    LANG_ES,
    LANG_FR,
    LANG_DE,
    LANG_PT,
    LANG_IT,
    LANG_NL,
    LANG_SV,
    LANG_DA,
    LANG_NO,
    LANG_FI,
    LANG_PL,
    LANG_CS,
    LANG_TR,
    LANG_RU,
    LANG_ID,
    LANG_HR,
    LANG_HU,
    LANG_RO,
    LANG_BG,
    LANG_UK,
    LANG_COUNT,
} lang_t;

typedef enum {
    // Settings menu + shared chrome
    STR_SETTINGS,
    STR_TIMEZONE,
    STR_BRIGHTNESS,
    STR_LED_BLINK,
    STR_ROTATE,
    STR_ABOUT,
    STR_LANGUAGE,
    STR_DONE,
    STR_BACK,
    STR_CANCEL,
    STR_DEL,
    STR_RUNNING,
    STR_ON,
    STR_OFF,
    STR_NTS_NO,
    STR_NTS_ATTEMPT,
    STR_NTS_REQUIRE,

    // Clock face
    STR_WAITING_NTP,
    STR_FMT_SYNCED,         // "Synced: %s"
    STR_FMT_SYNCING,        // "Syncing: %s"
    STR_FMT_WAITING,        // "Waiting: %lus"
    STR_FMT_PEERS_NOSYNC,   // "%d/%d peers  poll %s  no sync yet"
    STR_FMT_PEERS_AGO,      // "%d/%d peers  poll %s  %s ago"
    STR_FMT_OFF_DRIFT,      // "off %s %s drift %s"

    // WiFi setup
    STR_WIFI_SETUP,
    STR_SCANNING,
    STR_SCAN_FAILED,
    STR_TAP_RETRY,
    STR_SELECT_NETWORK,
    STR_NO_NETWORKS,
    STR_NETWORK_LABEL,
    STR_ENTER_PASSWORD,
    STR_CONNECTING,
    STR_CONNECTING_TO,
    STR_CONNECTED,
    STR_CONNECTION_FAILED,
    STR_KB_SHIFT,
    STR_KB_SPACE,
    STR_KB_GO,

    // About / OTA
    STR_VERSION,
    STR_FIRMWARE_URL,
    STR_OTA_URL,
    STR_STATUS,
    STR_READ,
    STR_UPDATE,
    STR_RESTARTING,
    STR_OTA_UPDATE,

    // NTP settings
    STR_NTP_SERVER,
    STR_SERVER_LABEL,
    STR_NTP_SETTINGS,
    STR_NTP_PRESETS,
    STR_PRESETS,
    STR_BENCHMARK,
    STR_STOPPING,
    STR_FAIL,

    // NTP stats
    STR_NTP_STATS,
    STR_UNSYNCED,
    STR_STRATUM_LABEL,
    STR_POLL_LABEL,
    STR_SYNCS_LABEL,
    STR_DRIFT_LABEL,
    STR_AGE_LABEL,
    STR_OFFSET_LABEL,
    STR_JITTER_LABEL,
    STR_ROOT_LABEL,
    STR_FMT_ROOT_DETAIL,
    STR_PEER_HEADER,
    STR_NONE,

    // Timezone
    STR_SELECT_REGION,

    STR_COUNT,
} str_id_t;

// Active-language string for id, falling back to English when a translation
// is NULL. Never returns NULL.
const char *tr(str_id_t id);

// Abbreviated weekday (dow 0=Sunday) / month (mon 0=January) names.
const char *tr_weekday(int dow);
const char *tr_month(int mon);

// Format the clock date for the active language into buf.
void tr_date(char *buf, size_t len, int wday, int mon, int mday, int year);

// Native display name of a language, for the picker (e.g. "Espanol").
const char *i18n_lang_name(lang_t lang);

lang_t i18n_get_language(void);
void i18n_set_language(lang_t lang);

#endif
