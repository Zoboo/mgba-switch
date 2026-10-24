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

static void _railItem(struct GUIFont* font, int top, const char* symbol, const char* label, bool selected) {
	if (selected)
		SwitchUIRoundRect(8, top, 56, 48, 9, 0xFF4F3038);
	SwitchUIFontSize(font, 22);
	if (!strcmp(symbol, "G") || !strcmp(symbol, "F")) {
		SwitchUIRailIcon(!strcmp(symbol, "G") ? 0 : 1, selected ? CONSOLE_FOCUS : CONSOLE_TEXT);
	} else {
		GUIFontPrint(font, 36, top + 24, GUI_ALIGN_HCENTER, selected ? CONSOLE_FOCUS : CONSOLE_TEXT, symbol);
	}
	SwitchUIFontSize(font, 12);
	GUIFontPrint(font, 36, top + 41, GUI_ALIGN_HCENTER, CONSOLE_MUTED, label);
}

static void _gameTitle(const struct GUIMenuItem* entry, char* title, size_t size) {
	strlcpy(title, entry->title, size);
	char* dot = strrchr(title, '.');
	if (dot)
		*dot = 0;
}

void SwitchUIConsoleDraw(struct GUIParams* params, const struct GUIMenu* menu, const size_t* entries, size_t count,
                         size_t position, float scroll) {
	(void) scroll;
	struct GUIFont* font = params->font;
	unsigned oldSize = SwitchUIFontSizeGet(font);
	SwitchUIDrawRect(0, 0, 72, 720, 0xDA2B1B21);
	SwitchUIFontSize(font, 14);
	GUIFontPrint(font, 36, 36, GUI_ALIGN_HCENTER, CONSOLE_TEXT, "mGBA");
	_railItem(font, 78, "G", "Library", true);
	_railItem(font, 148, "F", "Files", false);
	_railItem(font, 218, "+", "Settings", false);
	_railItem(font, 288, "+", "Add Games", false);
	_railItem(font, 588, "X", "Exit", false);

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
		unsigned shown = count < 5 ? count : 5;
		for (unsigned slot = 0; slot < shown; ++slot) {
			size_t index = (position + slot) % count;
			const struct GUIMenuItem* entry = GUIMenuItemListGetConstPointer(&menu->items, entries[index]);
			int x = slot ? 374 + ((int) slot - 1) * 244 : 104;
			int y = slot ? 268 : 250, w = slot ? 224 : 248, h = slot ? 336 : 368;
			char path[4096];
			SwitchUIRomPath(params, entry->title, path, sizeof(path));
			SwitchUIRoundRect(x - 4, y - 4, w + 8, h + 8, 12, 0xB00F0D0B);
			if (!SwitchUIArtworkDraw(path, false, x, y, w, h))
				SwitchUIFallback(font, entry, x, y, w, h);
			if (!slot)
				SwitchUIRoundFrame(x - 5, y - 5, w + 10, h + 10, 12, CONSOLE_FOCUS);
		}
		SwitchUIFontSize(font, 16);
		GUIFontPrintf(font, 1216, 650, GUI_ALIGN_RIGHT, CONSOLE_MUTED, "%zu / %zu", position + 1, count);
	} else {
		SwitchUIFontSize(font, 30);
		GUIFontPrint(font, 676, 330, GUI_ALIGN_HCENTER, CONSOLE_TEXT, "No games in this view");
		SwitchUIFontSize(font, 20);
		GUIFontPrint(font, 676, 374, GUI_ALIGN_HCENTER, CONSOLE_MUTED,
		             "Use Files to browse your games, or L/R to change the filter.");
	}
	SwitchUIDrawRect(72, 668, 1208, 52, 0xD4211E1B);
	SwitchUIHint(font, 104, 701, "A", count ? "Play" : "Browse");
	SwitchUIHint(font, 280, 701, "B", "Back");
	SwitchUIHint(font, 444, 701, "Y", "Files");
	SwitchUIHint(font, 614, 701, "L/R", "Filter");
	SwitchUIHint(font, 846, 701, "+", "Settings");
	SwitchUIHint(font, 1070, 701, "R3", "Add Games");
	SwitchUIFontSize(font, oldSize);
}
