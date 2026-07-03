#include "i18n.h"
#include "fonts.h"
#include "display.h"
#include <stddef.h>
#include <stdio.h>

// All translations live in i18n_data.inc as plain UTF-8 - portable C,
// hand-editable without any tooling. Glyph bitmaps for the characters
// those strings use are generated separately by tools/gen_fonts.swift
// (macOS-only, needed only when new characters appear); check coverage
// portably with tools/check_i18n.py.
#include "i18n_data.inc"


static lang_t current_lang = LANG_EN;

const char *tr(str_id_t id) {
    if (id < 0 || id >= STR_COUNT) return "";
    const char *s = lang_tables[current_lang][id];
    if (!s) s = lang_en[id];
    return s ? s : "";
}

const char *tr_weekday(int dow) {
    if (dow < 0 || dow > 6) return "";
    return weekdays_by_lang[current_lang][dow];
}

const char *tr_month(int mon) {
    if (mon < 0 || mon > 11) return "";
    return months_by_lang[current_lang][mon];
}

void tr_date(char *buf, size_t len, int wday, int mon, int mday, int year) {
    const char *wd = tr_weekday(wday);
    const char *mo = tr_month(mon);
    switch (current_lang) {
    case LANG_EN:
        snprintf(buf, len, "%s %s %d, %d", wd, mo, mday, year);
        break;
    case LANG_DE:
        snprintf(buf, len, "%s %d. %s %d", wd, mday, mo, year);
        break;
    // No spaces inside the ja/zh numeral+suffix run: that is the native
    // convention, and it keeps the worst case (zh year+YEAR month+MONTH
    // day+DAY weekday, 19 bytes = 304px at 2x) inside the 320px panel;
    // with spaces the 2-digit days of months 10-12 hit 336px and lost the
    // weekday to the centered-string clip path.
    case LANG_JA:
        snprintf(buf, len, "%d%s%s%d%s %s", year, date_year_ja, mo, mday, date_day_ja, wd);
        break;
    case LANG_ZH:
        snprintf(buf, len, "%d%s%s%d%s %s", year, date_year_zh, mo, mday, date_day_zh, wd);
        break;
    case LANG_ZH_HANT:
        snprintf(buf, len, "%d%s%s%d%s %s", year, date_year_zh_hant, mo, mday, date_day_zh_hant, wd);
        break;
    case LANG_KO:
        snprintf(buf, len, "%d%s %s %d%s %s", year, date_year_ko, mo, mday, date_day_ko, wd);
        break;
    default:  // most others (incl. el/vi): day-first, no comma
        snprintf(buf, len, "%s %d %s %d", wd, mday, mo, year);
        break;
    }
}

const char *i18n_lang_name(lang_t lang) {
    if (lang < 0 || lang >= LANG_COUNT) return "";
    return lang_names[lang];
}

const display_glyph_font_t *i18n_glyph_font(lang_t lang) {
    switch (lang) {
    case LANG_JA: return &font_ja;
    case LANG_ZH: return &font_zh;
    case LANG_ZH_HANT: return &font_zh_hant;
    case LANG_KO: return &font_ko;
    case LANG_KA: return &font_ka;
    case LANG_HY: return &font_hy;
    case LANG_AR: return &font_ar;
    case LANG_FA: return &font_fa;
    case LANG_HE: return &font_he;
    case LANG_HI: return &font_hi;
    case LANG_BN: return &font_bn;
    case LANG_TH: return &font_th;
    default: return NULL;
    }
}

const display_glyph_font_t *i18n_lang_name_font(lang_t lang) {
    return i18n_glyph_font(lang);
}

lang_t i18n_get_language(void) {
    return current_lang;
}

void i18n_set_language(lang_t lang) {
    if (lang >= 0 && lang < LANG_COUNT) {
        current_lang = lang;
        display_set_glyph_font(i18n_glyph_font(lang));
    }
}
