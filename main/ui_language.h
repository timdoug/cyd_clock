#ifndef UI_LANGUAGE_H
#define UI_LANGUAGE_H

typedef enum {
    LANGUAGE_RESULT_NONE,
    LANGUAGE_RESULT_BACK,
} language_result_t;

void ui_language_init(void);
language_result_t ui_language_update(void);

#endif
