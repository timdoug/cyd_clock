#ifndef UI_TIMEZONE_H
#define UI_TIMEZONE_H

#include <stdbool.h>

// Result of timezone selection
typedef enum {
    TZ_SELECT_CONTINUE,   // Still selecting
    TZ_SELECT_DONE,       // Selection made
    TZ_SELECT_CANCELLED,  // User cancelled
} tz_select_result_t;

// Initialize timezone selector UI
void ui_timezone_init(void);

// Run one iteration of timezone selector (call in loop)
tz_select_result_t ui_timezone_update(void);

// Get the selected timezone string (POSIX format)
const char *ui_timezone_get_selected(void);

// Get the selected timezone display name
const char *ui_timezone_get_name(void);

#endif // UI_TIMEZONE_H
