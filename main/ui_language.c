#include "ui_language.h"
#include "esp_log.h"
#include "config.h"
#include "display.h"
#include "i18n.h"
#include "nvs_config.h"
#include "touch.h"
#include "ui_common.h"

static const char *TAG = "ui_language";

static ui_list_touch_t list_touch;
static int list_scroll = 0;

// Display order for the picker: native names sorted A-Z (diacritics folded),
// with the Cyrillic-script names grouped at the end. Rows map to languages
// through this table, so add new languages in their sorted position here.
static const lang_t lang_order[LANG_COUNT] = {
    LANG_CS,  // Cestina
    LANG_DA,  // Dansk
    LANG_DE,  // Deutsch
    LANG_EN,  // English
    LANG_ES,  // Espanol
    LANG_FR,  // Francais
    LANG_HR,  // Hrvatski
    LANG_ID,  // Indonesia
    LANG_IT,  // Italiano
    LANG_HU,  // Magyar
    LANG_NL,  // Nederlands
    LANG_NO,  // Norsk
    LANG_PL,  // Polski
    LANG_PT,  // Portugues
    LANG_RO,  // Romana
    LANG_FI,  // Suomi
    LANG_SV,  // Svenska
    LANG_TR,  // Turkce
    LANG_JA,  // Japanese native name
    LANG_ZH,  // Simplified Chinese native name
    LANG_ZH_HANT,  // Traditional Chinese native name
    LANG_KO,  // Korean native name
    LANG_BG,  // Bulgarski
    LANG_RU,  // Russkiy
    LANG_UK,  // Ukrainska
};

// Row index in lang_order for a given language (0 if not found).
static int lang_row(lang_t lang) {
    for (int i = 0; i < LANG_COUNT; i++) {
        if (lang_order[i] == lang) return i;
    }
    return 0;
}

static void draw_screen(void) {
    const char *labels[LANG_COUNT];
    const display_cjk_font_t *fonts[LANG_COUNT];
    for (int i = 0; i < LANG_COUNT; i++) {
        labels[i] = i18n_lang_name(lang_order[i]);
        fonts[i] = i18n_lang_name_font(lang_order[i]);
    }
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_LANGUAGE), true);
    ui_draw_list_fonts(labels, fonts, LANG_COUNT, list_scroll, lang_row(i18n_get_language()));
}

void ui_language_init(void) {
    ESP_LOGI(TAG, "Initializing language picker");
    list_scroll = ui_list_scroll_to_item(lang_row(i18n_get_language()), LANG_COUNT);
    ui_list_touch_reset(&list_touch);
    draw_screen();
}

language_result_t ui_language_update(void) {
    touch_point_t touch;
    bool pressed = touch_read(&touch);

    ui_list_touch_result_t r =
        ui_list_touch_update(&list_touch, &touch, pressed, LANG_COUNT, &list_scroll);
    if (r == UI_LIST_TOUCH_SCROLLED) {
        draw_screen();
    } else if (r == UI_LIST_TOUCH_TAPPED) {
        const touch_point_t *tap = &list_touch.tap_start;
        if (ui_back_button_hit(tap)) {
            return LANGUAGE_RESULT_BACK;
        }
        int item = ui_list_tap_to_item(tap, list_scroll, LANG_COUNT);
        if (item >= 0 && lang_order[item] != i18n_get_language()) {
            i18n_set_language(lang_order[item]);
            nvs_config_set_language((uint8_t)lang_order[item]);
            // Repaint in the newly selected language so the change is
            // visible immediately; the user taps Back to return.
            draw_screen();
        }
    }

    return LANGUAGE_RESULT_NONE;
}
