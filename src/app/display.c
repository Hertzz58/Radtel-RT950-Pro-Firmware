/*
 * display.c - Display / UI layer for the RT-950 Pro
 *
 * Provides text rendering using a 5x7 bitmap font and basic drawing
 * primitives on top of the LCD 8080-bus driver.  All output is direct
 * (immediate) - no framebuffer is used.
 *
 * Font covers printable ASCII 0x20 (' ') through 0x7E ('~'), 95 glyphs.
 * Each glyph is 5 columns x 7 rows, stored column-major (5 bytes each,
 * LSB = top row).
 * Turkish and Russian UTF-8 glyphs use the same width with an extra row.
 *
 * Main screen layout (240x320 portrait):
 *   y=  0..19  : Status bar (battery, TX/RX, icons)
 *   y= 20..159 : VFO A (active) - large display font for frequency
 *   y=160..239 : VFO B (inactive) - smaller font
 *   y=240..269 : S-meter bar graph
 *   y=270..319 : Info bar (channel name / status)
 */

#include "app/display.h"
#include "app/font.h"
#include "app/vfo.h"
#include "app/radio.h"
#include "app/power.h"
#include "app/settings.h"
#include "app/menu.h"
#include "app/freq_entry.h"
#include "app/aprs.h"
#include "app/bluetooth.h"
#include "app/gps.h"
#include "app/vox.h"
#include "drivers/lcd.h"
#include "drivers/bk4829.h"
#include <stdarg.h>
#include <stddef.h>

/* ========================================================================
 *  5x7 bitmap font - ASCII 0x20 to 0x7E (95 characters)
 *
 *  Each character is 5 bytes, one per column (left to right).
 *  Bit 0 = top pixel row, bit 6 = bottom pixel row.
 * ======================================================================== */

