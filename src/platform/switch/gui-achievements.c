/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-ui.h"
#include "achievements.h"
#include "ra-badges.h"
#include "feature/gui/gui-runner.h"
#include <mgba-util/gui.h>
#include <mgba-util/gui/font.h>
#include <mgba-util/string.h>
#include <mgba-util/image.h>
#include <GLES3/gl3.h>
#include <switch.h>

static struct mGUIRunner* _runner;
static struct GUIMenu _menu;
static struct GUIMenu _gameMenu;
static char _username[513], _password[1025];
static struct SwitchAchievementNotice _notice;
static bool _remember;
static bool _unlockNotifications = true;
static unsigned _notificationSeconds = 5;
static u64 _noticeUntil;
static bool _gamePage;
static unsigned _achievementIndex, _achievementFirst, _achievementCount;
static u64 _achievementRefresh;
static struct SwitchAchievement _achievements[4];
static unsigned _achievementFilter;
static PadState _achievementPad;
static struct {
	char url[512];
	GLuint texture;
	u64 used, retry;
	bool pending;
} _badges[8];

static void _pollBadges(struct GUIParams* params) {
	char url[512];
	struct mImage* image;
	while (SwitchRABadgesPoll(url, sizeof(url), &image)) {
		for (unsigned i = 0; i < 8; ++i) {
			if (strcmp(url, _badges[i].url)) continue;
			_badges[i].pending = false;
			_badges[i].retry = armTicksToNs(armGetSystemTick()) + 30000000000ULL;
			if (!image) break;
			GUIFontDrawSubmit(params->font);
			if (_badges[i].texture) glDeleteTextures(1, &_badges[i].texture);
			glGenTextures(1, &_badges[i].texture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, _badges[i].texture);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, image->stride);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->data);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			break;
		}
		if (image) mImageDestroy(image);
	}
}

static int _cacheBadge(const char* url, u64 now) {
	if (!url[0]) return -1;
	unsigned slot = 0;
	for (unsigned i = 0; i < 8; ++i) {
		if (!strcmp(url, _badges[i].url)) { slot = i; goto found; }
		if (_badges[i].used < _badges[slot].used) slot = i;
	}
	if (_badges[slot].texture) glDeleteTextures(1, &_badges[slot].texture);
	memset(&_badges[slot], 0, sizeof(_badges[slot]));
	strlcpy(_badges[slot].url, url, sizeof(_badges[slot].url));
found:
	_badges[slot].used = now;
	if (!_badges[slot].texture && !_badges[slot].pending && now >= _badges[slot].retry) {
		_badges[slot].pending = SwitchRABadgesRequest(url);
		_badges[slot].retry = now + 1000000000ULL;
	}
	return slot;
}

static void _drawBadge(const char* url, int x, int y, u64 now) {
	SwitchUIRoundRect(x, y, 64, 64, 6, 0xFF1F1417);
	int slot = _cacheBadge(url, now);
	if (slot >= 0 && _badges[slot].texture) SwitchUIDrawTexture(_badges[slot].texture, x, y, 64, 64);
}

static void _clearPassword(void) {
	volatile char* password = _password;
	for (size_t i = 0; i < sizeof(_password); ++i) password[i] = 0;
}

static void _edit(bool password, struct GUIParams* params) {
	SwkbdConfig keyboard;
	if (R_FAILED(swkbdCreate(&keyboard, 0))) return;
	if (password) swkbdConfigMakePresetPassword(&keyboard);
	else swkbdConfigMakePresetDefault(&keyboard);
	swkbdConfigSetHeaderText(&keyboard, password ? "RetroAchievements password" : "RetroAchievements username");
	swkbdConfigSetStringLenMax(&keyboard, password ? 256 : 128);
	if (!password) swkbdConfigSetInitialText(&keyboard, _username);
	char value[sizeof(_password)] = { 0 };
	if (R_SUCCEEDED(swkbdShow(&keyboard, value, sizeof(value)))) {
		if (password) strlcpy(_password, value, sizeof(_password));
		else strlcpy(_username, value, sizeof(_username));
	}
	volatile char* wipe = value;
	for (size_t i = 0; i < sizeof(value); ++i) wipe[i] = 0;
	swkbdClose(&keyboard);
	GUIInvalidateKeys(params);
}

