/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "switch-updater.h"
#include <switch.h>
#include <curl/curl.h>
#include "../../third-party/discord-rpc/include/rapidjson/document.h"
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <string>
#include <unistd.h>

#ifndef SWITCH_UPDATE_REPOSITORY
#define SWITCH_UPDATE_REPOSITORY ""
#endif
#ifndef SWITCH_UPDATE_VERSION
#define SWITCH_UPDATE_VERSION "1.0.0"
#endif

namespace {
Mutex lock;
CondVar changed;
Thread worker;
bool initialized, running;
bool pendingInstall;
int request;
SwitchUpdaterState state = {};
std::string executable, downloadUrl, expectedHash;
size_t expectedSize;
const size_t maxDownload = 64 * 1024 * 1024;

void status(SwitchUpdatePhase phase, const char* message) {
	mutexLock(&lock);
	state.phase = phase;
	snprintf(state.status, sizeof(state.status), "%s", message);
	mutexUnlock(&lock);
}

bool cancelled() {
	mutexLock(&lock);
	bool result = !running;
	mutexUnlock(&lock);
	return result;
}

int progress(void*, curl_off_t, curl_off_t current, curl_off_t, curl_off_t) {
	mutexLock(&lock);
	if (state.phase == SWITCH_UPDATE_DOWNLOADING && expectedSize) {
		state.progress = static_cast<unsigned>(current * 100 / expectedSize);
		if (state.progress > 100) state.progress = 100;
	}
	bool stop = !running;
	mutexUnlock(&lock);
	return stop;
}

size_t receive(char* data, size_t size, size_t count, void* userdata) {
	std::string& body = *static_cast<std::string*>(userdata);
	if (size && count > (1024 * 1024 - body.size()) / size) return 0;
	body.append(data, size * count);
	return size * count;
}

CURL* connection(const std::string& url) {
	CURL* curl = curl_easy_init();
	if (!curl) return nullptr;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 180L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "mGBA-Switch/" SWITCH_UPDATE_VERSION);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	return curl;
}

bool fetch(const std::string& url, std::string& body, long* responseCode = nullptr) {
	CURL* curl = connection(url);
	if (!curl) return false;
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	CURLcode result = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	if (responseCode) *responseCode = code;
	curl_easy_cleanup(curl);
	return result == CURLE_OK && code == 200;
}

bool version(const char* text, unsigned (&parts)[3]) {
	for (unsigned i = 0; i < 3; ++i) {
		if (*text < '0' || *text > '9') return false;
		parts[i] = 0;
		unsigned digits = 0;
		while (*text >= '0' && *text <= '9') {
			if (++digits > 6) return false;
			parts[i] = parts[i] * 10 + *text++ - '0';
		}
		if (i < 2 && *text++ != '.') return false;
	}
	return !*text;
}

bool newer(const char* candidate) {
	unsigned next[3], current[3];
	if (!version(candidate, next) || !version(SWITCH_UPDATE_VERSION, current)) return false;
	for (unsigned i = 0; i < 3; ++i) {
		if (next[i] != current[i]) return next[i] > current[i];
	}
	return false;
}

