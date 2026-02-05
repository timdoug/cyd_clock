#ifndef UI_ABOUT_H
#define UI_ABOUT_H

typedef enum {
    ABOUT_RESULT_NONE,
    ABOUT_RESULT_BACK,
} about_result_t;

void ui_about_init(void);
about_result_t ui_about_update(void);

#endif // UI_ABOUT_H