void SwitchUIAchievementsInit(struct mGUIRunner* runner, const char* directory) {
	_runner = runner;
	_menu.title = "RetroAchievements";
	GUIMenuItemListInit(&_menu.items, 0);
	_gameMenu.title = "Achievements";
	GUIMenuItemListInit(&_gameMenu.items, 0);
	int remember = 0;
	mCoreConfigGetIntValue(&runner->config, "switch.achievements.remember", &remember);
	_remember = remember != 0;
	SwitchAchievementsInit(directory);
	int notifications = 1, seconds = 5;
	mCoreConfigGetIntValue(&runner->config, "switch.achievements.notifications", &notifications);
	mCoreConfigGetIntValue(&runner->config, "switch.achievements.notificationSeconds", &seconds);
	_unlockNotifications = notifications != 0;
	_notificationSeconds = seconds >= 2 && seconds <= 10 ? seconds : 5;
	SwitchAchievementsSetNotificationsEnabled(_unlockNotifications);
	char cacheDirectory[PATH_MAX];
	snprintf(cacheDirectory, sizeof(cacheDirectory), "%s/retroachievements-badges", directory);
	SwitchRABadgesInit(cacheDirectory);
	padInitializeDefault(&_achievementPad);
}

void SwitchUIAchievementsDeinit(void) {
	SwitchRABadgesDeinit();
	for (unsigned i = 0; i < 8; ++i) {
		if (_badges[i].texture) glDeleteTextures(1, &_badges[i].texture);
	}
	memset(_badges, 0, sizeof(_badges));
	SwitchAchievementsDeinit();
	_clearPassword();
	GUIMenuItemListDeinit(&_menu.items);
	GUIMenuItemListDeinit(&_gameMenu.items);
	_runner = NULL;
}

struct GUIMenu* SwitchUIAchievementsMenu(void) { return &_menu; }
struct GUIMenu* SwitchUIAchievementsGameMenu(void) { return &_gameMenu; }

static bool _inside(unsigned x, unsigned y, int left, int top, int width, int height) {
	return x >= (unsigned) left && y >= (unsigned) top && x < (unsigned) (left + width) && y < (unsigned) (top + height);
}

