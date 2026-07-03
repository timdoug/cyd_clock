#include "display.h"
#include "fonts.h"
#include <string.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "config.h"

static const char *TAG = "display";
static const display_glyph_font_t *active_glyph_font = NULL;
static uint8_t glyph_buf[DISPLAY_GLYPH_WIDTH * DISPLAY_GLYPH_HEIGHT * 2];
static uint8_t glyph_2x_buf[DISPLAY_GLYPH_2X_WIDTH * DISPLAY_GLYPH_2X_HEIGHT * 2];

// ILI9341 commands
#define ILI9341_SWRESET    0x01
#define ILI9341_SLPOUT     0x11
#define ILI9341_DISPON     0x29
#define ILI9341_CASET      0x2A
#define ILI9341_PASET      0x2B
#define ILI9341_RAMWR      0x2C
#define ILI9341_MADCTL     0x36
#define ILI9341_PIXFMT     0x3A

// MADCTL bits
#define MADCTL_MY  0x80
#define MADCTL_MX  0x40
#define MADCTL_MV  0x20
#define MADCTL_BGR 0x08

// MADCTL from the board's panel flags. MADCTL_BASE is landscape; MADCTL_FLIPPED
// is the 180-degree rotation, which toggles both axis-mirror bits.
#if BOARD_PANEL_BGR
#define MADCTL_COLOR    MADCTL_BGR
#else
#define MADCTL_COLOR    0
#endif
#if BOARD_PANEL_MIRROR_X
#define MADCTL_XFLIP    MADCTL_MX
#else
#define MADCTL_XFLIP    0
#endif
#define MADCTL_BASE     (MADCTL_MV | MADCTL_COLOR | MADCTL_XFLIP)
#define MADCTL_FLIPPED  (MADCTL_BASE ^ (MADCTL_MX | MADCTL_MY))

static spi_device_handle_t spi_dev;
static bool display_rotated = false;

// Payloads below 0x80 are rejected so a token pair mangled by byte-oriented
// truncation (escape + stray ASCII byte) degrades to the '?' fallback
// instead of indexing an arbitrary glyph.
static inline bool glyph_token_id(unsigned char payload, unsigned int *glyph_id) {
    if (payload >= 0x80) {
        *glyph_id = payload - 0x80;
        return true;
    }
    return false;
}

// Antialiased glyph support: all glyph bitmaps (built-in and supplemental)
// store 4-bit alpha per pixel, two pixels per byte, high nibble = left
// pixel. Each blit builds a 16-entry bg-to-fg ramp once and indexes it per
// pixel; the perceptual (gamma) shaping of the levels is baked into the
// generated tables.
static void aa_build_palette(uint16_t fg, uint16_t bg, uint16_t palette[16]) {
    int fr = (fg >> 11) & 0x1F, fgr = (fg >> 5) & 0x3F, fb = fg & 0x1F;
    int br = (bg >> 11) & 0x1F, bgr = (bg >> 5) & 0x3F, bb = bg & 0x1F;
    for (int i = 0; i < 16; i++) {
        int r = (br * (15 - i) + fr * i + 7) / 15;
        int g = (bgr * (15 - i) + fgr * i + 7) / 15;
        int b = (bb * (15 - i) + fb * i + 7) / 15;
        palette[i] = (uint16_t)((r << 11) | (g << 5) | b);
    }
}

static inline uint8_t aa_alpha_at(const uint8_t *row, int col) {
    uint8_t b = row[col >> 1];
    return (col & 1) ? (b & 0x0F) : (b >> 4);
}

// Resolve a byte to its glyph index in the generated built-in font:
// printable ASCII plus the populated high slots; empty high slots and
// control bytes fall back to '?'.
static int font_glyph_index(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc < FONT_BUILTIN_FIRST) return '?' - FONT_BUILTIN_FIRST;
    int idx = uc - FONT_BUILTIN_FIRST;
    if (uc >= 0x80) {
        const uint8_t *glyph = font_builtin[idx];
        bool populated = false;
        for (int i = 0; i < FONT_BUILTIN_GLYPH_BYTES; i++) {
            if (glyph[i] != 0) { populated = true; break; }
        }
        if (!populated) idx = '?' - FONT_BUILTIN_FIRST;
    }
    return idx;
}

// 7-segment patterns for digits 0-9 and dash
// Segments: bit 0=top, 1=top-right, 2=bottom-right, 3=bottom, 4=bottom-left, 5=top-left, 6=middle
static const uint8_t seg7_patterns[11] = {
    0b00111111, // 0
    0b00000110, // 1
    0b01011011, // 2
    0b01001111, // 3
    0b01100110, // 4
    0b01101101, // 5
    0b01111100, // 6 (no top)
    0b00000111, // 7
    0b01111111, // 8
    0b01100111, // 9 (no bottom)
    0b01000000, // 10 = dash (middle segment only)
};

