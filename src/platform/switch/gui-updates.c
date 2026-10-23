/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include "switch-updater.h"
#include "feature/gui/gui-runner.h"
#include <mgba-util/gui/font.h>
#include <mgba-util/string.h>
#include <switch.h>

static struct mGUIRunner* _runner;
static struct GUIMenu _menu;
static char _executable[PATH_MAX], _promptedVersion[32];
static bool _restart, _restartPrompted;

void SwitchUIUpdatesInit(struct mGUIRunner* runner) {
	_runner = runner;
	_menu.title = "Updates";
	GUIMenuItemListInit(&_menu.items, 0);
}

void SwitchUIUpdatesStart(const char* executable) {
	if (executable) strlcpy(_executable, executable, sizeof(_executable));
	if (SwitchUpdaterInit(_executable)) SwitchUpdaterCheck();
}

void SwitchUIUpdatesDeinit(void) {
	SwitchUpdaterDeinit();
	GUIMenuItemListDeinit(&_menu.items);
	_runner = NULL;
}

struct GUIMenu* SwitchUIUpdatesMenu(void) { return &_menu; }
bool SwitchUIUpdatesShouldExit(void) { return _restart; }

void SwitchUIUpdatesFinishRestart(void) {
	if (!_restart) return;
	const char* error = NULL;
	struct SwitchUpdaterState state;
	if (!SwitchUpdaterApplyPending()) {
		SwitchUpdaterGetState(&state);
		error = state.status;
	} else {
		char arguments[PATH_MAX + 3];
		snprintf(arguments, sizeof(arguments), "\"%s\"", _executable);
		if (R_FAILED(envSetNextLoad(_executable, arguments)))
			error = "Update installed. Reopen the application from the homebrew menu.";
	}
	if (error) {
		ErrorApplicationConfig dialog;
		if (R_SUCCEEDED(errorApplicationCreate(&dialog, error, error))) errorApplicationShow(&dialog);
	}
}

static void _confirmRestart(struct GUIParams* params) {
	_restartPrompted = true;
	if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
	    "Update downloaded and verified.\nRestart now to install it?\nA: Restart    B: Later") == GUI_MENU_EXIT_ACCEPT)
		_restart = true;
	GUIInvalidateKeys(params);
}

static void _install(struct GUIParams* params, const struct SwitchUpdaterState* state, bool showProgress) {
	if (!_runner || _runner->core) return;
	if (!state->canInstall) {
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0,
		    "Automatic updates require an SD-card NRO launch and a homebrew loader that supports restarting applications.");
		GUIInvalidateKeys(params);
		return;
	}
	if (GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK | GUI_MESSAGE_BOX_CANCEL, 0,
	    "Install version %s?\nYou will be asked to restart after the download finishes.\nA: Download    B: Later", state->version) == GUI_MENU_EXIT_ACCEPT) {
		_restartPrompted = false;
		SwitchUpdaterInstall();
		if (showProgress) {
			struct GUIMenuItem* ignored = NULL;
			GUIShowMenu(params, &_menu, &ignored);
		}
	}
	GUIInvalidateKeys(params);
}

void SwitchUIUpdatesPoll(struct GUIParams* params, bool prompt) {
	struct SwitchUpdaterState state;
	SwitchUpdaterGetState(&state);
	if (_runner && !_runner->core && state.phase == SWITCH_UPDATE_READY && !_restartPrompted) {
		_confirmRestart(params);
	}
	if (prompt && _runner && !_runner->core && state.canInstall && state.phase == SWITCH_UPDATE_AVAILABLE &&
	    strcmp(state.version, _promptedVersion)) {
		strlcpy(_promptedVersion, state.version, sizeof(_promptedVersion));
		_install(params, &state, true);
	}
}

bool SwitchUIUpdatesRun(struct GUIParams* params, enum GUIMenuExitReason* result) {
	SwitchUIUpdatesPoll(params, false);
	struct SwitchUpdaterState state;
	SwitchUpdaterGetState(&state);
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x = params->cx, y = params->cy;
	bool tap = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
	if (tap) { x = params->cx; y = params->cy; }
	if (_restart) { *result = GUI_MENU_EXIT_CANCEL; return true; }
	if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) || (tap && x < 260 && y >= 664)) {
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	bool busy = state.phase == SWITCH_UPDATE_CHECKING || state.phase == SWITCH_UPDATE_DOWNLOADING;
	if (!busy && ((input & (1U << GUI_INPUT_SELECT)) || (tap && x >= 408 && x < 1192 && y >= 270 && y < 326))) {
		if (state.phase == SWITCH_UPDATE_AVAILABLE) _install(params, &state, false);
		else if (state.phase == SWITCH_UPDATE_READY && !_runner->core) _confirmRestart(params);
		else if (state.phase != SWITCH_UPDATE_READY) { _promptedVersion[0] = 0; SwitchUpdaterCheck(); }
	}
	params->drawStart();
	if (params->guiPrepare) params->guiPrepare();
	unsigned size = SwitchUIFontSizeGet(params->font);
	SwitchUIDrawRect(0, 0, 1280, 720, 0xFF1F1417);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 66, 300, 0xFFF5F3EF, "Settings");
	SwitchUIRoundRect(40, 140, 280, 48, 8, 0xFF332024);
	SwitchUIFontSize(params->font, 20);
	SwitchUIPrint(params->font, 62, 172, 238, 0xFFFFA3BD, "Updates");
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 408, 142, 784, 0xFFF5F3EF, "Application updates");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 194, 784, 0xFFB8B2AA, "Checks run in the background. Installation requires your approval.");
	SwitchUIPrint(params->font, 408, 234, 784, 0xFFFFA3BD, state.status);
	SwitchUIRoundRect(408, 270, 784, 56, 8, 0xFF332024);
	SwitchUIRoundFrame(408, 270, 784, 56, 8, 0xFFFFA3BD);
	SwitchUIPrint(params->font, 426, 305, 748, 0xFFF5F3EF, busy ? "Working in the background..." :
	    state.phase == SWITCH_UPDATE_READY ? (_runner->core ? "Return to the library to restart" : "Update downloaded — restart to install") :
	    state.phase == SWITCH_UPDATE_AVAILABLE ? (_runner->core ? "Return to the library to install this update" : "Download update") : "Check for updates");
	if (state.phase == SWITCH_UPDATE_DOWNLOADING) {
		SwitchUIRoundRect(408, 364, 784, 12, 6, 0xFF332024);
		if (state.progress) SwitchUIRoundRect(408, 364, 784 * state.progress / 100, 12, 6, 0xFFFFA3BD);
		char progress[64];
		snprintf(progress, sizeof(progress), "%u%% downloaded", state.progress);
		SwitchUIPrint(params->font, 408, 410, 784, 0xFFFFA3BD, progress);
	}
	SwitchUIPrint(params->font, 408, 436, 784, 0xFFB8B2AA, "Updates preserve settings, artwork, saves, and achievements.");
	SwitchUIHint(params->font, 48, 696, "B", "Back");
	SwitchUIHint(params->font, 1040, 696, "A", "Select");
	SwitchUIFontSize(params->font, size);
	if (params->guiFinish) params->guiFinish();
	params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}
