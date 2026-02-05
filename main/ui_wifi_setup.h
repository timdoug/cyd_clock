#ifndef UI_WIFI_SETUP_H
#define UI_WIFI_SETUP_H

#include <stdbool.h>

// Result of WiFi setup UI
typedef enum {
    WIFI_SETUP_CONTINUE,  // Still in setup mode
    WIFI_SETUP_CONNECTED, // Successfully connected
    WIFI_SETUP_CANCELLED, // User cancelled
} wifi_setup_result_t;

// Initialize WiFi setup UI
void ui_wifi_setup_init(void);

// Run one iteration of WiFi setup UI (call in loop)
// Returns the current state
wifi_setup_result_t ui_wifi_setup_update(void);

// Get the connected SSID and password (valid after WIFI_SETUP_CONNECTED)
void ui_wifi_setup_get_credentials(char *ssid, char *password);

#endif // UI_WIFI_SETUP_H
