/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include "plaza.h"
#include <mgba-util/gui/font.h>
#include <mgba-util/string.h>
#include <switch.h>

#define TEXT 0xFFF5F3EF
#define MUTED 0xFFB8B2AA
#define FOCUS 0xFFFFA3BD
#define BASE 0xFF1F1417
#define SURFACE 0xFF332024

static size_t _index, _file;
static bool _details;
static unsigned _line;
static unsigned _descriptionLines;
static PadState _pad;
static bool _initialized;

static void _key(struct GUIParams* params) {
	SwkbdConfig keyboard;
	if (R_FAILED(swkbdCreate(&keyboard, 0))) return;
	swkbdConfigMakePresetPassword(&keyboard);
	swkbdConfigSetHeaderText(&keyboard, "Romhack Plaza API key (entries:read, entries:download)");
	swkbdConfigSetStringLenMax(&keyboard, 512);
	char key[513] = { 0 };
	bool accepted = R_SUCCEEDED(swkbdShow(&keyboard, key, sizeof(key)));
	swkbdClose(&keyboard);
	if (accepted && !SwitchPlazaSetKey(key))
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not store the API key. Check SD card access.");
	memset(key, 0, sizeof(key));
	GUIInvalidateKeys(params);
}

static void _description(struct GUIFont* font, const char* text) {
	unsigned row = 0, shown = 0;
	while (*text) {
		while (*text == ' ' || *text == '\n' || *text == '\r') ++text;
		if (!*text) break;
		size_t n = 0, space = 0;
		while (text[n] && text[n] != '\n' && n < 480) {
			size_t next = n + 1;
			while (((unsigned char) text[next] & 0xC0) == 0x80) ++next;
			if (GUIFontSpanCountWidth(font, text, next) > 1040) break;
			if (text[n] == ' ') space = n;
			n = next;
		}
		if (!n) break;
		if (text[n] && text[n] != '\n' && space) n = space;
		if (row++ >= _line && shown < 5) {
			char line[513];
			memcpy(line, text, n); line[n] = 0;
			SwitchUIPrint(font, 120, 254 + 28 * shown++, 1040, TEXT, line);
		}
		text += n;
	}
	_descriptionLines = row;
}

