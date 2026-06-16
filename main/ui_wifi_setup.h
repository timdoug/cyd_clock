#ifndef UI_WIFI_SETUP_H
#define UI_WIFI_SETUP_H

#include <stdbool.h>

typedef enum {
    WIFI_SETUP_CONTINUE,
    WIFI_SETUP_CONNECTED,
    WIFI_SETUP_CANCELLED,
} wifi_setup_result_t;

void ui_wifi_setup_init(bool show_back);

wifi_setup_result_t ui_wifi_setup_update(void);

void ui_wifi_setup_get_credentials(char *ssid, char *password);

#endif
