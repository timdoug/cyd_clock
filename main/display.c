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


// Antialiased glyph support: all glyph bitmaps store 4-bit linear
// coverage per pixel, two pixels per byte, high nibble = left pixel.
// Each blit builds a 16-entry bg-to-fg ramp once and indexes it per
// pixel. The ramp is mixed in linear light (panel gamma ~2.2): lerping
// the gamma-encoded RGB565 channels directly puts mid-coverage pixels
// at the wrong brightness by an amount that depends on the fg/bg pair,
// which no fixed shaping baked into the glyph data can compensate for
// every background color.

// Gamma-encoded 5/6-bit channel -> linear light, round(16383*(i/max)^2.2).
static const uint16_t aa_lin5[32] = {
    0, 9, 39, 96, 181, 296, 442, 620,
    832, 1078, 1360, 1677, 2030, 2421, 2850, 3317,
    3824, 4369, 4954, 5580, 6247, 6955, 7704, 8496,
    9330, 10206, 11126, 12089, 13096, 14147, 15243, 16383,
};
static const uint16_t aa_lin6[64] = {
    0, 2, 8, 20, 38, 62, 93, 130,
    175, 227, 286, 352, 427, 509, 599, 697,
    803, 918, 1041, 1172, 1313, 1461, 1619, 1785,
    1960, 2144, 2338, 2540, 2752, 2972, 3203, 3442,
    3691, 3950, 4218, 4496, 4783, 5080, 5387, 5704,
    6031, 6367, 6714, 7071, 7438, 7815, 8202, 8599,
    9007, 9425, 9853, 10292, 10741, 11201, 11671, 12152,
    12643, 13145, 13658, 14181, 14716, 15261, 15816, 16383,
};

// Nearest gamma-encoded channel value for a linear-light target.
static int aa_encode(const uint16_t *tab, int n, int lin) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid] < lin) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && lin - tab[lo - 1] < tab[lo] - lin) lo--;
    return lo;
}

static void aa_build_palette(uint16_t fg, uint16_t bg, uint16_t palette[16]) {
    // Memoize the last fg/bg pair: strings blit one glyph at a time with the
    // same colors, and the 96 aa_encode bisections per glyph were the bulk of
    // the per-tick redraw cost. All drawing happens on the main task.
    static uint16_t cached[16];
    static uint16_t last_fg, last_bg;
    static bool     have;
    if (!have || fg != last_fg || bg != last_bg) {
        int fr = aa_lin5[(fg >> 11) & 0x1F], fgr = aa_lin6[(fg >> 5) & 0x3F], fb = aa_lin5[fg & 0x1F];
        int br = aa_lin5[(bg >> 11) & 0x1F], bgr = aa_lin6[(bg >> 5) & 0x3F], bb = aa_lin5[bg & 0x1F];
        for (int i = 0; i < 16; i++) {
            int r = aa_encode(aa_lin5, 32, (br * (15 - i) + fr * i + 7) / 15);
            int g = aa_encode(aa_lin6, 64, (bgr * (15 - i) + fgr * i + 7) / 15);
            int b = aa_encode(aa_lin5, 32, (bb * (15 - i) + fb * i + 7) / 15);
            cached[i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
        last_fg = fg;
        last_bg = bg;
        have = true;
    }
    memcpy(palette, cached, sizeof(cached));
}

static inline uint8_t aa_alpha_at(const uint8_t *row, int col) {
    uint8_t b = row[col >> 1];
    return (col & 1) ? (b & 0x0F) : (b >> 4);
}

static void aa_blit(int16_t x, int16_t y, const uint8_t *glyph,
                    int width, int height, int stride,
                    uint16_t fg, uint16_t bg);
static void display_string_font(int16_t x, int16_t y, const char *str,
                                const display_glyph_font_t *font,
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
    const uint8_t *g16_rle;  // PackBits-compressed 1x rows
    unsigned g16_len;
    const uint8_t *g32_rle;  // PackBits-compressed 2x rows
    unsigned g32_len;        // 0: no 2x bitmap, pixel-double 1x
    uint8_t width;           // 1x advance in pixels
    uint8_t stride16;        // bytes per unpacked 1x row
    uint8_t stride32;        // bytes per unpacked 2x row
} glyph_ref_t;

// Raw-DEFLATE inflater restricted to what the generator emits: one
// stored or fixed-Huffman block per glyph. No dynamic-Huffman support
// and no window buffer (the output itself is the window; matches never
// reach back further than the glyph).
typedef struct {
    const uint8_t *src;
    unsigned len;
    unsigned pos;   // byte position
    unsigned bit;   // bit position within src[pos]
} bit_reader_t;

static unsigned get_bits(bit_reader_t *br, unsigned n) {
    unsigned v = 0;
    for (unsigned k = 0; k < n; k++) {
        unsigned b = 0;
        if (br->pos < br->len) {
            b = (br->src[br->pos] >> br->bit) & 1u;
        }
        if (++br->bit == 8) { br->bit = 0; br->pos++; }
        v |= b << k;
    }
    return v;
}

// Fixed-Huffman literal/length symbol: codes are read MSB-first.
static unsigned get_fixed_litlen(bit_reader_t *br) {
    unsigned code = 0;
    for (int k = 0; k < 7; k++) code = (code << 1) | get_bits(br, 1);
    if (code <= 0x17) return 256 + code;
    code = (code << 1) | get_bits(br, 1);
    if (code >= 0x30 && code <= 0xBF) return code - 0x30;
    if (code >= 0xC0 && code <= 0xC7) return 280 + code - 0xC0;
    code = (code << 1) | get_bits(br, 1);
    return 144 + code - 0x190;
}

static const uint16_t inflate_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const uint8_t inflate_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
static const uint16_t inflate_dist_base[20] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769,
};
static const uint8_t inflate_dist_extra[20] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
};