static void dc_command(void) {
    gpio_set_level(PIN_DC, 0);
}

static void dc_data(void) {
    gpio_set_level(PIN_DC, 1);
}

static void spi_write_byte(uint8_t data) {
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &data,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void spi_write_bytes(const uint8_t *data, size_t len) {
    if (len == 0) return;
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit(spi_dev, &t);
}

static void write_command(uint8_t cmd) {
    dc_command();
    spi_write_byte(cmd);
}

static void write_data(uint8_t data) {
    dc_data();
    spi_write_byte(data);
}

static void set_addr_window(int16_t x, int16_t y, int16_t w, int16_t h) {
    write_command(ILI9341_CASET);
    dc_data();
    uint8_t ca[] = {(uint8_t)(x >> 8), (uint8_t)x, (uint8_t)((x + w - 1) >> 8), (uint8_t)(x + w - 1)};
    spi_write_bytes(ca, 4);

    write_command(ILI9341_PASET);
    dc_data();
    uint8_t pa[] = {(uint8_t)(y >> 8), (uint8_t)y, (uint8_t)((y + h - 1) >> 8), (uint8_t)(y + h - 1)};
    spi_write_bytes(pa, 4);

    write_command(ILI9341_RAMWR);
}

void display_init(void) {
    ESP_LOGI(TAG, "Initializing display");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_DC),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_dev));

    write_command(ILI9341_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(150));

    write_command(ILI9341_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(150));

    write_command(ILI9341_PIXFMT);
    write_data(0x55);

    // NOTE: do not touch FRMCTR1 (0xB1). Raising the refresh to the
    // datasheet-max ~119 Hz to shrink GRAM-to-glass scan latency was tried
    // and washed the panel out badly: at 16 clocks/line the source drivers
    // on this clone panel don't settle, visibly killing contrast. The
    // default 70 Hz scan is the price of a readable display.

    write_command(ILI9341_MADCTL);
    write_data(MADCTL_BASE);

    write_command(ILI9341_DISPON);
    vTaskDelay(pdMS_TO_TICKS(100));

    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = PIN_BL,
        // Boot at the default brightness through the same gamma curve the
        // settings slider uses, so first boot (no stored value) matches what
        // display_set_backlight would paint for BRIGHTNESS_DEFAULT.
        .duty = gamma_correct(BRIGHTNESS_DEFAULT),
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);

    display_fill(COLOR_BLACK);
    ESP_LOGI(TAG, "Display initialized");
}

void display_fill(uint16_t color) {
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, color);
}

void display_fill_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x + w > DISPLAY_WIDTH) w = DISPLAY_WIDTH - x;
    if (y + h > DISPLAY_HEIGHT) h = DISPLAY_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    set_addr_window(x, y, w, h);
    dc_data();

    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;

    #define FILL_BUF_PIXELS 256
    static uint8_t buf[FILL_BUF_PIXELS * 2];
    for (int i = 0; i < FILL_BUF_PIXELS; i++) {
        buf[i * 2] = hi;
        buf[i * 2 + 1] = lo;
    }

    int32_t total = (int32_t)w * h;
    while (total > 0) {
        int chunk = (total > FILL_BUF_PIXELS) ? FILL_BUF_PIXELS : total;
        spi_write_bytes(buf, chunk * 2);
        total -= chunk;
    }
}


void display_hline(int16_t x, int16_t y, int16_t w, uint16_t color) {
    display_fill_rect(x, y, w, 1, color);
}

void display_vline(int16_t x, int16_t y, int16_t h, uint16_t color) {
    display_fill_rect(x, y, 1, h, color);
}

void display_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    display_hline(x, y, w, color);
    display_hline(x, y + h - 1, w, color);
    display_vline(x, y, h, color);
    display_vline(x + w - 1, y, h, color);
}

