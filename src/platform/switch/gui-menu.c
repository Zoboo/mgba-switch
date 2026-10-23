/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/gui/menu.h>
#include <mgba-util/gui/font.h>
#include "gui-ui.h"
#include "achievements.h"

enum GUIMenuExitReason SwitchGUIMenuRunOriginal(struct GUIParams*, struct GUIMenu*, struct GUIMenuState*);

/* Keep the shared menu's state transitions and input handling in sync. */
#define GUIShowMenu SwitchGUIShowMenuOriginal
#define GUIMenuRun SwitchGUIMenuRunOriginal
#define GUIShowMessageBox SwitchGUIShowMessageBoxOriginal
#include "../../util/gui/menu.c"
#undef GUIShowMessageBox
#undef GUIMenuRun
#undef GUIShowMenu

static enum GUICursorState (*_switchMenuCursor)(unsigned*, unsigned*);
static unsigned _switchMenuLineHeight;
static unsigned _switchMenuWidth;
static unsigned _switchMenuScreenWidth;
static bool _switchConfigView;
static unsigned _switchCategory;
static unsigned _switchNextCategory;
static size_t _switchConfigStart;
static size_t _switchConfigCount;
static size_t _switchConfigTouchStart;
static const char* const _switchCategories[] = {
	"All settings", "Display", "Audio", "Controls", "System", "Interface", "Artwork", "RetroAchievements"
};

static unsigned _switchItemCategory(const struct GUIMenuItem* item) {
	if (item->submenu == SwitchUIAchievementsMenu()) return 7;
	if (item->submenu == SwitchUIInterfaceMenu()) return 5;
	if (item->submenu == SwitchUIArtworkMenu()) {
		return 6;
	}
	if (!strcmp(item->title, "Remap controls")) {
		return 3;
	}
	if (!GUIVariantIsString(item->data)) {
		return 4;
	}
	const char* key = item->data.v.s;
	if (!strcmp(key, "mute") || !strcmp(key, "fastForwardMute") || !strcmp(key, "volume")) {
		return 2;
	}
	if (!strcmp(key, "fastForwardCap") || !strcmp(key, "useLightSensor")) {
		return 3;
	}
	if (!strcmp(key, "showOSD") || !strcmp(key, "autosave") || !strcmp(key, "autoload")) {
		return 5;
	}
	if (!strcmp(key, "screenMode") || !strcmp(key, "filterMode") || !strcmp(key, "hwaccelVideo") ||
	    !strcmp(key, "videoScale") || !strcmp(key, "fpsCounter") || !strcmp(key, "gb.pal") ||
	    !strncmp(key, "interframe", 10) || !strcmp(key, "sgb.borders") || !strcmp(key, "sgb.borderCrop")) {
		return 1;
	}
	return 4;
}

static enum GUICursorState _switchPollMenuCursor(unsigned* x, unsigned* y) {
	enum GUICursorState cursor = _switchMenuCursor(x, y);
	if (cursor != GUI_CURSOR_NOT_PRESENT) {
		if (_switchConfigView && *x >= 48 && *x < 330 && *y >= 128 && *y < 128 + 8 * 56) {
			if (cursor == GUI_CURSOR_DOWN) {
				_switchNextCategory = (*y - 128) / 56;
			}
			*x = 0;
			*y = 0;
		} else if (_switchConfigView && *x >= 400 && *x < _switchMenuWidth && *y >= 568 && *y < 660) {
			size_t index = _switchConfigCount - (*y < 614 ? 2 : 1);
			*y = (index - _switchConfigTouchStart + 2) * _switchMenuLineHeight;
		} else if (*x >= _switchMenuWidth && *x < _switchMenuWidth + 24 && *y >= 112 && *y < 640) {
			*x = _switchMenuScreenWidth - 1;
			*y = 2 * _switchMenuLineHeight + (*y - 112) * ((_switchConfigView ? 10 : 12) * _switchMenuLineHeight) / 528;
		} else if (*x >= (_switchConfigView ? 400 : 48) && *x < _switchMenuWidth && *y >= 112 && *y < (_switchConfigView ? 544 : 640)) {
			*y = ((*y - 112) / 48 + 2) * _switchMenuLineHeight;
		} else {
			*x = 0;
			*y = 0;
		}
	}
	return cursor;
}

