/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_RA_BADGES_H
#define SWITCH_RA_BADGES_H
#include <mgba-util/common.h>
CXX_GUARD_START
struct mImage;
void SwitchRABadgesInit(const char* directory);
void SwitchRABadgesDeinit(void);
bool SwitchRABadgesRequest(const char* url);
/* Poll transfers ownership of the image; NULL indicates a failed request. */
bool SwitchRABadgesPoll(char* url, size_t size, struct mImage** image);
CXX_GUARD_END
#endif
