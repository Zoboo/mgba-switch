/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_ARTWORK_JPEG_H
#define SWITCH_ARTWORK_JPEG_H

#include <mgba-util/common.h>

CXX_GUARD_START
bool SwitchArtworkConvertJPEG(const void* data, size_t size, const char* output);
CXX_GUARD_END

#endif
