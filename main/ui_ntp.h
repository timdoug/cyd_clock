#ifndef UI_NTP_H
#define UI_NTP_H

typedef enum {
    NTP_RESULT_NONE,
    NTP_RESULT_BACK,
} ntp_result_t;

void ui_ntp_init(void);
ntp_result_t ui_ntp_update(void);

#endif // UI_NTP_H
