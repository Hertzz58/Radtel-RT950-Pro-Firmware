/*
 * text_input.h - T9-style alphanumeric text input for RT-950 Pro
 *
 * Keypad 2-9 cycles through letters (multi-tap), 0=space, 1=symbols.
 * Encoder moves cursor left/right.  MENU confirms, EXIT cancels.
 *
 * Usage:
 *   text_input_start(buf, max_len);
 *   // route keys + encoder to text_input_handle_key / handle_encoder
 *   // call text_input_draw("Edit Name:") in the display loop
 *   // check text_input_is_active() - becomes false on confirm/cancel
 *   // text_input_confirmed() true if user pressed MENU (not EXIT)
 */

#ifndef APP_TEXT_INPUT_H
#define APP_TEXT_INPUT_H

#include <stdint.h>

/* Start editing a buffer (copies in, edits copy, copies back on confirm) */
void text_input_start(char *buf, uint8_t max_len);

/* Returns 1 while the editor is active */
uint8_t text_input_is_active(void);

/* Returns 1 if user confirmed (MENU), 0 if cancelled (EXIT) */
uint8_t text_input_confirmed(void);

/* Input handlers */
void text_input_handle_key(uint8_t key);
void text_input_handle_encoder(int8_t direction);

/* Draw the editor UI (call from display refresh) */
void text_input_draw(const char *title);

#endif /* APP_TEXT_INPUT_H */
