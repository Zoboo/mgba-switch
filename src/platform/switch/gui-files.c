/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <mgba-util/gui/file-select.h>

#include <mgba-util/gui/font.h>
#include <mgba-util/gui/menu.h>
#include <mgba-util/string.h>
#include <mgba-util/vfs.h>

#include <stdlib.h>
#include <switch.h>
#include "gui-ui.h"

struct ScanJob {
	Thread thread;
	struct ScanJob* next;
	struct GUIMenuItemList files;
	char path[PATH_MAX], preselect[PATH_MAX];
	bool (*filterName)(const char*);
	bool (*filterContents)(struct VFile*);
	size_t index, scanned;
	unsigned done, cancel;
	bool success, failed;
};
static struct ScanJob* _jobs;
static bool _scanCancelled;
static bool _libraryFiltersSet;
static bool (*_libraryFilterName)(const char*);
static bool (*_libraryFilterContents)(struct VFile*);

static void _cleanFiles(struct GUIMenuItemList* currentFiles) {
	size_t size = GUIMenuItemListSize(currentFiles);
	size_t i;
	for (i = 1; i < size; ++i) {
		free((char*) GUIMenuItemListGetPointer(currentFiles, i)->title);
	}
	GUIMenuItemListClear(currentFiles);
}

static void _upDirectory(char* currentPath) {
	char* end = strrchr(currentPath, '/');
	if (!end) {
		currentPath[0] = '\0';
		return;
	}
	if (!end[1]) {
		// Trailing slash
		end[0] = '\0';
		return _upDirectory(currentPath);
	}
	end[1] = '\0';
}

static int _strpcmp(const void* a, const void* b) {
	return strcasecmp(((const struct GUIMenuItem*) a)->title, ((const struct GUIMenuItem*) b)->title);
}

static struct GUIMenuItem* _appendFile(struct ScanJob* job) {
	struct GUIMenuItemList* files = &job->files;
	if (files->size == files->capacity) {
		size_t capacity = files->capacity ? files->capacity * 2 : 4;
		void* items = realloc(files->vector, capacity * sizeof(*files->vector));
		if (!items) { job->failed = true; return NULL; }
		files->vector = items;
		files->capacity = capacity;
	}
	return &files->vector[files->size++];
}

static bool _scanDirectory(struct ScanJob* job) {
	const char* currentPath = job->path;
	struct GUIMenuItemList* currentFiles = &job->files;
	bool (*filterName)(const char*) = job->filterName;
	bool (*filterContents)(struct VFile*) = job->filterContents;
	const char* preselect = job->preselect[0] ? job->preselect : NULL;
	_cleanFiles(currentFiles);

	struct VDir* dir = VDirOpen(currentPath);
	if (!dir) {
		return false;
	}
	struct GUIMenuItem* slot = _appendFile(job);
	if (!slot) { dir->close(dir); return false; }
	*slot = (struct GUIMenuItem) { .title = "(Up)" };
	size_t i = 0;
	struct VDirEntry* de;
	while ((de = dir->listNext(dir))) {
		++i;
		__atomic_store_n(&job->scanned, i, __ATOMIC_RELAXED);
		if (__atomic_load_n(&job->cancel, __ATOMIC_RELAXED)) { dir->close(dir); return false; }
		const char* name = de->name(de);
		if (name[0] == '.') {
			continue;
		}
		if (de->type(de) == VFS_DIRECTORY) {
			size_t len = strlen(name) + 2;
			char* n2 = malloc(len);
			if (!n2) { job->failed = true; dir->close(dir); return false; }
			snprintf(n2, len, "%s/", name);
			name = n2;
		} else if (filterName && !filterName(name)) {
			continue;
		} else {
			name = strdup(name);
		}
		if (!name) { job->failed = true; dir->close(dir); return false; }
		slot = _appendFile(job);
		if (!slot) { free((char*) name); dir->close(dir); return false; }
		*slot = (struct GUIMenuItem) { .title = name, .data = GUI_V_U(de->type(de)) };
	}
	if (GUIMenuItemListSize(currentFiles) > 2) {
		qsort(GUIMenuItemListGetPointer(currentFiles, 1), GUIMenuItemListSize(currentFiles) - 1, sizeof(struct GUIMenuItem), _strpcmp);
	}
	if (preselect || filterContents) {
		i = 0;
		size_t item = 0;
		while (item < GUIMenuItemListSize(currentFiles)) {
			++i;
			__atomic_store_n(&job->scanned, i, __ATOMIC_RELAXED);
			if (__atomic_load_n(&job->cancel, __ATOMIC_RELAXED)) { dir->close(dir); return false; }
			struct GUIMenuItem* testItem = GUIMenuItemListGetPointer(currentFiles, item);
			if (!GUIVariantCompareUInt(testItem->data, VFS_FILE)) {
				++item;
				continue;
			}
			bool failed = false;
			if (filterContents) {
				struct VFile* vf = dir->openFile(dir, testItem->title, O_RDONLY);
				if (!vf) {
					failed = true;
				} else {
					if (!filterContents(vf)) {
						failed = true;
					}
					vf->close(vf);
				}
			}

			if (failed) {
				free((char*) testItem->title);
				GUIMenuItemListShift(currentFiles, item, 1);
			} else {
				if (preselect && strncmp(testItem->title, preselect, PATH_MAX) == 0) {
					job->index = item;
				}
				++item;
			}
		}
	}
	dir->close(dir);

	return true;
}

