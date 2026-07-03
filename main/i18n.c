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

// Gregorian to Solar Hijri (Jalali), the calendar Persian devices show.
// Arithmetic 33-year-cycle algorithm (jalaali-js lineage), exact for the
// clock's era.
static void gregorian_to_jalali(int gy, int gm, int gd, int *jy, int *jm, int *jd) {
    static const int gdm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int gy2 = gy - 1600;
    long g_day_no = 365L * gy2 + (gy2 + 3) / 4 - (gy2 + 99) / 100 + (gy2 + 399) / 400;
    for (int i = 0; i < gm - 1; i++) g_day_no += gdm[i];
    if (gm > 2 && ((gy % 4 == 0 && gy % 100 != 0) || gy % 400 == 0)) g_day_no++;
    g_day_no += gd - 1;
    long j_day_no = g_day_no - 79;
    long j_np = j_day_no / 12053;
    j_day_no %= 12053;
    int y = (int)(979 + 33 * j_np + 4 * (j_day_no / 1461));
    j_day_no %= 1461;
    if (j_day_no >= 366) {
        y += (int)((j_day_no - 366) / 365 + 1);
        j_day_no = (j_day_no - 366) % 365;
    }
    int i;
    for (i = 0; i < 11 && j_day_no >= (i < 6 ? 31 : 30); i++) {
        j_day_no -= (i < 6 ? 31 : 30);
    }
    *jy = y;
    *jm = i + 1;
    *jd = (int)j_day_no + 1;
}

// Replace ASCII digits with Persian digits (U+06F0.., two UTF-8 bytes).
static void fa_digits(char *dst, size_t len, const char *src) {
    size_t pos = 0;
    for (const char *p = src; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            if (pos + 3 > len) break;
            unsigned cp = 0x6F0u + (unsigned)(*p - '0');
            dst[pos++] = (char)(0xC0 | (cp >> 6));
            dst[pos++] = (char)(0x80 | (cp & 0x3F));
        } else {
            if (pos + 2 > len) break;
            dst[pos++] = *p;
        }
    }
    dst[pos] = '\0';
}

void tr_date(char *buf, size_t len, int wday, int mon, int mday, int year) {
    const char *wd = tr_weekday(wday);
    const char *mo = tr_month(mon);
    switch (current_lang) {
    case LANG_EN:
        snprintf(buf, len, "%s %s %d, %d", wd, mo, mday, year);
        break;
    // Day takes a period in German, Czech, Slovak, and Finnish.
    case LANG_DE:
    case LANG_CS:
    case LANG_SK:
    case LANG_FI:
        snprintf(buf, len, "%s %d. %s %d", wd, mday, mo, year);
        break;
    // Hungarian and Lithuanian dates are year-first.
    case LANG_HU:
        // CLDR also puts a comma before the weekday, but September
        // ("szept.") would then exceed the panel at 2x.
        snprintf(buf, len, "%d. %s %d. %s", year, mo, mday, wd);
        break;
    case LANG_LT:
        snprintf(buf, len, "%d %s %d, %s", year, mo, mday, wd);
        break;
    // Assembled in visual order (every fragment renders left to right):
    // year month day weekday reads correctly right-to-left.
    case LANG_AR:
    case LANG_HE:
    case LANG_UR:
        snprintf(buf, len, "%d %s %d %s", year, mo, mday, wd);
        break;
    case LANG_FA: {
        // Solar Hijri with Persian digits, visual RTL order like ar/he.
        int jy, jm, jd;
        gregorian_to_jalali(year, mon + 1, mday, &jy, &jm, &jd);
        char tmp[48];
        snprintf(tmp, sizeof(tmp), "%d %s %d %s",
                 jy, months_by_lang[LANG_FA][jm - 1], jd, wd);
        fa_digits(buf, len, tmp);
        break;
    }
    // Thai uses the Buddhist Era, 543 years ahead of Gregorian.
    case LANG_TH:
        snprintf(buf, len, "%s %d %s %d", wd, mday, mo, year + 543);
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
    case LANG_UR: return &font_ur;
    case LANG_MR: return &font_mr;
    case LANG_TA: return &font_ta;
    case LANG_TE: return &font_te;
    case LANG_PA: return &font_pa;
    case LANG_GU: return &font_gu;
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
