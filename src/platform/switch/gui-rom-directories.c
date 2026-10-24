/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include <mgba/core/config.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/string.h>
#include <stdlib.h>
#include <strings.h>
#include <switch.h>

#define MAX_ROM_DIRECTORIES 16

static bool _validRoot(const char* path) {
	return path && (path[0] == '/' || !strncasecmp(path, "sdmc:/", 6)) &&
	    strlen(path) < PATH_MAX && !strpbrk(path, "\r\n");
}
size_t SwitchUIRomDirectoriesGet(const struct mCoreConfig* config, const char** roots, size_t capacity) {
	int count;
	if (!mCoreConfigGetIntValue(config, "switch.romDirectoryCount", &count)) {
		const char* legacy = mCoreConfigGetValue(config, "switch.romDirectory");
		if (capacity && _validRoot(legacy)) { roots[0] = legacy; return 1; }
		return 0;
	}
	if (count < 0) count = 0;
	if (count > MAX_ROM_DIRECTORIES) count = MAX_ROM_DIRECTORIES;
	size_t found = 0;
	for (int i = 0; i < count && found < capacity; ++i) {
		char key[48];
		snprintf(key, sizeof(key), "switch.romDirectory.%d", i);
		const char* path = mCoreConfigGetValue(config, key);
		if (_validRoot(path)) roots[found++] = path;
	}
	return found;
}

static void _normalize(char* path) {
	if (!strncasecmp(path, "sdmc:/", 6)) memmove(path, path + 5, strlen(path + 5) + 1);
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/') path[--len] = '\0';
}

static bool _save(struct GUIParams* params, struct mCoreConfig* config, char (*paths)[PATH_MAX], size_t count) {
	char* previous[MAX_ROM_DIRECTORIES + 2] = {0};
	const char* keys[MAX_ROM_DIRECTORIES + 2];
	char names[MAX_ROM_DIRECTORIES][48];
	keys[0] = "switch.romDirectoryCount";
	keys[1] = "switch.romDirectory";
	for (unsigned i = 0; i < MAX_ROM_DIRECTORIES; ++i) {
		snprintf(names[i], sizeof(names[i]), "switch.romDirectory.%u", i);
		keys[i + 2] = names[i];
	}
	for (unsigned i = 0; i < MAX_ROM_DIRECTORIES + 2; ++i) {
		const char* old = mCoreConfigGetValue(config, keys[i]);
		if (old && !(previous[i] = strdup(old))) {
			for (unsigned j = 0; j < i; ++j) free(previous[j]);
			GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Not enough memory to save ROM folders.");
			return false;
		}
	}
	mCoreConfigSetIntValue(config, keys[0], count);
	mCoreConfigSetValue(config, keys[1], count ? paths[0] : NULL);
	for (unsigned i = 0; i < MAX_ROM_DIRECTORIES; ++i)
		mCoreConfigSetValue(config, keys[i + 2], i < count ? paths[i] : NULL);
	bool saved = mCoreConfigSave(config);
	if (!saved) {
		for (unsigned i = 0; i < MAX_ROM_DIRECTORIES + 2; ++i) mCoreConfigSetValue(config, keys[i], previous[i]);
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Could not save ROM folders. Previous settings are unchanged.");
	}
	for (unsigned i = 0; i < MAX_ROM_DIRECTORIES + 2; ++i) free(previous[i]);
	return saved;
}