static void _scanWorker(void* context) {
	struct ScanJob* job = context;
	job->success = _scanDirectory(job);
	__atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
}

static void _reapJobs(bool shutdown) {
	struct ScanJob** link = &_jobs;
	while (*link) {
		struct ScanJob* job = *link;
		if (shutdown) __atomic_store_n(&job->cancel, 1, __ATOMIC_RELAXED);
		if (!shutdown && !__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) { link = &job->next; continue; }
		threadWaitForExit(&job->thread);
		threadClose(&job->thread);
		_cleanFiles(&job->files);
		GUIMenuItemListDeinit(&job->files);
		*link = job->next;
		free(job);
	}
}

void SwitchUIFilesDeinit(void) {
	_reapJobs(true);
}

static bool _scanFrame(struct GUIParams* params, const char* currentPath, struct ScanJob* job) {
	uint32_t input = 0;
	GUIPollInput(params, &input, NULL);
	if (input & ((1 << GUI_INPUT_CANCEL) | (1 << GUI_INPUT_BACK))) {
		if (job) __atomic_store_n(&job->cancel, 1, __ATOMIC_RELAXED);
		_scanCancelled = true;
		return false;
	}
	params->drawStart();
	if (params->guiPrepare) params->guiPrepare();
	unsigned oldSize = SwitchUIFontSizeGet(params->font);
	SwitchUIFontSize(params->font, 26);
	GUIFontPrintf(params->font, 48, 80, GUI_ALIGN_LEFT, 0xFFFFFFFF, job ? "Loading files" : "Waiting for previous scan");
	SwitchUIFontSize(params->font, 20);
	SwitchUIPrint(params->font, 48, 124, params->width - 96, 0xFFE4DBD4, currentPath);
	GUIFontPrintf(params->font, 48, 164, GUI_ALIGN_LEFT, 0xFFE4DBD4, "%zu items scanned", job ? __atomic_load_n(&job->scanned, __ATOMIC_RELAXED) : 0);
	SwitchUIHint(params->font, 48, params->height - 48, "B", "Cancel");
	SwitchUIFontSize(params->font, oldSize);
	if (params->guiFinish) params->guiFinish();
	params->drawEnd();
	return true;
}

static bool _refreshDirectory(struct GUIParams* params, const char* currentPath, struct GUIMenuItemList* files, bool (*filterName)(const char*), bool (*filterContents)(struct VFile*), const char* preselect) {
	_scanCancelled = false;
	_reapJobs(false);
	while (_jobs && _jobs->next) {
		if (!_scanFrame(params, currentPath, NULL)) return false;
		_reapJobs(false);
	}
	struct ScanJob* job = calloc(1, sizeof(*job));
	if (!job) { _scanCancelled = true; return false; }
	strlcpy(job->path, currentPath, sizeof(job->path));
	if (preselect) strlcpy(job->preselect, preselect, sizeof(job->preselect));
	job->index = params->fileIndex;
	job->filterName = filterName;
	job->filterContents = filterContents;
	GUIMenuItemListInit(&job->files, 0);
	if (!job->files.vector) { free(job); _scanCancelled = true; return false; }
	if (R_FAILED(threadCreate(&job->thread, _scanWorker, job, NULL, 128 * 1024, 0x3B, -2))) {
		GUIMenuItemListDeinit(&job->files); free(job); _scanCancelled = true; return false;
	}
	if (R_FAILED(threadStart(&job->thread))) {
		threadClose(&job->thread); GUIMenuItemListDeinit(&job->files); free(job); _scanCancelled = true; return false;
	}
	job->next = _jobs;
	_jobs = job;
	while (!__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
		if (!_scanFrame(params, currentPath, job)) return false;
	}
	bool success = job->success;
	_scanCancelled = job->failed;
	if (success) {
		_cleanFiles(files);
		GUIMenuItemListDeinit(files);
		*files = job->files;
		GUIMenuItemListInit(&job->files, 0);
		params->fileIndex = job->index;
	}
	_reapJobs(false);
	return success;
}

