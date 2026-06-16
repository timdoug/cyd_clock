#ifndef UI_TIMEZONE_H
#define UI_TIMEZONE_H

#include <stdbool.h>

typedef enum {
    TZ_SELECT_CONTINUE,
    TZ_SELECT_DONE,
    TZ_SELECT_CANCELLED,
} tz_select_result_t;

void ui_timezone_init(const char *current_tz, bool show_back);

tz_select_result_t ui_timezone_update(void);

const char *ui_timezone_get_selected(void);

const char *ui_timezone_get_name(void);

#endif
