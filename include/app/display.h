/*
 * display.h - Display / UI abstraction for the RT-950 Pro
 *
 * Higher-level drawing primitives on top of the LCD 8080 driver.
 * Uses a built-in 5x7 bitmap font for text rendering and flash fonts
 * (via font.h) for the main screen layout.
 * All coordinates are in pixels; (0,0) = top-left corner.
 *
 * V0.27 OEM display architecture:
 *   display_periodic_refresh @ 0x08003B3C (184B, called from main_task_dispatch)
 *   display_render_engine    @ 0x08019344 (27,380B, TBB jump table on r0)
 *     - Mode 0: screen_draw_main   (normal VFO view)
 *     - Mode 1: @ 0x0801FB50       (status/alternate view)
 *     - Mode 3: @ 0x080174D4       (frequency display mode)
 *     - Mode 4: @ 0x08017530       (channel info display)
 *     - Mode 6: @ 0x0800B65C       (special mode, arg=1)
 *     - Mode 7: @ 0x0801FDA4       (extended display)
 *   g_display state          @ 0x2000A8D0 (173B render state)
 *   g_rf_state[3]            @ 0x2000A3B7 (display mode selector)
 *
 *   screen_draw_main   @ 0x08017710 (VFO/channel main view)
 *   screen_draw_freq   @ 0x080173F4 (large frequency digits)
 *   screen_draw_line_a @ 0x08017084 (VFO A info line)
 *   screen_draw_line_b @ 0x08017174 (VFO B info line)
 *   screen_draw_status_line @ 0x08017AC8 (footer/status)
 *
 *   display_status_bar @ 0x08020340 (status icons, all procedural)
 *   display_bar_pixel_set (S-meter bar drawing)
 *
 * Boot splash (V0.27): RGB565 240x320, stored at SPI 0x090000.
 *   Loaded in 10 blocks x 15360 bytes, big-endian byte-swapped on read.
 *   Splash disabled when calibration validity flag at 0xF0E0 == 0xFF.
 *   OEM LCD_Init @ fw 0x08026954, LCD_BulkTransfer @ fw 0x080266C4.
 *
 * OEM uses procedural icon rendering (no pre-rendered bitmap assets).
 * Status bar icons: text labels + filled rectangles for battery.
 * S-meter: 12-segment bar graph (S1-S9 + 3 dB-over).
 *
 * See font.h for glyph flash addresses and CJK index formula.
 */

#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdint.h>

/* Font metrics (5x7 bitmap, 1-pixel inter-character gap) */
#define DISPLAY_FONT_W  5
#define DISPLAY_FONT_H  7
#define DISPLAY_CHAR_GAP 1

/* RGB565 color palette ----------------------------------------------- */
#define COLOR_BLACK     0x0000
#define COLOR_WHITE     0xFFFF
#define COLOR_RED       0xF800
#define COLOR_GREEN     0x07E0
#define COLOR_BLUE      0x001F
#define COLOR_YELLOW    0xFFE0
#define COLOR_DARK_GRAY 0x4208
#define COLOR_GRAY      0x8410
#define COLOR_ORANGE    0xFD20
#define COLOR_CYAN      0x07FF

/* Screen layout geometry --------------------------------------------- */
#define LAYOUT_STATUS_Y     0
#define LAYOUT_STATUS_H     20
#define LAYOUT_VFO_A_Y      20
#define LAYOUT_VFO_A_H      140
#define LAYOUT_VFO_B_Y      160
#define LAYOUT_VFO_B_H      80
#define LAYOUT_SMETER_Y     240
#define LAYOUT_SMETER_H     30
#define LAYOUT_INFO_Y       270
#define LAYOUT_INFO_H       50

/*
 * Display modes - OEM has 11 modes via TBB at 0x08019344.
 * We implement a subset, expanding as features are added.
 */
typedef enum {
    DISPLAY_MODE_MAIN = 0,      /* Normal VFO/channel view */
    DISPLAY_MODE_MENU,          /* Menu overlay */
    DISPLAY_MODE_FREQ_ENTRY,    /* Direct frequency input */
    DISPLAY_MODE_FM_RADIO,      /* FM broadcast receiver */
    DISPLAY_MODE_AM_RADIO,      /* AM/SW broadcast receiver */
    DISPLAY_MODE_CHANNEL,       /* Channel/zone browser */
    DISPLAY_MODE_COUNT,
} display_mode_t;

/* Get/set current display mode */
display_mode_t display_get_mode(void);
void display_set_mode(display_mode_t mode);

/*
 * display_init - Initialize the display subsystem.
 * Calls lcd_init() and prepares the screen.
 */
void display_init(void);

/*
 * display_clear - Fill the entire screen with a solid RGB565 color.
 */
void display_clear(uint16_t color);

/*
 * display_draw_char - Draw a single ASCII character at pixel position (x, y).
 * Characters outside the printable range (0x20-0x7E) are rendered as '?'.
 */
void display_draw_char(uint16_t x, uint16_t y, char ch, uint16_t fg, uint16_t bg);

/*
 * display_draw_text - Draw a null-terminated string starting at (x, y).
 * UTF-8 text supports ASCII plus Turkish and Russian glyphs.
 * Characters advance by (DISPLAY_FONT_W + DISPLAY_CHAR_GAP) pixels horizontally.
 */
void display_draw_text(uint16_t x, uint16_t y, const char *str, uint16_t fg, uint16_t bg);

/*
 * display_draw_hline - Draw a horizontal line of 1-pixel thickness.
 */
void display_draw_hline(uint16_t x, uint16_t y, uint16_t w, uint16_t color);

/*
 * display_draw_vline - Draw a vertical line of 1-pixel thickness.
 */
void display_draw_vline(uint16_t x, uint16_t y, uint16_t h, uint16_t color);

/*
 * display_printf - Formatted text output at pixel position (x, y).
 * Supports standard printf format specifiers (limited to 128-char buffer).
 */
void display_printf(uint16_t x, uint16_t y, uint16_t fg, uint16_t bg,
                    const char *fmt, ...);

/*
 * display_update - Main display refresh, called at ~30 fps from the main loop.
 * Routes to main screen, menu, or freq-entry screen as appropriate.
 */
void display_update(void);

/* Main screen layout rendering --------------------------------------- */

/* Draw the complete main screen (all sections) */
void display_draw_main_screen(void);

/* Draw individual sections (for partial updates) */
void display_draw_status_bar(void);
void display_draw_vfo_a(void);
void display_draw_vfo_b(void);
void display_draw_smeter(void);
void display_draw_info_bar(void);

#endif /* APP_DISPLAY_H */