static bool _showGame(struct GUIParams* params, enum GUIMenuExitReason* result, bool fromPause) {
	padUpdate(&_achievementPad);
	u64 buttons = padGetButtonsDown(&_achievementPad);
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x = params->cx, y = params->cy;
	bool tapped = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
	if (tapped) { x = params->cx; y = params->cy; }
	unsigned filter = _achievementFilter;
	if (buttons & HidNpadButton_L) filter = (filter + 2) % 3;
	if (buttons & HidNpadButton_R) filter = (filter + 1) % 3;
	if (tapped && y >= 174 && y < 202 && x >= 48 && x < 504) filter = (x - 48) / 152;
	if (filter != _achievementFilter) {
		_achievementFilter = filter;
		_achievementIndex = _achievementFirst = _achievementCount = 0;
		_achievementRefresh = 0;
	}
	if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) ||
	    (tapped && _inside(x, y, 48, 664, 180, 56))) {
		_gamePage = false;
		GUIInvalidateKeys(params);
		*result = fromPause ? GUI_MENU_EXIT_BACK : GUI_MENU_CONTINUE;
		return true;
	}
	unsigned previous = _achievementIndex;
	if (_achievementCount) {
		if ((input & (1U << GUI_INPUT_UP)) && _achievementIndex) --_achievementIndex;
		if ((input & (1U << GUI_INPUT_DOWN)) && _achievementIndex + 1 < _achievementCount) ++_achievementIndex;
		if (input & (1U << GUI_INPUT_LEFT)) _achievementIndex = _achievementIndex > 4 ? _achievementIndex - 4 : 0;
		if (input & (1U << GUI_INPUT_RIGHT)) _achievementIndex = _achievementIndex + 4 < _achievementCount ? _achievementIndex + 4 : _achievementCount - 1;
	}
	if (_achievementIndex < _achievementFirst) _achievementFirst = _achievementIndex;
	if (_achievementIndex >= _achievementFirst + 4) _achievementFirst = _achievementIndex - 3;
	u64 now = armTicksToNs(armGetSystemTick());
	if (previous != _achievementIndex || now >= _achievementRefresh) {
		_achievementCount = SwitchAchievementsGetPage(_achievementFirst, _achievements, 4, _achievementFilter);
		if (_achievementIndex >= _achievementCount) {
			_achievementIndex = _achievementFirst = 0;
			_achievementCount = SwitchAchievementsGetPage(0, _achievements, 4, _achievementFilter);
		}
		_achievementRefresh = now + 500000000ULL;
	}
	bool details = input & (1U << GUI_INPUT_SELECT);
	if (tapped) for (unsigned i = 0; i < 4 && _achievementFirst + i < _achievementCount; ++i) {
		if (_inside(x, y, 48, 202 + i * 108, 1184, 98)) { _achievementIndex = _achievementFirst + i; details = true; }
	}
	if (details && _achievementCount) {
		struct SwitchAchievement* entry = &_achievements[_achievementIndex - _achievementFirst];
		GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "%s\n\n%s\n\n%s | %u points", entry->title, entry->description, entry->progress, entry->points);
		GUIInvalidateKeys(params);
	}
	struct SwitchAchievementsState state;
	SwitchAchievementsGetState(&state);
	params->drawStart();
	if (params->guiPrepare) params->guiPrepare();
	unsigned oldSize = SwitchUIFontSizeGet(params->font);
	_pollBadges(params);
	SwitchUIDrawRect(0, 0, 1280, 720, 0xFF1F1417);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 64, 1184, 0xFFF5F3EF, "Current game achievements");
	SwitchUIFontSize(params->font, 22);
	SwitchUIPrint(params->font, 48, 105, 1184, 0xFFFFA3BD, state.game[0] ? state.game : "No achievement set loaded");
	char summary[128];
	snprintf(summary, sizeof(summary), "%u / %u unlocked  |  %u points earned  |  Softcore", state.unlocked, state.achievements, state.points);
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 48, 142, 1184, 0xFFB8B2AA, summary);
	SwitchUIRoundRect(48, 162, 1184, 8, 4, 0xFF332024);
	if (state.achievements && state.unlocked) SwitchUIRoundRect(48, 162, 1184 * state.unlocked / state.achievements, 8, 4, 0xFFFFA3BD);
	const char* filters[] = { "All", "Locked", "Unlocked" };
	SwitchUIFontSize(params->font, 16);
	for (unsigned i = 0; i < 3; ++i) {
		if (i == _achievementFilter) SwitchUIRoundRect(48 + i * 152, 174, 144, 26, 6, 0xFF332024);
		SwitchUIPrint(params->font, 62 + i * 152, 194, 120, i == _achievementFilter ? 0xFFFFA3BD : 0xFFB8B2AA, filters[i]);
	}
	if (!_achievementCount) {
		const char* message = !state.loggedIn ? "Sign in from RetroAchievements settings to view achievements." :
		    state.busy ? "Loading achievements asynchronously..." : !state.game[0] ? "Launch a supported game to view its achievements." :
		    _achievementFilter == 1 ? "No locked achievements." : _achievementFilter == 2 ? "No unlocked achievements yet." : "This game has no achievements.";
		SwitchUIPrint(params->font, 48, 252, 1184, 0xFFF5F3EF, message);
		SwitchUIPrint(params->font, 48, 292, 1184, 0xFFB8B2AA, state.status);
	}
	for (unsigned i = 0; i < 4 && _achievementFirst + i < _achievementCount; ++i) {
		struct SwitchAchievement* entry = &_achievements[i];
		int top = 202 + i * 108;
		SwitchUIRoundRect(48, top, 1184, 98, 8, 0xFF332024);
		if (_achievementIndex == _achievementFirst + i) SwitchUIRoundFrame(48, top, 1184, 98, 8, 0xFFFFA3BD);
		_drawBadge(entry->badge, 62, top + 17, now);
		SwitchUIFontSize(params->font, 20);
		SwitchUIPrint(params->font, 142, top + 28, 834, 0xFFF5F3EF, entry->title);
		snprintf(summary, sizeof(summary), "%s | %u pts", entry->unlocked ? "Unlocked" : entry->unsupported ? "Unsupported" : "Locked", entry->points);
		SwitchUIFontSize(params->font, 16);
		SwitchUIPrint(params->font, 994, top + 28, 218, 0xFFFFA3BD, summary);
		SwitchUIPrint(params->font, 142, top + 53, 1070, 0xFFB8B2AA, entry->description);
		SwitchUIPrint(params->font, 142, top + 79, 660, 0xFFFFA3BD, entry->progress);
		SwitchUIRoundRect(820, top + 70, 392, 6, 3, 0xFF1F1417);
		if (entry->percent > 0) SwitchUIRoundRect(820, top + 70, (int) (392 * entry->percent / 100), 6, 3, 0xFFFFA3BD);
	}
	SwitchUIHint(params->font, 48, 696, "B", fromPause ? "Pause menu" : "Account settings");
	SwitchUIHint(params->font, 470, 696, "D-pad", "Browse");
	SwitchUIHint(params->font, 750, 696, "L/R", "Filter");
	SwitchUIHint(params->font, 1040, 696, "A", "Details");
	SwitchUIFontSize(params->font, oldSize);
	if (params->guiFinish) params->guiFinish();
	params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}

