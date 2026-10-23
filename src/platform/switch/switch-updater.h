/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_UPDATER_H
#define SWITCH_UPDATER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum SwitchUpdatePhase {
	SWITCH_UPDATE_IDLE,
	SWITCH_UPDATE_CHECKING,
	SWITCH_UPDATE_AVAILABLE,
	SWITCH_UPDATE_DOWNLOADING,
	SWITCH_UPDATE_READY,
	SWITCH_UPDATE_ERROR
};

struct SwitchUpdaterState {
	enum SwitchUpdatePhase phase;
	char version[32];
	char status[256];
	unsigned progress;
	bool canInstall;
};

bool SwitchUpdaterInit(const char* executable);
void SwitchUpdaterDeinit(void);
void SwitchUpdaterCheck(void);
void SwitchUpdaterInstall(void);
void SwitchUpdaterGetState(struct SwitchUpdaterState* state);

#ifdef __cplusplus
}
#endif
#endif
