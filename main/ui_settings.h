#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

typedef enum {
    SETTINGS_RESULT_NONE,
    SETTINGS_RESULT_TIMEZONE,
    SETTINGS_RESULT_WIFI,
    SETTINGS_RESULT_NTP,
    SETTINGS_RESULT_ABOUT,
    SETTINGS_RESULT_DONE,
} settings_result_t;

void ui_settings_init(void);
settings_result_t ui_settings_update(void);

#endif // UI_SETTINGS_H