static void _switchDrawMenu(struct GUIParams* params, const struct GUIMenu* menu, const struct GUIMenuState* state) {
	size_t count = GUIMenuItemListSize(&menu->items);
	int width = menu->background && !_switchConfigView ? 440 : (int) params->width - 48;
	int left = _switchConfigView ? 400 : 48;
	params->drawStart();
	if (menu->background && count) {
		menu->background->draw(menu->background, GUIMenuItemListGetConstPointer(&menu->items, menu->index)->data.v.p);
	}
	if (params->guiPrepare) {
		params->guiPrepare();
	}
	if (menu->background && !_switchConfigView) {
		SwitchUIDrawRect(0, 0, params->width, params->height, 0x780D1412);
	}
	SwitchUIDrawRect(0, 0, menu->background && !_switchConfigView ? 472 : params->width, params->height, 0xF51D1915);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 56, width - 48, 0xFFFFFFFF,
	              _switchConfigView ? "Settings" : menu->title);
	SwitchUIFontSize(params->font, 18);
	if (menu->subtitle) {
		SwitchUIPrint(params->font, 48, 96, width - 48, 0xFFB8B2AA, menu->subtitle);
	}
	if (_switchConfigView) {
		SwitchUIDrawRect(368, 112, 1, 528, 0xFF574050);
		for (unsigned c = 0; c < 8; ++c) {
			int y = 128 + c * 56;
			if (c == _switchCategory) {
				SwitchUIRoundRect(48, y, 282, 48, 8, 0xFF4F3038);
				SwitchUIDrawRect(48, y + 12, 3, 24, 0xFFFFA3BD);
			}
			SwitchUIPrint(params->font, 64, y + 33, 250, c == _switchCategory ? 0xFFFFA3BD : 0xFFB8B2AA, _switchCategories[c]);
		}
	}
	SwitchUIFontSize(params->font, 22);
	size_t visible = _switchConfigView ? 9 : 11;
	size_t regularCount = _switchConfigView ? count - 2 : count;
	for (size_t row = 0; row < visible + (_switchConfigView ? 2 : 0); ++row) {
		size_t i = state->start + row;
		int y = 112 + row * 48;
		if (_switchConfigView && row >= visible) {
			i = regularCount + row - visible;
			y = 568 + (row - visible) * 48;
		} else if (i >= regularCount) {
			continue;
		}
		if (i >= count) {
			continue;
		}
		const struct GUIMenuItem* item = GUIMenuItemListGetConstPointer(&menu->items, i);
		uint32_t color = item->readonly ? 0xFF85827D : 0xFFF5F3EF;
		if (i == menu->index) {
			SwitchUIRoundRect(left, y, width - left, 44, 7, 0xFF4F3038);
			SwitchUIRoundFrame(left, y, width - left, 44, 7, 0xFFFFA3BD);
		}
		const char* value = NULL;
		if (item->validStates && item->state < item->nStates) {
			value = item->validStates[item->state];
		}
		int valueWidth = value ? GUIFontSpanWidth(params->font, value) : 0;
		if (valueWidth > (width - left - 32) / 2) {
			valueWidth = (width - left - 32) / 2;
		}
		SwitchUIPrint(params->font, left + 16, y + 31, width - left - 48 - (value ? valueWidth + 24 : 0), color, item->title);
		if (value) {
			SwitchUIPrint(params->font, width - 16 - valueWidth, y + 31, valueWidth, 0xFFFFA3BD, value);
		}
	}
	if (regularCount > visible) {
		SwitchUIDrawRect(width + 12, 112, 3, 528, 0xFF574050);
		SwitchUIDrawRect(width + 12, 112 + (menu->index < regularCount ? menu->index : regularCount - 1) * 504 / (regularCount - 1), 3, 24, 0xFFFFA3BD);
	}
	SwitchUIDrawRect(48, 662, params->width - 96, 1, 0xFF38332D);
	if (_switchConfigView) {
		SwitchUIHint(params->font, 48, 699, "L/R", "Category");
		SwitchUIHint(params->font, 300, 699, "A", "Select");
		SwitchUIHint(params->font, 480, 699, "B", "Back");
	} else {
		SwitchUIHint(params->font, 48, 699, "A", "Select");
		SwitchUIHint(params->font, 224, 699, "B", "Back");
	}
	const struct GUIMenuItem* selected = count ? GUIMenuItemListGetConstPointer(&menu->items, menu->index) : NULL;
	if (selected && selected->validStates && !selected->readonly) {
		SwitchUIHint(params->font, _switchConfigView ? 680 : 440, 699, "Left/Right", "Adjust");
	} else if (!_switchConfigView && count > visible) {
		SwitchUIHint(params->font, 440, 699, "Left/Right", "Page");
	}
	SwitchUIFontSize(params->font, 24);
	GUIDrawBattery(params);
	GUIDrawClock(params);
	if (params->guiFinish) {
		params->guiFinish();
	}
	params->drawEnd();
}

