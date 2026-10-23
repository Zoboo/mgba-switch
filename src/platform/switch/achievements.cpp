/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "achievements.h"
#include "ra-http.h"
#include <mgba/core/core.h>
#include <rc_client.h>
#include <rc_hash.h>
#include <rc_consoles.h>
#include <rc_error.h>
#include <switch.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <sys/stat.h>

namespace {
rc_client_t* client;
mCore* core;
mCoreCallbacks callbacks;
SwitchAchievementsState state;
std::string credentialPath;
std::deque<SwitchAchievementNotice> notices;
bool notificationsEnabled = true;
char hash[33], hashResult[33];
bool paused = true, rememberLogin, shuttingDown;
uint32_t lastFrame;
rc_client_async_handle_t* loginRequest;
rc_client_async_handle_t* gameRequest;
Thread hashThread;
bool hashThreadOpen;
std::atomic<bool> hashDone(false);
std::vector<uint8_t> rom;
uint32_t consoleId;

void status(const char* text) { snprintf(state.status, sizeof(state.status), "%s", text ? text : "Request failed"); }
void notify(const char* text) {
	SwitchAchievementNotice notice = {};
	snprintf(notice.title, sizeof(notice.title), "%s", text ? text : "RetroAchievements error");
	notices.push_back(notice);
}
void removeCredentials() { if (!credentialPath.empty()) remove(credentialPath.c_str()); }
void saveCredentials() {
	const rc_client_user_t* user = rc_client_get_user_info(client);
	if (!rememberLogin || !user) { removeCredentials(); return; }
	FILE* file = fopen(credentialPath.c_str(), "wb");
	if (!file) { notify("Signed in; could not save login token"); return; }
	chmod(credentialPath.c_str(), 0600);
	bool ok = fprintf(file, "%s\n%s\n", user->username, user->token) > 0;
	if (fclose(file)) ok = false;
	if (!ok) { removeCredentials(); notify("Signed in; could not save login token"); }
}

uint8_t* memoryBlock(uint32_t base, size_t* size) {
	const mCoreMemoryBlock* blocks;
	size_t count = core->listMemoryBlocks(core, &blocks);
	for (size_t i = 0; i < count; ++i) {
		if (blocks[i].start == base && (blocks[i].flags & mCORE_MEMORY_MAPPED))
			return static_cast<uint8_t*>(core->getMemoryBlock(core, blocks[i].id, size));
	}
	return nullptr;
}

uint32_t readMemory(uint32_t address, uint8_t* buffer, uint32_t count, rc_client_t*) {
	if (!core) return 0;
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t a = address + i;
		if (a < address) return i;
		if (core->platform(core) == mPLATFORM_GBA) {
			uint32_t native;
			if (a < 0x8000) native = 0x03000000 + a;
			else if (a < 0x48000) native = 0x02000000 + a - 0x8000;
			else if (a < 0x58000) native = 0x0E000000 + a - 0x48000;
			else return i;
			size_t size = 0;
			const mCoreMemoryBlock* info = mCoreGetMemoryBlockInfo(core, native);
			uint8_t* block = info ? static_cast<uint8_t*>(core->getMemoryBlock(core, info->id, &size)) : nullptr;
			if (!block || !info || native - info->start >= size) return i;
			buffer[i] = block[native - info->start];
		} else {
			uint32_t base = 0, offset = 0;
			if (a >= 0xA000 && a < 0xC000) { base = 0xA000; offset = a - 0xA000; }
			else if (a >= 0xC000 && a < 0xE000) { base = 0xC000; offset = a - 0xC000; }
			else if (a >= 0xE000 && a < 0xFE00) { base = 0xC000; offset = a - 0xE000; }
			else if (a >= 0x10000 && a < 0x16000) { base = 0xC000; offset = a - 0x10000 + 0x2000; }
			else if (a >= 0x16000 && a < 0x34000) { base = 0xA000; offset = a - 0x16000 + 0x2000; }
			else if (a < 0x10000) { buffer[i] = core->rawRead8(core, a, -1); continue; }
			else return i;
			size_t size = 0;
			uint8_t* block = memoryBlock(base, &size);
			if (!block || offset >= size) return i;
			buffer[i] = block[offset];
		}
	}
	return count;
}
void serverCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* data, rc_client_t*) {
	SwitchRAHttpSend(request, callback, data, "mGBA-Switch/0.11 (Softcore)");
}
void eventHandler(const rc_client_event_t* event, rc_client_t*) {
	switch (event->type) {
	case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED: {
		if (!notificationsEnabled) break;
		SwitchAchievementNotice notice = {};
		notice.unlocked = true;
		notice.points = event->achievement->points;
		snprintf(notice.title, sizeof(notice.title), "%s", event->achievement->title);
		rc_client_achievement_get_image_url(event->achievement, RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED,
		    notice.badge, sizeof(notice.badge));
		notices.push_back(notice);
		break;
	}
	case RC_CLIENT_EVENT_GAME_COMPLETED: notify("All achievements unlocked!"); break;
	case RC_CLIENT_EVENT_SERVER_ERROR: notify(event->server_error->error_message); break;
	case RC_CLIENT_EVENT_DISCONNECTED: notify("Achievements offline; unlock submissions queued for retry"); break;
	case RC_CLIENT_EVENT_RECONNECTED: notify("Achievements reconnected"); break;
	}
}
void gameLoaded(int result, const char* error, rc_client_t*, void*) {
	gameRequest = nullptr; state.busy = false;
	if (result == RC_OK) {
		const rc_client_game_t* game = rc_client_get_game_info(client);
		status(game ? "Softcore achievements active" : "No achievements found for this game");
	} else status(error);
	notify(state.status);
}
void loadGame() {
	if (!core || !hash[0] || !rc_client_get_user_info(client) || gameRequest) return;
	state.busy = true; status("Loading game achievements...");
	gameRequest = rc_client_begin_load_game(client, hash, gameLoaded, nullptr);
}
void beginHash();
void loggedIn(int result, const char* error, rc_client_t*, void*) {
	loginRequest = nullptr; state.busy = false;
	if (result != RC_OK) {
		status(error);
		if (result == RC_INVALID_CREDENTIALS || result == RC_EXPIRED_TOKEN || result == RC_ACCESS_DENIED)
			removeCredentials();
		notify(state.status);
		return;
	}
	status("Signed in — Softcore"); saveCredentials(); notify(state.status);
	if (core && !hash[0] && !hashThreadOpen) beginHash(); else loadGame();
}
void hashROM(void*) {
	hashResult[0] = 0;
	rc_hash_generate_from_buffer(hashResult, consoleId, rom.data(), rom.size());
	hashDone.store(true, std::memory_order_release);
}
void beginHash() {
	size_t size = 0;
	uint32_t start = core->platform(core) == mPLATFORM_GBA ? 0x08000000 : 0;
	const uint8_t* data = static_cast<const uint8_t*>(mCoreGetMemoryBlock(core, start, &size));
	size = core->romSize(core) < size ? core->romSize(core) : size;
	if (!data || !size) { status("ROM identification unavailable"); return; }
	consoleId = core->platform(core) == mPLATFORM_GBA ? RC_CONSOLE_GAMEBOY_ADVANCE :
		(size > 0x143 && (data[0x143] & 0x80) ? RC_CONSOLE_GAMEBOY_COLOR : RC_CONSOLE_GAMEBOY);
	try { rom.assign(data, data + size); } catch (...) { status("Not enough memory to identify game"); return; }
	hashDone.store(false); state.busy = true; status("Identifying game...");
	if (R_FAILED(threadCreate(&hashThread, hashROM, nullptr, nullptr, 128 * 1024, 0x3B, -2))) {
		rom.clear(); state.busy = false; status("Game identification worker unavailable"); return;
	}
	if (R_FAILED(threadStart(&hashThread))) {
		threadClose(&hashThread); rom.clear(); state.busy = false; status("Game identification worker unavailable"); return;
	}
	hashThreadOpen = true;
}
void frameEnded(void*) {
	if (!client || !core || paused) return;
	if (!rc_client_get_user_info(client)) return;
	SwitchAchievementsIdle();
	uint32_t frame = core->frameCounter(core);
	if (frame != lastFrame + 1) rc_client_reset(client);
	lastFrame = frame;
	rc_client_do_frame(client);
}
}

