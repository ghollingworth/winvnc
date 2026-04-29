/*
 * Windows input injection: mouse and keyboard.
 *
 * Uses SendInput() to inject mouse movements, button clicks, scroll events,
 * and keyboard presses/releases. Translates X11 keysyms (from VNC) to
 * Windows virtual key codes.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Inject a mouse event.
 * x, y:         normalised position in [0, 1] across the primary display
 * button_mask:  RFB button mask (bit0=left, bit1=middle, bit2=right,
 *               bit3=scrollup, bit4=scrolldown)
 */
void win_input_mouse(double x, double y, uint8_t button_mask);

/* Inject a keyboard event.
 * keysym:  X11 keysym (from VNC KeyEvent message)
 * down:    true for key press, false for release
 */
void win_input_keyboard(uint32_t keysym, bool down);