void display_char(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg) {
    if (x < 0 || y < 0 ||
        x + FONT_CHAR_WIDTH > DISPLAY_WIDTH ||
        y + FONT_CHAR_HEIGHT > DISPLAY_HEIGHT) {
        return;
    }
    const uint8_t *glyph = font_builtin[font_glyph_index(c)];

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, FONT_CHAR_WIDTH, FONT_CHAR_HEIGHT);
    dc_data();

    uint8_t buf[FONT_CHAR_WIDTH * FONT_CHAR_HEIGHT * 2];
    int idx = 0;
    for (int row = 0; row < FONT_CHAR_HEIGHT; row++) {
        const uint8_t *row_px = glyph + row * (FONT_CHAR_WIDTH / 2);
        for (int col = 0; col < FONT_CHAR_WIDTH; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            buf[idx++] = color >> 8;
            buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(buf, sizeof(buf));
}

static uint8_t glyph_font_width(const display_glyph_font_t *font) {
    if (!font || font->glyph_width == 0 || font->glyph_width > DISPLAY_GLYPH_WIDTH) {
        return DISPLAY_GLYPH_WIDTH;
    }
    return font->glyph_width;
}

static void display_glyph(int16_t x, int16_t y, const uint8_t glyph[DISPLAY_GLYPH_BYTES],
                              uint8_t glyph_width, uint16_t fg, uint16_t bg) {
    if (!glyph || x < 0 || y < 0 ||
        x + glyph_width > DISPLAY_WIDTH ||
        y + DISPLAY_GLYPH_HEIGHT > DISPLAY_HEIGHT) {
        return;
    }

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, glyph_width, DISPLAY_GLYPH_HEIGHT);
    dc_data();

    int idx = 0;
    for (int row = 0; row < DISPLAY_GLYPH_HEIGHT; row++) {
        const uint8_t *row_px = glyph + row * (DISPLAY_GLYPH_WIDTH / 2);
        for (int col = 0; col < glyph_width; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            glyph_buf[idx++] = color >> 8;
            glyph_buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(glyph_buf, glyph_width * DISPLAY_GLYPH_HEIGHT * 2);
}

static void display_glyph_2x(int16_t x, int16_t y, const uint8_t glyph[DISPLAY_GLYPH_BYTES],
                                 uint8_t glyph_width, uint16_t fg, uint16_t bg) {
    uint8_t width_2x = glyph_width * 2;
    if (!glyph || x < 0 || y < 0 ||
        x + width_2x > DISPLAY_WIDTH ||
        y + DISPLAY_GLYPH_HEIGHT * 2 > DISPLAY_HEIGHT) {
        return;
    }

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, width_2x, DISPLAY_GLYPH_HEIGHT * 2);
    dc_data();

    int idx = 0;
    for (int row = 0; row < DISPLAY_GLYPH_HEIGHT; row++) {
        const uint8_t *row_px = glyph + row * (DISPLAY_GLYPH_WIDTH / 2);
        for (int dup = 0; dup < 2; dup++) {
            for (int col = 0; col < glyph_width; col++) {
                uint16_t color = palette[aa_alpha_at(row_px, col)];
                glyph_2x_buf[idx++] = color >> 8;
                glyph_2x_buf[idx++] = color & 0xFF;
                glyph_2x_buf[idx++] = color >> 8;
                glyph_2x_buf[idx++] = color & 0xFF;
            }
        }
    }
    spi_write_bytes(glyph_2x_buf, width_2x * DISPLAY_GLYPH_HEIGHT * 2 * 2);
}

static void display_glyph_32(int16_t x, int16_t y,
                                 const uint8_t glyph[DISPLAY_GLYPH_2X_BYTES],
                                 uint8_t glyph_width, uint16_t fg, uint16_t bg) {
    uint8_t width_2x = glyph_width * 2;
    if (!glyph || x < 0 || y < 0 ||
        x + width_2x > DISPLAY_WIDTH ||
        y + DISPLAY_GLYPH_2X_HEIGHT > DISPLAY_HEIGHT) {
        return;
    }

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, width_2x, DISPLAY_GLYPH_2X_HEIGHT);
    dc_data();

    int idx = 0;
    for (int row = 0; row < DISPLAY_GLYPH_2X_HEIGHT; row++) {
        const uint8_t *row_px = glyph + row * (DISPLAY_GLYPH_2X_WIDTH / 2);
        for (int col = 0; col < width_2x; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            glyph_2x_buf[idx++] = color >> 8;
            glyph_2x_buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(glyph_2x_buf, width_2x * DISPLAY_GLYPH_2X_HEIGHT * 2);
}

void display_set_glyph_font(const display_glyph_font_t *font) {
    active_glyph_font = font;
}

int display_text_width_font(const char *str, const display_glyph_font_t *font) {
    int width = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p == (unsigned char)DISPLAY_GLYPH_ESCAPE && p[1] != '\0') {
            unsigned int glyph_id = 0;
            bool valid_id = glyph_token_id(p[1], &glyph_id);
            if (valid_id && font && glyph_id < font->count) {
                width += glyph_font_width(font);
            } else {
                width += FONT_CHAR_WIDTH;
            }
            p += 2;
        } else {
            width += FONT_CHAR_WIDTH;
            p++;
        }
    }
    return width;
}

