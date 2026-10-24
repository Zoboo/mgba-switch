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

enum PlazaPage { PLAZA_HOME, PLAZA_KEY, PLAZA_BROWSE, PLAZA_DETAILS, PLAZA_GAMES };
static enum PlazaPage _page;
static size_t _focus, _rom;
static bool _loadDetails;
static const struct mCoreConfig* _config;
static struct GUIMenu _menu = { .title = "Romhack Plaza" };

struct GUIMenu* SwitchUIPlazaMenu(void) { return &_menu; }

static void _button(struct GUIParams* params, int x, int y, int width, const char* label, bool focused) {
	SwitchUIRoundRect(x, y, width, 52, 10, SURFACE);
	if (focused) SwitchUIRoundFrame(x, y, width, 52, 10, FOCUS);
	SwitchUIFontSize(params->font, 20);
	SwitchUIPrint(params->font, x + 18, y + 33, width - 36, TEXT, label);
}

static void _cardTitle(struct GUIFont* font, int x, const char* text) {
	for (unsigned row = 0; *text && row < 3; ++row) {
		while (*text == ' ') ++text;
		size_t n = 0, space = 0;
		while (text[n]) {
			size_t next = n + 1;
			while (((unsigned char) text[next] & 0xC0) == 0x80) ++next;
			if (GUIFontSpanCountWidth(font, text, next) > 312) break;
			if (text[n] == ' ') space = n;
			n = next;
		}
		if (!n) break;
		if (text[n] && space) n = space;
		char line[257];
		memcpy(line, text, n); line[n] = 0;
		SwitchUIPrint(font, x, 345 + row * 34, 312, TEXT, line);
		text += n;
	}
}

static void _listGames(struct GUIParams* params, bool modal) {
	const char* roots[16];
	size_t count = _config ? SwitchUIRomDirectoriesGet(_config, roots, 16) : 0;
	if (!count) { roots[0] = params->currentPath; count = 1; }
	SwitchPlazaListRoms(roots, count);
	_rom = 0;
	if (modal) _page = PLAZA_GAMES;
}