bool SwitchUIAchievementsGameRun(struct GUIParams* params, enum GUIMenuExitReason* result) {
	if (!_gamePage) {
		_gamePage = true;
		_achievementIndex = _achievementFirst = _achievementCount = 0;
		_achievementRefresh = 0;
		padUpdate(&_achievementPad);
	}
	return _showGame(params, result, true);
}

bool SwitchUIAchievementsRun(struct GUIParams* params, enum GUIMenuExitReason* result) {
	if (_gamePage) return _showGame(params, result, false);
	struct SwitchAchievementsState state;
	SwitchAchievementsGetState(&state);
	if ((state.loggedIn || state.busy) && _menu.index < 3) _menu.index = 3;
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	unsigned x = params->cx, y = params->cy;
	bool tapped = GUIPollCursor(params, &x, &y) == GUI_CURSOR_CLICKED;
	if (tapped) { x = params->cx; y = params->cy; }
	if ((input & ((1U << GUI_INPUT_BACK) | (1U << GUI_INPUT_CANCEL))) ||
	    (tapped && _inside(x, y, 48, 664, 180, 56))) {
		_clearPassword();
		*result = GUI_MENU_EXIT_BACK;
		return true;
	}
	unsigned first = state.loggedIn || state.busy ? 3 : 0;
	if (input & (1U << GUI_INPUT_UP)) _menu.index = _menu.index > first ? _menu.index - 1 : 6;
	if (input & (1U << GUI_INPUT_DOWN)) _menu.index = _menu.index < 6 ? _menu.index + 1 : first;
	bool activate = input & (1U << GUI_INPUT_SELECT);
	if (tapped) {
		for (unsigned i = 0; i < 7; ++i) {
			if (i < 3 && (state.loggedIn || state.busy)) continue;
			if (_inside(x, y, 408, 204 + i * 54, 784, 36)) { _menu.index = i; activate = true; }
		}
	}
	if ((_menu.index == 2 || _menu.index >= 5) && (input & ((1U << GUI_INPUT_LEFT) | (1U << GUI_INPUT_RIGHT)))) activate = true;
	if (activate && _menu.index >= 5) {
		if (_menu.index == 5) {
			_unlockNotifications = !_unlockNotifications;
			SwitchAchievementsSetNotificationsEnabled(_unlockNotifications);
			if (!_unlockNotifications && _notice.unlocked) _noticeUntil = 0;
			mCoreConfigSetIntValue(&_runner->config, "switch.achievements.notifications", _unlockNotifications);
		} else {
			if (input & (1U << GUI_INPUT_LEFT)) _notificationSeconds = _notificationSeconds > 2 ? _notificationSeconds - 1 : 10;
			else _notificationSeconds = _notificationSeconds < 10 ? _notificationSeconds + 1 : 2;
			u64 now = armTicksToNs(armGetSystemTick());
			if (_notice.unlocked && now < _noticeUntil) _noticeUntil = now + (u64) _notificationSeconds * 1000000000ULL;
			mCoreConfigSetIntValue(&_runner->config, "switch.achievements.notificationSeconds", _notificationSeconds);
		}
		mCoreConfigSave(&_runner->config);
	}
	if (activate && _menu.index == 4) {
		_gamePage = true;
		_achievementIndex = _achievementFirst = _achievementCount = 0;
		_achievementRefresh = 0;
		GUIInvalidateKeys(params);
		*result = GUI_MENU_CONTINUE;
		return true;
	}
	if (activate && state.available) {
		if (_menu.index < 2 && !state.loggedIn && !state.busy) _edit(_menu.index == 1, params);
		else if (_menu.index == 2 && !state.loggedIn && !state.busy) {
			_remember = !_remember;
			mCoreConfigSetIntValue(&_runner->config, "switch.achievements.remember", _remember);
			mCoreConfigSave(&_runner->config);
		} else if (_menu.index == 3) {
			if (state.loggedIn || state.busy) SwitchAchievementsLogout();
			else if (_username[0] && _password[0]) {
				SwitchAchievementsLogin(_username, _password, _remember);
			} else {
				GUIShowMessageBox(params, GUI_MESSAGE_BOX_OK, 0, "Enter your RetroAchievements username and password first.");
			}
			_clearPassword();
			GUIInvalidateKeys(params);
		}
		SwitchAchievementsGetState(&state);
	}
	params->drawStart();
	if (params->guiPrepare) params->guiPrepare();
	unsigned oldSize = SwitchUIFontSizeGet(params->font);
	SwitchUIDrawRect(0, 0, 1280, 720, 0xFF1F1417);
	SwitchUIFontSize(params->font, 30);
	SwitchUIPrint(params->font, 48, 66, 300, 0xFFF5F3EF, "Settings");
	SwitchUIRoundRect(40, 140, 280, 48, 8, 0xFF332024);
	SwitchUIFontSize(params->font, 20);
	SwitchUIPrint(params->font, 62, 172, 238, 0xFFFFA3BD, "RetroAchievements");
	SwitchUIFontSize(params->font, 28);
	SwitchUIPrint(params->font, 408, 142, 784, 0xFFF5F3EF, "RetroAchievements");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 184, 784, 0xFFB8B2AA, "Softcore: save states, cheats, and rewind remain available.");
	const char* labels[] = { "Username", "Password", "Remember sign-in", "Account", "Current game", "Unlock notifications", "Unlock notification duration" };
	char duration[64];
	snprintf(duration, sizeof(duration), "%u seconds (Left / Right to adjust)", _notificationSeconds);
	for (unsigned i = 0; i < 7; ++i) {
		int top = 204 + i * 54;
		SwitchUIFontSize(params->font, 14);
		SwitchUIPrint(params->font, 408, top - 4, 784, 0xFFB8B2AA, labels[i]);
		SwitchUIRoundRect(408, top, 784, 36, 8, 0xFF332024);
		if (_menu.index == i) SwitchUIRoundFrame(408, top, 784, 36, 8, 0xFFFFA3BD);
		SwitchUIFontSize(params->font, 18);
		const char* value = i == 0 ? (state.loggedIn ? state.username : (_username[0] ? _username : "Select to enter username")) :
		    i == 1 ? (state.loggedIn ? "Signed in (password not retained)" : _password[0] ? "****************" : "Select to enter password") :
		    i == 2 ? (_remember ? "On: store an API token on this SD card" : "Off: sign in for this session only") :
		    i == 4 ? "View achievements and progress" : i == 5 ? (_unlockNotifications ? "On" : "Off") : i == 6 ? duration :
		    !state.available ? "Service unavailable" : state.busy ? "Cancel sign-in / loading" : state.loggedIn ? "Sign out" : "Sign in";
		SwitchUIPrint(params->font, 424, top + 25, 752, 0xFFF5F3EF, value);
	}
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, 408, 598, 784, 0xFFFFA3BD, state.status);
	char summary[320];
	if (state.game[0]) snprintf(summary, sizeof(summary), "%s: %u/%u unlocked, %u points", state.game, state.unlocked, state.achievements, state.points);
	else strlcpy(summary, "Only achievement progress is stored online; game saves stay on your SD card.", sizeof(summary));
	SwitchUIPrint(params->font, 408, 630, 784, 0xFFB8B2AA, summary);
	SwitchUIDrawRect(48, 658, 1184, 1, 0xFF38332D);
	SwitchUIHint(params->font, 48, 696, "B", "Back");
	SwitchUIHint(params->font, 1040, 696, "A", "Select");
	SwitchUIFontSize(params->font, oldSize);
	if (params->guiFinish) params->guiFinish();
	params->drawEnd();
	*result = GUI_MENU_CONTINUE;
	return true;
}