static enum GUIMenuExitReason _switchRunMenu(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuState* state) {
	size_t count = GUIMenuItemListSize(&menu->items);
	if (!count) {
		return GUI_MENU_EXIT_BACK;
	}
	if (menu->index >= count) {
		menu->index = count - 1;
	}
	if (state->start > menu->index) {
		state->start = menu->index;
	}
	unsigned height = params->height;
	_switchMenuCursor = params->pollCursor;
	_switchMenuLineHeight = GUIFontHeight(params->font);
	_switchMenuWidth = menu->background && !_switchConfigView ? 440 : params->width - 48;
	_switchMenuScreenWidth = params->width;
	size_t oldStart = state->start;
	_switchConfigTouchStart = state->start;
	_switchConfigCount = count;
	params->height = _switchMenuLineHeight * (_switchConfigView ? 13 : 15);
	if (_switchMenuCursor) {
		params->pollCursor = _switchPollMenuCursor;
	}
	enum GUIMenuExitReason reason = GUIMenuPollInput(params, menu, state);
	params->height = height;
	params->pollCursor = _switchMenuCursor;
	size_t visible = _switchConfigView ? 9 : 11;
	if (_switchConfigView && menu->index >= count - 2) {
		state->start = oldStart;
	} else if (menu->index >= state->start + visible) {
		state->start = menu->index - visible + 1;
	}
	if (reason == GUI_MENU_CONTINUE) {
		_switchDrawMenu(params, menu, state);
	}
	return reason;
}

static enum GUIMenuExitReason _switchRunConfig(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuState* state) {
	static PadState categoryPad;
	static bool padInitialized;
	if (!padInitialized) {
		padInitializeDefault(&categoryPad);
		padInitialized = true;
	}
	padUpdate(&categoryPad);
	u64 buttons = padGetButtonsDown(&categoryPad);
	if (buttons & (HidNpadButton_L | HidNpadButton_ZL)) {
		_switchNextCategory = (_switchCategory + 7) % 8;
	} else if (buttons & (HidNpadButton_R | HidNpadButton_ZR)) {
		_switchNextCategory = (_switchCategory + 1) % 8;
	}
	bool changed = _switchCategory != _switchNextCategory;
	_switchCategory = _switchNextCategory;
	size_t count = GUIMenuItemListSize(&menu->items);
	if (count < 2) {
		return _switchRunMenu(params, menu, state);
	}
	size_t* mapping = malloc(count * sizeof(*mapping));
	if (!mapping) {
		return _switchRunMenu(params, menu, state);
	}
	struct GUIMenu view = *menu;
	GUIMenuItemListInit(&view.items, count);
	if (!view.items.vector) {
		free(mapping);
		return _switchRunMenu(params, menu, state);
	}
	view.index = 0;
	size_t included = 0;
	for (size_t i = 0; i < count; ++i) {
		const struct GUIMenuItem* item = GUIMenuItemListGetConstPointer(&menu->items, i);
		if (_switchCategory && i < count - 2 && _switchItemCategory(item) != _switchCategory) {
			continue;
		}
		mapping[included] = i;
		if (!changed && menu->index == i) {
			view.index = included;
		}
		*GUIMenuItemListAppend(&view.items) = *item;
		++included;
	}
	struct GUIMenuState viewState = *state;
	viewState.resultItem = NULL;
	viewState.start = changed ? 0 : _switchConfigStart;
	if (viewState.start >= included - 2) {
		viewState.start = 0;
	}
	if (changed) {
		viewState.cursorOverItem = 0;
	}
	_switchConfigView = true;
	enum GUIMenuExitReason reason = _switchRunMenu(params, &view, &viewState);
	_switchConfigView = false;
	_switchConfigStart = viewState.start;
	for (size_t i = 0; i < included; ++i) {
		struct GUIMenuItem* original = GUIMenuItemListGetPointer(&menu->items, mapping[i]);
		const struct GUIMenuItem* copy = GUIMenuItemListGetConstPointer(&view.items, i);
		original->state = copy->state;
		if (viewState.resultItem == copy) {
			state->resultItem = original;
		}
	}
	menu->index = mapping[view.index];
	state->start = mapping[viewState.start];
	state->cursor = viewState.cursor;
	state->cursorOverItem = viewState.cursorOverItem;
	state->cx = viewState.cx;
	state->cy = viewState.cy;
	GUIMenuItemListDeinit(&view.items);
	free(mapping);
	return reason;
}