bool SwitchUIFilesRefreshLibrary(struct GUIParams* params, struct GUIMenu* menu) {
	bool ok = _refreshDirectory(params, params->currentPath, &menu->items, _libraryFilterName, _libraryFilterContents, NULL);
	if (ok) menu->index = params->fileIndex < GUIMenuItemListSize(&menu->items) ? params->fileIndex : 0;
	GUIInvalidateKeys(params);
	return ok;
}

static bool _excludeFiles(const char* name) {
	UNUSED(name);
	return false;
}

bool SwitchUISelectDirectory(struct GUIParams* params, const char* initialPath, char* outPath, size_t outLen) {
	struct GUIParams browser = *params;
	strlcpy(browser.currentPath, "sdmc:/", sizeof(browser.currentPath));
	if (initialPath && !strncmp(initialPath, "sdmc:/", 6)) {
		strlcpy(browser.currentPath, initialPath, sizeof(browser.currentPath));
	} else if (initialPath && initialPath[0] == '/') {
		snprintf(browser.currentPath, sizeof(browser.currentPath), "sdmc:%s", initialPath);
	}
	browser.fileIndex = 0;
	struct GUIMenuItemList folders;
	GUIMenuItemListInit(&folders, 0);
	bool accepted = false, refresh = true;
	size_t selected = 0, first = 0;
	for (;;) {
		if (refresh) {
			if (!_refreshDirectory(&browser, browser.currentPath, &folders, _excludeFiles, NULL, NULL)) {
				if (!_scanCancelled && strcmp(browser.currentPath, "sdmc:/")) {
					strlcpy(browser.currentPath, "sdmc:/", sizeof(browser.currentPath));
					continue;
				}
				if (!_scanCancelled) GUIShowMessageBox(&browser, GUI_MESSAGE_BOX_OK, 0, "Could not read the SD card folders.");
				break;
			}
			selected = first = 0;
			refresh = false;
		}
		uint32_t input = 0;
		GUIPollInput(&browser, &input, NULL);
		unsigned x = browser.cx, y = browser.cy;
		bool tapped = GUIPollCursor(&browser, &x, &y) == GUI_CURSOR_CLICKED;
		if (tapped) { x = browser.cx; y = browser.cy; }
		if (input & (1U << GUI_INPUT_CANCEL)) break;
		bool back = input & (1U << GUI_INPUT_BACK);
		if (tapped && y >= 664 && x < 240) back = true;
		size_t count = GUIMenuItemListSize(&folders) + 1;
		if (selected && (input & (1U << GUI_INPUT_UP))) --selected;
		if (selected + 1 < count && (input & (1U << GUI_INPUT_DOWN))) ++selected;
		bool previous = (input & (1U << GUI_INPUT_LEFT)) || (tapped && y >= 664 && x >= 280 && x < 540);
		bool next = (input & (1U << GUI_INPUT_RIGHT)) || (tapped && y >= 664 && x >= 560 && x < 820);
		if (previous) selected = selected > 8 ? selected - 8 : 0;
		if (next) selected = selected + 8 < count ? selected + 8 : count - 1;
		bool activate = input & (1U << GUI_INPUT_SELECT);
		if (tapped && y >= 664 && x >= 860) activate = true;
		if (tapped && x >= 48 && x < 1232 && y >= 152 && y < 632) {
			size_t row = first + (y - 152) / 60;
			if (row < count) { selected = row; activate = true; }
		}
		if (back || (activate && selected == 1)) {
			if (!strcmp(browser.currentPath, "sdmc:/")) {
				if (back) break;
			} else {
				_upDirectory(browser.currentPath);
				refresh = true;
			}
		} else if (activate && selected == 0) {
			if (strlen(browser.currentPath) < outLen) {
				strlcpy(outPath, browser.currentPath, outLen);
				accepted = true;
			}
			break;
		} else if (activate) {
			const char* name = GUIMenuItemListGetPointer(&folders, selected - 1)->title;
			char path[PATH_MAX];
			size_t len = strlen(browser.currentPath);
			if (snprintf(path, sizeof(path), "%s%s%s", browser.currentPath,
			             len && browser.currentPath[len - 1] == '/' ? "" : "/", name) < (int) sizeof(path)) {
				strlcpy(browser.currentPath, path, sizeof(browser.currentPath));
				refresh = true;
			} else GUIShowMessageBox(&browser, GUI_MESSAGE_BOX_OK, 0, "This folder path is too long.");
		}
		if (refresh) continue;
		if (selected < first) first = selected;
		if (selected >= first + 8) first = selected - 7;
		browser.drawStart();
		if (browser.guiPrepare) browser.guiPrepare();
		SwitchUIDrawRect(0, 0, 1280, 720, 0xFF1F1417);
		unsigned oldSize = SwitchUIFontSizeGet(browser.font);
		SwitchUIFontSize(browser.font, 28);
		SwitchUIPrint(browser.font, 48, 66, 1184, 0xFFFFFFFF, "Choose a directory");
		SwitchUIFontSize(browser.font, 18);
		SwitchUIPrint(browser.font, 48, 112, 1184, 0xFFB8B2AA, browser.currentPath);
		for (size_t row = first; row < count && row < first + 8; ++row) {
			int top = 152 + (row - first) * 60;
			if (row == selected) {
				SwitchUIRoundRect(48, top, 1184, 54, 8, 0xFF332024);
				SwitchUIRoundFrame(48, top, 1184, 54, 8, 0xFFFFA3BD);
			}
			SwitchUIFontSize(browser.font, 22);
			const char* label = row == 0 ? "Use This Directory" :
			    row == 1 ? "..  Parent directory" : GUIMenuItemListGetPointer(&folders, row - 1)->title;
			SwitchUIPrint(browser.font, 68, top + 35, 1120, 0xFFFFFFFF, label);
		}
		SwitchUIHint(browser.font, 48, 699, "B", "Back / Cancel");
		SwitchUIHint(browser.font, 300, 699, "<", "Previous");
		SwitchUIHint(browser.font, 580, 699, ">", "Next");
		SwitchUIHint(browser.font, 880, 699, "A", selected ? "Open folder" : "Use This Directory");
		SwitchUIFontSize(browser.font, oldSize);
		if (browser.guiFinish) browser.guiFinish();
		browser.drawEnd();
	}
	_cleanFiles(&folders);
	GUIMenuItemListDeinit(&folders);
	GUIInvalidateKeys(params);
	return accepted;
}