bool SwitchUIPlazaRun(struct GUIParams* params, bool* files) {
	if (!_initialized) { padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&_pad); _initialized = true; }
	struct SwitchPlazaState state;
	SwitchPlazaGetState(&state);
	if (_index >= state.count) _index = state.count ? state.count - 1 : 0;
	if (_file >= state.fileCount) _file = state.fileCount ? state.fileCount - 1 : 0;
	padUpdate(&_pad);
	u64 buttons = padGetButtonsDown(&_pad);
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x = params->cx, y = params->cy;
	bool click = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
	if (click) {
		x = params->cx; y = params->cy;
		if (y >= 668) {
			if (x < 230) input |= 1U << GUI_INPUT_BACK;
			else if (x < 500) input |= 1U << GUI_INPUT_SELECT;
			else if (x < 760) buttons |= HidNpadButton_Y;
			else if (x < 1010) buttons |= HidNpadButton_Plus;
			else buttons |= HidNpadButton_X;
		} else if (y >= 24 && y < 80 && x >= 526 && x < 718) {
			*files = x >= 638;
			_details = false; GUIInvalidateKeys(params); return false;
		} else if (!_details && x >= 48 && x < 1232 && y >= 180 && y < 635) {
			size_t index = _index / 7 * 7 + (y - 180) / 65;
			if (index < state.count) { _index = index; input |= 1U << GUI_INPUT_SELECT; }
		} else if (_details && x >= 112 && x < 1168 && y >= 436 && y < 592) {
			size_t index = _file / 3 * 3 + (y - 436) / 52;
			if (index < state.fileCount) _file = index;
		} else if (_details && y >= 598 && y < 642) {
			if (x < 640) buttons |= HidNpadButton_L;
			else buttons |= HidNpadButton_R;
		}
	}
	if (input & (1U << GUI_INPUT_BACK)) {
		if (_details) _details = false;
		else { GUIInvalidateKeys(params); return false; }
	}
	if (buttons & HidNpadButton_Plus) _key(params);
	if (buttons & HidNpadButton_X) SwitchPlazaCancel();
	if ((buttons & HidNpadButton_Y) && !state.busy) {
		_details = false; _index = _file = 0;
		SwitchPlazaStartScan(params->currentPath);
	}
	if (_details) {
		if (buttons & HidNpadButton_L) _line = _line > 4 ? _line - 4 : 0;
		if ((buttons & HidNpadButton_R) && _line + 5 < _descriptionLines) _line += 4;
		if (state.filesEntry == _index && state.fileCount) {
			if (input & (1U << GUI_INPUT_UP)) _file = (_file + state.fileCount - 1) % state.fileCount;
			if (input & (1U << GUI_INPUT_DOWN)) _file = (_file + 1) % state.fileCount;
		}
	} else if (state.count) {
		if (input & (1U << GUI_INPUT_UP)) _index = (_index + state.count - 1) % state.count;
		if (input & (1U << GUI_INPUT_DOWN)) _index = (_index + 1) % state.count;
	}
	if ((input & (1U << GUI_INPUT_SELECT)) && state.count && !state.busy) {
		if (!_details) {
			_details = true; _line = 0; _file = 0;
			SwitchPlazaRequestFiles(_index);
		} else if (state.filesEntry == _index && state.fileCount) {
			struct SwitchPlazaEntry entry;
			struct SwitchPlazaFile file;
			if (SwitchPlazaGetEntry(_index, &entry) && SwitchPlazaGetFile(_file, &file) &&
			    GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			    "Install %s using %s? A separate ROM will be created. Your original stays untouched.", entry.title, file.filename) == GUI_MENU_EXIT_ACCEPT)
				SwitchPlazaInstall(_index, _file);
			GUIInvalidateKeys(params);
		} else SwitchPlazaRequestFiles(_index);
	}
	SwitchPlazaGetState(&state);
	params->drawStart(); params->guiPrepare();
	SwitchUIDrawRect(0, 0, 1280, 720, BASE);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 62, 400, TEXT, "Add Games");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 526, 60, 104, MUTED, "Library");
	SwitchUIPrint(params->font, 646, 60, 72, MUTED, "Files");
	SwitchUIPrint(params->font, 734, 60, 110, FOCUS, "Add Games");
	SwitchUIDrawRect(734, 76, 90, 3, FOCUS);
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 48, 104, 1184, MUTED, "Romhack Plaza • Matching published ROM hashes • Downloads limited to 5 per minute");
	SwitchUIPrint(params->font, 48, 140, 1184, FOCUS, state.message);
	if (state.busy) {
		SwitchUIRoundRect(48, 158, 1184, 4, 2, SURFACE);
		if (state.percent) SwitchUIRoundRect(48, 158, 1184 * state.percent / 100, 4, 2, FOCUS);
	}
	if (_details && state.count) {
		struct SwitchPlazaEntry entry;
		if (SwitchPlazaGetEntry(_index, &entry)) {
			SwitchUIRoundRect(88, 182, 1104, 458, 12, SURFACE);
			SwitchUIFontSize(params->font, 24);
			SwitchUIPrint(params->font, 120, 220, 1040, TEXT, entry.title);
			SwitchUIFontSize(params->font, 18);
			_description(params->font, entry.description);
			const char* base = strrchr(entry.baseRom, '/');
			SwitchUIPrint(params->font, 120, 416, 1040, MUTED, base ? base + 1 : entry.baseRom);
			if (state.filesEntry == _index && state.fileCount) {
				size_t first = _file / 3 * 3;
				for (size_t i = first; i < first + 3 && i < state.fileCount; ++i) {
					struct SwitchPlazaFile file;
					if (!SwitchPlazaGetFile(i, &file)) continue;
					int y = 436 + (i - first) * 52;
					if (i == _file) SwitchUIRoundFrame(112, y, 1056, 48, 8, FOCUS);
					SwitchUIPrint(params->font, 128, y + 30, 1000, TEXT, file.filename);
				}
			} else SwitchUIPrint(params->font, 120, 480, 1040, MUTED, state.busy ? "Loading patch files..." : "A: retry loading patch files");
			SwitchUIHint(params->font, 120, 620, "L/R", "Read description");
		}
	} else if (state.count) {
		size_t first = _index / 7 * 7;
		for (size_t i = first; i < first + 7 && i < state.count; ++i) {
			struct SwitchPlazaEntry entry;
			if (!SwitchPlazaGetEntry(i, &entry)) continue;
			int y = 180 + (i - first) * 65;
			SwitchUIRoundRect(48, y, 1184, 58, 8, SURFACE);
			if (i == _index) SwitchUIRoundFrame(48, y, 1184, 58, 8, FOCUS);
			SwitchUIFontSize(params->font, 22);
			SwitchUIPrint(params->font, 68, y + 36, 1144, TEXT, entry.title);
		}
	} else {
		SwitchUIFontSize(params->font, 26);
		SwitchUIPrint(params->font, 120, 290, 1040, TEXT, state.busy ? "Finding compatible ROM hacks..." : "Discover patches for games you own");
		SwitchUIFontSize(params->font, 20);
		SwitchUIPrint(params->font, 120, 340, 1040, MUTED, "1. Create an API key at romhackplaza.org → Settings → API Keys.");
		SwitchUIPrint(params->font, 120, 380, 1040, MUTED, "2. Enable entries:read and entries:download. Press + to enter it here.");
		SwitchUIPrint(params->font, 120, 420, 1040, MUTED, "3. Press Y to scan this library folder. Extract compressed ROMs first.");
		SwitchUIPrint(params->font, 120, 460, 1040, MUTED, "IPS, BPS and UPS patches only. No base ROMs are downloaded.");
	}
	SwitchUIFontSize(params->font, 18);
	SwitchUIDrawRect(48, 658, 1184, 1, SURFACE);
	SwitchUIHint(params->font, 48, 699, "B", _details ? "Results" : "Library");
	SwitchUIHint(params->font, 250, 699, "A", _details ? "Install / Retry" : "Details");
	SwitchUIHint(params->font, 510, 699, "Y", "Scan / Retry");
	SwitchUIHint(params->font, 780, 699, "+", "API key");
	SwitchUIHint(params->font, 1030, 699, "X", "Cancel");
	SwitchUIFontSize(params->font, 24);
	params->guiFinish(); params->drawEnd();
	return true;
}
