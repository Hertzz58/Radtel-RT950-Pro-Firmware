/*
 * text_input.c - T9-style multi-tap text input for the RT-950 Pro
 *
 * Each keypad digit (2-9) cycles through a letter group on repeated press.
 * Key 0 = space, key 1 = common symbols.  Encoder moves cursor.
 * MENU (KEY_C_MENU) confirms, EXIT (KEY_D_BAND) cancels.
 *
 * Multi-tap timeout: if no repeat of the same key within 800 ms,
 * the current character is committed and cursor advances.
 */

#include "app/text_input.h"
#include "app/keypad.h"
#include "app/font.h"
#include "app/display.h"
#include "drivers/lcd.h"

extern uint32_t get_tick(void);

/* T9 multi-tap character maps ------------------------------------------ */

static const char *key_map[] = {
    /* KEY_0  (13) */ " 0",
    /* KEY_1  (0)  */ "1.,-?!@#",
    /* KEY_2  (1)  */ "ABC2abc",
    /* KEY_3  (2)  */ "DEF3def",
    /* KEY_4  (4)  */ "GHI4ghi",
    /* KEY_5  (5)  */ "JKL5jkl",
    /* KEY_6  (6)  */ "MNO6mno",
    /* KEY_7  (8)  */ "PQRS7pqrs",
    /* KEY_8  (9)  */ "TUV8tuv",
    /* KEY_9  (10) */ "WXYZ9wxyz",
};

/* Map KEY_* code to key_map index, or -1 if not a text key */
static int key_to_map(uint8_t key)
{
    switch (key) {
    case KEY_0: return 0;
    case KEY_1: return 1;
    case KEY_2: return 2;
    case KEY_3: return 3;
    case KEY_4: return 4;
    case KEY_5: return 5;
    case KEY_6: return 6;
    case KEY_7: return 7;
    case KEY_8: return 8;
    case KEY_9: return 9;
    default:    return -1;
    }
}

/* Editor state --------------------------------------------------------- */

#define TEXT_INPUT_MAX  12   /* max channel name length */
#define MULTITAP_TIMEOUT_MS  800

static char    edit_buf[TEXT_INPUT_MAX + 1];
static char   *dest_buf;        /* caller's buffer - written on confirm */
static uint8_t max_len;
static uint8_t cursor;          /* cursor position 0..len-1 */
static uint8_t active;
static uint8_t confirmed;

/* Multi-tap state */
static int8_t  last_map_idx;    /* which key_map row was last pressed */
static uint8_t tap_pos;         /* position within that row's string */
static uint32_t last_tap_ms;    /* tick of last tap */

/* Lifecycle ------------------------------------------------------------ */

void text_input_start(char *buf, uint8_t mlen)
{
    dest_buf = buf;
    max_len = (mlen > TEXT_INPUT_MAX) ? TEXT_INPUT_MAX : mlen;

    /* Copy existing content, trim trailing spaces */
    uint8_t i;
    for (i = 0; i < max_len && buf[i] != '\0'; i++)
        edit_buf[i] = buf[i];
    edit_buf[i] = '\0';

    /* Strip trailing spaces */
    while (i > 0 && edit_buf[i - 1] == ' ') {
        i--;
        edit_buf[i] = '\0';
    }

    cursor = i;  /* cursor at end */
    active = 1;
    confirmed = 0;
    last_map_idx = -1;
    tap_pos = 0;
    last_tap_ms = 0;
}

uint8_t text_input_is_active(void) { return active; }
uint8_t text_input_confirmed(void) { return confirmed; }

/* Commit multi-tap (advance cursor past current char) */
static void commit_tap(void)
{
    if (last_map_idx >= 0) {
        last_map_idx = -1;
        /* Cursor already points at the character we just placed */
        if (cursor < max_len)
            cursor++;
    }
}

/* Key handler ---------------------------------------------------------- */