void check() {
	std::string body;
	long responseCode = 0;
	if (!fetch("https://api.github.com/repos/" SWITCH_UPDATE_REPOSITORY "/releases/latest", body, &responseCode)) {
		if (responseCode == 404) {
			status(SWITCH_UPDATE_IDLE, "No Switch releases are available yet.");
			return;
		}
		status(SWITCH_UPDATE_ERROR, "Could not check releases. Try again later.");
		return;
	}
	rapidjson::Document release;
	release.Parse(body.c_str());
	if (!release.IsObject() || !release.HasMember("tag_name") || !release["tag_name"].IsString() ||
	    !release.HasMember("assets") || !release["assets"].IsArray()) {
		status(SWITCH_UPDATE_ERROR, "The release information is invalid.");
		return;
	}
	std::string tag = release["tag_name"].GetString();
	if (tag.compare(0, 8, "switch-v") || !newer(tag.c_str() + 8)) {
		status(SWITCH_UPDATE_IDLE, "You have the latest Switch release.");
		return;
	}
	std::string base = "https://github.com/" SWITCH_UPDATE_REPOSITORY "/releases/download/" + tag + "/";
	bool found = false;
	for (auto& asset : release["assets"].GetArray()) {
		if (asset.IsObject() && asset.HasMember("name") && asset["name"].IsString() &&
		    !strcmp(asset["name"].GetString(), "update.json") &&
		    asset.HasMember("browser_download_url") && asset["browser_download_url"].IsString() &&
		    base + "update.json" == asset["browser_download_url"].GetString()) found = true;
	}
	body.clear();
	if (!found || !fetch(base + "update.json", body)) {
		status(SWITCH_UPDATE_ERROR, "This release has no downloadable update manifest.");
		return;
	}
	rapidjson::Document manifest;
	manifest.Parse(body.c_str());
	if (!manifest.IsObject() || !manifest.HasMember("version") || !manifest["version"].IsString() ||
	    tag != std::string("switch-v") + manifest["version"].GetString() ||
	    !manifest.HasMember("url") || !manifest["url"].IsString() ||
	    base + "mgba.nro" != manifest["url"].GetString() ||
	    !manifest.HasMember("sha256") || !manifest["sha256"].IsString() ||
	    manifest["sha256"].GetStringLength() != 64 ||
	    !manifest.HasMember("size") || !manifest["size"].IsUint64() ||
	    manifest["size"].GetUint64() < 0x80 || manifest["size"].GetUint64() > maxDownload) {
		status(SWITCH_UPDATE_ERROR, "The update manifest is invalid.");
		return;
	}
	expectedHash = manifest["sha256"].GetString();
	for (char& c : expectedHash) {
		if (c >= 'A' && c <= 'F') c += 'a' - 'A';
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			status(SWITCH_UPDATE_ERROR, "The update checksum is invalid.");
			return;
		}
	}
	expectedSize = static_cast<size_t>(manifest["size"].GetUint64());
	downloadUrl = manifest["url"].GetString();
	mutexLock(&lock);
	snprintf(state.version, sizeof(state.version), "%s", manifest["version"].GetString());
	mutexUnlock(&lock);
	status(SWITCH_UPDATE_AVAILABLE, "A new Switch release is available.");
}

struct Download {
	FILE* file;
	size_t received;
	Sha256Context hash;
	unsigned char header[0x80];
};

size_t writeDownload(char* bytes, size_t size, size_t count, void* userdata) {
	Download& data = *static_cast<Download*>(userdata);
	if (size && count > (expectedSize - data.received) / size) return 0;
	size_t length = size * count;
	if (data.received < sizeof(data.header)) {
		size_t headerBytes = sizeof(data.header) - data.received;
		if (headerBytes > length) headerBytes = length;
		memcpy(data.header + data.received, bytes, headerBytes);
	}
	if (fwrite(bytes, 1, length, data.file) != length) return 0;
	sha256ContextUpdate(&data.hash, bytes, length);
	data.received += length;
	return length;
}

void install() {
	std::string temporary = executable + ".update";
	unsigned char currentHeader[0x14];
	FILE* current = fopen(executable.c_str(), "rb");
	bool validCurrent = current && fread(currentHeader, 1, sizeof(currentHeader), current) == sizeof(currentHeader) &&
	                    !memcmp(currentHeader + 0x10, "NRO0", 4);
	if (current) fclose(current);
	if (!validCurrent) {
		status(SWITCH_UPDATE_ERROR, "The launch path does not identify an installed NRO. Update manually.");
		return;
	}
	Download data = {};
	data.file = fopen(temporary.c_str(), "wb");
	if (!data.file) { status(SWITCH_UPDATE_ERROR, "Could not create update file on the SD card."); return; }
	sha256ContextCreate(&data.hash);
	CURL* curl = connection(downloadUrl);
	CURLcode result = CURLE_FAILED_INIT;
	long code = 0;
	if (curl) {
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDownload);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
		result = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		curl_easy_cleanup(curl);
	}
	bool flushed = fflush(data.file) == 0;
	if (flushed) flushed = fsync(fileno(data.file)) == 0;
	if (fclose(data.file)) flushed = false;
	unsigned char digest[32];
	sha256ContextGetHash(&data.hash, digest);
	char hash[65];
	for (unsigned i = 0; i < sizeof(digest); ++i) snprintf(hash + i * 2, 3, "%02x", digest[i]);
	uint32_t nroSize = static_cast<uint32_t>(data.header[0x18]) |
	                   static_cast<uint32_t>(data.header[0x19]) << 8 |
	                   static_cast<uint32_t>(data.header[0x1A]) << 16 |
	                   static_cast<uint32_t>(data.header[0x1B]) << 24;
	if (cancelled() || result != CURLE_OK || code != 200 || !flushed || data.received != expectedSize ||
	    expectedHash != hash || memcmp(data.header + 0x10, "NRO0", 4) || nroSize < 0x80 || nroSize > expectedSize) {
		remove(temporary.c_str());
		status(SWITCH_UPDATE_ERROR, "Update download failed validation. The current app is unchanged.");
		return;
	}
	mutexLock(&lock);
	pendingInstall = true;
	state.progress = 100;
	mutexUnlock(&lock);
	status(SWITCH_UPDATE_READY, "Download verified. Restart to install the update.");
}