bool SwitchUIPlazaRun(struct GUIParams* params, bool* files) {
	UNUSED(files);
	if (!_initialized) { padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&_pad); _initialized = true; }
	struct SwitchPlazaState state;
	SwitchPlazaGetState(&state);
	if (_index >= state.count) _index = state.count ? state.count - 1 : 0;
	if (_file >= state.fileCount) _file = state.fileCount ? state.fileCount - 1 : 0;
	size_t romCount = SwitchPlazaRomCount();
	if (_rom >= romCount) _rom = romCount ? romCount - 1 : 0;
	padUpdate(&_pad);
	u64 buttons = padGetButtonsDown(&_pad);
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x = params->cx, y = params->cy;
	bool click = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
	if (click) { x = params->cx; y = params->cy; }
	if (input & (1U << GUI_INPUT_BACK)) {
		if (_page == PLAZA_HOME) { GUIInvalidateKeys(params); return false; }
		_page = _page == PLAZA_DETAILS || _page == PLAZA_GAMES ? PLAZA_BROWSE : PLAZA_HOME;
		_focus = 0;
		GUIInvalidateKeys(params);
		return true;
	}
	if (buttons & HidNpadButton_X) SwitchPlazaCancel();
	bool accept = input & (1U << GUI_INPUT_SELECT);
	if (click && y >= 662 && x < 230) {
		if (_page == PLAZA_HOME) return false;
		_page = _page == PLAZA_DETAILS || _page == PLAZA_GAMES ? PLAZA_BROWSE : PLAZA_HOME;
		_focus = 0; click = false; accept = false;
	}
	if (_page == PLAZA_HOME || _page == PLAZA_KEY) {
		if (input & ((1U << GUI_INPUT_UP) | (1U << GUI_INPUT_DOWN))) _focus ^= 1;
		if (click && x >= 100 && x < 1180 && y >= 180 && y < 304) {
			_focus = y >= 252; accept = true;
		}
		if (accept) {
			if (_page == PLAZA_HOME) {
				_page = _focus ? PLAZA_KEY : PLAZA_BROWSE; _focus = 0;
				if (_page == PLAZA_BROWSE && !state.busy) _listGames(params, false);
			} else if (!_focus) _key(params);
			else if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0, "Remove the saved Romhack Plaza API key?") == GUI_MENU_EXIT_ACCEPT) SwitchPlazaSetKey("");
		}
	} else if (_page == PLAZA_GAMES) {
		if (romCount && (input & (1U << GUI_INPUT_UP)) && _rom) --_rom;
		if (romCount && (input & (1U << GUI_INPUT_DOWN)) && _rom + 1 < romCount) ++_rom;
		if (romCount && (input & (1U << GUI_INPUT_LEFT))) _rom = _rom > 6 ? _rom - 6 : 0;
		if (romCount && (input & (1U << GUI_INPUT_RIGHT))) _rom = _rom + 6 < romCount ? _rom + 6 : romCount - 1;
		if (click && x >= 128 && x < 1152 && y >= 200 && y < 572) {
			size_t target = _rom / 6 * 6 + (y - 200) / 62;
			if (target < romCount) { _rom = target; accept = true; }
		}
		if (click && y >= 590 && y < 642 && x >= 128 && x < 1152) { _page = PLAZA_BROWSE; _focus = 0; accept = false; }
		if (accept && romCount) {
			struct SwitchPlazaRom rom;
			if (SwitchPlazaGetRom(_rom, &rom)) SwitchPlazaSetRomChecked(_rom, !rom.checked);
		}
	} else if (_page == PLAZA_BROWSE) {
		if (input & (1U << GUI_INPUT_UP)) _focus = 0;
		if (input & (1U << GUI_INPUT_DOWN)) _focus = 2;
		if (_focus < 2 && input & ((1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_RIGHT))) _focus ^= 1;
		if (_focus == 2 && state.count) {
			if ((input & (1U << GUI_INPUT_LEFT)) && _index) --_index;
			if ((input & (1U << GUI_INPUT_RIGHT)) && _index + 1 < state.count) ++_index;
		}
		if (click && y >= 150 && y < 202 && x >= 96 && x < 1184) { _focus = x < 640 ? 0 : 1; accept = true; }
		if (click && y >= 254 && y < 564 && x >= 96 && x < 1184) {
			size_t target = _index / 3 * 3 + (x - 96) / 368;
			if (target < state.count) { _focus = 2; _index = target; accept = true; }
		}
		if (accept) {
			if (!_focus) _listGames(params, true);
			else if (_focus == 1) { if (SwitchPlazaSearchSelected()) { _index = 0; _focus = 2; } }
			else if (state.count) { _page = PLAZA_DETAILS; _line = 0; _file = 0; _loadDetails = true; }
		}
	} else {
		if (_loadDetails && !state.busy && SwitchPlazaRequestFiles(_index)) {
			_loadDetails = false;
			SwitchPlazaGetState(&state);
		}
		if (buttons & HidNpadButton_L) _line = _line > 4 ? _line - 4 : 0;
		if ((buttons & HidNpadButton_R) && _line + 5 < _descriptionLines) _line += 4;
		if (state.fileCount && state.filesEntry == _index) {
			if ((input & (1U << GUI_INPUT_UP)) && _file) --_file;
			if ((input & (1U << GUI_INPUT_DOWN)) && _file + 1 < state.fileCount) ++_file;
		}
		if (click && x >= 112 && x < 1168 && y >= 436 && y < 592) {
			size_t target = _file / 3 * 3 + (y - 436) / 52;
			if (target < state.fileCount) { _file = target; accept = true; }
		}
		if (click && y >= 598 && y < 642) {
			if (x < 640) _line = _line > 4 ? _line - 4 : 0;
			else if (_line + 5 < _descriptionLines) _line += 4;
		}
		if (accept && !state.busy) {
			if (state.filesEntry == _index && state.fileCount) {
				struct SwitchPlazaEntry entry;
				struct SwitchPlazaFile file;
				if (SwitchPlazaGetEntry(_index, &entry) && SwitchPlazaGetFile(_file, &file) &&
				    GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
				    "Install %s using %s? A separate ROM will be created. Your original stays untouched.", entry.title, file.filename) == GUI_MENU_EXIT_ACCEPT)
					SwitchPlazaInstall(_index, _file);
				GUIInvalidateKeys(params);
			} else SwitchPlazaRequestFiles(_index);
		}
	}
	SwitchPlazaGetState(&state);
	params->drawStart(); params->guiPrepare();
	SwitchUIDrawRect(0, 0, 1280, 720, BASE);
	SwitchUIFontSize(params->font, 30);
	const char* title = _page == PLAZA_HOME ? "Romhack Plaza" : _page == PLAZA_KEY ? "Configure API Key" : "Browse Romhacks";
	SwitchUIPrint(params->font, 96, 62, 1088, TEXT, title);
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 96, 104, 1088, MUTED, "Verified base ROM matches only. Patch downloads limited to 5 per minute.");
	if (_page == PLAZA_HOME || _page == PLAZA_KEY) {
		_button(params, 100, 180, 1080, _page == PLAZA_HOME ? "Browse Romhacks" : "Enter / Replace API Key", _focus == 0);
		_button(params, 100, 252, 1080, _page == PLAZA_HOME ? "Configure API Key" : "Remove API Key", _focus == 1);
		if (_page == PLAZA_KEY) {
			SwitchUIPrint(params->font, 100, 358, 1080, FOCUS, SwitchPlazaHasKey() ? "API key configured" : "No API key configured");
			SwitchUIPrint(params->font, 100, 410, 1080, MUTED, "Create a key at romhackplaza.org > Settings > API Keys.");
			SwitchUIPrint(params->font, 100, 450, 1080, MUTED, "Enable entries:read and entries:download permissions.");
			SwitchUIPrint(params->font, 100, 490, 1080, MUTED, "Or place the key in /mGBA/romhackingplaza/api-key.txt and restart.");
		}
	} else if (_page == PLAZA_DETAILS && state.count) {
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
					int row = 436 + (i - first) * 52;
					if (i == _file) SwitchUIRoundFrame(112, row, 1056, 48, 8, FOCUS);
					SwitchUIPrint(params->font, 128, row + 30, 1000, TEXT, file.filename);
				}
			} else SwitchUIPrint(params->font, 120, 480, 1040, MUTED, _loadDetails ? "Waiting for search to finish before loading patch files..." : state.busy ? "Loading patch files..." : "A: retry loading patch files");
			SwitchUIHint(params->font, 120, 620, "L/R", "Read description");
		}
	} else {
		_button(params, 96, 150, 528, "Set Games To Search For", _focus == 0);
		_button(params, 656, 150, 528, state.busy ? "Search (task in progress)" : "Search", _focus == 1);
		if (state.count) {
			size_t first = _index / 3 * 3;
			for (size_t i = first; i < first + 3 && i < state.count; ++i) {
				struct SwitchPlazaEntry entry;
				if (!SwitchPlazaGetEntry(i, &entry)) continue;
				int left = 96 + (i - first) * 368;
				SwitchUIRoundRect(left, 254, 352, 310, 12, SURFACE);
				if (i == _index && _focus == 2) SwitchUIRoundFrame(left, 254, 352, 310, 12, FOCUS);
				SwitchUIFontSize(params->font, 16);
				SwitchUIPrint(params->font, left + 20, 290, 312, FOCUS, "COMPATIBLE ROM HACK");
				SwitchUIFontSize(params->font, 24);
				_cardTitle(params->font, left + 20, entry.title);
				SwitchUIFontSize(params->font, 18);
				const char* base = strrchr(entry.baseRom, '/');
				SwitchUIPrint(params->font, left + 20, 474, 312, MUTED, base ? base + 1 : entry.baseRom);
				SwitchUIPrint(params->font, left + 20, 532, 312, MUTED, "View description and patches");
			}
			char count[64]; snprintf(count, sizeof(count), "%zu / %zu", _index + 1, state.count);
			SwitchUIPrint(params->font, 96, 608, 1088, MUTED, count);
		} else {
			SwitchUIFontSize(params->font, 24);
			SwitchUIPrint(params->font, 120, 330, 1040, TEXT, state.busy ? "Finding compatible ROM hacks..." : "Choose your games, then select Search");
			SwitchUIFontSize(params->font, 18);
			SwitchUIPrint(params->font, 120, 380, 1040, MUTED, "Only checked games are searched. Original ROMs remain untouched.");
		}
	}
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 96, 642, 1088, FOCUS, state.message);
	if (state.busy) {
		SwitchUIRoundRect(96, 650, 1088, 4, 2, SURFACE);
		if (state.percent) SwitchUIRoundRect(96, 650, 1088 * state.percent / 100, 4, 2, FOCUS);
	}
	SwitchUIHint(params->font, 96, 699, "B", "Back");
	SwitchUIHint(params->font, 300, 699, "A", _page == PLAZA_DETAILS ? "Install / Retry" : "Select");
	if (state.busy) SwitchUIHint(params->font, 1010, 699, "X", "Cancel task");
	if (_page == PLAZA_GAMES) {
		SwitchUIRoundRect(96, 120, 1088, 532, 16, BASE);
		SwitchUIRoundFrame(96, 120, 1088, 532, 16, FOCUS);
		SwitchUIFontSize(params->font, 26);
		SwitchUIPrint(params->font, 128, 162, 1024, TEXT, "Set Games To Search For");
		size_t first = _rom / 6 * 6;
		for (size_t i = first; i < first + 6 && i < romCount; ++i) {
			struct SwitchPlazaRom rom;
			if (!SwitchPlazaGetRom(i, &rom)) continue;
			int row = 200 + (i - first) * 62;
			if (i == _rom) SwitchUIRoundRect(128, row, 1024, 56, 8, SURFACE);
			if (rom.checked) SwitchUIRoundRect(144, row + 15, 26, 26, 4, FOCUS);
			else SwitchUIRoundFrame(144, row + 15, 26, 26, 4, MUTED);
			SwitchUIFontSize(params->font, 20);
			SwitchUIPrint(params->font, 190, row + 35, 940, TEXT, rom.title);
		}
		if (!romCount) SwitchUIPrint(params->font, 128, 250, 1024, MUTED, state.busy ? "Reading and identifying ROMs..." : "No supported ROMs found in configured folders.");
		_button(params, 128, 590, 1024, "Done (B) - checked games are remembered", false);
		SwitchUIFontSize(params->font, 16);
		if (state.busy) SwitchUIPrint(params->font, 128, 580, 1024, MUTED, state.message);
		SwitchUIHint(params->font, 300, 699, "A", "Check / Uncheck");
	}
	SwitchUIFontSize(params->font, 24);
	params->guiFinish(); params->drawEnd();
	return true;
}

void SwitchUIPlazaOpen(struct GUIParams* params, const struct mCoreConfig* config) {
	_config = config;
	_page = PLAZA_HOME;
	_focus = 0;
	GUIInvalidateKeys(params);
	bool files = false;
	while (appletMainLoop() && SwitchUIPlazaRun(params, &files)) {}
	_config = NULL;
	GUIInvalidateKeys(params);
}