static void unpack_packbits(const uint8_t *src, unsigned len,
                            uint8_t *dst, unsigned cap) {
    bit_reader_t br = { src, len, 0, 0 };
    unsigned di = 0;

    get_bits(&br, 1);                 // BFINAL (single block per glyph)
    unsigned btype = get_bits(&br, 2);
    if (btype == 0) {                 // stored
        if (br.bit) { br.bit = 0; br.pos++; }
        unsigned n = get_bits(&br, 16);
        get_bits(&br, 16);            // NLEN, trusted (generated data)
        while (n-- && br.pos < br.len && di < cap) dst[di++] = src[br.pos++];
    } else if (btype == 1) {          // fixed Huffman
        for (;;) {
            unsigned sym = get_fixed_litlen(&br);
            if (sym == 256 || br.pos >= br.len) break;
            if (sym < 256) {
                if (di < cap) dst[di++] = (uint8_t)sym;
                continue;
            }
            unsigned ls = sym - 257;
            if (ls >= 29) break;
            unsigned mlen = inflate_len_base[ls] + get_bits(&br, inflate_len_extra[ls]);
            unsigned ds = 0;
            for (int k = 0; k < 5; k++) ds = (ds << 1) | get_bits(&br, 1);
            if (ds >= 20) break;
            unsigned dist = inflate_dist_base[ds] + get_bits(&br, inflate_dist_extra[ds]);
            if (dist > di) break;
            while (mlen-- && di < cap) { dst[di] = dst[di - dist]; di++; }
        }
    }
    while (di < cap) dst[di++] = 0;
}

static uint8_t glyph_2x_unpacked[DISPLAY_GLYPH_2X_BYTES];

// Recently decoded 1x glyphs, keyed by compressed-stream address (which
// uniquely identifies a glyph: dedup gives repeated glyphs the same
// offset). The centisecond fraction and the stats rows redraw the same
// handful of glyphs every tick; a hit skips the ~20us inflate. 2x is not
// cached - it only draws for the date and the waiting banner.
#define GLYPH_CACHE_SLOTS 24
static struct {
    const uint8_t *key;
    uint64_t stamp;   // 64-bit so the LRU clock never wraps in practice
    uint8_t data[DISPLAY_GLYPH_BYTES];
} glyph_cache[GLYPH_CACHE_SLOTS];
static uint64_t glyph_cache_clock;

