/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_ARTWORK_H
#define SWITCH_ARTWORK_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct SwitchArtworkState { bool applying, busy, needsChoice, searching, finished, resetSucceeded, bulk; unsigned percent, revision, resetId, resetDone, bulkDone, bulkTotal; size_t count; char rom[4096], message[192]; };
struct SwitchArtworkOption { unsigned id; bool banner; char title[192], preview[4096]; };
void SwitchArtworkGetState(struct SwitchArtworkState* state);
bool SwitchArtworkGetOption(size_t index, struct SwitchArtworkOption* option);
void SwitchArtworkSearch(const char* title);
bool SwitchArtworkOpen(const char* romPath, bool banner);
bool SwitchArtworkImport(const char* romPath, const char* imagePath, bool banner);
bool SwitchArtworkReset(const char* romPath, bool banner);
void SwitchArtworkChoose(size_t index);
void SwitchArtworkCancelChoice(void);
void SwitchArtworkInit(const char* directory);
void SwitchArtworkDeinit(void);
void SwitchArtworkSuspend(void);
bool SwitchArtworkSetKey(const char* key);
bool SwitchArtworkHasKey(void);
void SwitchArtworkSetDownloads(bool covers, bool banners);
void SwitchArtworkRequest(const char* romPath);
bool SwitchArtworkAttempt(const char* romPath);
bool SwitchArtworkDownloadBest(const char* romPath, const char* title);
bool SwitchArtworkDownloadMissing(const char* directory);
void SwitchArtworkCancelBulk(void);
bool SwitchArtworkPath(const char* romPath, bool banner, char* out, size_t size);
void SwitchArtworkTest(void);
void SwitchArtworkStatus(char* out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
