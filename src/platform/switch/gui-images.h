/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_GUI_IMAGES_H
#define SWITCH_GUI_IMAGES_H
#include <stdbool.h>
#include <stddef.h>
struct mImage;
void SwitchImagesInit(void);
void SwitchImagesDeinit(void);
void SwitchImagesSuspend(void);
void SwitchImagesInvalidate(void);
void SwitchImagesRequest(const char* rom, bool banner);
void SwitchImagesRequestPath(const char* path);
bool SwitchImagesPoll(char* rom, size_t size, bool* banner, struct mImage** image);
#endif
