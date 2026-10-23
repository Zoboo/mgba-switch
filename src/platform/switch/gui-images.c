/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "gui-images.h"
#include "artwork.h"
#include <mgba-util/image.h>
#include <switch.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define REQUEST_COUNT 12
#define RESULT_COUNT 4

struct ImageJob {
	char rom[PATH_MAX];
	bool banner;
	bool direct;
	unsigned generation;
	struct mImage* image;
};
static Mutex _lock;
static CondVar _changed;
static Thread _worker;
static bool _initialized, _running, _active;
static unsigned _generation, _requests, _results;
static struct ImageJob _pending[REQUEST_COUNT], _complete[RESULT_COUNT], _current;

static struct mImage* _load(const char* path) {
	FILE* file = fopen(path, "rb");
	if (!file) return NULL;
	unsigned char header[24];
	bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) && !memcmp(header, "\x89PNG\r\n\x1a\n", 8);
	fclose(file);
	if (!valid) return NULL;
	uint32_t width = ((uint32_t) header[16] << 24) | ((uint32_t) header[17] << 16) | ((uint32_t) header[18] << 8) | header[19];
	uint32_t height = ((uint32_t) header[20] << 24) | ((uint32_t) header[21] << 16) | ((uint32_t) header[22] << 8) | header[23];
	if (!width || !height || width > 4096 || height > 4096 || (uint64_t) width * height > 8388608) return NULL;
	struct mImage* image = mImageLoad(path);
	if (!image) return NULL;
	struct mImage* rgba = mImageConvertToFormat(image, mCOLOR_ABGR8);
	mImageDestroy(image);
	if (!rgba) return NULL;
	unsigned w = rgba->width, h = rgba->height;
	if (w > 1280) { h = (uint64_t) h * 1280 / w; w = 1280; }
	if (h > 720) { w = (uint64_t) w * 720 / h; h = 720; }
	if (!w) w = 1;
	if (!h) h = 1;
	if (w == rgba->width && h == rgba->height) return rgba;
	struct mImage* resized = mImageCreate(w, h, mCOLOR_ABGR8);
	if (resized) {
		for (unsigned y = 0; y < h; ++y) {
			for (unsigned x = 0; x < w; ++x) {
				((uint32_t*) resized->data)[y * resized->stride + x] =
				    ((uint32_t*) rgba->data)[(y * rgba->height / h) * rgba->stride + x * rgba->width / w];
			}
		}
	}
	mImageDestroy(rgba);
	return resized;
}

static void _work(void* unused) {
	(void) unused;
	for (;;) {
		mutexLock(&_lock);
		while (_running && (!_requests || _results == RESULT_COUNT)) condvarWait(&_changed, &_lock);
		if (!_running) { mutexUnlock(&_lock); break; }
		_current = _pending[0];
		memmove(_pending, _pending + 1, --_requests * sizeof(*_pending));
		struct ImageJob job = _current;
		_active = true;
		mutexUnlock(&_lock);
		char path[PATH_MAX];
		if (job.direct) job.image = _load(job.rom);
		else if (SwitchArtworkPath(job.rom, job.banner, path, sizeof(path))) job.image = _load(path);
		mutexLock(&_lock);
		_active = false;
		bool accepted = _running && job.generation == _generation;
		if (accepted) _complete[_results++] = job;
		mutexUnlock(&_lock);
		if (!accepted && job.image) mImageDestroy(job.image);
	}
}

void SwitchImagesInit(void) {
	if (_initialized) return;
	mutexInit(&_lock);
	condvarInit(&_changed);
	_running = true;
	if (R_FAILED(threadCreate(&_worker, _work, NULL, NULL, 128 * 1024, 0x3B, -2))) { _running = false; return; }
	if (R_FAILED(threadStart(&_worker))) { threadClose(&_worker); _running = false; return; }
	_initialized = true;
}

void SwitchImagesInvalidate(void) {
	if (!_initialized) return;
	struct mImage* images[RESULT_COUNT];
	mutexLock(&_lock);
	++_generation;
	_requests = 0;
	unsigned count = _results;
	for (unsigned i = 0; i < count; ++i) images[i] = _complete[i].image;
	_results = 0;
	condvarWakeAll(&_changed);
	mutexUnlock(&_lock);
	for (unsigned i = 0; i < count; ++i) if (images[i]) mImageDestroy(images[i]);
}

void SwitchImagesSuspend(void) { SwitchImagesInvalidate(); }

void SwitchImagesDeinit(void) {
	if (!_initialized) return;
	SwitchImagesInvalidate();
	mutexLock(&_lock);
	_running = false;
	condvarWakeAll(&_changed);
	mutexUnlock(&_lock);
	threadWaitForExit(&_worker);
	threadClose(&_worker);
	_initialized = false;
}

static void _request(const char* rom, bool banner, bool direct) {
	if (!_initialized || !rom || !*rom || strlen(rom) >= PATH_MAX) return;
	mutexLock(&_lock);
	bool exists = _active && _current.generation == _generation && _current.banner == banner && !strcmp(_current.rom, rom);
	for (unsigned i = 0; i < _requests; ++i) exists |= _pending[i].banner == banner && !strcmp(_pending[i].rom, rom);
	for (unsigned i = 0; i < _results; ++i) exists |= _complete[i].banner == banner && !strcmp(_complete[i].rom, rom);
	if (!exists && _requests < REQUEST_COUNT) {
		struct ImageJob* job = &_pending[_requests++];
		memset(job, 0, sizeof(*job));
		strcpy(job->rom, rom);
		job->banner = banner;
		job->direct = direct;
		job->generation = _generation;
		condvarWakeAll(&_changed);
	}
	mutexUnlock(&_lock);
}

void SwitchImagesRequest(const char* rom, bool banner) { _request(rom, banner, false); }
void SwitchImagesRequestPath(const char* path) { _request(path, false, true); }

bool SwitchImagesPoll(char* rom, size_t size, bool* banner, struct mImage** image) {
	if (!_initialized || !rom || !size || !banner || !image) return false;
	mutexLock(&_lock);
	if (!_results) { mutexUnlock(&_lock); return false; }
	struct ImageJob job = _complete[0];
	memmove(_complete, _complete + 1, --_results * sizeof(*_complete));
	condvarWakeAll(&_changed);
	mutexUnlock(&_lock);
	snprintf(rom, size, "%s", job.rom);
	*banner = job.banner;
	*image = job.image;
	return true;
}
