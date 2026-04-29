/*
 * Windows input injection implementation.
 *
 * Uses SendInput() with absolute coordinates for mouse and virtual key
 * codes for keyboard. Translates X11 keysyms to Windows VK codes.
 */

#ifdef _WIN32

#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "win-input.h"

#define WHEEL_DELTA_VAL 120

static uint8_t prev_button_mask = 0;

void win_input_mouse(double x, double y, uint8_t button_mask)
{
	/* MOUSEEVENTF_ABSOLUTE without MOUSEEVENTF_VIRTUALDESK maps the
	 * (0,0)–(65535,65535) range to the primary monitor's pixel grid,
	 * with 65535 hitting the last pixel. We capture only the primary
	 * display, so this matches the framebuffer one-to-one and stays
	 * correct across any client-driven desktop resize. */
	if (x < 0.0) x = 0.0;
	if (x > 1.0) x = 1.0;
	if (y < 0.0) y = 0.0;
	if (y > 1.0) y = 1.0;

	INPUT input = { 0 };
	input.type = INPUT_MOUSE;
	input.mi.dx = (LONG)lround(x * 65535.0);
	input.mi.dy = (LONG)lround(y * 65535.0);
	input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

	/* Left button */
	if ((button_mask & 1) && !(prev_button_mask & 1))
		input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
	else if (!(button_mask & 1) && (prev_button_mask & 1))
		input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;

	/* Middle button */
	if ((button_mask & 2) && !(prev_button_mask & 2))
		input.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN;
	else if (!(button_mask & 2) && (prev_button_mask & 2))
		input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;

	/* Right button */
	if ((button_mask & 4) && !(prev_button_mask & 4))
		input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
	else if (!(button_mask & 4) && (prev_button_mask & 4))
		input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;

	/* Scroll wheel */
	if (button_mask & 8) {
		input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
		input.mi.mouseData = WHEEL_DELTA_VAL;
	}
	if (button_mask & 16) {
		input.mi.dwFlags |= MOUSEEVENTF_WHEEL;
		input.mi.mouseData = (DWORD)(-(int)WHEEL_DELTA_VAL);
	}

	SendInput(1, &input, sizeof(INPUT));
	prev_button_mask = button_mask;
}

static uint16_t keysym_to_vk(uint32_t keysym, bool* extended)
{
	*extended = false;

	/* Latin-1 printable ASCII */
	if (keysym >= 0x20 && keysym <= 0x7e) {
		uint8_t ch = (uint8_t)keysym;
		if (ch >= 'a' && ch <= 'z')
			return ch - 32; /* VK codes are uppercase */
		return ch;
	}

	switch (keysym) {
	case 0xff08: return VK_BACK;
	case 0xff09: return VK_TAB;
	case 0xff0d: return VK_RETURN;
	case 0xff1b: return VK_ESCAPE;
	case 0xffff: *extended = true; return VK_DELETE;
	case 0xff50: *extended = true; return VK_HOME;
	case 0xff51: *extended = true; return VK_LEFT;
	case 0xff52: *extended = true; return VK_UP;
	case 0xff53: *extended = true; return VK_RIGHT;
	case 0xff54: *extended = true; return VK_DOWN;
	case 0xff55: *extended = true; return VK_PRIOR;
	case 0xff56: *extended = true; return VK_NEXT;
	case 0xff57: *extended = true; return VK_END;
	case 0xff63: *extended = true; return VK_INSERT;

	case 0xffe1: return VK_LSHIFT;
	case 0xffe2: return VK_RSHIFT;
	case 0xffe3: return VK_LCONTROL;
	case 0xffe4: *extended = true; return VK_RCONTROL;
	case 0xffe9: return VK_LMENU;
	case 0xffea: *extended = true; return VK_RMENU;
	case 0xffeb: return VK_LWIN;
	case 0xffec: return VK_RWIN;
	case 0xffe5: return VK_CAPITAL;

	case 0xffbe: return VK_F1;
	case 0xffbf: return VK_F2;
	case 0xffc0: return VK_F3;
	case 0xffc1: return VK_F4;
	case 0xffc2: return VK_F5;
	case 0xffc3: return VK_F6;
	case 0xffc4: return VK_F7;
	case 0xffc5: return VK_F8;
	case 0xffc6: return VK_F9;
	case 0xffc7: return VK_F10;
	case 0xffc8: return VK_F11;
	case 0xffc9: return VK_F12;

	case 0xff13: return VK_PAUSE;
	case 0xff14: return VK_SCROLL;
	case 0xff61: return VK_SNAPSHOT;
	case 0xff7f: return VK_NUMLOCK;
	}

	return 0;
}

void win_input_keyboard(uint32_t keysym, bool down)
{
	bool extended = false;
	uint16_t vk = keysym_to_vk(keysym, &extended);
	if (vk == 0)
		return;

	INPUT input = { 0 };
	input.type = INPUT_KEYBOARD;
	input.ki.wVk = vk;
	input.ki.dwFlags = 0;

	if (!down)
		input.ki.dwFlags |= KEYEVENTF_KEYUP;
	if (extended)
		input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;

	SendInput(1, &input, sizeof(INPUT));
}

#endif /* _WIN32 */