bool SwitchAchievementsInit(const char* directory) {
	if (client) return true;
	credentialPath = std::string(directory ? directory : ".") + "/retroachievements.token";
	if (!SwitchRAHttpInit()) { status("Network worker unavailable"); return false; }
	client = rc_client_create(readMemory, serverCall);
	if (!client) { SwitchRAHttpDeinit(); status("Achievements unavailable"); return false; }
	shuttingDown = false;
	rc_client_set_hardcore_enabled(client, 0);
	rc_client_set_event_handler(client, eventHandler);
	state.available = true; status("Not signed in — Softcore");
	FILE* file = fopen(credentialPath.c_str(), "rb");
	if (file) {
		char user[128] = {}, token[256] = {};
		bool valid = fgets(user, sizeof(user), file) && fgets(token, sizeof(token), file);
		fclose(file); user[strcspn(user, "\r\n")] = 0; token[strcspn(token, "\r\n")] = 0;
		if (valid && user[0] && token[0]) {
			rememberLogin = true; state.busy = true; status("Signing in...");
			loginRequest = rc_client_begin_login_with_token(client, user, token, loggedIn, nullptr);
		}
		memset(token, 0, sizeof(token));
	}
	return true;
}
void SwitchAchievementsIdle(void) {
	if (!client || shuttingDown) return;
	SwitchRAHttpPump();
	if (hashThreadOpen && hashDone.load(std::memory_order_acquire)) {
		threadWaitForExit(&hashThread); threadClose(&hashThread); hashThreadOpen = false;
		std::vector<uint8_t>().swap(rom);
		memcpy(hash, hashResult, sizeof(hash));
		state.busy = loginRequest != nullptr;
		if (hash[0]) loadGame(); else { state.busy = false; status("Game identification unavailable"); }
	}
	rc_client_idle(client);
}
void SwitchAchievementsAttach(mCore* value) {
	SwitchAchievementsDetach();
	if (!client || !value) return;
	core = value; lastFrame = core->frameCounter(core); paused = false;
	callbacks = {}; callbacks.videoFrameEnded = frameEnded; core->addCoreCallbacks(core, &callbacks);
	if (rc_client_get_user_info(client)) beginHash();
}
void SwitchAchievementsDetach(void) {
	if (core) core->removeCoreCallbacks(core, &callbacks);
	core = nullptr; paused = true;
	if (hashThreadOpen) { threadWaitForExit(&hashThread); threadClose(&hashThread); hashThreadOpen = false; }
	rom.clear(); hash[0] = 0;
	if (client) {
		if (gameRequest) { rc_client_abort_async(client, gameRequest); gameRequest = nullptr; }
		rc_client_unload_game(client);
	}
	state.game[0] = 0; state.achievements = state.unlocked = state.points = 0;
	state.busy = loginRequest != nullptr;
}
void SwitchAchievementsSetPaused(bool value) { paused = value; }
void SwitchAchievementsResetProgress(void) { if (client) rc_client_reset(client); }
void SwitchAchievementsLogin(const char* user, const char* password, bool remember) {
	if (!client || !user || !password || !user[0] || !password[0]) return;
	SwitchAchievementsLogout(); rememberLogin = remember;
	state.busy = true; status("Signing in...");
	loginRequest = rc_client_begin_login_with_password(client, user, password, loggedIn, nullptr);
}
void SwitchAchievementsLogout(void) {
	if (!client) return;
	if (loginRequest) { rc_client_abort_async(client, loginRequest); loginRequest = nullptr; }
	if (gameRequest) { rc_client_abort_async(client, gameRequest); gameRequest = nullptr; }
	rc_client_logout(client); removeCredentials(); state.busy = false;
	state.loggedIn = false; state.username[0] = 0; status("Not signed in — Softcore");
}
void SwitchAchievementsGetState(SwitchAchievementsState* out) {
	if (!out) return;
	if (client) {
		const rc_client_user_t* user = rc_client_get_user_info(client);
		state.loggedIn = user != nullptr;
		snprintf(state.username, sizeof(state.username), "%s", user ? user->display_name : "");
		const rc_client_game_t* game = rc_client_get_game_info(client);
		snprintf(state.game, sizeof(state.game), "%s", game ? game->title : "");
		rc_client_user_game_summary_t summary = {};
		if (game) rc_client_get_user_game_summary(client, &summary);
		state.achievements = summary.num_core_achievements;
		state.unlocked = summary.num_unlocked_achievements; state.points = summary.points_unlocked;
	}
	*out = state;
}
bool SwitchAchievementsGetNotice(SwitchAchievementNotice* out) {
	if (notices.empty() || !out) return false;
	*out = notices.front();
	notices.pop_front();
	return true;
}
bool SwitchAchievementsPeekNotice(unsigned index, SwitchAchievementNotice* out) {
	if (!out || index >= notices.size()) return false;
	*out = notices[index];
	return true;
}
void SwitchAchievementsSetNotificationsEnabled(bool enabled) {
	notificationsEnabled = enabled;
	if (!enabled) {
		for (auto it = notices.begin(); it != notices.end();) {
			if (it->unlocked) it = notices.erase(it);
			else ++it;
		}
	}
}
unsigned SwitchAchievementsGetPage(unsigned first, SwitchAchievement* entries, unsigned capacity, unsigned filter) {
	if (!client || !entries) return 0;
	rc_client_achievement_list_t* list = rc_client_create_achievement_list(client,
	    RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE, RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
	if (!list) return 0;
	unsigned count = 0;
	for (unsigned b = 0; b < list->num_buckets; ++b) {
		const rc_client_achievement_bucket_t& bucket = list->buckets[b];
		for (unsigned i = 0; i < bucket.num_achievements; ++i) {
			const rc_client_achievement_t* achievement = bucket.achievements[i];
			bool unlocked = achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			if ((filter == 1 && unlocked) || (filter == 2 && !unlocked)) continue;
			unsigned index = count++;
			if (index < first || index - first >= capacity) continue;
			SwitchAchievement& entry = entries[index - first];
			entry = {};
			snprintf(entry.title, sizeof(entry.title), "%s", achievement->title ? achievement->title : "");
			snprintf(entry.description, sizeof(entry.description), "%s", achievement->description ? achievement->description : "");
			entry.points = achievement->points;
			entry.unlocked = achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED;
			rc_client_achievement_get_image_url(achievement, entry.unlocked ? RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED :
			    RC_CLIENT_ACHIEVEMENT_STATE_ACTIVE, entry.badge, sizeof(entry.badge));
			entry.unsupported = achievement->state == RC_CLIENT_ACHIEVEMENT_STATE_DISABLED;
			entry.percent = entry.unlocked ? 100.0f : achievement->measured_percent;
			if (!(entry.percent >= 0)) entry.percent = 0;
			if (entry.percent > 100) entry.percent = 100;
			snprintf(entry.progress, sizeof(entry.progress), "%s", entry.unlocked ? "Unlocked" :
			    entry.unsupported ? "Unsupported" : achievement->measured_progress[0] ? achievement->measured_progress : "Locked — no measured progress");
		}
	}
	rc_client_destroy_achievement_list(list);
	return count;
}
void SwitchAchievementsDeinit(void) {
	if (!client) return;
	shuttingDown = true; SwitchAchievementsDetach();
	if (loginRequest) { rc_client_abort_async(client, loginRequest); loginRequest = nullptr; }
	SwitchRAHttpDeinit(); rc_client_destroy(client); client = nullptr; state = {};
	notices.clear();
}