enum GUIMenuExitReason GUIMenuRun(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuState* state) {
	enum GUIMenuExitReason libraryResult;
	if (menu == SwitchUIAchievementsGameMenu()) {
		SwitchUIAchievementsGameRun(params, &libraryResult);
		return libraryResult;
	}
	if (SwitchUILibraryMenu(params, menu, state, &libraryResult)) {
		return libraryResult;
	}
	if (menu->title && !strcmp(menu->title, "Configure")) {
		return _switchRunConfig(params, menu, state);
	}
	return _switchRunMenu(params, menu, state);
}

enum GUIMenuExitReason GUIShowMenu(struct GUIParams* params, struct GUIMenu* menu, struct GUIMenuItem** item) {
	if (menu->title && !strcmp(menu->title, "Game Paused")) {
		size_t count = GUIMenuItemListSize(&menu->items), position = count;
		bool present = false;
		for (size_t i = 0; i < count; ++i) {
			const struct GUIMenuItem* entry = GUIMenuItemListGetConstPointer(&menu->items, i);
			if (entry->submenu == SwitchUIAchievementsGameMenu()) present = true;
			if (entry->title && !strcmp(entry->title, "Configure")) position = i;
		}
		if (!present) {
			GUIMenuItemListAppend(&menu->items);
			memmove(menu->items.vector + position + 1, menu->items.vector + position,
			        (count - position) * sizeof(struct GUIMenuItem));
			menu->items.vector[position] = (struct GUIMenuItem) {
				.title = "Achievements", .submenu = SwitchUIAchievementsGameMenu()
			};
			if (menu->index >= position) ++menu->index;
		}
	}
	SwitchUIBeginMenu(menu);
	if (menu->title && !strcmp(menu->title, "Configure")) {
		_switchCategory = 0;
		_switchNextCategory = 0;
		_switchConfigStart = 0;
	}
	struct GUIMenuState state;
	GUIMenuStateInit(&state);
	GUIInvalidateKeys(params);
	while (appletMainLoop()) {
		if (SwitchUIUpdatesShouldExit()) {
			*item = NULL;
			GUIMenuStateDeinit(&state);
			return GUI_MENU_EXIT_CANCEL;
		}
		enum GUIMenuExitReason reason = GUIMenuRun(params, menu, &state);
		if (reason == GUI_MENU_EXIT_BACK && GUIMenuSavedListSize(&state.stack)) {
			struct GUIMenuSavedState* last = GUIMenuSavedListGetPointer(&state.stack, GUIMenuSavedListSize(&state.stack) - 1);
			state.start = last->start;
			menu = last->menu;
			GUIMenuSavedListResize(&state.stack, -1);
		} else if (reason == GUI_MENU_ENTER) {
			*GUIMenuSavedListAppend(&state.stack) = (struct GUIMenuSavedState) { .start = state.start, .menu = menu };
			menu = state.resultItem->submenu;
			state.start = 0;
		} else if (reason != GUI_MENU_CONTINUE) {
			if (reason == GUI_MENU_EXIT_ACCEPT && menu->title && !strcmp(menu->title, "Load state"))
				SwitchAchievementsResetProgress();
			*item = state.resultItem;
			GUIMenuStateDeinit(&state);
			return reason;
		}
	}
	*item = NULL;
	GUIMenuStateDeinit(&state);
	return GUI_MENU_EXIT_CANCEL;
}


