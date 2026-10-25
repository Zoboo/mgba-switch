/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_GAME_SETTINGS_H
#define SWITCH_GAME_SETTINGS_H

#include <mgba-util/common.h>

struct mGUIRunner;
struct SwitchGameSettings {
	char directory[PATH_MAX];
	char extension[16];
	bool boxArt;
};

bool SwitchGameSettingsLoad(const char* rom, struct SwitchGameSettings* settings);
void SwitchGameSettingsDefaults(struct SwitchGameSettings* settings);
bool SwitchGameSettingsSave(const char* rom, const struct SwitchGameSettings* settings);
void SwitchGameSettingsSetRom(const char* rom);
void SwitchGameSettingsApply(struct mGUIRunner* runner);

#endif
