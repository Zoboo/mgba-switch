/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_GUI_UI_H
#define SWITCH_GUI_UI_H

#include <mgba-util/gui/menu.h>

struct mGUIRunner;
struct GUIFont;
void SwitchUIInit(struct mGUIRunner* runner);
void SwitchUIDeinit(void);
void SwitchUIFilesDeinit(void);
bool SwitchUIFilesRefreshLibrary(struct GUIParams*, struct GUIMenu*);
bool SwitchUIPlazaRun(struct GUIParams*, bool* files);
void SwitchUIAchievementsInit(struct mGUIRunner*, const char* directory);
void SwitchUIAchievementsDeinit(void);
struct GUIMenu* SwitchUIAchievementsMenu(void);
struct GUIMenu* SwitchUIAchievementsGameMenu(void);
bool SwitchUIAchievementsGameRun(struct GUIParams*, enum GUIMenuExitReason*);
bool SwitchUIAchievementsRun(struct GUIParams*, enum GUIMenuExitReason*);
void SwitchUIAchievementsNotice(void);
void SwitchUIUpdatesInit(struct mGUIRunner*);
void SwitchUIUpdatesStart(const char* executable);
void SwitchUIUpdatesDeinit(void);
struct GUIMenu* SwitchUIUpdatesMenu(void);
bool SwitchUIUpdatesRun(struct GUIParams*, enum GUIMenuExitReason*);
void SwitchUIUpdatesPoll(struct GUIParams*, bool prompt);
bool SwitchUIUpdatesShouldExit(void);
void SwitchUIUpdatesFinishRestart(void);
struct GUIMenu* SwitchUIArtworkMenu(void);
struct GUIMenu* SwitchUIInterfaceMenu(void);
void SwitchUIConsoleDraw(struct GUIParams*, const struct GUIMenu*, const size_t*, size_t, size_t, float);
bool SwitchUIArtworkDraw(const char*, bool, int, int, int, int);
void SwitchUIRomPath(struct GUIParams*, const char*, char*, size_t);
void SwitchUIFallback(struct GUIFont*, const struct GUIMenuItem*, int, int, int, int);
void SwitchUIRailIcon(int kind, uint32_t color);
/* Returns -1 for content input, -2 for consumed input, or the activated rail item (0-4). */
int SwitchUINavigationInput(struct GUIParams*, unsigned current, bool enter, uint32_t input,
                            bool tapped, unsigned x, unsigned y);
void SwitchUINavigationDraw(struct GUIParams*, unsigned current);
bool SwitchUINavigationExitRequested(void);
void SwitchUIOpenSettings(struct GUIParams*);
bool SwitchUILibraryMenu(struct GUIParams*, struct GUIMenu*, struct GUIMenuState*, enum GUIMenuExitReason*);
void SwitchUIDrawRect(int x, int y, int width, int height, uint32_t color);
void SwitchUIDrawTexture(unsigned texture, int x, int y, int width, int height);
void SwitchUIDrawFrame(int x, int y, int width, int height, uint32_t color);
void SwitchUIRoundRect(int x, int y, int width, int height, int radius, uint32_t color);
void SwitchUIRoundFrame(int x, int y, int width, int height, int radius, uint32_t color);
void SwitchUIHint(struct GUIFont*, int x, int y, const char* button, const char* label);
void SwitchUIFontSize(struct GUIFont*, unsigned size);
unsigned SwitchUIFontSizeGet(const struct GUIFont*);
void SwitchUIBeginMenu(const struct GUIMenu*);
void SwitchUIPrint(struct GUIFont*, int x, int y, int maxWidth, uint32_t color, const char* text);

#endif
