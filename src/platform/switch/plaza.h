/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_PLAZA_H
#define SWITCH_PLAZA_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct SwitchPlazaState { bool busy; unsigned percent, revision, installedRevision; size_t count, fileCount, filesEntry; char message[256], installedPath[4096]; };
struct SwitchPlazaEntry { uint64_t id; char title[256], description[2048], baseRom[4096]; };
struct SwitchPlazaFile { char filename[256]; uint64_t size; };
void SwitchPlazaInit(const char* configDirectory);
void SwitchPlazaDeinit(void);
bool SwitchPlazaHasKey(void);
bool SwitchPlazaSetKey(const char* key);
bool SwitchPlazaStartScan(const char* romDirectory);
void SwitchPlazaCancel(void);
void SwitchPlazaGetState(struct SwitchPlazaState* state);
bool SwitchPlazaGetEntry(size_t index, struct SwitchPlazaEntry* entry);
bool SwitchPlazaRequestFiles(size_t index);
bool SwitchPlazaGetFile(size_t index, struct SwitchPlazaFile* file);
bool SwitchPlazaInstall(size_t entryIndex, size_t fileIndex);
#ifdef __cplusplus
}
#include <functional>
#include <string>
#include <vector>
// Called only by background artwork workers. Main images are not advertised as banners.
bool SwitchPlazaArtwork(const char* title, std::vector<unsigned char>& image, const std::function<bool()>& current, std::string& error);
bool SwitchPlazaArtworkOptions(const char* title, const std::function<bool()>& current, const std::function<void(const std::string&, const std::vector<unsigned char>&)>& result, std::string& error);
#endif
#endif