void SwitchUIAchievementsNotice(void) {
	if (!_runner) return;
	u64 now = armTicksToNs(armGetSystemTick());
	if (now >= _noticeUntil) {
		if (!SwitchAchievementsGetNotice(&_notice)) return;
		_noticeUntil = now + (_notice.unlocked ? (u64) _notificationSeconds : 5ULL) * 1000000000ULL;
	}
	struct GUIParams* params = &_runner->params;
	if (params->guiPrepare) params->guiPrepare();
	unsigned oldSize = SwitchUIFontSizeGet(params->font);
	if (_notice.unlocked) _cacheBadge(_notice.badge, now);
	struct SwitchAchievementNotice queued;
	unsigned preloaded = 0;
	for (unsigned i = 0; preloaded < 3 && SwitchAchievementsPeekNotice(i, &queued); ++i) {
		if (!queued.unlocked) continue;
		_cacheBadge(queued.badge, now);
		++preloaded;
	}
	_pollBadges(params);
	int height = _notice.unlocked ? 96 : 64;
	SwitchUIRoundRect(440, 10, 808, height, 10, 0xF51F1417);
	SwitchUIRoundFrame(440, 10, 808, height, 10, 0xFFFFA3BD);
	int textX = _notice.unlocked ? 538 : 458;
	int textWidth = 1230 - textX;
	if (_notice.unlocked) _drawBadge(_notice.badge, 458, 26, now);
	SwitchUIFontSize(params->font, 16);
	SwitchUIPrint(params->font, textX, 32, textWidth, 0xFFFFA3BD,
	    _notice.unlocked ? "Achievement unlocked" : "RetroAchievements");
	SwitchUIFontSize(params->font, 18);
	SwitchUIPrint(params->font, textX, 59, textWidth, 0xFFF5F3EF, _notice.title);
	if (_notice.unlocked) {
		char points[64];
		snprintf(points, sizeof(points), "%u %s", _notice.points, _notice.points == 1 ? "point" : "points");
		SwitchUIFontSize(params->font, 16);
		SwitchUIPrint(params->font, textX, 87, textWidth, 0xFFFFA3BD, points);
	}
	GUIFontDrawSubmit(params->font);
	SwitchUIFontSize(params->font, oldSize);
}
