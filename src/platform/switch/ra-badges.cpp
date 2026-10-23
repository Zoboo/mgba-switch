/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ra-badges.h"
#include <mgba-util/image.h>
#include <mgba-util/sha1.h>
#include <switch.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

namespace {
struct BadgeResult { std::string url; mImage* image; };
struct Failure { std::string url; uint64_t tick; };
Mutex lock;
CondVar changed;
Thread worker;
bool initialized, running;
std::string directory, active;
std::deque<std::string> pending;
std::deque<BadgeResult> complete;
std::deque<Failure> failures;

mImage* load(const std::string& path) {
	FILE* file = fopen(path.c_str(), "rb");
	if (!file) return nullptr;
	unsigned char header[24];
	bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) && !memcmp(header, "\x89PNG\r\n\x1a\n", 8);

	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fclose(file);
	if (!valid || length < 24 || length > 1024 * 1024) return nullptr;
	uint32_t w = (uint32_t(header[16]) << 24) | (uint32_t(header[17]) << 16) | (uint32_t(header[18]) << 8) | header[19];
	uint32_t h = (uint32_t(header[20]) << 24) | (uint32_t(header[21]) << 16) | (uint32_t(header[22]) << 8) | header[23];
	if (!w || !h || w > 512 || h > 512) return nullptr;
	mImage* source = mImageLoad(path.c_str());
	if (!source) return nullptr;
	mImage* rgba = mImageConvertToFormat(source, mCOLOR_ABGR8);
	mImageDestroy(source);
	if (!rgba || (rgba->width <= 64 && rgba->height <= 64)) return rgba;
	w = rgba->width; h = rgba->height;
	if (w > 64) { h = h * 64 / w; w = 64; }
	if (h > 64) { w = w * 64 / h; h = 64; }
	if (!w) w = 1;
	if (!h) h = 1;
	mImage* resized = mImageCreate(w, h, mCOLOR_ABGR8);
	if (resized) for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x)
		static_cast<uint32_t*>(resized->data)[y * resized->stride + x] =
		    static_cast<uint32_t*>(rgba->data)[(y * rgba->height / h) * rgba->stride + x * rgba->width / w];
	mImageDestroy(rgba);
	return resized;
}

size_t receive(char* data, size_t size, size_t count, void* userdata) {
	std::string& body = *static_cast<std::string*>(userdata);
	if (size && count > (1024 * 1024 - body.size()) / size) return 0;
	body.append(data, size * count);
	return size * count;
}

int progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	mutexLock(&lock);
	bool cancelled = !running;
	mutexUnlock(&lock);
	return cancelled;
}

mImage* fetch(const std::string& url) {
	uint8_t digest[20];
	sha1Buffer(url.data(), url.size(), digest);
	char name[41];
	for (unsigned i = 0; i < 20; ++i) snprintf(name + i * 2, 3, "%02x", digest[i]);
	std::string path = directory + "/" + name + ".png";
	mImage* image = load(path);
	if (image) return image;
	std::string body;
	CURL* curl = curl_easy_init();
	if (!curl) return nullptr;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "mGBA-Switch/0.11");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	if (code != CURLE_OK || status != 200) return nullptr;
	mkdir(directory.c_str(), 0755);
	std::string temporary = path + ".tmp";
	FILE* file = fopen(temporary.c_str(), "wb");
	if (!file) return nullptr;
	bool written = fwrite(body.data(), 1, body.size(), file) == body.size();
	if (fclose(file)) written = false;
	if (written) image = load(temporary);
	if (image) rename(temporary.c_str(), path.c_str());
	remove(temporary.c_str());
	return image;
}

void work(void*) {
	for (;;) {
		mutexLock(&lock);
		while (running && (pending.empty() || complete.size() >= 4)) condvarWait(&changed, &lock);
		if (!running) { mutexUnlock(&lock); return; }
		std::string url = pending.front();
		pending.pop_front();
		active = url;
		mutexUnlock(&lock);
		mImage* image = fetch(url);
		mutexLock(&lock);
		active.clear();
		if (!image) {
			if (failures.size() == 64) failures.pop_front();
			failures.push_back({url, armGetSystemTick()});
		}
		if (running) complete.push_back({url, image});
		else if (image) mImageDestroy(image);
		mutexUnlock(&lock);
	}
}
}

void SwitchRABadgesInit(const char* path) {
	if (initialized || !path || !*path || curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return;
	directory = path;
	mutexInit(&lock);
	condvarInit(&changed);
	running = true;
	if (R_FAILED(threadCreate(&worker, work, nullptr, nullptr, 128 * 1024, 0x3B, -2))) { running = false; curl_global_cleanup(); return; }
	if (R_FAILED(threadStart(&worker))) { threadClose(&worker); running = false; curl_global_cleanup(); return; }
	initialized = true;
}

void SwitchRABadgesDeinit(void) {
	if (!initialized) return;
	mutexLock(&lock);
	running = false;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	threadWaitForExit(&worker);
	threadClose(&worker);
	for (auto& result : complete) if (result.image) mImageDestroy(result.image);
	complete.clear(); pending.clear(); failures.clear(); active.clear();
	initialized = false;
	curl_global_cleanup();
}

bool SwitchRABadgesRequest(const char* url) {
	if (!initialized || !url || strncmp(url, "https://", 8) || strlen(url) >= 1024) return false;
	mutexLock(&lock);
	bool exists = active == url;
	for (const auto& request : pending) exists |= request == url;
	for (const auto& result : complete) exists |= result.url == url;
	uint64_t tick = armGetSystemTick();
	bool cooldown = false;
	for (auto it = failures.begin(); it != failures.end();) {
		if (tick - it->tick >= armGetSystemTickFreq() * 30) it = failures.erase(it);
		else { cooldown |= it->url == url; ++it; }
	}
	bool accepted = exists || (!cooldown && pending.size() < 16);
	if (!exists && accepted) { pending.emplace_back(url); condvarWakeAll(&changed); }
	mutexUnlock(&lock);
	return accepted;
}

bool SwitchRABadgesPoll(char* url, size_t size, mImage** image) {
	if (!initialized || !url || !size || !image) return false;
	mutexLock(&lock);
	if (complete.empty()) { mutexUnlock(&lock); return false; }
	BadgeResult result = complete.front();
	complete.pop_front();
	snprintf(url, size, "%s", result.url.c_str());
	*image = result.image;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	return true;
}