bool SwitchUIRomDirectoriesEdit(struct GUIParams* params, struct mCoreConfig* config) {
	char (*paths)[PATH_MAX] = calloc(MAX_ROM_DIRECTORIES, sizeof(*paths));
	if (!paths) {
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Not enough memory to edit ROM folders.");
		return false;
	}
	PadState pad;
	padInitializeDefault(&pad);
	bool changed = false;
	size_t selected = 0, first = 0;
	unsigned oldSize = SwitchUIFontSizeGet(params->font);
	for (;;) {
		const char* roots[MAX_ROM_DIRECTORIES];
		size_t count = SwitchUIRomDirectoriesGet(config, roots, MAX_ROM_DIRECTORIES);
		for (size_t i = 0; i < count; ++i) { strlcpy(paths[i], roots[i], PATH_MAX); _normalize(paths[i]); }
		size_t rows = count + 2;
		if (selected >= rows) selected = rows - 1;
		uint32_t input = 0;
		GUIPollInput(params, &input, NULL);
		padUpdate(&pad);
		unsigned x = params->cx, y = params->cy;
		bool tap = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
		if (tap) { x = params->cx; y = params->cy; }
		if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) || (tap && y >= 664 && x < 220)) break;
		if (selected && (input & (1U << GUI_INPUT_UP))) --selected;
		if (selected + 1 < rows && (input & (1U << GUI_INPUT_DOWN))) ++selected;
		if ((input & (1U << GUI_INPUT_LEFT)) || (tap && y >= 664 && x >= 220 && x < 355)) selected = selected > 7 ? selected - 7 : 0;
		if ((input & (1U << GUI_INPUT_RIGHT)) || (tap && y >= 664 && x >= 355 && x < 510)) selected = selected + 7 < rows ? selected + 7 : rows - 1;
		bool activate = input & (1U << GUI_INPUT_SELECT);
		bool remove = padGetButtonsDown(&pad) & HidNpadButton_Y;
		if (tap && x >= 48 && x < 1232 && y >= 168 && y < 616) {
			size_t row = first + (y - 168) / 64;
			if (row < rows) { selected = row; activate = true; }
		}
		if (tap && y >= 664 && x >= 880) activate = true;
		if (tap && y >= 664 && x >= 510 && x < 820) remove = true;
		if (remove && selected < count) {
			if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			    "Remove this ROM folder from the library? No files will be deleted.\n%s", paths[selected]) == GUI_MENU_EXIT_ACCEPT) {
				memmove(paths + selected, paths + selected + 1, (count - selected - 1) * sizeof(*paths));
				changed |= _save(params, config, paths, count - 1);
			}
			GUIInvalidateKeys(params);
		} else if (activate && selected == count + 1) {
			if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
			    "Restore upstream ROM browsing behavior? This removes the folder list only; no files will be deleted.") == GUI_MENU_EXIT_ACCEPT)
				changed |= _save(params, config, paths, 0);
			GUIInvalidateKeys(params);
		} else if (activate) {
			if (selected == count && count == MAX_ROM_DIRECTORIES) {
				GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "You can add up to 16 ROM folders.");
			} else {
				char chosen[PATH_MAX];
				if (SwitchUISelectDirectory(params, selected < count ? paths[selected] : params->currentPath, chosen, sizeof(chosen))) {
					_normalize(chosen);
					bool duplicate = false;
					for (size_t i = 0; i < count; ++i) if (i != selected && !strcasecmp(paths[i], chosen)) duplicate = true;
					if (duplicate) GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "That ROM folder is already in the library.");
					else if (selected == count || strcmp(paths[selected], chosen)) {
						strlcpy(paths[selected], chosen, PATH_MAX);
						changed |= _save(params, config, paths, count + (selected == count));
					}
				}
			}
			GUIInvalidateKeys(params);
		}
		/* Reload after an action, including a failed save, before drawing. */
		count = SwitchUIRomDirectoriesGet(config, roots, MAX_ROM_DIRECTORIES);
		rows = count + 2;
		if (selected >= rows) selected = rows - 1;
		if (selected < first) first = selected;
		if (selected >= first + 7) first = selected - 6;
		params->drawStart();
		if (params->guiPrepare) params->guiPrepare();
		SwitchUIDrawRect(0, 0, 1280, 720, 0xFF1F1417);
		SwitchUIFontSize(params->font, 30);
		SwitchUIPrint(params->font, 48, 66, 1184, 0xFFF5F3EF, "ROM directories");
		SwitchUIFontSize(params->font, 18);
		SwitchUIPrint(params->font, 48, 112, 1184, 0xFFB8B2AA, count ? "All selected folders appear together in the game library." : "Upstream defaults: browse ROMs using Files. Add folders for a combined library.");
		for (size_t row = first; row < rows && row < first + 7; ++row) {
			int top = 168 + (row - first) * 64;
			if (row == selected) {
				SwitchUIRoundRect(48, top, 1184, 58, 8, 0xFF332024);
				SwitchUIRoundFrame(48, top, 1184, 58, 8, 0xFFFFA3BD);
			}
			SwitchUIFontSize(params->font, 22);
			SwitchUIPrint(params->font, 68, top + 37, 1120, 0xFFF5F3EF,
			    row < count ? roots[row] : row == count ? "Add ROM directory" : "Restore upstream defaults");
		}
		SwitchUIHint(params->font, 48, 699, "B", "Back");
		SwitchUIHint(params->font, 240, 699, "< >", "Page");
		SwitchUIHint(params->font, 530, 699, "Y", "Remove folder");
		SwitchUIHint(params->font, 900, 699, "A", selected < count ? "Change folder" : "Select");
		SwitchUIFontSize(params->font, oldSize);
		if (params->guiFinish) params->guiFinish();
		params->drawEnd();
	}
	free(paths);
	GUIInvalidateKeys(params);
	return changed;
}