void text_input_handle_key(uint8_t key)
{
    if (!active) return;

    /* Check multi-tap timeout */
    if (last_map_idx >= 0 && (get_tick() - last_tap_ms) > MULTITAP_TIMEOUT_MS)
        commit_tap();

    /* Confirm */
    if (key == KEY_C_MENU) {
        commit_tap();
        /* Copy result back, pad with spaces */
        uint8_t i;
        for (i = 0; i < max_len && edit_buf[i]; i++)
            dest_buf[i] = edit_buf[i];
        for (; i < max_len; i++)
            dest_buf[i] = ' ';
        dest_buf[max_len] = '\0';
        confirmed = 1;
        active = 0;
        return;
    }

    /* Cancel */
    if (key == KEY_D_BAND) {
        active = 0;
        confirmed = 0;
        return;
    }

    /* Backspace: STAR key */
    if (key == KEY_STAR) {
        last_map_idx = -1;
        if (cursor > 0) {
            cursor--;
            /* Shift left */
            for (uint8_t j = cursor; edit_buf[j]; j++)
                edit_buf[j] = edit_buf[j + 1];
        }
        return;
    }

    /* Hash key: toggle case of current char */
    if (key == KEY_HASH) {
        if (cursor > 0) {
            char c = edit_buf[cursor - 1];
            if (c >= 'A' && c <= 'Z')
                edit_buf[cursor - 1] = c + 32;
            else if (c >= 'a' && c <= 'z')
                edit_buf[cursor - 1] = c - 32;
        }
        return;
    }

    /* T9 multi-tap keys */
    int mi = key_to_map(key);
    if (mi < 0) return;

    if (mi == last_map_idx) {
        /* Same key - cycle to next character in group */
        const char *group = key_map[mi];
        uint8_t glen = 0;
        while (group[glen]) glen++;
        tap_pos = (tap_pos + 1) % glen;
        edit_buf[cursor] = group[tap_pos];
        last_tap_ms = get_tick();
    } else {
        /* Different key - commit previous tap, start new */
        commit_tap();

        if (cursor >= max_len) return;  /* buffer full */

        /* Insert character at cursor */
        last_map_idx = mi;
        tap_pos = 0;
        edit_buf[cursor] = key_map[mi][0];
        edit_buf[cursor + 1] = '\0';
        last_tap_ms = get_tick();
    }
}

/* Encoder handler ------------------------------------------------------ */

void text_input_handle_encoder(int8_t direction)
{
    if (!active) return;

    commit_tap();

    uint8_t len = 0;
    while (edit_buf[len]) len++;

    if (direction > 0 && cursor < len)
        cursor++;
    else if (direction < 0 && cursor > 0)
        cursor--;
}

/* Draw ----------------------------------------------------------------- */

void text_input_draw(const char *title)
{
    if (!active) return;

    /* Check multi-tap timeout for visual feedback */
    if (last_map_idx >= 0 && (get_tick() - last_tap_ms) > MULTITAP_TIMEOUT_MS)
        commit_tap();

    /* Draw on info bar area */
    lcd_fill_rect(0, LAYOUT_INFO_Y, LCD_WIDTH, LAYOUT_INFO_H, COLOR_BLACK);

    /* Header */
    display_draw_text(4, LAYOUT_INFO_Y + 2, title, COLOR_YELLOW, COLOR_BLACK);

    /* Draw text with cursor underline */
    uint16_t x = 8;
    uint16_t y = LAYOUT_INFO_Y + 20;

    uint8_t len = 0;
    while (edit_buf[len]) len++;

    for (uint8_t i = 0; i <= max_len; i++) {
        char ch = (i < len) ? edit_buf[i] : ' ';
        char str[2] = { ch, '\0' };

        uint16_t fg = COLOR_WHITE;
        if (i == cursor && last_map_idx >= 0)
            fg = COLOR_YELLOW;  /* active multi-tap char */

        font_draw_string(FONT_MEDIUM, x, y, str, fg, COLOR_BLACK);

        /* Cursor underline */
        if (i == cursor) {
            uint16_t cw = font_string_width(FONT_MEDIUM, str);
            lcd_fill_rect(x, y + 16, cw, 2, COLOR_CYAN);
        }

        x += font_string_width(FONT_MEDIUM, str);
        if (x > LCD_WIDTH - 10) break;
    }
}