static const uint8_t font5x7[95][5] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* 0x20 ' ' */
    { 0x00, 0x00, 0x5F, 0x00, 0x00 }, /* 0x21 '!' */
    { 0x00, 0x07, 0x00, 0x07, 0x00 }, /* 0x22 '"' */
    { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, /* 0x23 '#' */
    { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, /* 0x24 '$' */
    { 0x23, 0x13, 0x08, 0x64, 0x62 }, /* 0x25 '%' */
    { 0x36, 0x49, 0x55, 0x22, 0x50 }, /* 0x26 '&' */
    { 0x00, 0x05, 0x03, 0x00, 0x00 }, /* 0x27 ''' */
    { 0x00, 0x1C, 0x22, 0x41, 0x00 }, /* 0x28 '(' */
    { 0x00, 0x41, 0x22, 0x1C, 0x00 }, /* 0x29 ')' */
    { 0x08, 0x2A, 0x1C, 0x2A, 0x08 }, /* 0x2A '*' */
    { 0x08, 0x08, 0x3E, 0x08, 0x08 }, /* 0x2B '+' */
    { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* 0x2C ',' */
    { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* 0x2D '-' */
    { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* 0x2E '.' */
    { 0x20, 0x10, 0x08, 0x04, 0x02 }, /* 0x2F '/' */
    { 0x3E, 0x51, 0x49, 0x45, 0x3E }, /* 0x30 '0' */
    { 0x00, 0x42, 0x7F, 0x40, 0x00 }, /* 0x31 '1' */
    { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* 0x32 '2' */
    { 0x21, 0x41, 0x45, 0x4B, 0x31 }, /* 0x33 '3' */
    { 0x18, 0x14, 0x12, 0x7F, 0x10 }, /* 0x34 '4' */
    { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* 0x35 '5' */
    { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, /* 0x36 '6' */
    { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* 0x37 '7' */
    { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* 0x38 '8' */
    { 0x06, 0x49, 0x49, 0x29, 0x1E }, /* 0x39 '9' */
    { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* 0x3A ':' */
    { 0x00, 0x56, 0x36, 0x00, 0x00 }, /* 0x3B ';' */
    { 0x00, 0x08, 0x14, 0x22, 0x41 }, /* 0x3C '<' */
    { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* 0x3D '=' */
    { 0x41, 0x22, 0x14, 0x08, 0x00 }, /* 0x3E '>' */
    { 0x02, 0x01, 0x51, 0x09, 0x06 }, /* 0x3F '?' */
    { 0x32, 0x49, 0x79, 0x41, 0x3E }, /* 0x40 '@' */
    { 0x7E, 0x11, 0x11, 0x11, 0x7E }, /* 0x41 'A' */
    { 0x7F, 0x49, 0x49, 0x49, 0x36 }, /* 0x42 'B' */
    { 0x3E, 0x41, 0x41, 0x41, 0x22 }, /* 0x43 'C' */
    { 0x7F, 0x41, 0x41, 0x22, 0x1C }, /* 0x44 'D' */
    { 0x7F, 0x49, 0x49, 0x49, 0x41 }, /* 0x45 'E' */
    { 0x7F, 0x09, 0x09, 0x01, 0x01 }, /* 0x46 'F' */
    { 0x3E, 0x41, 0x41, 0x51, 0x32 }, /* 0x47 'G' */
    { 0x7F, 0x08, 0x08, 0x08, 0x7F }, /* 0x48 'H' */
    { 0x00, 0x41, 0x7F, 0x41, 0x00 }, /* 0x49 'I' */
    { 0x20, 0x40, 0x41, 0x3F, 0x01 }, /* 0x4A 'J' */
    { 0x7F, 0x08, 0x14, 0x22, 0x41 }, /* 0x4B 'K' */
    { 0x7F, 0x40, 0x40, 0x40, 0x40 }, /* 0x4C 'L' */
    { 0x7F, 0x02, 0x04, 0x02, 0x7F }, /* 0x4D 'M' */
    { 0x7F, 0x04, 0x08, 0x10, 0x7F }, /* 0x4E 'N' */
    { 0x3E, 0x41, 0x41, 0x41, 0x3E }, /* 0x4F 'O' */
    { 0x7F, 0x09, 0x09, 0x09, 0x06 }, /* 0x50 'P' */
    { 0x3E, 0x41, 0x51, 0x21, 0x5E }, /* 0x51 'Q' */
    { 0x7F, 0x09, 0x19, 0x29, 0x46 }, /* 0x52 'R' */
    { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* 0x53 'S' */
    { 0x01, 0x01, 0x7F, 0x01, 0x01 }, /* 0x54 'T' */
    { 0x3F, 0x40, 0x40, 0x40, 0x3F }, /* 0x55 'U' */
    { 0x1F, 0x20, 0x40, 0x20, 0x1F }, /* 0x56 'V' */
    { 0x3F, 0x40, 0x38, 0x40, 0x3F }, /* 0x57 'W' */
    { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* 0x58 'X' */
    { 0x07, 0x08, 0x70, 0x08, 0x07 }, /* 0x59 'Y' */
    { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* 0x5A 'Z' */
    { 0x00, 0x00, 0x7F, 0x41, 0x41 }, /* 0x5B '[' */
    { 0x02, 0x04, 0x08, 0x10, 0x20 }, /* 0x5C '\' */
    { 0x41, 0x41, 0x7F, 0x00, 0x00 }, /* 0x5D ']' */
    { 0x04, 0x02, 0x01, 0x02, 0x04 }, /* 0x5E '^' */
    { 0x40, 0x40, 0x40, 0x40, 0x40 }, /* 0x5F '_' */
    { 0x00, 0x01, 0x02, 0x04, 0x00 }, /* 0x60 '`' */
    { 0x20, 0x54, 0x54, 0x54, 0x78 }, /* 0x61 'a' */
    { 0x7F, 0x48, 0x44, 0x44, 0x38 }, /* 0x62 'b' */
    { 0x38, 0x44, 0x44, 0x44, 0x20 }, /* 0x63 'c' */
    { 0x38, 0x44, 0x44, 0x48, 0x7F }, /* 0x64 'd' */
    { 0x38, 0x54, 0x54, 0x54, 0x18 }, /* 0x65 'e' */
    { 0x08, 0x7E, 0x09, 0x01, 0x02 }, /* 0x66 'f' */
    { 0x08, 0x54, 0x54, 0x54, 0x3C }, /* 0x67 'g' */
    { 0x7F, 0x08, 0x04, 0x04, 0x78 }, /* 0x68 'h' */
    { 0x00, 0x44, 0x7D, 0x40, 0x00 }, /* 0x69 'i' */
    { 0x20, 0x40, 0x44, 0x3D, 0x00 }, /* 0x6A 'j' */
    { 0x00, 0x7F, 0x10, 0x28, 0x44 }, /* 0x6B 'k' */
    { 0x00, 0x41, 0x7F, 0x40, 0x00 }, /* 0x6C 'l' */
    { 0x7C, 0x04, 0x18, 0x04, 0x78 }, /* 0x6D 'm' */
    { 0x7C, 0x08, 0x04, 0x04, 0x78 }, /* 0x6E 'n' */
    { 0x38, 0x44, 0x44, 0x44, 0x38 }, /* 0x6F 'o' */
    { 0x7C, 0x14, 0x14, 0x14, 0x08 }, /* 0x70 'p' */
    { 0x08, 0x14, 0x14, 0x18, 0x7C }, /* 0x71 'q' */
    { 0x7C, 0x08, 0x04, 0x04, 0x08 }, /* 0x72 'r' */
    { 0x48, 0x54, 0x54, 0x54, 0x20 }, /* 0x73 's' */
    { 0x04, 0x3F, 0x44, 0x40, 0x20 }, /* 0x74 't' */
    { 0x3C, 0x40, 0x40, 0x20, 0x7C }, /* 0x75 'u' */
    { 0x1C, 0x20, 0x40, 0x20, 0x1C }, /* 0x76 'v' */
    { 0x3C, 0x40, 0x30, 0x40, 0x3C }, /* 0x77 'w' */
    { 0x44, 0x28, 0x10, 0x28, 0x44 }, /* 0x78 'x' */
    { 0x0C, 0x50, 0x50, 0x50, 0x3C }, /* 0x79 'y' */
    { 0x44, 0x64, 0x54, 0x4C, 0x44 }, /* 0x7A 'z' */
    { 0x00, 0x08, 0x36, 0x41, 0x00 }, /* 0x7B '{' */
    { 0x00, 0x00, 0x7F, 0x00, 0x00 }, /* 0x7C '|' */
    { 0x00, 0x41, 0x36, 0x08, 0x00 }, /* 0x7D '}' */
    { 0x08, 0x04, 0x08, 0x10, 0x08 }, /* 0x7E '~' */
};

/* ========================================================================
 *  display_init - Initialize LCD hardware and turn on the display.
 * ======================================================================== */

void display_init(void)
{
    lcd_init();
}

/* ========================================================================
 *  display_clear - Fill entire 240x320 screen with a solid color.
 * ======================================================================== */

void display_clear(uint16_t color)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

#include "font_menu_ext.inc"

static void draw_glyph(uint16_t x, uint16_t y, const uint8_t *glyph, uint8_t height,
                       uint16_t fg, uint16_t bg)
{
    if (x + DISPLAY_FONT_W > LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    if (y + height > LCD_HEIGHT)
        height = LCD_HEIGHT - y;
    lcd_set_window(x, y, x + DISPLAY_FONT_W - 1, y + height - 1);
    lcd_gram_write();

    /* Pixel data is sent row-major (top-left to bottom-right) */
    for (uint8_t row = 0; row < height; row++) {
        for (uint8_t col = 0; col < DISPLAY_FONT_W; col++) {
            uint16_t color = (glyph[col] & (1 << row)) ? fg : bg;
            lcd_write_data(color >> 8);
            lcd_write_data(color & 0xFF);
        }
    }
}

void display_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg)
{
    if (ch < 0x20 || ch > 0x7E)
        ch = '?';
    draw_glyph(x, y, font5x7[ch - 0x20], DISPLAY_FONT_H, fg, bg);
}

static uint32_t read_codepoint(const char **text)
{
    uint32_t codepoint = (uint8_t)**text;
    ++*text;
    if (codepoint < 0x80)
        return codepoint;

    uint8_t remaining;
    uint32_t minimum;
    if (codepoint >= 0xC2 && codepoint <= 0xDF) {
        codepoint &= 0x1F;
        remaining = 1;
        minimum = 0x80;
    } else if (codepoint >= 0xE0 && codepoint <= 0xEF) {
        codepoint &= 0x0F;
        remaining = 2;
        minimum = 0x800;
    } else if (codepoint >= 0xF0 && codepoint <= 0xF4) {
        codepoint &= 0x07;
        remaining = 3;
        minimum = 0x10000;
    } else {
        return '?';
    }

    while (remaining--) {
        uint8_t byte = (uint8_t)**text;
        if ((byte & 0xC0) != 0x80)
            return '?';
        ++*text;
        codepoint = (codepoint << 6) | (byte & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF ||
        (codepoint >= 0xD800 && codepoint <= 0xDFFF))
        return '?';
    return codepoint;
}

static const uint8_t *extended_glyph(uint32_t codepoint)
{
    if (codepoint >= 0x0410 && codepoint <= 0x044F)
        return cyrillic_glyphs[codepoint - 0x0410];

    for (unsigned i = 0; i < sizeof extra_glyphs / sizeof extra_glyphs[0]; i++) {
        if (extra_glyphs[i].codepoint == codepoint)
            return extra_glyphs[i].columns;
    }
    return NULL;
}

/* ========================================================================
 *  display_draw_text - Draw a string of characters starting at (x, y).
 * ======================================================================== */

void display_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg)
{
    while (*str) {
        if (x + DISPLAY_FONT_W > LCD_WIDTH)
            break;
        uint32_t codepoint = read_codepoint(&str);
        const uint8_t *glyph = (codepoint >= 0x80) ? extended_glyph(codepoint) : NULL;
        if (glyph)
            draw_glyph(x, y, glyph, 8, fg, bg);
        else
            display_draw_char(x, y, (codepoint < 0x80) ? (char)codepoint : '?', fg, bg);
        x += DISPLAY_FONT_W + DISPLAY_CHAR_GAP;
    }
}

/* ========================================================================
 *  display_draw_hline - Horizontal line (1 pixel tall).
 * ======================================================================== */

void display_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color)
{
    lcd_fill_rect(x, y, w, 1, color);
}

/* ========================================================================
 *  display_draw_vline - Vertical line (1 pixel wide).
 * ======================================================================== */

void display_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color)
{
    lcd_fill_rect(x, y, 1, h, color);
}

/* ========================================================================
 *  display_printf - Formatted text output at (x, y).
 *
 *  Uses a small stack buffer (128 bytes).  Relies on vsnprintf from
 *  the toolchain's minimal libc (newlib-nano).
 * ======================================================================== */

void display_printf(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg,
                    const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);

    /* Manual minimal vsnprintf - avoid pulling in full libc.
     * For production, link newlib-nano with --specs=nano.specs. */
    int i = 0;
    const char *p = fmt;
    while (*p && i < (int)(sizeof(buf) - 1)) {
        if (*p == '%' && *(p + 1)) {
            p++;
            switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && i < (int)(sizeof(buf) - 1))
                    buf[i++] = *s++;
                break;
            }
            case 'd': {
                int val = va_arg(ap, int);
                if (val < 0) {
                    buf[i++] = '-';
                    val = -val;
                }
                /* Convert digits into temp buffer then reverse */
                char tmp[12];
                int ti = 0;
                if (val == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (val > 0 && ti < (int)(sizeof(tmp) - 1)) {
                        tmp[ti++] = '0' + (val % 10);
                        val /= 10;
                    }
                }
                for (int j = ti - 1; j >= 0 && i < (int)(sizeof(buf) - 1); j--)
                    buf[i++] = tmp[j];
                break;
            }
            case 'u': {
                unsigned int val = va_arg(ap, unsigned int);
                char tmp[12];
                int ti = 0;
                if (val == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (val > 0 && ti < (int)(sizeof(tmp) - 1)) {
                        tmp[ti++] = '0' + (val % 10);
                        val /= 10;
                    }
                }
                for (int j = ti - 1; j >= 0 && i < (int)(sizeof(buf) - 1); j--)
                    buf[i++] = tmp[j];
                break;
            }
            case 'x':
            case 'X': {
                unsigned int val = va_arg(ap, unsigned int);
                const char *hex = (*p == 'X') ? "0123456789ABCDEF"
                                              : "0123456789abcdef";
                char tmp[9];
                int ti = 0;
                if (val == 0) {
                    tmp[ti++] = '0';
                } else {
                    while (val > 0 && ti < (int)(sizeof(tmp) - 1)) {
                        tmp[ti++] = hex[val & 0xF];
                        val >>= 4;
                    }
                }
                for (int j = ti - 1; j >= 0 && i < (int)(sizeof(buf) - 1); j--)
                    buf[i++] = tmp[j];
                break;
            }
            case 'c':
                buf[i++] = (char)va_arg(ap, int);
                break;
            case '%':
                buf[i++] = '%';
                break;
            default:
                buf[i++] = '%';
                if (i < (int)(sizeof(buf) - 1))
                    buf[i++] = *p;
                break;
            }
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    va_end(ap);

    display_draw_text(x, y, buf, fg, bg);
}

/* ========================================================================
 *  Display mode management
 *
 *  OEM: display_render_engine @ 0x08019344 uses TBB jump table on
 *  g_rf_state[3] (11 modes). We use an enum + switch for clarity.
 * ======================================================================== */

static display_mode_t cur_display_mode = DISPLAY_MODE_MAIN;

display_mode_t display_get_mode(void) { return cur_display_mode; }

void display_set_mode(display_mode_t mode)
{
    if (mode < DISPLAY_MODE_COUNT)
        cur_display_mode = mode;
}

/* ========================================================================
 *  display_update - Main display refresh, called at ~30 fps.
 *
 *  OEM equivalent: display_render_engine @ 0x08019344 (TBB dispatch,
 *  27,380B). Both use direct pixel push (no RAM framebuffer).
 *  OEM called from display_periodic_refresh @ 0x08003B3C.
 * ======================================================================== */

void display_update(void)
{
    /* Menu overlay takes priority regardless of display mode */
    if (menu_get_state() != MENU_STATE_CLOSED) {
        menu_draw();
        return;
    }

    /* Frequency entry overlay */
    if (freq_entry_is_active()) {
        display_draw_status_bar();
        display_draw_vfo_a();   /* vfo_a handles freq entry rendering */
        return;
    }

    switch (cur_display_mode) {
    case DISPLAY_MODE_MAIN:
    default:
        display_draw_main_screen();
        break;
    case DISPLAY_MODE_FM_RADIO:
    case DISPLAY_MODE_AM_RADIO:
        /* TODO: dedicated FM/AM radio screen (Phase 22) */
        display_draw_main_screen();
        break;
    case DISPLAY_MODE_CHANNEL:
        /* TODO: channel/zone browser screen (Phase 22) */
        display_draw_main_screen();
        break;
    case DISPLAY_MODE_MENU:
        menu_draw();
        break;
    case DISPLAY_MODE_FREQ_ENTRY:
        display_draw_status_bar();
        display_draw_vfo_a();
        break;
    }
}

/* ========================================================================
 *  Helpers: format frequency and tone strings
 * ======================================================================== */

/* CTCSS tone table (tenths of Hz) from bk4829.c */
extern const uint16_t ctcss_tone_table[];

static const char *mod_names[] = { "FM", "AM", "USB", "LSB" };

/*
 * format_freq_display - Format freq_hz into "XXX.XXX" for FONT_DISPLAY.
 * FONT_DISPLAY only covers '0'-'9' and ':' (0x30-0x3A).
 * We use ':' as the decimal point glyph.
 */
static void format_freq_display(uint32_t freq_hz, char *buf, uint8_t size)
{
    uint32_t mhz = freq_hz / 1000000;
    uint32_t khz = (freq_hz / 1000) % 1000;
    uint8_t i = 0;

    /* MHz part (up to 3 digits) */
    if (mhz >= 100 && i < size - 1) buf[i++] = (char)('0' + (mhz / 100));
    if (mhz >= 10  && i < size - 1) buf[i++] = (char)('0' + ((mhz / 10) % 10));
    if (i < size - 1) buf[i++] = (char)('0' + (mhz % 10));

    /* Decimal point - ':' is 0x3A, within FONT_DISPLAY range */
    if (i < size - 1) buf[i++] = ':';

    /* kHz part (3 digits, zero-padded) */
    if (i < size - 1) buf[i++] = (char)('0' + (khz / 100));
    if (i < size - 1) buf[i++] = (char)('0' + ((khz / 10) % 10));
    if (i < size - 1) buf[i++] = (char)('0' + (khz % 10));

    buf[i] = '\0';
}

/*
 * format_freq_text - Format freq_hz as "XXX.XXX" for regular text fonts.
 */
static void format_freq_text(uint32_t freq_hz, char *buf, uint8_t size)
{
    uint32_t mhz = freq_hz / 1000000;
    uint32_t khz = (freq_hz / 1000) % 1000;
    uint8_t i = 0;

    if (mhz >= 100 && i < size - 1) buf[i++] = (char)('0' + (mhz / 100));
    if (mhz >= 10  && i < size - 1) buf[i++] = (char)('0' + ((mhz / 10) % 10));
    if (i < size - 1) buf[i++] = (char)('0' + (mhz % 10));
    if (i < size - 1) buf[i++] = '.';
    if (i < size - 1) buf[i++] = (char)('0' + (khz / 100));
    if (i < size - 1) buf[i++] = (char)('0' + ((khz / 10) % 10));
    if (i < size - 1) buf[i++] = (char)('0' + (khz % 10));

    buf[i] = '\0';
}

/*
 * format_tone_info - Build tone info string (e.g. "CTCSS 67.0" or "DCS 023N").
 */
static void format_tone_info(const vfo_state_t *vfo, char *buf, uint8_t size)
{
    if (vfo->ctcss_tx_idx != 0xFF && vfo->ctcss_tx_idx < CTCSS_TONE_COUNT) {
        uint16_t tone = ctcss_tone_table[vfo->ctcss_tx_idx];
        uint16_t whole = tone / 10;
        uint16_t frac  = tone % 10;
        uint8_t i = 0;

        /* "CT " prefix */
        if (i < size - 1) buf[i++] = 'C';
        if (i < size - 1) buf[i++] = 'T';
        if (i < size - 1) buf[i++] = ' ';

        /* Whole part */
        if (whole >= 100 && i < size - 1)
            buf[i++] = (char)('0' + (whole / 100));
        if (whole >= 10 && i < size - 1)
            buf[i++] = (char)('0' + ((whole / 10) % 10));
        if (i < size - 1)
            buf[i++] = (char)('0' + (whole % 10));
        if (i < size - 1) buf[i++] = '.';
        if (i < size - 1) buf[i++] = (char)('0' + frac);

        buf[i] = '\0';
    } else if (vfo->dcs_code_idx != 0xFF) {
        buf[0] = 'D'; buf[1] = 'C'; buf[2] = 'S';
        buf[3] = '\0';
    } else {
        buf[0] = '\0';
    }
}

/* ========================================================================
 *  display_draw_status_bar - Battery, TX/RX, mode icons (y=0..19)
 *
 *  OEM: display_status_bar @ 0x08020340 renders 5 fields:
 *    1. Signal strength (RSSI bars)
 *    2. Battery level
 *    3. TX/RX indicator (red/green text)
 *    4. Lock/encryption status
 *    5. RF mode flags
 *  All OEM icons are procedurally drawn (text + rectangles, no bitmaps).
 * ======================================================================== */

void display_draw_status_bar(void)
{
    /* Clear status bar area */
    lcd_fill_rect(0, LAYOUT_STATUS_Y, LCD_WIDTH, LAYOUT_STATUS_H, COLOR_BLACK);

    /* Left: TX/RX indicator */
    if (radio_is_transmitting()) {
        /* Blink TX text when TOT warning active (last 10s before cutoff) */
        static uint8_t blink;
        uint16_t tx_color = COLOR_RED;
        if (radio_tot_warning_active()) {
            blink++;
            tx_color = (blink & 0x04) ? COLOR_RED : COLOR_YELLOW;
        }
        font_draw_string(FONT_SMALL, 2, 4, "TX", tx_color, COLOR_BLACK);
    } else {
        font_draw_string(FONT_SMALL, 2, 4, "RX", COLOR_GREEN, COLOR_BLACK);
    }

    /* Status icons region: x=20..86 - GPS, VOX, lock, encryption */
    {
        uint16_t ix = 20;
        const settings_t *s = settings_get();

        /* GPS fix indicator */
        const gps_data_t *gps = gps_get_data();
        if (gps && gps->fix_quality > 0) {
            font_draw_string(FONT_SMALL, ix, 4, "GP", COLOR_GREEN, COLOR_BLACK);
        } else if (s->aprs_enable) {
            /* GPS enabled but no fix - show dim */
            font_draw_string(FONT_SMALL, ix, 4, "GP", COLOR_DARK_GRAY, COLOR_BLACK);
        }
        ix += 18;

        /* VOX indicator */
        if (s->vox_switch && vox_is_triggered())
            font_draw_string(FONT_SMALL, ix, 4, "VX", COLOR_YELLOW, COLOR_BLACK);
        else if (s->vox_switch)
            font_draw_string(FONT_SMALL, ix, 4, "VX", COLOR_DARK_GRAY, COLOR_BLACK);
        ix += 18;

        /* Key lock indicator */
        if (s->keypad_lock)
            font_draw_string(FONT_SMALL, ix, 4, "LK", COLOR_ORANGE, COLOR_BLACK);
        ix += 18;

        /* Encryption/scrambler indicator */
        {
            radio_vfo_t act = vfo_get_active();
            const vfo_state_t *vs = vfo_get_state(act);
            if (vs->scrambler)
                font_draw_string(FONT_SMALL, ix, 4, "EN", COLOR_CYAN, COLOR_BLACK);
        }
    }

    /* Center: active VFO label + dual-watch indicator */
    {
        radio_vfo_t active = vfo_get_active();
        const char *label = (active == RADIO_VFO_A) ? "A" : "B";

        /* Highlight VFO label in cyan if DW has switched RX focus */
        uint16_t vfo_col = COLOR_YELLOW;
        if (settings_get()->dual_watch && radio_get_rx_vfo() != active)
            vfo_col = COLOR_CYAN;

        font_draw_string(FONT_SMALL, 110, 4, label, vfo_col, COLOR_BLACK);

        /* Show "DW" icon when dual-watch is enabled */
        if (settings_get()->dual_watch)
            font_draw_string(FONT_SMALL, 90, 4, "DW", COLOR_CYAN, COLOR_BLACK);
    }

    /* Right: battery icon - 4 bars based on level
     * OEM: 28x12 procedural rectangle at right edge.
     * 4 inner bars: 5px wide x 8px tall, 1px gap. Terminal nub: 3x6.
     * Green when >1 bar, red when 1 bar, dark gray when empty. */
    {
        battery_level_t level = power_get_battery_level();
        uint8_t bars = 0;
        if (level >= BATT_MED_HIGH)  bars = 4;
        else if (level >= BATT_MEDIUM)   bars = 3;
        else if (level >= BATT_MED_LOW)  bars = 2;
        else if (level >= BATT_LOW)      bars = 1;

        /* Low-battery flash: toggle outline red/white at ~2 Hz (display @ 30fps) */
        static uint8_t batt_blink;
        uint8_t is_alert = power_is_low_batt_alert();
        uint16_t outline_color = COLOR_WHITE;
        if (is_alert) {
            batt_blink++;
            outline_color = (batt_blink & 0x08) ? COLOR_RED : COLOR_WHITE;
        }

        /* Battery outline: 28x12 at right edge */
        uint16_t bx = LCD_WIDTH - 34;
        uint16_t by = 4;
        /* Outline */
        lcd_fill_rect(bx, by, 28, 1, outline_color);
        lcd_fill_rect(bx, by + 11, 28, 1, outline_color);
        lcd_fill_rect(bx, by, 1, 12, outline_color);
        lcd_fill_rect(bx + 27, by, 1, 12, outline_color);
        /* Terminal nub */
        lcd_fill_rect(bx + 28, by + 3, 3, 6, outline_color);
        /* Fill bars */
        for (uint8_t i = 0; i < 4; i++) {
            uint16_t bar_color = (i < bars) ?
                ((bars <= 1) ? COLOR_RED : COLOR_GREEN) : COLOR_DARK_GRAY;
            lcd_fill_rect(bx + 2 + i * 6, by + 2, 5, 8, bar_color);
        }

        /* Voltage text left of battery icon: "X.XV" */
        uint16_t mv = power_get_battery_mv();
        char vbuf[6];
        uint8_t volts = (uint8_t)(mv / 1000);
        uint8_t deci  = (uint8_t)((mv / 100) % 10);
        vbuf[0] = (char)('0' + volts);
        vbuf[1] = '.';
        vbuf[2] = (char)('0' + deci);
        vbuf[3] = 'V';
        vbuf[4] = '\0';
        uint16_t vcol = (level <= BATT_LOW) ? COLOR_RED : COLOR_GRAY;
        font_draw_string(FONT_SMALL, bx - 34, 4, vbuf, vcol, COLOR_BLACK);
    }

    /* Bluetooth indicator - show "BT" when connected */
    if (bt_is_connected())
        font_draw_string(FONT_SMALL, 130, 4, "BT", COLOR_BLUE, COLOR_BLACK);

    /* Separator line */
    lcd_fill_rect(0, LAYOUT_STATUS_H - 1, LCD_WIDTH, 1, COLOR_DARK_GRAY);
}

/* ========================================================================
 *  display_draw_vfo_a - Active VFO with large display font (y=20..159)
 * ======================================================================== */

void display_draw_vfo_a(void)
{
    /* Background for active VFO - subtle highlight */
    lcd_fill_rect(0, LAYOUT_VFO_A_Y, LCD_WIDTH, LAYOUT_VFO_A_H, COLOR_BLACK);

    /* If freq entry is active, show entry display instead */
    if (freq_entry_is_active()) {
        const char *entry = freq_entry_get_display();
        /* Center the entry string using FONT_LARGE */
        uint16_t w = font_string_width(FONT_LARGE, entry);
        uint16_t x = (LCD_WIDTH > w) ? (LCD_WIDTH - w) / 2 : 0;
        font_draw_string(FONT_LARGE, x, LAYOUT_VFO_A_Y + 50,
                         entry, COLOR_YELLOW, COLOR_BLACK);
        return;
    }

    radio_vfo_t active = vfo_get_active();
    const vfo_state_t *vfo = vfo_get_state(active);
    const settings_t *s = settings_get();
    uint8_t work_mode = (active == RADIO_VFO_A) ? s->work_mode_a : s->work_mode_b;

    /* VFO/CH label */
    {
        if (work_mode == 0 || vfo->channel_num == 0xFFFF) {
            /* VFO mode */
            const char *label = (active == RADIO_VFO_A) ? "VFO A" : "VFO B";
            font_draw_string(FONT_SMALL, 4, LAYOUT_VFO_A_Y + 4,
                             label, COLOR_GREEN, COLOR_BLACK);
        } else {
            /* Channel mode - show "CH XXX" */
            char label[10];
            uint16_t ch = vfo->channel_num;
            label[0] = 'C'; label[1] = 'H'; label[2] = ' ';
            if (ch >= 100)
                label[3] = (char)('0' + (ch / 100));
            else
                label[3] = ' ';
            label[4] = (char)('0' + ((ch / 10) % 10));
            label[5] = (char)('0' + (ch % 10));
            label[6] = '\0';
            font_draw_string(FONT_SMALL, 4, LAYOUT_VFO_A_Y + 4,
                             label, COLOR_ORANGE, COLOR_BLACK);
        }
    }

    /* Large frequency display - centered */
    {
        char freq_str[12];
        format_freq_display(vfo->freq_hz, freq_str, sizeof(freq_str));

        uint16_t w = font_string_width(FONT_DISPLAY, freq_str);
        uint16_t x = (LCD_WIDTH > w) ? (LCD_WIDTH - w) / 2 : 0;
        uint16_t y = LAYOUT_VFO_A_Y + 22;
        font_draw_string(FONT_DISPLAY, x, y, freq_str, COLOR_WHITE, COLOR_BLACK);
    }

    /* Info line below frequency: mode, BW, squelch, tone */
    {
        uint16_t y = LAYOUT_VFO_A_Y + LAYOUT_VFO_A_H - 22;
        uint16_t x = 4;

        /* Modulation mode */
        const char *mod = (vfo->modulation < 4) ?
            mod_names[vfo->modulation] : "??";
        font_draw_string(FONT_MEDIUM, x, y, mod, COLOR_YELLOW, COLOR_BLACK);
        x += font_string_width(FONT_MEDIUM, mod) + 8;

        /* Bandwidth: N/W */
        const char *bw = vfo->bandwidth ? "W" : "N";
        font_draw_string(FONT_MEDIUM, x, y, bw, COLOR_WHITE, COLOR_BLACK);
        x += font_string_width(FONT_MEDIUM, bw) + 8;

        /* Squelch level */
        {
            char sq[6];
            sq[0] = 'S'; sq[1] = 'Q'; sq[2] = ':';
            sq[3] = (char)('0' + vfo->squelch_level);
            sq[4] = '\0';
            font_draw_string(FONT_MEDIUM, x, y, sq, COLOR_WHITE, COLOR_BLACK);
            x += font_string_width(FONT_MEDIUM, sq) + 8;
        }

        /* Tone info */
        {
            char tone[16];
            format_tone_info(vfo, tone, sizeof(tone));
            if (tone[0] != '\0')
                font_draw_string(FONT_MEDIUM, x, y, tone,
                                 COLOR_ORANGE, COLOR_BLACK);
        }
    }
}

/* ========================================================================
 *  display_draw_vfo_b - Inactive VFO with smaller font (y=160..239)
 * ======================================================================== */

void display_draw_vfo_b(void)
{
    lcd_fill_rect(0, LAYOUT_VFO_B_Y, LCD_WIDTH, LAYOUT_VFO_B_H, COLOR_BLACK);

    /* Separator line at top */
    lcd_fill_rect(0, LAYOUT_VFO_B_Y, LCD_WIDTH, 1, COLOR_DARK_GRAY);

    radio_vfo_t active = vfo_get_active();
    radio_vfo_t inactive = (active == RADIO_VFO_A) ? RADIO_VFO_B : RADIO_VFO_A;
    const vfo_state_t *vfo = vfo_get_state(inactive);

    /* "VFO B" label (dimmer) */
    {
        const char *label = (inactive == RADIO_VFO_A) ? "VFO A" : "VFO B";
        font_draw_string(FONT_SMALL, 4, LAYOUT_VFO_B_Y + 4,
                         label, COLOR_DARK_GRAY, COLOR_BLACK);
    }

    /* Frequency with FONT_LARGE */
    {
        char freq_str[12];
        format_freq_text(vfo->freq_hz, freq_str, sizeof(freq_str));

        uint16_t w = font_string_width(FONT_LARGE, freq_str);
        uint16_t x = (LCD_WIDTH > w) ? (LCD_WIDTH - w) / 2 : 0;
        uint16_t y = LAYOUT_VFO_B_Y + 20;
        font_draw_string(FONT_LARGE, x, y, freq_str, COLOR_DARK_GRAY, COLOR_BLACK);
    }

    /* Info line: mode, BW, squelch */
    {
        uint16_t y = LAYOUT_VFO_B_Y + LAYOUT_VFO_B_H - 20;
        uint16_t x = 4;

        const char *mod = (vfo->modulation < 4) ?
            mod_names[vfo->modulation] : "??";
        font_draw_string(FONT_SMALL, x, y, mod, COLOR_DARK_GRAY, COLOR_BLACK);
        x += font_string_width(FONT_SMALL, mod) + 6;

        const char *bw = vfo->bandwidth ? "W" : "N";
        font_draw_string(FONT_SMALL, x, y, bw, COLOR_DARK_GRAY, COLOR_BLACK);
        x += font_string_width(FONT_SMALL, bw) + 6;

        {
            char sq[6];
            sq[0] = 'S'; sq[1] = 'Q'; sq[2] = ':';
            sq[3] = (char)('0' + vfo->squelch_level);
            sq[4] = '\0';
            font_draw_string(FONT_SMALL, x, y, sq, COLOR_DARK_GRAY, COLOR_BLACK);
            x += font_string_width(FONT_SMALL, sq) + 6;
        }

        /* Compact tone indicator: "T" for CTCSS, "D" for DCS */
        {
            const char *ti = 0;
            if (vfo->ctcss_tx_idx != 0xFF)
                ti = "T";
            else if (vfo->dcs_code_idx != 0xFF)
                ti = "D";
            if (ti)
                font_draw_string(FONT_SMALL, x, y, ti,
                                 COLOR_ORANGE, COLOR_BLACK);
        }
    }
}

/* ========================================================================
 *  display_draw_smeter - Signal strength bar graph (y=240..269)
 *
 *  RSSI from BK4829 reg 0x0C, mapped to S0-S9 + dB-over segments.
 *  Green (S1-S5) -> Yellow (S6-S9) -> Red (+20/+40/+60 dB)
 * ======================================================================== */

#define SMETER_SEGMENTS     12   /* S1-S9 + 20 + 40 + 60 */
#define SMETER_BAR_X        30
#define SMETER_BAR_W        196
#define SMETER_BAR_H        16
#define SMETER_SEG_GAP      2

void display_draw_smeter(void)
{
    lcd_fill_rect(0, LAYOUT_SMETER_Y, LCD_WIDTH, LAYOUT_SMETER_H, COLOR_BLACK);

    /* Separator line */
    lcd_fill_rect(0, LAYOUT_SMETER_Y, LCD_WIDTH, 1, COLOR_DARK_GRAY);

    /* "S" label */
    font_draw_string(FONT_SMALL, 4, LAYOUT_SMETER_Y + 9, "S", COLOR_WHITE, COLOR_BLACK);

    /* Read RSSI from active VFO's chip */
    radio_vfo_t active = vfo_get_active();
    const vfo_state_t *vfo = vfo_get_state(active);
    uint16_t rssi = bk4829_read_rssi(vfo->chip);

    /*
     * EMA smoothing - fast attack, slow decay (~500 ms effective window
     * at 30 fps display rate).  Fixed-point 8.8: smooth_rssi is raw x 256.
     * Attack alpha = 1/4 (fast needle rise), decay alpha = 1/16 (slow fall).
     */
    static uint32_t smooth_rssi;  /* 8.8 fixed point */
    uint32_t raw_fp = (uint32_t)rssi << 8;

    if (raw_fp > smooth_rssi) {
        /* Attack: alpha = 1/4 -> fast rise */
        smooth_rssi += (raw_fp - smooth_rssi) >> 2;
    } else {
        /* Decay: alpha = 1/16 -> slow fall */
        smooth_rssi -= (smooth_rssi - raw_fp) >> 4;
    }

    uint16_t display_rssi = (uint16_t)(smooth_rssi >> 8);

    /*
     * Map raw RSSI (0-511 range from BK4829 reg 0x0C, 9 bits) to S-meter.
     * Approximate: S0 < 50, S1=50, S3=100, S5=150, S7=200, S9=256,
     * +20=320, +40=384, +60=448
     */
    uint8_t filled = 0;
    if      (display_rssi >= 448) filled = 12;
    else if (display_rssi >= 384) filled = 11;
    else if (display_rssi >= 320) filled = 10;
    else if (display_rssi >= 256) filled = 9;
    else if (display_rssi >= 50)  filled = (uint8_t)(1 + ((display_rssi - 50) * 8) / 206);
    /* else filled = 0 */

    if (filled > SMETER_SEGMENTS)
        filled = SMETER_SEGMENTS;

    /* Draw segments */
    uint16_t seg_w = (SMETER_BAR_W - (SMETER_SEGMENTS - 1) * SMETER_SEG_GAP) /
                     SMETER_SEGMENTS;
    uint16_t bar_y = LAYOUT_SMETER_Y + 7;

    for (uint8_t i = 0; i < SMETER_SEGMENTS; i++) {
        uint16_t sx = SMETER_BAR_X + i * (seg_w + SMETER_SEG_GAP);
        uint16_t color;

        if (i >= filled) {
            color = COLOR_DARK_GRAY;
        } else if (i < 5) {
            color = COLOR_GREEN;
        } else if (i < 9) {
            color = COLOR_YELLOW;
        } else {
            color = COLOR_RED;
        }

        lcd_fill_rect(sx, bar_y, seg_w, SMETER_BAR_H, color);
    }
}

/* ========================================================================
 *  display_draw_info_bar - Channel name or status text (y=270..319)
 * ======================================================================== */

void display_draw_info_bar(void)
{
    lcd_fill_rect(0, LAYOUT_INFO_Y, LCD_WIDTH, LAYOUT_INFO_H, COLOR_BLACK);

    /* Separator line */
    lcd_fill_rect(0, LAYOUT_INFO_Y, LCD_WIDTH, 1, COLOR_DARK_GRAY);

    /* DTMF decode display - show incoming digits when present */
    const char *dtmf = radio_dtmf_decode_buf();
    if (dtmf[0] != '\0') {
        font_draw_string(FONT_MEDIUM, 4, LAYOUT_INFO_Y + 6,
                         "DTMF:", COLOR_YELLOW, COLOR_BLACK);
        font_draw_string(FONT_MEDIUM, 50, LAYOUT_INFO_Y + 6,
                         dtmf, COLOR_WHITE, COLOR_BLACK);
        return;
    }

    /* APRS RX packet display - show last decoded station */
    const aprs_packet_t *aprs_pkt = aprs_rx_last_packet();
    if (aprs_pkt) {
        font_draw_string(FONT_MEDIUM, 4, LAYOUT_INFO_Y + 6,
                         "APRS:", COLOR_GREEN, COLOR_BLACK);
        font_draw_string(FONT_MEDIUM, 50, LAYOUT_INFO_Y + 6,
                         aprs_pkt->src_call, COLOR_WHITE, COLOR_BLACK);
        return;
    }

    /* Display active VFO frequency as status text */
    radio_vfo_t active = vfo_get_active();
    const vfo_state_t *vfo = vfo_get_state(active);

    char freq_str[12];
    format_freq_text(vfo->freq_hz, freq_str, sizeof(freq_str));

    /* Build "XXX.XXX MHz" string */
    char label[20];
    {
        uint8_t i = 0;
        const char *p = freq_str;
        while (*p && i < sizeof(label) - 5)
            label[i++] = *p++;
        label[i++] = ' ';
        label[i++] = 'M';
        label[i++] = 'H';
        label[i++] = 'z';
        label[i]   = '\0';
    }

    uint16_t w = font_string_width(FONT_MEDIUM, label);
    uint16_t x = (LCD_WIDTH > w) ? (LCD_WIDTH - w) / 2 : 0;
    font_draw_string(FONT_MEDIUM, x, LAYOUT_INFO_Y + 24, label,
                     COLOR_WHITE, COLOR_BLACK);
}

/* ========================================================================
 *  display_draw_main_screen - Compose all main screen sections.
 * ======================================================================== */

void display_draw_main_screen(void)
{
    display_draw_status_bar();
    display_draw_vfo_a();
    display_draw_vfo_b();
    display_draw_smeter();
    display_draw_info_bar();
}
