/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_ACHIEVEMENTS_H
#define SWITCH_ACHIEVEMENTS_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct mCore;
struct SwitchAchievement {
	char title[256], description[1024], progress[64];
	char badge[512];
	unsigned points;
	float percent;
	bool unlocked, unsupported;
};
/* filter: 0 = all, 1 = locked, 2 = unlocked. */
unsigned SwitchAchievementsGetPage(unsigned first, struct SwitchAchievement* entries, unsigned capacity, unsigned filter);
struct SwitchAchievementsState {
	bool available, loggedIn, busy;
	char username[128], status[256], game[256];
	unsigned achievements, unlocked, points;
};
bool SwitchAchievementsInit(const char* directory);
void SwitchAchievementsDeinit(void);
void SwitchAchievementsIdle(void);
void SwitchAchievementsAttach(struct mCore* core);
void SwitchAchievementsDetach(void);
void SwitchAchievementsSetPaused(bool paused);
void SwitchAchievementsResetProgress(void);
void SwitchAchievementsLogin(const char* username, const char* password, bool remember);
void SwitchAchievementsLogout(void);
void SwitchAchievementsGetState(struct SwitchAchievementsState* state);
struct SwitchAchievementNotice {
	char title[512], badge[512];
	unsigned points;
	bool unlocked;
};
bool SwitchAchievementsGetNotice(struct SwitchAchievementNotice* notice);
bool SwitchAchievementsPeekNotice(unsigned index, struct SwitchAchievementNotice* notice);
void SwitchAchievementsSetNotificationsEnabled(bool enabled);
#ifdef __cplusplus
}
#endif
#endif
