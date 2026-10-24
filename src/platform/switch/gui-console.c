/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/string.h>

#define CONSOLE_TEXT 0xFFF5F3EF
#define CONSOLE_MUTED 0xFFD0C9C1
#define CONSOLE_FOCUS 0xFFFFA3BD

static bool _navigationFocused;
static unsigned _navigationIndex;
static bool _navigationExit;

bool SwitchUINavigationExitRequested(void) {
	bool requested = _navigationExit;
	_navigationExit = false;
	return requested;
}

int SwitchUINavigationInput(struct GUIParams* params, unsigned current, bool enter, uint32_t input,
                            bool tapped, unsigned x, unsigned y) {
	static const unsigned tops[] = { 78, 148, 218, 288, 588 };
	int activate = -1;
	if (tapped && x < (_navigationFocused ? 220U : 72U)) {
		if (!_navigationFocused) _navigationIndex = current;
		_navigationFocused = true;
		for (unsigned i = 0; i < 5; ++i)
			if (y >= tops[i] && y < tops[i] + 48) { _navigationIndex = i; activate = i; }
	} else if (!_navigationFocused && enter && (input & (1U << GUI_INPUT_LEFT))) {
		_navigationFocused = true;
		_navigationIndex = current;
		return -2;
	} else if (!_navigationFocused) return -1;
	if (input & ((1U << GUI_INPUT_RIGHT) | (1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) {
		_navigationFocused = false;
		return -2;
	}
	if (tapped && x >= 220) { _navigationFocused = false; return -3; }
	if (input & (1U << GUI_INPUT_UP)) _navigationIndex = (_navigationIndex + 4) % 5;
	if (input & (1U << GUI_INPUT_DOWN)) _navigationIndex = (_navigationIndex + 1) % 5;
	if (input & (1U << GUI_INPUT_SELECT)) activate = _navigationIndex;
	if (activate >= 0) {
		_navigationFocused = false;
		GUIInvalidateKeys(params);
		if (activate == 4) _navigationExit = true;
		return activate;
	}
	return -2;
}

void SwitchUINavigationDraw(struct GUIParams* params, unsigned current) {
	static const unsigned tops[] = { 78, 148, 218, 288, 588 };
	static const char* labels[] = { "Library", "Files", "Settings", "Download Romhacks", "Exit" };
	static const char* symbols[] = { "", "", "", "", "X" };
	struct GUIFont* font = params->font;
	unsigned oldSize = SwitchUIFontSizeGet(font);
	int width = _navigationFocused ? 220 : 72;
	SwitchUIDrawRect(0, 0, width, 668, 0xF52B1B21);
	SwitchUIFontSize(font, _navigationFocused ? 20 : 12);
	GUIFontPrint(font, _navigationFocused ? 24 : 36, 36,
	             _navigationFocused ? GUI_ALIGN_LEFT : GUI_ALIGN_HCENTER, CONSOLE_TEXT,
	             "mGBA");
	for (unsigned i = 0; i < 5; ++i) {
		bool focused = _navigationFocused && i == _navigationIndex;
		uint32_t color = focused || i == current ? CONSOLE_FOCUS : CONSOLE_TEXT;
		if (focused || i == current)
			SwitchUIRoundRect(3, tops[i], width - 6, 48, 7, 0xFF4F3038);
		if (focused) SwitchUIRoundFrame(3, tops[i], width - 6, 48, 7, CONSOLE_FOCUS);
		if (i == 0) {
			SwitchUIRoundRect(26, tops[i] + 14, 20, 19, 3, color);
			SwitchUIDrawRect(30, tops[i] + 18, 12, 7, 0xFF2B1B21);
		} else if (i == 1) {
			SwitchUIRoundRect(26, tops[i] + 13, 10, 6, 2, color);
			SwitchUIRoundRect(26, tops[i] + 17, 20, 16, 2, color);
		} else if (i == 2) {
			SwitchUIRoundFrame(27, tops[i] + 15, 18, 18, 7, color);
			SwitchUIDrawRect(33, tops[i] + 11, 6, 4, color);
			SwitchUIDrawRect(33, tops[i] + 33, 6, 4, color);
			SwitchUIDrawRect(23, tops[i] + 21, 4, 6, color);
			SwitchUIDrawRect(45, tops[i] + 21, 4, 6, color);
		} else if (i == 3) {
			SwitchUIDrawRect(34, tops[i] + 11, 4, 16, color);
			for (int step = 0; step < 4; ++step) {
				SwitchUIDrawRect(28 + step * 2, tops[i] + 20 + step * 2, 4, 3, color);
				SwitchUIDrawRect(40 - step * 2, tops[i] + 20 + step * 2, 4, 3, color);
			}
			SwitchUIDrawRect(26, tops[i] + 33, 20, 3, color);
		} else {
			SwitchUIFontSize(font, 22);
			GUIFontPrint(font, 36, tops[i] + 31, GUI_ALIGN_HCENTER, color, symbols[i]);
		}
		if (_navigationFocused) {
			if (i == 3) {
				SwitchUIFontSize(font, 18);
				GUIFontPrint(font, 72, tops[i] + 20, GUI_ALIGN_LEFT, color, "Download");
				GUIFontPrint(font, 72, tops[i] + 40, GUI_ALIGN_LEFT, color, "Romhacks");
			} else {
				SwitchUIFontSize(font, 22);
				GUIFontPrint(font, 72, tops[i] + 31, GUI_ALIGN_LEFT, color, labels[i]);
			}
		}
	}
	if (_navigationFocused) {
		SwitchUIFontSize(font, 16);
		GUIFontPrint(font, 16, 654, GUI_ALIGN_LEFT, CONSOLE_MUTED, "A Open   B Back");
	}
	SwitchUIFontSize(font, oldSize);
}

static void _gameTitle(const struct GUIMenuItem* entry, char* title, size_t size) {
	strlcpy(title, entry->title, size);
	char* dot = strrchr(title, '.');
	if (dot)
		*dot = 0;
}

void SwitchUIConsoleDraw(struct GUIParams* params, const struct GUIMenu* menu, const size_t* entries, size_t count,
                         size_t position, float scroll) {
	struct GUIFont* font = params->font;
	unsigned oldSize = SwitchUIFontSizeGet(font);

	if (count) {
		const struct GUIMenuItem* selected = GUIMenuItemListGetConstPointer(&menu->items, entries[position]);
		char title[512];
		_gameTitle(selected, title, sizeof(title));
		SwitchUIFontSize(font, 36);
		int width = GUIFontSpanWidth(font, title);
		SwitchUIPrint(font, width < 1040 ? 700 - width / 2 : 180, 120, 1040, CONSOLE_TEXT, title);
		const char* ext = strrchr(selected->title, '.');
		const char* platform = ext && !strcasecmp(ext, ".gba") ? "Game Boy Advance" :
		    ext && !strcasecmp(ext, ".gbc")                    ? "Game Boy Color" :
		    ext && !strcasecmp(ext, ".gb")                     ? "Game Boy" :
		                                                         "Game archive";
		SwitchUIFontSize(font, 20);
		GUIFontPrint(font, 700, 164, GUI_ALIGN_HCENTER, CONSOLE_MUTED, platform);
		int first = (int) scroll - 1;
		for (int ordinal = first; ordinal < first + 7; ++ordinal) {
			if (ordinal < 0 || (size_t) ordinal >= count) continue;
			size_t index = ordinal;
			const struct GUIMenuItem* entry = GUIMenuItemListGetConstPointer(&menu->items, entries[index]);
			bool selected = index == position;
			int x = 104 + (ordinal - scroll) * 254;
			if (x + 248 < 72 || x >= 1280) continue;
			int y = selected ? 250 : 268, w = selected ? 248 : 224, h = selected ? 368 : 336;
			char path[4096];
			SwitchUIRomPath(params, entry->title, path, sizeof(path));
			SwitchUIRoundRect(x - 4, y - 4, w + 8, h + 8, 12, 0xB00F0D0B);
			if (!SwitchUIArtworkDraw(path, false, x, y, w, h))
				SwitchUIFallback(font, entry, x, y, w, h);
			if (selected)
				SwitchUIRoundFrame(x - 5, y - 5, w + 10, h + 10, 12, CONSOLE_FOCUS);
		}
		SwitchUIFontSize(font, 16);
		GUIFontPrintf(font, 1216, 650, GUI_ALIGN_RIGHT, CONSOLE_MUTED, "%zu / %zu", position + 1, count);
	} else {
		SwitchUIFontSize(font, 30);
		GUIFontPrint(font, 676, 330, GUI_ALIGN_HCENTER, CONSOLE_TEXT, "No games in this view");
		SwitchUIFontSize(font, 20);
		GUIFontPrint(font, 676, 374, GUI_ALIGN_HCENTER, CONSOLE_MUTED,
		             "Use Files to browse your games, or L+R to change the filter.");
	}
	SwitchUIDrawRect(72, 668, 1208, 52, 0xD4211E1B);
	SwitchUIHint(font, 104, 701, "A", count ? "Play" : "Browse");
	SwitchUIHint(font, 280, 701, "B", "Back");
	SwitchUIHint(font, 444, 701, "Y", "Files");
	SwitchUIHint(font, 614, 701, "L/R", "First / Last");
	SwitchUIHint(font, 846, 701, "+", "Settings");
	SwitchUIHint(font, 1070, 701, "R3", "");
	SwitchUIFontSize(font, 16);
	SwitchUIPrint(font, 1120, 686, 140, CONSOLE_TEXT, "Download");
	SwitchUIPrint(font, 1120, 706, 140, CONSOLE_TEXT, "Romhacks");
	SwitchUIFontSize(font, oldSize);
}
