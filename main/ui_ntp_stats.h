#ifndef UI_NTP_STATS_H
#define UI_NTP_STATS_H

typedef enum {
    NTP_STATS_RESULT_NONE,
    NTP_STATS_RESULT_BACK,
    NTP_STATS_RESULT_SETTINGS,
} ntp_stats_result_t;

void ui_ntp_stats_init(void);
ntp_stats_result_t ui_ntp_stats_update(void);

#endif // UI_NTP_STATS_H