enum GUIMenuExitReason GUIShowMessageBox(struct GUIParams* params, int buttons, int frames, const char* format, ...) {
	char message[256];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	GUIInvalidateKeys(params);
	int pressedButton = 0;
	while (appletMainLoop()) {
		if (frames && !--frames) {
			break;
		}
		params->drawStart();
		if (params->guiPrepare) {
			params->guiPrepare();
		}
		SwitchUIDrawRect(0, 0, params->width, params->height, 0xF01D1915);
		SwitchUIRoundRect(240, 180, 800, 360, 16, 0xFF302922);
		SwitchUIFontSize(params->font, 28);
		SwitchUIPrint(params->font, 280, 236, 720, 0xFFF1F6F2, "mGBA");
		SwitchUIFontSize(params->font, 22);
		const char* line = message;
		int y = 292;
		while (*line && y < 464) {
			char buffer[1024];
			size_t length = 0;
			size_t space = 0;
			while (line[length] && line[length] != '\n' && length < sizeof(buffer) - 1) {
				buffer[length] = line[length];
				buffer[length + 1] = 0;
				if (GUIFontSpanWidth(params->font, buffer) > 720) {
					break;
				}
				if (line[length] == ' ') {
					space = length;
				}
				++length;
			}
			if (line[length] && line[length] != '\n' && space) {
				length = space;
			}
			if (!length && *line != '\n') {
				length = 1;
			}
			memcpy(buffer, line, length);
			buffer[length] = 0;
			SwitchUIPrint(params->font, 280, y, 720, 0xFFF1F6F2, buffer);
			line += length;
			if (*line == '\n' || *line == ' ') {
				++line;
			}
			y += 30;
		}
		SwitchUIFontSize(params->font, 18);
		if (buttons & GUI_MESSAGE_BOX_OK) {
			SwitchUIRoundRect(264, 476, 336, 48, 8, 0xFF4F3038);
			SwitchUIHint(params->font, 280, 508, "A", "OK");
		}
		if (buttons & GUI_MESSAGE_BOX_CANCEL) {
			SwitchUIRoundRect(744, 476, 280, 48, 8, 0xFF4F3038);
			SwitchUIHint(params->font, 760, 508, "B", "Cancel");
		}
		SwitchUIFontSize(params->font, 24);
		if (params->guiFinish) {
			params->guiFinish();
		}
		params->drawEnd();
		uint32_t input = 0;
		GUIPollInput(params, &input, NULL);
		unsigned cx = params->cx;
		unsigned cy = params->cy;
		enum GUICursorState cursor = GUIPollCursor(params, &cx, &cy);
		int hoveredButton = 0;
		if (cy >= 476 && cy < 524) {
			if (cx >= 264 && cx < 600 && (buttons & GUI_MESSAGE_BOX_OK)) {
				hoveredButton = GUI_MESSAGE_BOX_OK;
			} else if (cx >= 744 && cx < 1024 && (buttons & GUI_MESSAGE_BOX_CANCEL)) {
				hoveredButton = GUI_MESSAGE_BOX_CANCEL;
			}
		}
		if (cursor == GUI_CURSOR_DOWN) {
			pressedButton = hoveredButton;
		} else if (cursor == GUI_CURSOR_CLICKED || cursor == GUI_CURSOR_UP) {
			if (cursor == GUI_CURSOR_CLICKED && pressedButton && pressedButton == hoveredButton) {
				return pressedButton == GUI_MESSAGE_BOX_OK ? GUI_MENU_EXIT_ACCEPT : GUI_MENU_EXIT_BACK;
			}
			pressedButton = 0;
		} else if (cursor == GUI_CURSOR_NOT_PRESENT) {
			pressedButton = 0;
		}
		if (input & (1 << GUI_INPUT_SELECT)) {
			if (buttons & GUI_MESSAGE_BOX_OK) {
				return GUI_MENU_EXIT_ACCEPT;
			}
			if (buttons & GUI_MESSAGE_BOX_CANCEL) {
				return GUI_MENU_EXIT_CANCEL;
			}
		}
		if (input & ((1 << GUI_INPUT_BACK) | (1 << GUI_INPUT_CANCEL))) {
			if (buttons & GUI_MESSAGE_BOX_CANCEL) {
				return input & (1 << GUI_INPUT_BACK) ? GUI_MENU_EXIT_BACK : GUI_MENU_EXIT_CANCEL;
			}
			if (buttons & GUI_MESSAGE_BOX_OK) {
				return GUI_MENU_EXIT_ACCEPT;
			}
		}
	}
	return GUI_MENU_EXIT_CANCEL;
}