static const uint8_t *unpack_1x(const glyph_ref_t *g) {
    int victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < GLYPH_CACHE_SLOTS; i++) {
        if (glyph_cache[i].key == g->g16_rle) {
            glyph_cache[i].stamp = ++glyph_cache_clock;
            return glyph_cache[i].data;
        }
        if (glyph_cache[i].stamp < oldest) {
            oldest = glyph_cache[i].stamp;
            victim = i;
        }
    }
    unpack_packbits(g->g16_rle, g->g16_len, glyph_cache[victim].data,
                    (unsigned)g->stride16 * FONT_CHAR_HEIGHT);
    glyph_cache[victim].key = g->g16_rle;
    glyph_cache[victim].stamp = ++glyph_cache_clock;
    return glyph_cache[victim].data;
}

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
        out->g16_rle = font_base_ascii_rle + font_base_ascii_off[idx];
        out->g16_len = (unsigned)(font_base_ascii_off[idx + 1] - font_base_ascii_off[idx]);
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
            out->g16_rle = font->glyphs_rle + font->glyphs_off[i];
            out->g16_len = (unsigned)(font->glyphs_off[i + 1] - font->glyphs_off[i]);
            out->g32_rle = font->glyphs_2x_rle + font->glyphs_2x_off[i];
            out->g32_len = (unsigned)(font->glyphs_2x_off[i + 1] - font->glyphs_2x_off[i]);
            out->width = font->widths ? font->widths[i] : glyph_font_width(font);
            // The per-glyph width table is raw fonts.bin data; a corrupt or
            // mismatched byte must not size writes past the staging buffers.
            if (out->width > DISPLAY_GLYPH_WIDTH) out->width = DISPLAY_GLYPH_WIDTH;
            out->stride16 = DISPLAY_GLYPH_WIDTH / 2;
            out->stride32 = DISPLAY_GLYPH_2X_WIDTH / 2;
            return;
        }
    }
    {
        int i = cp_find(font_base_ext_cp, font_base_ext_count, cp);
        if (i >= 0) {
            out->g16_rle = font_base_ext_rle + font_base_ext_off[i];
            out->g16_len = (unsigned)(font_base_ext_off[i + 1] - font_base_ext_off[i]);
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

static void display_vline(int16_t x, int16_t y, int16_t h, uint16_t color) {
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
    aa_blit(x, y, unpack_1x(&g), g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
}

// Blit one 4bpp glyph as a single address window write, clipping glyphs that
// straddle a screen edge to their visible part (like the compose path does)
// instead of dropping them whole.
static void aa_blit(int16_t x, int16_t y, const uint8_t *glyph,
                    int width, int height, int stride,
                    uint16_t fg, uint16_t bg) {
    if (!glyph) return;
    int col0 = x < 0 ? -x : 0;
    int row0 = y < 0 ? -y : 0;
    int col1 = (x + width  > DISPLAY_WIDTH)  ? DISPLAY_WIDTH  - x : width;
    int row1 = (y + height > DISPLAY_HEIGHT) ? DISPLAY_HEIGHT - y : height;
    if (col0 >= col1 || row0 >= row1) return;

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    set_addr_window(x + col0, y + row0, col1 - col0, row1 - row0);
    dc_data();

    int idx = 0;
    for (int row = row0; row < row1; row++) {
        const uint8_t *row_px = glyph + row * stride;
        for (int col = col0; col < col1; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            glyph_stage_buf[idx++] = color >> 8;
            glyph_stage_buf[idx++] = color & 0xFF;
        }
    }
    spi_write_bytes(glyph_stage_buf, idx);
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

static int display_text_width_font(const char *str, const display_glyph_font_t *font) {
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

static void display_string_font(int16_t x, int16_t y, const char *str,
                                const display_glyph_font_t *font,
                                uint16_t fg, uint16_t bg) {
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp;
        glyph_ref_t g;
        p += utf8_decode(p, &cp);
        resolve_glyph(cp, font, &g);
        aa_blit(x, y, unpack_1x(&g), g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
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
            aa_blit_doubled(x, y, unpack_1x(&g), g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
        }
        x += g.width * 2;
    }
}

// UI regions (7-seg digits, header, buttons, menu and list rows) are
// composed into this RAM buffer and pushed to the panel as a single
// address window + one SPI transaction. Drawing piecewise instead - a
// background fill followed by glyph blits, or per-segment lines - costs
// 10-20 us fixed overhead per polling transaction and repaints the same
// pixels twice, and the long multi-transaction window is what lets the
// panel scan catch a half-drawn region (tearing). Sized for a full-width
// 30 px strip (the header); the size-3 digit needs less.
#define DISPLAY_COMPOSE_MAX_H 30
static uint8_t compose_buf[DISPLAY_WIDTH * DISPLAY_COMPOSE_MAX_H * 2];
_Static_assert(sizeof(compose_buf) >= (48 + 8) * (2 * 48 + 2 * 8 - 1) * 2,
               "compose_buf must fit the size-3 7-seg digit");
static int16_t compose_w, compose_h;

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

// Segment shapes with pointed ends. The taper must be symmetric across
// the (even) thickness - a 2px blunt point, insets [2,1,0,0,1,2] - or
// every tip leans half a pixel toward one side and the corner gaps of a
// digit come out unequal (the old |t - thick/2| taper made the top-left
// junction visibly tighter than the other three).
static int seg_taper_inset(int t, int thick) {
    int c_lo = (thick - 1) / 2, c_hi = thick / 2;
    if (t < c_lo) return c_lo - t;
    if (t > c_hi) return t - c_hi;
    return 0;
}

static void buf_segment_h(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                          int16_t w, int16_t thick, uint16_t color) {
    for (int t = 0; t < thick; t++) {
        int inset = seg_taper_inset(t, thick);
        buf_hline(buf, buf_w, x + inset, y + t, w - 2 * inset, color);
    }
}

static void buf_segment_v(uint8_t *buf, int16_t buf_w, int16_t x, int16_t y,
                          int16_t h, int16_t thick, uint16_t color) {
    for (int t = 0; t < thick; t++) {
        int inset = seg_taper_inset(t, thick);
        buf_vline(buf, buf_w, x + t, y + inset, h - 2 * inset, color);
    }
}

bool display_compose_begin(int16_t w, int16_t h, uint16_t bg) {
    if (w <= 0 || h <= 0 || (size_t)w * h * 2 > sizeof(compose_buf)) {
        compose_w = compose_h = 0;
        return false;
    }
    compose_w = w;
    compose_h = h;
    uint8_t hi = bg >> 8, lo = bg & 0xFF;
    int32_t total = (int32_t)w * h;
    for (int32_t i = 0; i < total; i++) {
        compose_buf[i * 2]     = hi;
        compose_buf[i * 2 + 1] = lo;
    }
    return true;
}

void display_compose_fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > compose_w) w = compose_w - x;
    if (y + h > compose_h) h = compose_h - y;
    if (w <= 0 || h <= 0) return;
    for (int16_t r = 0; r < h; r++) {
        buf_hline(compose_buf, compose_w, x, y + r, w, color);
    }
}

void display_compose_rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    if (w <= 0 || h <= 0) return;
    display_compose_fill(x, y, w, 1, color);
    display_compose_fill(x, y + h - 1, w, 1, color);
    display_compose_fill(x, y, 1, h, color);
    display_compose_fill(x + w - 1, y, 1, h, color);
}

// Blend one glyph into the compose buffer, clipped to the region.
static void compose_glyph(int16_t x, int16_t y, const uint8_t *glyph,
                          int width, int height, int stride,
                          uint16_t fg, uint16_t bg) {
    if (!glyph || x >= compose_w || y >= compose_h) return;
    int c0 = (x < 0) ? -x : 0;
    int c1 = (x + width > compose_w) ? compose_w - x : width;
    if (c0 >= c1) return;

    uint16_t palette[16];
    aa_build_palette(fg, bg, palette);

    for (int row = 0; row < height; row++) {
        int py = y + row;
        if (py < 0) continue;
        if (py >= compose_h) break;
        const uint8_t *row_px = glyph + row * stride;
        uint8_t *dst = compose_buf + ((size_t)py * compose_w + x + c0) * 2;
        for (int col = c0; col < c1; col++) {
            uint16_t color = palette[aa_alpha_at(row_px, col)];
            *dst++ = color >> 8;
            *dst++ = color & 0xFF;
        }
    }
}

void display_compose_string_font(int16_t x, int16_t y, const char *str,
                                 const display_glyph_font_t *font,
                                 uint16_t fg, uint16_t bg) {
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        uint32_t cp;
        glyph_ref_t g;
        p += utf8_decode(p, &cp);
        resolve_glyph(cp, font, &g);
        compose_glyph(x, y, unpack_1x(&g), g.width, FONT_CHAR_HEIGHT, g.stride16, fg, bg);
        x += g.width;
    }
}

void display_compose_string(int16_t x, int16_t y, const char *str,
                            uint16_t fg, uint16_t bg) {
    display_compose_string_font(x, y, str, active_glyph_font, fg, bg);
}

void display_compose_push(int16_t x, int16_t y) {
    if (compose_w <= 0 || compose_h <= 0 || x < 0 || y < 0 ||
        x + compose_w > DISPLAY_WIDTH || y + compose_h > DISPLAY_HEIGHT) {
        return;
    }
    set_addr_window(x, y, compose_w, compose_h);
    dc_data();
    spi_write_bytes(compose_buf, (size_t)compose_w * compose_h * 2);
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
    // The middle segment sits half a pixel above the digit's center (odd
    // digit height, even segment thickness), so lower verticals placed at
    // the upper verticals' mirrored rows would leave one more row of
    // daylight below the middle bar than above it. Start them one row
    // earlier and one row taller: they interlock the middle segment by
    // one row exactly as the upper verticals do, and their bottom ends
    // keep the mirrored interlock with the bottom segment.
    int16_t v_low_y = seg_len + 3 * seg_thick / 2 - 2;
    int16_t v_low_len = v_len + 1;

    // Background-fill the buffer; "off" segments need no explicit erase since
    // the full bounding box is pushed every time (same no-flash behavior).
    uint8_t hi = bg >> 8, lo = bg & 0xFF;
    int32_t total = (int32_t)w * h;
    for (int32_t i = 0; i < total; i++) {
        compose_buf[i * 2]     = hi;
        compose_buf[i * 2 + 1] = lo;
    }

    if (pattern & 0x01) buf_segment_h(compose_buf, w, seg_thick / 2 + gap, 0, h_len, seg_thick, color);
    if (pattern & 0x02) buf_segment_v(compose_buf, w, seg_len, seg_thick / 2 + gap, v_len, seg_thick, color);
    if (pattern & 0x04) buf_segment_v(compose_buf, w, seg_len, v_low_y, v_low_len, seg_thick, color);
    if (pattern & 0x08) buf_segment_h(compose_buf, w, seg_thick / 2 + gap, seg_len * 2 + seg_thick - 1, h_len, seg_thick, color);
    if (pattern & 0x10) buf_segment_v(compose_buf, w, 0, v_low_y, v_low_len, seg_thick, color);
    if (pattern & 0x20) buf_segment_v(compose_buf, w, 0, seg_thick / 2 + gap, v_len, seg_thick, color);
    if (pattern & 0x40) buf_segment_h(compose_buf, w, seg_thick / 2 + gap, seg_len + seg_thick / 2 - 1, h_len, seg_thick, color);

    set_addr_window(x, y, w, h);
    dc_data();
    spi_write_bytes(compose_buf, (size_t)w * h * 2);
}

void display_colon_7seg(int16_t x, int16_t y, uint8_t size, uint16_t color) {
    int16_t seg_len, seg_thick, dot_size;
    switch (size) {
        case 1: seg_len = 16; seg_thick = 4; dot_size = 4; break;
        case 2: seg_len = 32; seg_thick = 6; dot_size = 6; break;
        case 3:
        default: seg_len = 48; seg_thick = 8; dot_size = 8; break;
    }

    // Dots centered in the COLON_7SEG_WIDTH slot, the pair symmetric
    // within the digit bounding box (dot-to-top-segment distance equals
    // dot-to-middle and mirrors below).
    int16_t h = 2 * seg_len + 2 * seg_thick - 1;
    int16_t dot_x = x + (COLON_7SEG_WIDTH - dot_size) / 2;
    int16_t upper = seg_len / 2 + seg_thick / 2 - 2;
    display_fill_rect(dot_x, y + upper, dot_size, dot_size, color);
    display_fill_rect(dot_x, y + h - upper - dot_size, dot_size, dot_size, color);
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