int display_text_width(const char *str) {
    return display_text_width_font(str, active_glyph_font);
}

void display_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg) {
    display_string_font(x, y, str, active_glyph_font, fg, bg);
}

void display_string_font(int16_t x, int16_t y, const char *str,
                         const display_glyph_font_t *font,
                         uint16_t fg, uint16_t bg) {
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        if (*p == (unsigned char)DISPLAY_GLYPH_ESCAPE && p[1] != '\0') {
            unsigned int glyph_id = 0;
            bool valid_id = glyph_token_id(p[1], &glyph_id);
            if (valid_id && font && glyph_id < font->count) {
                uint8_t glyph_width = glyph_font_width(font);
                display_glyph(x, y, font->glyphs[glyph_id], glyph_width, fg, bg);
                x += glyph_width;
            } else {
                display_char(x, y, '?', fg, bg);
                x += FONT_CHAR_WIDTH;
            }
            p += 2;
        } else {
            display_char(x, y, (char)*p++, fg, bg);
            x += FONT_CHAR_WIDTH;
        }
    }
}

static void display_char_2x(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg) {
    if (x < 0 || y < 0 ||
        x + FONT_CHAR_WIDTH_2X > DISPLAY_WIDTH ||
        y + FONT_CHAR_HEIGHT_2X > DISPLAY_HEIGHT) {
        return;
    }
    const uint8_t *glyph = font_builtin_2x[font_glyph_index(c)];

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, FONT_CHAR_WIDTH_2X, FONT_CHAR_HEIGHT_2X);
    dc_data();

    uint8_t buf[FONT_CHAR_WIDTH_2X * FONT_CHAR_HEIGHT_2X * 2];
    int idx = 0;
    for (int row = 0; row < FONT_CHAR_HEIGHT_2X; row++) {
        const uint8_t *row_px = glyph + row * (FONT_CHAR_WIDTH_2X / 2);
        for (int col = 0; col < FONT_CHAR_WIDTH_2X; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            buf[idx++] = color >> 8;
            buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(buf, sizeof(buf));
}

void display_string_2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg) {
    while (*str) {
        if ((unsigned char)*str == (unsigned char)DISPLAY_GLYPH_ESCAPE && str[1] != '\0') {
            unsigned int glyph_id = 0;
            bool valid_id = glyph_token_id((unsigned char)str[1], &glyph_id);
            if (valid_id && active_glyph_font && glyph_id < active_glyph_font->count) {
                uint8_t glyph_width = glyph_font_width(active_glyph_font);
                if (active_glyph_font->glyphs_2x) {
                    display_glyph_32(x, y, active_glyph_font->glyphs_2x[glyph_id],
                                         glyph_width, fg, bg);
                } else {
                    display_glyph_2x(x, y, active_glyph_font->glyphs[glyph_id],
                                         glyph_width, fg, bg);
                }
                x += glyph_width * 2;
            } else {
                display_char_2x(x, y, '?', fg, bg);
                x += FONT_CHAR_WIDTH_2X;
            }
            str += 2;
        } else {
            display_char_2x(x, y, *str++, fg, bg);
            x += FONT_CHAR_WIDTH_2X;
        }
    }
}

// 7-segment digits are composed into this RAM buffer and pushed to the panel
// as a single address window + one SPI transaction. The previous path drew
// each segment as `thick` 1-px hlines/vlines, each a full set_addr_window +
// data write: ~250 polling transactions per digit at 10-20 us fixed overhead
// each, ~9.5 ms for a digit whose raw pixel payload is ~1.2 ms at 40 MHz.
// One transaction recovers that overhead and shrinks the window in which the
// panel scan can catch a half-drawn digit (tearing) by the same factor.
// Sized for the size-3 digit: (seg_len + thick) x (2*seg_len + 2*thick - 1).
static uint8_t digit_buf[(48 + 8) * (2 * 48 + 2 * 8 - 1) * 2];

static void buf_hline(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                      int16_t w, uint16_t color) {
    uint8_t hi = color >> 8, lo = color & 0xFF;
    uint8_t *p = buf + ((size_t)y * buf_w + x) * 2;
    for (int16_t i = 0; i < w; i++) {
        *p++ = hi;
        *p++ = lo;
    }
}

