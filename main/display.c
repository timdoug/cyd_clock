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
static uint8_t glyph_stage_buf[DISPLAY_GLYPH_2X_WIDTH * DISPLAY_GLYPH_2X_HEIGHT * 2];

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


// Antialiased glyph support: all glyph bitmaps store 4-bit alpha per
// pixel, two pixels per byte, high nibble = left pixel. Each blit builds a
// 16-entry bg-to-fg ramp once and indexes it per pixel; the perceptual
// (gamma) shaping of the levels is baked into the generated tables.
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

static void aa_blit(int16_t x, int16_t y, const uint8_t *glyph,
                    int width, int height, int stride,
                    uint16_t fg, uint16_t bg);

// Decode the UTF-8 sequence at s. Writes the codepoint and returns the
// byte length (1-4). Malformed input yields U+FFFD with length 1, so
// rendering degrades to one '?' per bad byte and always makes progress.
static int utf8_decode(const unsigned char *s, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len;
    uint32_t v;
    if ((c & 0xE0) == 0xC0)      { len = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (uint32_t)(s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static int cp_find(const uint16_t *cps, unsigned count, uint32_t cp) {
    if (cp > 0xFFFF) return -1;
    unsigned lo = 0, hi = count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (cps[mid] < cp) lo = mid + 1; else hi = mid;
    }
    return (lo < count && cps[lo] == cp) ? (int)lo : -1;
}

typedef struct {
    const uint8_t *g16;      // 1x rows, 4bpp
    const uint8_t *g32_rle;  // PackBits-compressed 2x rows
    unsigned g32_len;        // 0: no 2x bitmap, pixel-double g16
    uint8_t width;           // 1x advance in pixels
    uint8_t stride16;        // bytes per 1x row
    uint8_t stride32;        // bytes per unpacked 2x row
} glyph_ref_t;

// PackBits: control < 128 copies control+1 literal bytes; control > 128
// repeats the next byte 257-control times; 128 is a no-op.
static void unpack_packbits(const uint8_t *src, unsigned len,
                            uint8_t *dst, unsigned cap) {
    unsigned si = 0, di = 0;
    while (si < len && di < cap) {
        uint8_t c = src[si++];
        if (c < 128) {
            unsigned n = c + 1u;
            while (n-- && si < len && di < cap) dst[di++] = src[si++];
        } else if (c > 128) {
            unsigned n = 257u - c;
            uint8_t v = (si < len) ? src[si++] : 0;
            while (n-- && di < cap) dst[di++] = v;
        }
    }
    while (di < cap) dst[di++] = 0;
}

static uint8_t glyph_2x_unpacked[DISPLAY_GLYPH_2X_BYTES];

static uint8_t glyph_font_width(const display_glyph_font_t *font) {
    if (!font || font->glyph_width == 0 || font->glyph_width > DISPLAY_GLYPH_WIDTH) {
        return DISPLAY_GLYPH_WIDTH;
    }
    return font->glyph_width;
}

// Codepoint to glyph: ASCII directly, then the given per-language font,
// then the base supplementary table, then '?'.
static void resolve_glyph(uint32_t cp, const display_glyph_font_t *font, glyph_ref_t *out) {
    if (cp >= FONT_BASE_ASCII_FIRST && cp < FONT_BASE_ASCII_FIRST + FONT_BASE_ASCII_COUNT) {
        unsigned idx = cp - FONT_BASE_ASCII_FIRST;
        out->g16 = font_base_ascii[idx];
        out->g32_rle = font_base_ascii_2x_rle + font_base_ascii_2x_off[idx];
        out->g32_len = (unsigned)(font_base_ascii_2x_off[idx + 1] - font_base_ascii_2x_off[idx]);
        out->width = FONT_CHAR_WIDTH;
        out->stride16 = FONT_CHAR_WIDTH / 2;
        out->stride32 = FONT_CHAR_WIDTH;
        return;
    }
    if (font && font->codepoints) {
        int i = cp_find(font->codepoints, font->count, cp);
        if (i >= 0) {
            out->g16 = font->glyphs[i];
            out->g32_rle = font->glyphs_2x_rle + font->glyphs_2x_off[i];
            out->g32_len = (unsigned)(font->glyphs_2x_off[i + 1] - font->glyphs_2x_off[i]);
            out->width = font->widths ? font->widths[i] : glyph_font_width(font);
            out->stride16 = DISPLAY_GLYPH_WIDTH / 2;
            out->stride32 = DISPLAY_GLYPH_2X_WIDTH / 2;
            return;
        }
    }
    {
        int i = cp_find(font_base_ext_cp, font_base_ext_count, cp);
        if (i >= 0) {
            out->g16 = font_base_ext[i];
            out->g32_rle = font_base_ext_2x_rle + font_base_ext_2x_off[i];
            out->g32_len = (unsigned)(font_base_ext_2x_off[i + 1] - font_base_ext_2x_off[i]);
            out->width = FONT_CHAR_WIDTH;
            out->stride16 = FONT_CHAR_WIDTH / 2;
            out->stride32 = FONT_CHAR_WIDTH;
            return;
        }
    }
    resolve_glyph('?', NULL, out);
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
    unsigned char uc = (unsigned char)c;
    uint32_t cp = (uc >= FONT_BASE_ASCII_FIRST &&
                   uc < FONT_BASE_ASCII_FIRST + FONT_BASE_ASCII_COUNT) ? uc : '?';
    glyph_ref_t g;
    resolve_glyph(cp, NULL, &g);
    aa_blit(x, y, g.g16, g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
}

// Blit one 4bpp glyph as a single address window write.
static void aa_blit(int16_t x, int16_t y, const uint8_t *glyph,
                    int width, int height, int stride,
                    uint16_t fg, uint16_t bg) {
    if (!glyph || x < 0 || y < 0 ||
        x + width > DISPLAY_WIDTH || y + height > DISPLAY_HEIGHT) {
        return;
    }

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, width, height);
    dc_data();

    int idx = 0;
    for (int row = 0; row < height; row++) {
        const uint8_t *row_px = glyph + row * stride;
        for (int col = 0; col < width; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            glyph_stage_buf[idx++] = color >> 8;
            glyph_stage_buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(glyph_stage_buf, width * height * 2);
}

// Pixel-double a 1x glyph for 2x contexts when a font carries no 2x
// table (the pre-shaped scripts skip 2x bitmaps to save flash).
static void aa_blit_doubled(int16_t x, int16_t y, const uint8_t *glyph,
                            int width, int height, int stride,
                            uint16_t fg, uint16_t bg) {
    if (!glyph || x < 0 || y < 0 ||
        x + width * 2 > DISPLAY_WIDTH || y + height * 2 > DISPLAY_HEIGHT) {
        return;
    }

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x, y, width * 2, height * 2);
    dc_data();

    int idx = 0;
    for (int row = 0; row < height; row++) {
        const uint8_t *row_px = glyph + row * stride;
        for (int dup = 0; dup < 2; dup++) {
            for (int col = 0; col < width; col++) {
                uint16_t color = palette[aa_alpha_at(row_px, col)];
                glyph_stage_buf[idx++] = color >> 8;
                glyph_stage_buf[idx++] = color & 0xFF;
                glyph_stage_buf[idx++] = color >> 8;
                glyph_stage_buf[idx++] = color & 0xFF;
            }
        }
    }
    spi_write_bytes(glyph_stage_buf, width * 2 * height * 2 * 2);
}

void display_set_glyph_font(const display_glyph_font_t *font) {
    active_glyph_font = font;
}

int display_text_width_font(const char *str, const display_glyph_font_t *font) {
    int width = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp;
        glyph_ref_t g;
        p += utf8_decode(p, &cp);
        resolve_glyph(cp, font, &g);
        width += g.width;
    }
    return width;
}

int display_text_width(const char *str) {
    return display_text_width_font(str, active_glyph_font);
}

int display_cell_at(const char *s, int *width_px) {
    uint32_t cp;
    glyph_ref_t g;
    int len = utf8_decode((const unsigned char *)s, &cp);
    resolve_glyph(cp, active_glyph_font, &g);
    *width_px = g.width;
    return len;
}

void display_string(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg) {
    display_string_font(x, y, str, active_glyph_font, fg, bg);
}

void display_string_font(int16_t x, int16_t y, const char *str,
                         const display_glyph_font_t *font,
                         uint16_t fg, uint16_t bg) {
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp;
        glyph_ref_t g;
        p += utf8_decode(p, &cp);
        resolve_glyph(cp, font, &g);
        aa_blit(x, y, g.g16, g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
        x += g.width;
    }
}

void display_string_2x(int16_t x, int16_t y, const char *str, uint16_t fg, uint16_t bg) {
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp;
        glyph_ref_t g;
        p += utf8_decode(p, &cp);
        resolve_glyph(cp, active_glyph_font, &g);
        if (g.g32_len) {
            unpack_packbits(g.g32_rle, g.g32_len, glyph_2x_unpacked,
                            (unsigned)g.stride32 * FONT_CHAR_HEIGHT_2X);
            aa_blit(x, y, glyph_2x_unpacked, g.width * 2, FONT_CHAR_HEIGHT_2X,
                    g.stride32, fg, bg);
        } else {
            aa_blit_doubled(x, y, g.g16, g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
        }
        x += g.width * 2;
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
