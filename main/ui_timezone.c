#include "ui_timezone.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "display.h"
#include "touch.h"
#include "i18n.h"
#include "ui_common.h"

static const char *TAG = "ui_timezone";

// Timezone definitions: {region, city, POSIX_TZ_string}
typedef struct {
    const char *region;
    const char *city;
    const char *tz;
} timezone_entry_t;

// Generated from zone1970.tab - deduplicated within each region by
// functional POSIX equivalence, largest city by population as representative.

static const char *regions[] = {
    "Africa",
    "America",
    "Antarctica",
    "Asia",
    "Atlantic",
    "Australia",
    "Europe",
    "Indian",
    "Pacific",
    "UTC",
};
#define NUM_REGIONS 10

static const timezone_entry_t timezones[] = {
    {"Africa",       "Abidjan (UTC+0, no DST)",              "GMT0"},
    {"Africa",       "Ceuta (UTC+1, DST)",                   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Africa",       "Lagos (UTC+1, no DST)",                "WAT-1"},
    {"Africa",       "Cairo (UTC+2, DST)",                   "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"Africa",       "Khartoum (UTC+2, no DST)",             "CAT-2"},
    {"Africa",       "Nairobi (UTC+3, no DST)",              "EAT-3"},
    {"America",      "Adak (UTC-10, DST)",                   "HST10HDT,M3.2.0,M11.1.0"},
    {"America",      "Anchorage (UTC-9, DST)",               "AKST9AKDT,M3.2.0,M11.1.0"},
    {"America",      "Los Angeles (UTC-8, DST)",             "PST8PDT,M3.2.0,M11.1.0"},
    {"America",      "Denver (UTC-7, DST)",                  "MST7MDT,M3.2.0,M11.1.0"},
    {"America",      "Phoenix (UTC-7, no DST)",              "MST7"},
    {"America",      "Chicago (UTC-6, DST)",                 "CST6CDT,M3.2.0,M11.1.0"},
    {"America",      "Mexico City (UTC-6, no DST)",          "CST6"},
    {"America",      "Havana (UTC-5, DST)",                  "CST5CDT,M3.2.0/0,M11.1.0/1"},
    {"America",      "Lima (UTC-5, no DST)",                 "<-05>5"},
    {"America",      "New York (UTC-5, DST)",                "EST5EDT,M3.2.0,M11.1.0"},
    {"America",      "Halifax (UTC-4, DST)",                 "AST4ADT,M3.2.0,M11.1.0"},
    {"America",      "Santiago (UTC-4, DST)",                "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"America",      "Santo Domingo (UTC-4, no DST)",        "AST4"},
    {"America",      "St Johns (UTC-3:30, DST)",             "NST3:30NDT,M3.2.0,M11.1.0"},
    {"America",      "Miquelon (UTC-3, DST)",                "<-03>3<-02>,M3.2.0,M11.1.0"},
    {"America",      "Sao Paulo (UTC-3, no DST)",            "<-03>3"},
    {"America",      "Noronha (UTC-2, no DST)",              "<-02>2"},
    {"America",      "Nuuk (UTC-2, DST)",                    "<-02>2<-01>,M3.5.0/-1,M10.5.0/0"},
    {"America",      "Danmarkshavn (UTC+0, no DST)",         "GMT0"},
    {"Antarctica",   "Rothera (UTC-3, no DST)",              "<-03>3"},
    {"Antarctica",   "Troll (UTC+0, DST)",                   "<+00>0<+02>-2,M3.5.0/1,M10.5.0/3"},
    {"Antarctica",   "Mawson (UTC+5, no DST)",               "<+05>-5"},
    {"Antarctica",   "Davis (UTC+7, no DST)",                "<+07>-7"},
    {"Antarctica",   "Casey (UTC+8, no DST)",                "<+08>-8"},
    {"Antarctica",   "Macquarie (UTC+10, DST)",              "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Asia",         "Beirut (UTC+2, DST)",                  "EET-2EEST,M3.5.0/0,M10.5.0/0"},
    {"Asia",         "Gaza (UTC+2, DST)",                    "EET-2EEST,M3.4.4/50,M10.4.4/50"},
    {"Asia",         "Jerusalem (UTC+2, DST)",               "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"Asia",         "Nicosia (UTC+2, DST)",                 "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Asia",         "Baghdad (UTC+3, no DST)",              "<+03>-3"},
    {"Asia",         "Tehran (UTC+3:30, no DST)",            "<+0330>-3:30"},
    {"Asia",         "Dubai (UTC+4, no DST)",                "<+04>-4"},
    {"Asia",         "Kabul (UTC+4:30, no DST)",             "<+0430>-4:30"},
    {"Asia",         "Karachi (UTC+5, no DST)",              "PKT-5"},
    {"Asia",         "Kolkata (UTC+5:30, no DST)",           "IST-5:30"},
    {"Asia",         "Kathmandu (UTC+5:45, no DST)",         "<+0545>-5:45"},
    {"Asia",         "Dhaka (UTC+6, no DST)",                "<+06>-6"},
    {"Asia",         "Yangon (UTC+6:30, no DST)",            "<+0630>-6:30"},
    {"Asia",         "Jakarta (UTC+7, no DST)",              "WIB-7"},
    {"Asia",         "Shanghai (UTC+8, no DST)",             "CST-8"},
    {"Asia",         "Tokyo (UTC+9, no DST)",                "JST-9"},
    {"Asia",         "Vladivostok (UTC+10, no DST)",         "<+10>-10"},
    {"Asia",         "Sakhalin (UTC+11, no DST)",            "<+11>-11"},
    {"Asia",         "Kamchatka (UTC+12, no DST)",           "<+12>-12"},
    {"Atlantic",     "Bermuda (UTC-4, DST)",                 "AST4ADT,M3.2.0,M11.1.0"},
    {"Atlantic",     "Stanley (UTC-3, no DST)",              "<-03>3"},
    {"Atlantic",     "South Georgia (UTC-2, no DST)",        "<-02>2"},
    {"Atlantic",     "Azores (UTC-1, DST)",                  "<-01>1<+00>,M3.5.0/0,M10.5.0/1"},
    {"Atlantic",     "Cape Verde (UTC-1, no DST)",           "<-01>1"},
    {"Atlantic",     "Canary (UTC+0, DST)",                  "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Australia",    "Perth (UTC+8, no DST)",                "AWST-8"},
    {"Australia",    "Eucla (UTC+8:45, no DST)",             "<+0845>-8:45"},
    {"Australia",    "Adelaide (UTC+9:30, DST)",             "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"Australia",    "Darwin (UTC+9:30, no DST)",            "ACST-9:30"},
    {"Australia",    "Brisbane (UTC+10, no DST)",            "AEST-10"},
    {"Australia",    "Sydney (UTC+10, DST)",                 "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"Australia",    "Lord Howe (UTC+10:30, DST)",           "<+1030>-10:30<+11>-11,M10.1.0,M4.1.0"},
    {"Europe",       "Dublin (UTC+0, DST)",                  "IST-1GMT0,M10.5.0,M3.5.0/1"},
    {"Europe",       "London (UTC+0, DST)",                  "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe",       "Paris (UTC+1, DST)",                   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe",       "Athens (UTC+2, DST)",                  "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe",       "Kaliningrad (UTC+2, no DST)",          "EET-2"},
    {"Europe",       "Istanbul (UTC+3, no DST)",             "<+03>-3"},
    {"Europe",       "Samara (UTC+4, no DST)",               "<+04>-4"},
    {"Indian",       "Mauritius (UTC+4, no DST)",            "<+04>-4"},
    {"Indian",       "Maldives (UTC+5, no DST)",             "<+05>-5"},
    {"Indian",       "Chagos (UTC+6, no DST)",               "<+06>-6"},
    {"Pacific",      "Pago Pago (UTC-11, no DST)",           "SST11"},
    {"Pacific",      "Honolulu (UTC-10, no DST)",            "HST10"},
    {"Pacific",      "Marquesas (UTC-9:30, no DST)",         "<-0930>9:30"},
    {"Pacific",      "Gambier (UTC-9, no DST)",              "<-09>9"},
    {"Pacific",      "Pitcairn (UTC-8, no DST)",             "<-08>8"},
    {"Pacific",      "Easter (UTC-6, DST)",                  "<-06>6<-05>,M9.1.6/22,M4.1.6/22"},
    {"Pacific",      "Galapagos (UTC-6, no DST)",            "<-06>6"},
    {"Pacific",      "Palau (UTC+9, no DST)",                "<+09>-9"},
    {"Pacific",      "Guam (UTC+10, no DST)",                "ChST-10"},
    {"Pacific",      "Norfolk (UTC+11, DST)",                "<+11>-11<+12>,M10.1.0,M4.1.0/3"},
    {"Pacific",      "Noumea (UTC+11, no DST)",              "<+11>-11"},
    {"Pacific",      "Auckland (UTC+12, DST)",               "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"Pacific",      "Fiji (UTC+12, no DST)",                "<+12>-12"},
    {"Pacific",      "Chatham (UTC+12:45, DST)",             "<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45"},
    {"Pacific",      "Tongatapu (UTC+13, no DST)",           "<+13>-13"},
    {"Pacific",      "Kiritimati (UTC+14, no DST)",          "<+14>-14"},
    {"UTC",          "UTC (UTC+0)",                          "UTC0"},
};
#define NUM_TIMEZONES ((int)(sizeof(timezones) / sizeof(timezones[0])))

typedef enum {
    TZ_STATE_REGION,
    TZ_STATE_CITY,
} tz_ui_state_t;

static tz_ui_state_t ui_state = TZ_STATE_REGION;
static int selected_region = 0;
static int selected_tz = 0;
static int list_scroll = 0;
static bool selection_made = false;
static bool show_back_button = false;
static ui_list_touch_t list_touch;

static int city_indices[30];
static int city_count = 0;

static void draw_region_list(void);
static void draw_city_list(void);

static void build_city_list(int region_idx) {
    const char *region = regions[region_idx];
    city_count = 0;
    for (int i = 0; i < NUM_TIMEZONES; i++) {
        if (strcmp(timezones[i].region, region) == 0) {
            if (city_count >= (int)(sizeof(city_indices) / sizeof(city_indices[0]))) break;
            city_indices[city_count++] = i;
        }
    }
}

static int list_count(void) {
    return (ui_state == TZ_STATE_REGION) ? NUM_REGIONS : city_count;
}

static void draw_current_list(void) {
    if (ui_state == TZ_STATE_REGION) draw_region_list();
    else draw_city_list();
}

static void draw_region_list(void) {
    const char *labels[NUM_REGIONS];
    for (int i = 0; i < NUM_REGIONS; i++) {
        labels[i] = regions[i];
    }
    ui_draw_list(labels, NUM_REGIONS, list_scroll, selected_region);
}

static void draw_city_list(void) {
    const char *labels[city_count];
    int selected_city = -1;
    for (int i = 0; i < city_count; i++) {
        labels[i] = timezones[city_indices[i]].city;
        if (city_indices[i] == selected_tz) {
            selected_city = i;
        }
    }
    ui_draw_list(labels, city_count, list_scroll, selected_city);
}

static void enter_region_view(void) {
    ui_state = TZ_STATE_REGION;
    list_scroll = ui_list_scroll_to_item(selected_region, NUM_REGIONS);
    display_fill(COLOR_BLACK);
    ui_draw_header(tr(STR_SELECT_REGION), show_back_button);
    draw_region_list();
}

static void enter_city_view(void) {
    ui_state = TZ_STATE_CITY;
    build_city_list(selected_region);
    list_scroll = 0;

    for (int i = 0; i < city_count; i++) {
        if (city_indices[i] == selected_tz) {
            list_scroll = ui_list_scroll_to_item(i, city_count);
            break;
        }
    }
    list_scroll = ui_list_clamp_scroll(list_scroll, city_count);

    display_fill(COLOR_BLACK);
    ui_draw_header(regions[selected_region], true);
    draw_city_list();
}

void ui_timezone_init(const char *current_tz, bool show_back) {
    ESP_LOGI(TAG, "Initializing timezone selector");
    selection_made = false;
    show_back_button = show_back;
    ui_list_touch_reset(&list_touch);

    selected_tz = 0;
    selected_region = 0;
    if (current_tz) {
        for (int i = 0; i < NUM_TIMEZONES; i++) {
            if (strcmp(timezones[i].tz, current_tz) == 0) {
                selected_tz = i;
                for (int r = 0; r < NUM_REGIONS; r++) {
                    if (strcmp(timezones[i].region, regions[r]) == 0) {
                        selected_region = r;
                        break;
                    }
                }
                break;
            }
        }
    }

    enter_region_view();
}

tz_select_result_t ui_timezone_update(void) {
    if (selection_made) {
        return TZ_SELECT_DONE;
    }

    touch_point_t touch;
    bool pressed = touch_read(&touch);
    ui_list_touch_result_t touch_result =
        ui_list_touch_update(&list_touch, &touch, pressed, list_count(), &list_scroll);

    if (touch_result == UI_LIST_TOUCH_SCROLLED) {
        draw_current_list();
        return TZ_SELECT_CONTINUE;
    }

    if (touch_result != UI_LIST_TOUCH_TAPPED) {
        return TZ_SELECT_CONTINUE;
    }

    const touch_point_t *tap_start = &list_touch.tap_start;

    if (ui_back_button_hit(tap_start)) {
        if (ui_state == TZ_STATE_CITY) {
            enter_region_view();
            return TZ_SELECT_CONTINUE;
        } else if (show_back_button) {
            return TZ_SELECT_CANCELLED;
        }
    }

    int count = list_count();

    if (tap_start->y >= UI_LIST_START_Y && tap_start->y < UI_LIST_START_Y + UI_LIST_VISIBLE * UI_LIST_ITEM_H) {
        int item = (tap_start->y - UI_LIST_START_Y) / UI_LIST_ITEM_H + list_scroll;
        if (item < count) {
            if (ui_state == TZ_STATE_REGION) {
                selected_region = item;
                enter_city_view();
            } else {
                selected_tz = city_indices[item];
                selection_made = true;
                return TZ_SELECT_DONE;
            }
        }
    }

    return TZ_SELECT_CONTINUE;
}

const char *ui_timezone_get_selected(void) {
    return timezones[selected_tz].tz;
}

const char *ui_timezone_get_name(void) {
    static char name_buf[64];
    snprintf(name_buf, sizeof(name_buf), "%s/%s",
             timezones[selected_tz].region, timezones[selected_tz].city);
    return name_buf;
}
