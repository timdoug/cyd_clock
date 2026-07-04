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

// Picker display order is generated: native names sorted by Unicode root
// collation (see tools/gen_fonts.swift), the same order OS pickers use.
#include "lang_order.inc"

// Row index in lang_order for a given language (0 if not found).
static int lang_row(lang_t lang) {
    for (int i = 0; i < LANG_COUNT; i++) {
        if (lang_order[i] == lang) return i;
    }
    return 0;
}

// Scrolling repaints only the list rows, like the other list screens
// (timezone, wifi); clearing the whole screen per scroll step flashes
// the header. The full clear happens on entry and on language change,
// when the header text itself must be redrawn.
static void draw_list(void) {
    const char *labels[LANG_COUNT];
    const display_glyph_font_t *fonts[LANG_COUNT];
    for (int i = 0; i < LANG_COUNT; i++) {
        labels[i] = i18n_lang_name(lang_order[i]);
        fonts[i] = i18n_lang_name_font(lang_order[i]);
    }
    ui_draw_list_fonts(labels, fonts, LANG_COUNT, list_scroll, lang_row(i18n_get_language()));
}

static void draw_screen(void) {
    // The composed header and self-contained list rows cover everything
    // except the band between them; no full clear, no black flash.
    display_fill_rect(0, UI_HEADER_HEIGHT, DISPLAY_WIDTH,
                      UI_LIST_START_Y - UI_HEADER_HEIGHT, COLOR_BLACK);
    ui_draw_header(tr(STR_LANGUAGE), true);
    draw_list();
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
        draw_list();
    } else if (r == UI_LIST_TOUCH_TAPPED) {
        const touch_point_t *tap = &list_touch.tap_start;
        if (ui_back_button_hit(tap)) {
            return LANGUAGE_RESULT_BACK;
        }
        int item = ui_list_tap_to_item(tap, list_scroll, LANG_COUNT);
        if (item >= 0 && lang_order[item] != i18n_get_language()) {
            i18n_set_language(lang_order[item]);
            nvs_config_set_language(i18n_lang_code(lang_order[item]));
            // Repaint in the newly selected language so the change is
            // visible immediately; the user taps Back to return.
            draw_screen();
        }
    }

    return LANGUAGE_RESULT_NONE;
}