bool GUISelectFile(struct GUIParams* params, char* outPath, size_t outLen, bool (*filterName)(const char* name), bool (*filterContents)(struct VFile*), const char* preselect) {
	if (!_libraryFiltersSet) {
		_libraryFilterName = filterName;
		_libraryFilterContents = filterContents;
		_libraryFiltersSet = true;
	}
	struct GUIMenu menu = {
		.title = "Select file",
		.subtitle = params->currentPath,
	};
	GUIMenuItemListInit(&menu.items, 0);
	while (true) {
		if (_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, preselect)) {
			break;
		}
		if (_scanCancelled || strncmp(params->currentPath, params->basePath, PATH_MAX) == 0 || !params->currentPath[0]) {
			if (!_scanCancelled) mLOG(GUI_MENU, ERROR, "Failed to load base directory");
			GUIMenuItemListDeinit(&menu.items);
			return false;
		}
		_upDirectory(params->currentPath);
	}
	menu.index = params->fileIndex;

	while (true) {
		struct GUIMenuItem* item;
		enum GUIMenuExitReason reason = GUIShowMenu(params, &menu, &item);
		params->fileIndex = menu.index;
		if (reason == GUI_MENU_EXIT_CANCEL) {
			break;
		}
		if (reason == GUI_MENU_EXIT_ACCEPT) {
			if (params->fileIndex == 0) {
				if (strncmp(params->currentPath, params->basePath, PATH_MAX) == 0) {
					continue;
				}
				_upDirectory(params->currentPath);
				if (!_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, NULL)) {
					break;
				}
			} else {
				size_t len = strlen(params->currentPath);
				const char* sep = PATH_SEP;
				if (!len || params->currentPath[len - 1] == *sep) {
					sep = "";
				}
				snprintf(outPath, outLen, "%s%s%s", params->currentPath, sep, item->title);

				bool selectedFile = GUIVariantCompareUInt(item->data, VFS_FILE);
				struct GUIMenuItemList newFiles;
				GUIMenuItemListInit(&newFiles, 0);
				if (!_refreshDirectory(params, outPath, &newFiles, filterName, filterContents, NULL)) {
					_cleanFiles(&newFiles);
					GUIMenuItemListDeinit(&newFiles);
					_cleanFiles(&menu.items);
					GUIMenuItemListDeinit(&menu.items);
					return selectedFile && !_scanCancelled;
				} else {
					_cleanFiles(&menu.items);
					GUIMenuItemListDeinit(&menu.items);
					menu.items = newFiles;
					strlcpy(params->currentPath, outPath, PATH_MAX);
				}
			}
			params->fileIndex = 0;
			menu.index = 0;
		}
		if (reason == GUI_MENU_EXIT_BACK) {
			if (strncmp(params->currentPath, params->basePath, PATH_MAX) == 0) {
				break;
			}
			_upDirectory(params->currentPath);
			if (!_refreshDirectory(params, params->currentPath, &menu.items, filterName, filterContents, NULL)) {
				break;
			}
			params->fileIndex = 0;
			menu.index = 0;
		}
	}

	_cleanFiles(&menu.items);
	GUIMenuItemListDeinit(&menu.items);
	return false;
}
