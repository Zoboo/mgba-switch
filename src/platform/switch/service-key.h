/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef SWITCH_SERVICE_KEY_H
#define SWITCH_SERVICE_KEY_H
#include <string>
#include <cstdio>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* Create empty placeholders without ever truncating an existing key. */
static inline bool SwitchServiceKeyRead(const char* service, std::string& key) {
	mkdir("sdmc:/mGBA", 0777);
	std::string directory = std::string("sdmc:/mGBA/") + service;
	mkdir(directory.c_str(), 0700);
	std::string path = directory + "/api-key.txt";
	int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) close(fd);
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return false;
	char buffer[1025] = {};
	size_t size = fread(buffer, 1, sizeof(buffer) - 1, file);
	bool valid = !ferror(file) && size < sizeof(buffer) - 1;
	fclose(file);
	size_t first = size >= 3 && (unsigned char) buffer[0] == 0xEF && (unsigned char) buffer[1] == 0xBB && (unsigned char) buffer[2] == 0xBF ? 3 : 0;
	while (first < size && std::isspace((unsigned char) buffer[first])) ++first;
	while (size > first && std::isspace((unsigned char) buffer[size - 1])) --size;
	valid = valid && size > first && size - first <= 128;
	for (size_t i = first; valid && i < size; ++i) valid = std::isalnum((unsigned char) buffer[i]);
	if (valid) key.assign(buffer + first, size - first);
	volatile char* wipe = buffer;
	for (size_t i = 0; i < sizeof(buffer); ++i) wipe[i] = 0;
	return valid;
}
#endif