void work(void*) {
	for (;;) {
		mutexLock(&lock);
		while (running && !request) condvarWait(&changed, &lock);
		if (!running) { mutexUnlock(&lock); return; }
		int action = request;
		request = 0;
		mutexUnlock(&lock);
		if (action == 1) check();
		else install();
	}
}
}

bool SwitchUpdaterInit(const char* path) {
	if (initialized) return true;
	mutexInit(&lock);
	condvarInit(&changed);
	state = {};
	if (!*SWITCH_UPDATE_REPOSITORY) {
		snprintf(state.status, sizeof(state.status), "No update repository is configured.");
		return false;
	}
	bool validPath = path && *path && (!strncmp(path, "sdmc:/", 6) || path[0] == '/') &&
	                 !strstr(path, "..") && !strchr(path, '\\') && !strchr(path, '"');
	if (validPath) {
		for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
			if (*p < 32 || *p == 127) { validPath = false; break; }
		}
	}
	executable = validPath ? path : "";
	if (curl_global_init(CURL_GLOBAL_DEFAULT)) return false;
	running = true;
	if (R_FAILED(threadCreate(&worker, work, nullptr, nullptr, 128 * 1024, 0x3B, -2))) {
		running = false;
		curl_global_cleanup();
		return false;
	}
	if (R_FAILED(threadStart(&worker))) {
		running = false;
		threadClose(&worker);
		curl_global_cleanup();
		return false;
	}
	initialized = true;
	state.canInstall = validPath && envHasNextLoad();
	snprintf(state.status, sizeof(state.status), "Ready to check for updates.");
	return true;
}

void SwitchUpdaterDeinit(void) {
	if (!initialized) return;
	mutexLock(&lock);
	running = false;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	threadWaitForExit(&worker);
	threadClose(&worker);
	curl_global_cleanup();
	initialized = false;
}

bool SwitchUpdaterApplyPending(void) {
	if (initialized || !pendingInstall) return false;
	std::string temporary = executable + ".update";
	std::string previous = executable + ".previous";
	if (remove(previous.c_str()) && errno != ENOENT) {
		status(SWITCH_UPDATE_ERROR, "Could not prepare the update backup. The current app is unchanged.");
		return false;
	}
	if (rename(executable.c_str(), previous.c_str())) {
		status(SWITCH_UPDATE_ERROR, "Could not back up the current app after shutdown. The current app is unchanged.");
		return false;
	}
	if (rename(temporary.c_str(), executable.c_str())) {
		bool restored = rename(previous.c_str(), executable.c_str()) == 0;
		status(SWITCH_UPDATE_ERROR, restored ? "Could not install update. The original app was restored." :
		       "Could not restore the app. Rename its .previous backup on the SD card.");
		return false;
	}
	pendingInstall = false;
	return true;
}

void SwitchUpdaterCheck(void) {
	if (!initialized) return;
	mutexLock(&lock);
	if (state.phase == SWITCH_UPDATE_IDLE || state.phase == SWITCH_UPDATE_ERROR || state.phase == SWITCH_UPDATE_AVAILABLE) {
		state.phase = SWITCH_UPDATE_CHECKING;
		state.progress = 0;
		request = 1;
		snprintf(state.status, sizeof(state.status), "Checking GitHub releases...");
		condvarWakeOne(&changed);
	}
	mutexUnlock(&lock);
}

void SwitchUpdaterInstall(void) {
	if (!initialized) return;
	mutexLock(&lock);
	if (state.phase == SWITCH_UPDATE_AVAILABLE && state.canInstall) {
		state.phase = SWITCH_UPDATE_DOWNLOADING;
		state.progress = 0;
		request = 2;
		snprintf(state.status, sizeof(state.status), "Downloading and verifying update...");
		condvarWakeOne(&changed);
	}
	mutexUnlock(&lock);
}

void SwitchUpdaterGetState(struct SwitchUpdaterState* out) {
	if (!out) return;
	mutexLock(&lock);
	*out = state;
	mutexUnlock(&lock);
}