static void buf_vline(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                      int16_t h, uint16_t color) {
    uint8_t hi = color >> 8, lo = color & 0xFF;
    uint8_t *p = buf + ((size_t)y * buf_w + x) * 2;
    for (int16_t i = 0; i < h; i++) {
        p[0] = hi;
        p[1] = lo;
        p += (size_t)buf_w * 2;
    }
}

// Segment shapes with pointed ends; geometry identical to the old
// draw_segment_h/draw_segment_v, just rendered into the compose buffer.
static void buf_segment_h(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                          int16_t w, int16_t thick, uint16_t color) {
    for (int t = 0; t < thick; t++) {
        int inset = (t < thick / 2) ? (thick / 2 - t) : (t - thick / 2);
        buf_hline(buf, buf_w, x + inset, y + t, w - 2 * inset, color);
    }
}

static void buf_segment_v(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                          int16_t h, int16_t thick, uint16_t color) {
    for (int t = 0; t < thick; t++) {
        int inset = (t < thick / 2) ? (thick / 2 - t) : (t - thick / 2);
        buf_vline(buf, buf_w, x + t, y + inset, h - 2 * inset, color);
    }
}

void display_digit_7seg(int16_t x, int16_t y, uint8_t digit, uint8_t size, uint16_t color, uint16_t bg) {
    if (digit > 10) return;

    int16_t seg_len, seg_thick, gap;
    switch (size) {
        case 1: seg_len = 16; seg_thick = 4; gap = 1; break;
        case 2: seg_len = 32; seg_thick = 6; gap = 2; break;
        case 3:
        default: seg_len = 48; seg_thick = 8; gap = 2; break;
    }

    int16_t w = seg_len + seg_thick;
    int16_t h = 2 * seg_len + 2 * seg_thick - 1;
    if (x < 0 || y < 0 || x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) return;

    uint8_t pattern = seg7_patterns[digit];

    int16_t h_len = seg_len - gap * 2;
    int16_t v_len = seg_len - gap;

    // Background-fill the buffer; "off" segments need no explicit erase since
    // the full bounding box is pushed every time (same no-flash behavior).
    uint8_t hi = bg >> 8, lo = bg & 0xFF;
    int32_t total = (int32_t)w * h;
    for (int32_t i = 0; i < total; i++) {
        digit_buf[i * 2]     = hi;
        digit_buf[i * 2 + 1] = lo;
    }

    if (pattern & 0x01) buf_segment_h(digit_buf, w, seg_thick / 2 + gap, 0, h_len, seg_thick, color);
    if (pattern & 0x02) buf_segment_v(digit_buf, w, seg_len, seg_thick / 2 + gap, v_len, seg_thick, color);
    if (pattern & 0x04) buf_segment_v(digit_buf, w, seg_len, seg_len + seg_thick / 2 + gap * 2 + 1, v_len, seg_thick, color);
    if (pattern & 0x08) buf_segment_h(digit_buf, w, seg_thick / 2 + gap, seg_len * 2 + seg_thick - 1, h_len, seg_thick, color);
    if (pattern & 0x10) buf_segment_v(digit_buf, w, 0, seg_len + seg_thick / 2 + gap * 2 + 1, v_len, seg_thick, color);
    if (pattern & 0x20) buf_segment_v(digit_buf, w, 0, seg_thick / 2 + gap, v_len, seg_thick, color);
    if (pattern & 0x40) buf_segment_h(digit_buf, w, seg_thick / 2 + gap, seg_len + seg_thick / 2 - 1, h_len, seg_thick, color);

    set_addr_window(x, y, w, h);
    dc_data();
    spi_write_bytes(digit_buf, (size_t)w * h * 2);
}

void display_colon_7seg(int16_t x, int16_t y, uint8_t size, uint16_t color) {
    int16_t seg_len, seg_thick, dot_size;
    switch (size) {
        case 1: seg_len = 16; seg_thick = 4; dot_size = 4; break;
        case 2: seg_len = 32; seg_thick = 6; dot_size = 6; break;
        case 3:
        default: seg_len = 48; seg_thick = 8; dot_size = 8; break;
    }

    display_fill_rect(x + 2, y + seg_len / 2 + seg_thick / 2, dot_size, dot_size, color);
    display_fill_rect(x + 2, y + seg_len + seg_len / 2 + seg_thick, dot_size, dot_size, color);
}

void display_set_backlight(uint8_t brightness) {
    uint8_t corrected = gamma_correct(brightness);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, corrected);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_set_rotation(bool rotated) {
    display_rotated = rotated;
    write_command(ILI9341_MADCTL);
    write_data(rotated ? MADCTL_FLIPPED : MADCTL_BASE);
}

bool display_is_rotated(void) {
    return display_rotated;
}
