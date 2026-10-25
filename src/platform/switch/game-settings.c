/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "game-settings.h"

#include "feature/gui/gui-runner.h"
#include <mgba/core/core.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/vfs.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

static char _rom[PATH_MAX];

static bool _valid(const struct SwitchGameSettings* settings) {
	if (!memchr(settings->directory, 0, sizeof(settings->directory)) ||
	    !memchr(settings->extension, 0, sizeof(settings->extension))) return false;
	if (*settings->directory && (strncmp(settings->directory, "sdmc:/", 6) ||
	    strpbrk(settings->directory, "\r\n"))) return false;
	return !*settings->extension || !strcmp(settings->extension, ".sav") ||
	       !strcmp(settings->extension, ".srm") || !strcmp(settings->extension, ".dat");
}

static bool _path(const char* rom, char* path, size_t size, bool create) {
	if (!rom || !*rom || strlen(rom) >= PATH_MAX || strpbrk(rom, "\r\n")) return false;
	char directory[PATH_MAX];
	mCoreConfigDirectory(directory, sizeof(directory));
	if (strlcat(directory, "/game-settings", sizeof(directory)) >= sizeof(directory)) return false;
	if (create && mkdir(directory, 0777) && errno != EEXIST) return false;
	uint64_t hash = UINT64_C(14695981039346656037);
	for (const unsigned char* p = (const unsigned char*) rom; *p; ++p) {
		hash = (hash ^ *p) * UINT64_C(1099511628211);
	}
	return snprintf(path, size, "%s/%016llx.cfg", directory, (unsigned long long) hash) < (int) size;
}

static bool _readLine(FILE* file, char* value, size_t size) {
	size_t length = 0;
	int c;
	while ((c = fgetc(file)) != EOF) {
		if (c == '\n') { value[length] = 0; return true; }
		if (!c || c == '\r' || length + 1 >= size) return false;
		value[length++] = c;
	}
	return false;
}

void SwitchGameSettingsDefaults(struct SwitchGameSettings* settings) {
	memset(settings, 0, sizeof(*settings));
	strlcpy(settings->extension, ".sav", sizeof(settings->extension));
}

bool SwitchGameSettingsLoad(const char* rom, struct SwitchGameSettings* settings) {
	memset(settings, 0, sizeof(*settings));
	char path[PATH_MAX];
	if (!_path(rom, path, sizeof(path), false)) return false;
	struct stat st;
	if (stat(path, &st)) return errno == ENOENT;
	FILE* file = fopen(path, "rb");
	if (!file) return false;
	char header[32], storedRom[PATH_MAX];
	bool ok = _readLine(file, header, sizeof(header)) && !strcmp(header, "mGBA Switch saves 1") &&
	          _readLine(file, storedRom, sizeof(storedRom)) && !strcmp(storedRom, rom) &&
	          _readLine(file, settings->directory, sizeof(settings->directory)) &&
	          _readLine(file, settings->extension, sizeof(settings->extension)) &&
	          fgetc(file) == EOF && !ferror(file) && _valid(settings);
	if (fclose(file)) ok = false;
	return ok;
}

bool SwitchGameSettingsSave(const char* rom, const struct SwitchGameSettings* settings) {
	char path[PATH_MAX], temporary[PATH_MAX], backup[PATH_MAX];
	if (!_valid(settings) || !_path(rom, path, sizeof(path), true) ||
	    snprintf(temporary, sizeof(temporary), "%s.tmp", path) >= (int) sizeof(temporary) ||
	    snprintf(backup, sizeof(backup), "%s.previous", path) >= (int) sizeof(backup)) return false;
	if (*settings->directory) {
		struct stat st;
		if (stat(settings->directory, &st) || !S_ISDIR(st.st_mode)) return false;
	}
	FILE* file = fopen(temporary, "wb");
	if (!file) return false;
	bool ok = fprintf(file, "mGBA Switch saves 1\n%s\n%s\n%s\n", rom, settings->directory, settings->extension) > 0;
	if (fflush(file) || ferror(file)) ok = false;
	if (fclose(file)) ok = false;
	if (ok) {
		struct stat st;
		bool existing = !stat(path, &st);
		if (!existing && errno != ENOENT) ok = false;
		if (ok && existing) {
			if (remove(backup) && errno != ENOENT) ok = false;
			if (ok) ok = !rename(path, backup);
		}
		if (ok && rename(temporary, path)) {
			if (existing) rename(backup, path);
			ok = false;
		}
		if (ok && existing) remove(backup);
	}
	if (!ok) remove(temporary);
	return ok;
}

void SwitchGameSettingsSetRom(const char* rom) {
	strlcpy(_rom, rom ? rom : "", sizeof(_rom));
}

void SwitchGameSettingsApply(struct mGUIRunner* runner) {
	struct SwitchGameSettings settings;
	if (!*_rom) return;
	if (!SwitchGameSettingsLoad(_rom, &settings)) {
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 0, "Could not read this game's save settings. Using the default save.");
		return;
	}
	if (!*settings.directory && !*settings.extension) return;
	char suffix[16] = ".sav";
	int player = 0;
	mCoreConfigGetIntValue(&runner->core->config, "savePlayerId", &player);
	if (player > 1) snprintf(suffix, sizeof(suffix), ".sa%i", player);
	if (*settings.extension) strlcpy(suffix, settings.extension, sizeof(suffix));
	struct VDir* directory = *settings.directory ? VDirOpen(settings.directory) : runner->core->dirs.save;
	struct VFile* save = directory ? mDirectorySetOpenSuffix(&runner->core->dirs, directory, suffix, O_CREAT | O_RDWR) : NULL;
	if (*settings.directory && directory) directory->close(directory);
	if (!save || !runner->core->loadSave(runner->core, save)) {
		if (save) save->close(save);
		GUIShowMessageBox(&runner->params, GUI_MESSAGE_BOX_OK, 0, "Could not open the selected save location or type. Using the default save.");
	}
}
