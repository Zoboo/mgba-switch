/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "artwork.h"
#include "artwork-jpeg.h"
#include "plaza.h"
#include <mgba-util/image.h>
#include <switch.h>
#include <curl/curl.h>
#include "../../third-party/discord-rpc/include/rapidjson/document.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
Mutex lock;
CondVar changed;
bool busy, suspended;
Thread worker;
bool initialized, running, covers, banners, testKey;
bool pendingAttempt;
bool pendingExact;
std::string plazaError;
bool bulk, bulkScan, bulkPartial;
unsigned bulkDone, bulkTotal;
std::string bulkDirectory;
std::vector<std::string> bulkRoms;
void cancelBulkLocked() { bulk = bulkScan = false; bulkRoms.clear(); }
unsigned generation, activeRevision;
struct Option { SwitchArtworkOption view; std::string url; bool cached; };
SwitchArtworkSource choiceSource = SWITCH_ARTWORK_AUTO;
std::vector<Option> options;
bool needsChoice, searching, finished, searchPending, choosePending;
bool searchBest;
bool resetPending, resetBanner;
bool importPending, importBanner, applying;
std::string importRom, importPath;
bool resetSucceeded;
unsigned resetSerial, resetCompleted;
std::string resetRom;
bool choiceCovers, choiceBanners;
unsigned percent, imageRevision;
bool unavailable;
std::vector<std::string> discarded;
void discardOptions() { for (const auto& option : options) discarded.emplace_back(option.view.preview); options.clear(); }
size_t chosen;
std::string choiceRom;
std::string searchTitle;
std::string directory, key, pending, requested, status = "Artwork downloads disabled";

std::string base(const std::string& path) {
	size_t slash = path.find_last_of("/\\");
	std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
	size_t dot = name.find_last_of('.');
	return name.substr(0, dot);
}
std::string normalized(const std::string& title) {
	std::string out;
	for (size_t i = 0; i < title.size(); ++i) {
		unsigned char c = title[i];
		if (c == '(' || c == '[') break;
		if (c == 0xC3 && i + 1 < title.size()) {
			unsigned char accent = title[++i];
			if (accent >= 0x80 && accent <= 0xBF) {
				static const char latin[] = "aaaaaaaceeeeiiiidnooooo?ouuuuytsaaaaaaaceeeeiiiidnooooo?ouuuuyty";
				char replacement = latin[accent - 0x80];
				if (replacement != '?') out += replacement;
			}
		} else if (c >= 'A' && c <= 'Z') out += char(c + 'a' - 'A');
		else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += char(c);
	}
	return out;
}
std::string cleanTitle(std::string title) {
	size_t suffix = title.find_first_of("([");
	if (suffix != std::string::npos) title.resize(suffix);
	for (char& c : title) if (c == '_' || c == '-') c = ' ';
	std::string lower = title;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
	for (size_t i = 0; i < lower.size(); ++i) {
		if (i && !std::isspace(static_cast<unsigned char>(lower[i - 1]))) continue;
		size_t n = i;
		if (!lower.compare(i, 7, "version")) n += 7;
		else if (!lower.compare(i, 3, "ver")) n += 3;
		else if (!lower.compare(i, 3, "rev")) n += 3;
		else if (lower[i] == 'v') ++n;
		else continue;
		while (n < lower.size() && (lower[n] == ' ' || lower[n] == '.')) ++n;
		if (n < lower.size() && std::isdigit(static_cast<unsigned char>(lower[n]))) { title.resize(i); break; }
	}
	while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) title.pop_back();
	return title;
}
std::string withoutNumbers(const std::string& title) {
	std::string result;
	for (unsigned char c : title) {
		if (std::isdigit(c) || c == '.') continue;
		if (std::isspace(c) && (result.empty() || result.back() == ' ')) continue;
		result += c;
	}
	while (!result.empty() && result.back() == ' ') result.pop_back();
	return result;
}
unsigned matchScore(const std::string& name, const std::string& title) {
	std::string norm = normalized(name), needle = normalized(title);
	unsigned score = norm == needle ? 1000 : norm.find(needle) != std::string::npos ? 500 : 0;
	for (size_t i = 0; i + 3 <= needle.size(); ++i) if (norm.find(needle.substr(i, 3)) != std::string::npos) ++score;
	return score;
}
std::vector<const rapidjson::Value*> rankedArt(const rapidjson::Document& document) {
	std::vector<const rapidjson::Value*> result;
	for (const auto& item : document["data"].GetArray()) if (item.IsObject()) result.push_back(&item);
	std::stable_sort(result.begin(), result.end(), [](const rapidjson::Value* a, const rapidjson::Value* b) {
		double left = a->HasMember("score") && (*a)["score"].IsNumber() ? (*a)["score"].GetDouble() : 0;
		double right = b->HasMember("score") && (*b)["score"].IsNumber() ? (*b)["score"].GetDouble() : 0;
		return left > right;
	});
	return result;
}
std::string cachePath(const std::string& rom, bool banner) {
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned char c : rom) { hash ^= c; hash *= UINT64_C(1099511628211); }
	char name[64];
	snprintf(name, sizeof(name), "/%016llx.%s.png", (unsigned long long) hash, banner ? "banner" : "cover");
	return directory + name;
}
bool exists(const std::string& path) {
	struct stat info;
	return !stat(path.c_str(), &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 8 * 1024 * 1024;
}
std::string selectionPath(const std::string& rom, bool banner) {
	return cachePath(rom, banner) + ".selected.png";
}
bool replaceSelection(const std::string& temporary, const std::string& target) {
	std::string backup = target + ".bak";
	if (!rename(temporary.c_str(), target.c_str())) { remove(backup.c_str()); return true; }
	if (!exists(target)) return false;
	remove(backup.c_str());
	if (rename(target.c_str(), backup.c_str())) return false;
	if (rename(temporary.c_str(), target.c_str())) { rename(backup.c_str(), target.c_str()); return false; }
	remove(backup.c_str());
	return true;
}
void message(const char* value) {
	mutexLock(&lock); if (generation == activeRevision) status = value; mutexUnlock(&lock);
}
struct Transfer { std::string data; size_t limit; unsigned revision; };
size_t receive(char* ptr, size_t size, size_t count, void* opaque) {
	Transfer* transfer = static_cast<Transfer*>(opaque);
	if (size && count > transfer->limit / size) return 0;
	size_t bytes = size * count;
	if (bytes > transfer->limit - transfer->data.size()) return 0;
	transfer->data.append(ptr, bytes);
	return bytes;
}
int progress(void* opaque, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
	Transfer* transfer = static_cast<Transfer*>(opaque);
	mutexLock(&lock);
	bool cancelled = !running || transfer->revision != generation;
	if (!cancelled) percent = total > 0 ? unsigned(now * 100 / total) : 0;
	mutexUnlock(&lock);
	return cancelled;
}
bool fetch(const std::string& url, const std::string& token, unsigned revision, size_t limit, std::string& result) {
	mutexLock(&lock); bool current = running && revision == generation; mutexUnlock(&lock);
	if (!current) return false;
	CURL* curl = curl_easy_init();
	if (!curl) return false;
	Transfer transfer = { {}, limit, revision };
	curl_slist* headers = nullptr;
	if (!token.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "mGBA-Switch");
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
	CURLcode code = curl_easy_perform(curl);
	long response = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (code != CURLE_OK || response != 200) return false;
	result.swap(transfer.data);
	return true;
}
bool json(const std::string& path, const std::string& token, unsigned revision, rapidjson::Document& document) {
	std::string body;
	if (!fetch("https://www.steamgriddb.com/api/v2/" + path, token, revision, 1024 * 1024, body)) return false;
	document.Parse(body.c_str());
	return !document.HasParseError() && document.IsObject() && document.HasMember("data") && document["data"].IsArray();
}
bool validImage(const std::string& bytes) {
	if (bytes.size() < 24 || memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8)) return false;
	auto dimension = [&bytes](size_t offset) { const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data()) + offset; return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
	uint32_t width = dimension(16), height = dimension(20);
	return width && height && width <= 4096 && height <= 4096 && uint64_t(width) * height <= 8 * 1024 * 1024;
}
bool download(const std::string& rom, bool banner, unsigned id, const std::string& token, unsigned revision) {
	std::string target = cachePath(rom, banner);
	if (exists(target)) return true;
	rapidjson::Document document;
	std::string route = banner ? "heroes/game/" : "grids/game/";
	route += std::to_string(id);
	route += banner ? "?mimes=image/png&types=static" : "?dimensions=600x900&mimes=image/png&types=static";
	if (!json(route, token, revision, document)) return false;
	if (document["data"].Empty()) unavailable = true;
	for (const auto* entry : rankedArt(document)) {
		const auto& item = *entry;
		if (!item.IsObject() || !item.HasMember("url") || !item["url"].IsString()) continue;
		std::string url = item["url"].GetString();
		if (url.find("https://cdn2.steamgriddb.com/") != 0 && url.find("https://cdn.steamgriddb.com/") != 0) continue;
		std::string bytes;
		if (!fetch(url, "", revision, 8 * 1024 * 1024, bytes)) continue;
		if (!validImage(bytes)) continue;
		auto dimension = [&bytes](size_t offset) { const unsigned char* p = (const unsigned char*) bytes.data() + offset; return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; };
		if (!dimension(16) || !dimension(20) || dimension(16) > 4096 || dimension(20) > 4096 || uint64_t(dimension(16)) * dimension(20) > 8 * 1024 * 1024) continue;
		std::string temporary = target + ".tmp";
		FILE* file = fopen(temporary.c_str(), "wb");
		if (!file) return false;
		bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
		if (fclose(file)) ok = false;
		mutexLock(&lock);
		bool current = running && revision == generation;
		mutexUnlock(&lock);
		ok = ok && current && !rename(temporary.c_str(), target.c_str());
		if (ok) { mutexLock(&lock); ++imageRevision; mutexUnlock(&lock); }
		remove(temporary.c_str());
		return ok;
	}
	return false;
}
struct Job {
	unsigned revision;
	bool bulkItem;
	~Job() {
		mutexLock(&lock);
		busy = false;
		if (!choosePending && !importPending && !pendingAttempt) applying = false;
		if (bulkItem && bulk && revision == generation) {
			++bulkDone;
			if (bulkDone == bulkTotal) {
				cancelBulkLocked(); finished = true;
				status = bulkPartial ? "Partial scan complete; some folders were skipped. Try a smaller library folder." :
				    "Missing artwork scan complete; unavailable matches were skipped";
			}
		}
		condvarWakeAll(&changed); mutexUnlock(&lock);
	}
};
bool currentJob(unsigned revision) {
	mutexLock(&lock); bool result = running && revision == generation; mutexUnlock(&lock); return result;
}
bool plazaArtwork(const std::string& rom, const std::string& title, unsigned revision) {
	char existing[4096]; if (SwitchArtworkPath(rom.c_str(), false, existing, sizeof(existing))) return true;
	std::vector<unsigned char> image; plazaError.clear();
	if (!SwitchPlazaArtwork(title.c_str(), image, [revision]() { return currentJob(revision); }, plazaError)) return false;
	std::string bytes(reinterpret_cast<const char*>(image.data()), image.size());
	std::string target = cachePath(rom, false), temporary = target + ".tmp"; bool ok = false;
	if (validImage(bytes)) { FILE* f = fopen(temporary.c_str(), "wb"); ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(); if (f && fclose(f)) ok = false; }
	else if (image.size() > 2 && image[0] == 0xFF && image[1] == 0xD8) ok = SwitchArtworkConvertJPEG(image.data(), image.size(), temporary.c_str());
	struct mImage* decoded = ok ? mImageLoad(temporary.c_str()) : nullptr; ok = decoded != nullptr; if (decoded) mImageDestroy(decoded);
	ok = ok && currentJob(revision) && !rename(temporary.c_str(), target.c_str()); remove(temporary.c_str());
	if (ok) { mutexLock(&lock); ++imageRevision; mutexUnlock(&lock); } return ok;
}
bool scanRoms(const std::string& path, unsigned depth, unsigned revision, unsigned& visited, std::vector<std::string>& roms) {
	if (depth > 16 || visited >= 100000 || !currentJob(revision)) return false;
	DIR* dir = opendir(path.c_str());
	if (!dir) return false;
	bool complete = true;
	while (struct dirent* entry = readdir(dir)) {
		if (++visited > 100000 || !currentJob(revision)) { complete = false; break; }
		if (entry->d_name[0] == '.') continue;
		std::string child = path + (path.back() == '/' ? "" : "/") + entry->d_name;
		if (child.size() >= 4096) { complete = false; continue; }
		struct stat info;
		if (lstat(child.c_str(), &info)) { complete = false; continue; }
		if (S_ISDIR(info.st_mode)) { if (!scanRoms(child, depth + 1, revision, visited, roms)) complete = false; }
		else if (S_ISREG(info.st_mode)) {
			size_t dot = child.find_last_of('.');
			if (dot == std::string::npos) continue;
			std::string ext = child.substr(dot);
			std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
			if (ext != ".gba" && ext != ".gb" && ext != ".gbc" && ext != ".zip" && ext != ".7z" &&
			    ext != ".agb" && ext != ".bin" && ext != ".rom" && ext != ".mb" && ext != ".dmg" && ext != ".sgb" && ext != ".gbx") continue;
			char image[4096];
			if (!SwitchArtworkPath(child.c_str(), false, image, sizeof(image)) || !SwitchArtworkPath(child.c_str(), true, image, sizeof(image))) roms.push_back(child);
		}
	}
	closedir(dir);
	return complete;
}
void alternatives(const std::string& rom, const std::string& title, bool cover, bool banner, unsigned revision) {
	mutexLock(&lock);
	if (revision == generation) {
		choiceRom = rom; choiceCovers = cover; choiceBanners = banner;
		choiceSource = SWITCH_ARTWORK_AUTO; searchBest = false;
		searchTitle = title; searchPending = searching = needsChoice = true;
		finished = false; status = "Searching closest artwork matches...";
		condvarWakeAll(&changed);
	}
	mutexUnlock(&lock);
}
void chooseBestLocked(bool best) {
	if (!best || options.empty()) return;
	chosen = 0; applying = choosePending = true;
	searchPending = searching = needsChoice = finished = false;
	++generation;
	condvarWakeAll(&changed);
}
void work(void*) {
	while (true) {
		mutexLock(&lock);
		while (running && !resetPending && !importPending && discarded.empty() && (suspended || (key.empty() && !SwitchPlazaHasKey()) || (!bulk && !testKey && !searchPending && !choosePending && (pending.empty() || (!pendingAttempt && !covers && !banners))))) condvarWait(&changed, &lock);
		std::vector<std::string> cleanup; cleanup.swap(discarded);
		bool alive = running;
		bool reset = resetPending, restoreBanner = resetBanner;
		bool importing = importPending, localBanner = importBanner;
		std::string localRom = importRom, localPath = importPath;
		importPending = false;
		unsigned restoreId = resetSerial;
		std::string restoreRom = resetRom;
		resetPending = false;
		bool cleanupOnly = suspended || (key.empty() && !SwitchPlazaHasKey());
		busy = alive;
		std::string rom = pending, token = key;
		bool cover = covers, banner = banners, test = testKey;
		bool attempt = pendingAttempt;
		bool exact = pendingExact; pendingExact = false;
		bool scan = bulk && bulkScan;
		bool bulkItem = bulk && !scan && bulkDone < bulkRoms.size();
		std::string scanDirectory = bulkDirectory;
		if (bulkItem) { rom = bulkRoms[bulkDone]; cover = banner = true; }
		pendingAttempt = false;
		if (attempt) cover = banner = true;
		if (cleanupOnly) rom.clear();
		bool search = searchPending, choose = choosePending;
		bool best = search && searchBest; searchBest = false;
		SwitchArtworkSource source = search ? choiceSource : SWITCH_ARTWORK_AUTO;
		std::string query = searchTitle;
		Option selected = choose && chosen < options.size() ? options[chosen] : Option{};
		if (search || choose) rom = choiceRom;
		if (!search && !choose && rom.find(" [RHP ") != std::string::npos) exact = true;
		if (search) { cover = choiceCovers; banner = choiceBanners; }
		searchPending = choosePending = false;
		percent = 0; finished = false;
		unsigned revision = generation; activeRevision = revision;
		pending.clear(); testKey = false;
		mutexUnlock(&lock);
		for (const auto& path : cleanup) remove(path.c_str());
		if (!alive) break;
		Job job{revision, bulkItem};
		plazaError.clear();
		if (scan) {
			std::vector<std::string> roms;
			unsigned visited = 0;
			bool scanned = scanRoms(scanDirectory, 0, revision, visited, roms);
			mutexLock(&lock);
			if (revision == generation && bulk) {
				bulkRoms.swap(roms); bulkTotal = bulkRoms.size(); bulkScan = false; bulkPartial = !scanned;
				status = bulkTotal ? "Downloading missing covers and banners..." :
				    bulkPartial ? "Could not scan the entire folder. Check SD access or try a smaller folder." : "No missing artwork found";
				if (!bulkTotal) { cancelBulkLocked(); finished = true; }
			}
			mutexUnlock(&lock);
			continue;
		}
		if (reset) {
			std::string target = selectionPath(restoreRom, restoreBanner);
			bool ok = !remove((target + ".bak").c_str()) || errno == ENOENT;
			if (ok) ok = !remove(target.c_str()) || errno == ENOENT;
			mutexLock(&lock);
			++imageRevision;
			resetCompleted = restoreId;
			resetSucceeded = ok;
			if (revision == generation) {
				finished = true;
				status = ok ? "Original artwork restored" : "Could not reset artwork";
			}
			mutexUnlock(&lock);
			continue;
		}
		if (importing) {
			std::string bytes;
			FILE* input = fopen(localPath.c_str(), "rb");
			bool ok = false;
			if (input) {
				struct stat info;
				if (!fstat(fileno(input), &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 8 * 1024 * 1024) {
					bytes.resize(info.st_size);
					ok = fread(&bytes[0], 1, bytes.size(), input) == bytes.size();
				}
				fclose(input);
			}
			std::string target = selectionPath(localRom, localBanner), temp = target + ".tmp";
			if (ok && validImage(bytes)) {
				FILE* output = fopen(temp.c_str(), "wb");
				ok = output && fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size();
				if (output && fclose(output)) ok = false;
				struct mImage* image = ok ? mImageLoad(temp.c_str()) : nullptr;
				ok = image != nullptr;
				if (image) mImageDestroy(image);
			} else if (ok && bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
			           static_cast<unsigned char>(bytes[1]) == 0xD8 && static_cast<unsigned char>(bytes[2]) == 0xFF)
				ok = SwitchArtworkConvertJPEG(bytes.data(), bytes.size(), temp.c_str());
			else ok = false;
			mutexLock(&lock); bool current = revision == generation && running; mutexUnlock(&lock);
			ok = ok && current && replaceSelection(temp, target);
			remove(temp.c_str());
			mutexLock(&lock);
			if (revision == generation) { if (ok) ++imageRevision; finished = true; status = ok ? "Artwork imported" : "Import failed: choose a valid PNG, JPG or JPEG (up to 8 MiB)"; }
			mutexUnlock(&lock);
			continue;
		}
		if (cleanupOnly || (rom.empty() && !test && !search && !choose)) continue;
		if (!search && !choose && !test) {
			char path[4096];
			if (cover && SwitchArtworkPath(rom.c_str(), false, path, sizeof(path))) cover = false;
			if (banner && SwitchArtworkPath(rom.c_str(), true, path, sizeof(path))) banner = false;
			if (!cover && !banner) {
				message("Artwork ready");
				mutexLock(&lock); if (revision == generation) finished = true; mutexUnlock(&lock);
				continue;
			}
		}
		if (choose) {
			std::string bytes;
			bool ok = false;
			if (selected.cached) {
				FILE* file = fopen(selected.view.preview, "rb");
				struct stat info;
				if (file && !fstat(fileno(file), &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= 8 * 1024 * 1024) {
					bytes.resize(info.st_size); ok = fread(&bytes[0], 1, bytes.size(), file) == bytes.size();
				}
				if (file) fclose(file);
			} else ok = fetch(selected.url, "", revision, 8 * 1024 * 1024, bytes);
			if (ok && validImage(bytes)) {
				std::string target = selectionPath(rom, selected.view.banner), temp = target + ".tmp";
				FILE* file = fopen(temp.c_str(), "wb");
				ok = file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
				if (file && fclose(file)) ok = false;
				mutexLock(&lock); bool current = revision == generation && running; mutexUnlock(&lock);
				ok = ok && current && replaceSelection(temp, target); remove(temp.c_str());
			} else ok = false;
			mutexLock(&lock); if (revision == generation) { if (ok) ++imageRevision; finished = true; status = ok ? "Artwork ready" : "Artwork download failed"; } mutexUnlock(&lock);
			continue;
		}
		if (search && (source == SWITCH_ARTWORK_ROMHACKPLAZA || (source == SWITCH_ARTWORK_AUTO && token.empty()))) {
			message("Searching Romhack Plaza main images...");
			size_t count = 0;
			SwitchPlazaArtworkOptions(query.c_str(), [revision, best, &count]() { return currentJob(revision) && (!best || !count); }, [&](const std::string& name, const std::vector<unsigned char>& image) {
				if (!currentJob(revision)) return;
				std::string bytes(image.begin(), image.end());
				std::string path = directory + "/choice-" + std::to_string(revision) + "-" + std::to_string(count) + ".png";
				bool ok = false;
				if (validImage(bytes)) {
					FILE* file = fopen(path.c_str(), "wb"); ok = file && fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(); if (file && fclose(file)) ok = false;
				} else if (bytes.size() > 2 && image[0] == 0xFF && image[1] == 0xD8) {
					ok = SwitchArtworkConvertJPEG(image.data(), image.size(), path.c_str());
				}
				struct mImage* decoded = ok ? mImageLoad(path.c_str()) : nullptr;
				ok = decoded != nullptr;
				if (decoded) mImageDestroy(decoded);
				if (!ok || !currentJob(revision)) { remove(path.c_str()); return; }
				Option option{}; option.cached = true; option.view.banner = banner;
				snprintf(option.view.title, sizeof(option.view.title), "%s (main image)", name.c_str());
				snprintf(option.view.preview, sizeof(option.view.preview), "%s", path.c_str());
				mutexLock(&lock); bool current = revision == generation && running; if (current) options.push_back(option); mutexUnlock(&lock);
				if (!current) remove(path.c_str()); else ++count;
			}, plazaError);
			mutexLock(&lock);
			if (revision == generation) { searching = false; finished = true; status = !plazaError.empty() ? plazaError : count ? "Choose a main image for this artwork slot" : "No Romhack Plaza main images found"; chooseBestLocked(best); }
			mutexUnlock(&lock);
			continue;
		}
		message("Contacting SteamGridDB...");
		CURL* curl = curl_easy_init();
		if (!curl) { message("Network unavailable"); mutexLock(&lock); if (revision == generation) { searching = false; finished = true; } mutexUnlock(&lock); continue; }
		std::string title = cleanTitle(test ? "Metroid" : (search || attempt) && !query.empty() ? query : base(rom));
		if (token.empty() && !test) { bool ok = cover && plazaArtwork(rom, title, revision); message(ok ? "Romhack Plaza main image downloaded; banners unavailable from this source" : plazaError.empty() ? "No exact Romhack Plaza artwork found" : plazaError.c_str()); mutexLock(&lock); if (revision == generation) { searching = false; finished = true; } mutexUnlock(&lock); curl_easy_cleanup(curl); continue; }
		size_t suffix = title.find_first_of("([");
		if (!search && suffix != std::string::npos) title.resize(suffix);
		char* encoded = curl_easy_escape(curl, title.c_str(), title.size());
		std::string route = "search/autocomplete/" + std::string(encoded ? encoded : "");
		curl_free(encoded); curl_easy_cleanup(curl);
		rapidjson::Document document;
		if (!json(route, token, revision, document)) { bool fallback = !test && !search && cover && plazaArtwork(rom, title, revision); message(fallback ? "Romhack Plaza main image downloaded; no banner source available" : plazaError.empty() ? "SteamGridDB request failed; check key or network" : plazaError.c_str()); mutexLock(&lock); if (revision == generation) { searching = false; finished = true; } mutexUnlock(&lock); continue; }
		if (test) { message("SteamGridDB connection successful"); continue; }
		std::string numberless = withoutNumbers(title);
		if (!exact && document["data"].Empty() && numberless != title && !numberless.empty()) {
			CURL* escape = curl_easy_init();
			if (escape) {
				char* value = curl_easy_escape(escape, numberless.c_str(), numberless.size());
				std::string fallback = "search/autocomplete/" + std::string(value ? value : "");
				curl_free(value); curl_easy_cleanup(escape);
				rapidjson::Document matches;
				if (json(fallback, token, revision, matches)) document.Swap(matches);
			}
		}
		if (search) {
			std::string shorter = title;
			for (unsigned retry = 0; document["data"].Empty() && retry < 3; ++retry) {
				size_t split = shorter.find_last_of(" :_-.");
				if (split == std::string::npos || split < 4) {
					if (shorter.size() <= 4) break;
					split = std::max(size_t(4), (shorter.size() + 4) / 2);
					while (split && (static_cast<unsigned char>(shorter[split]) & 0xC0) == 0x80) --split;
					if (split < 4) break;
				}
				shorter.resize(split);
				CURL* escape = curl_easy_init();
				if (!escape) break;
				char* value = curl_easy_escape(escape, shorter.c_str(), shorter.size());
				std::string fallback = "search/autocomplete/" + std::string(value ? value : "");
				curl_free(value); curl_easy_cleanup(escape);
				rapidjson::Document matches;
				if (!json(fallback, token, revision, matches)) break;
				document.Swap(matches);
			}
			struct Game { unsigned id; std::string title; unsigned score; };
			std::vector<Game> games;
			for (const auto& item : document["data"].GetArray()) {
				if (!item.IsObject() || !item.HasMember("id") || !item["id"].IsUint() || !item.HasMember("name") || !item["name"].IsString()) continue;
				std::string name = item["name"].GetString();
				unsigned score = matchScore(name, title);
				games.push_back({item["id"].GetUint(), name, score});
			}
			std::stable_sort(games.begin(), games.end(), [](const Game& a, const Game& b) { return a.score > b.score; });
			size_t count = 0;
			for (size_t g = 0; g < games.size() && g < 6 && count < 12 && (!best || !count); ++g) {
				for (unsigned kind = 0; kind < 2 && count < 12 && (!best || !count); ++kind) {
					if (kind ? !banner : !cover) continue;
					rapidjson::Document art;
					std::string route = (kind ? "heroes/game/" : "grids/game/") + std::to_string(games[g].id) + (kind ? "?mimes=image/png&types=static" : "?dimensions=600x900&mimes=image/png&types=static");
					if (!json(route, token, revision, art)) continue;
					unsigned perGame = 0;
					for (const auto* entry : rankedArt(art)) {
						const auto& item = *entry;
						if (count >= 12 || perGame >= 2 || (best && count)) break;
						if (!item.IsObject() || !item.HasMember("url") || !item["url"].IsString()) continue;
						std::string url = item["url"].GetString();
						if (url.find("https://cdn2.steamgriddb.com/") && url.find("https://cdn.steamgriddb.com/")) continue;
						std::string bytes;
						if (!fetch(url, "", revision, 8 * 1024 * 1024, bytes) || !validImage(bytes)) continue;
						Option option{}; option.url = url; option.view.id = games[g].id; option.view.banner = kind;
						snprintf(option.view.title, sizeof(option.view.title), "%s (%s)", games[g].title.c_str(), kind ? "Banner" : "Cover");
						std::string path = directory + "/choice-" + std::to_string(revision) + "-" + std::to_string(count) + ".png";
						FILE* f = fopen(path.c_str(), "wb"); bool ok = f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(); if (f && fclose(f)) ok = false;
						if (!ok) { remove(path.c_str()); continue; }
						snprintf(option.view.preview, sizeof(option.view.preview), "%s", path.c_str());
						mutexLock(&lock); bool current = revision == generation && running; if (current) options.push_back(option); mutexUnlock(&lock);
						if (!current) { remove(path.c_str()); break; } ++count; ++perGame;
					}
				}
			}
			mutexLock(&lock); if (revision == generation) { searching = false; finished = true; status = count ? "Choose artwork or cancel" : "No matching artwork found"; chooseBestLocked(best); } mutexUnlock(&lock);
			continue;
		}
		unsigned id = 0, bestScore = 0;
		for (const auto& item : document["data"].GetArray()) {
			if (!item.IsObject() || !item.HasMember("id") || !item["id"].IsUint() || !item.HasMember("name") || !item["name"].IsString()) continue;
			if (exact && normalized(item["name"].GetString()) != normalized(title)) continue;
			unsigned score = matchScore(item["name"].GetString(), title);
			if (!id || score > bestScore) { id = item["id"].GetUint(); bestScore = score; }
		}
		if (!id) {
			if (cover) { plazaArtwork(rom, title, revision); if (!plazaError.empty()) message(plazaError.c_str()); }
			char path[4096]; bool missingCover = cover && !SwitchArtworkPath(rom.c_str(), false, path, sizeof(path));
			bool missingBanner = banner && !SwitchArtworkPath(rom.c_str(), true, path, sizeof(path));
			if (!missingCover && !missingBanner) message("Artwork ready");
			mutexLock(&lock); if (revision == generation) finished = true; mutexUnlock(&lock);
			if (attempt && !exact && (missingCover || missingBanner)) alternatives(rom, title, missingCover, missingBanner, revision);
			continue;
		}
		bool complete = true; unavailable = false;
		if (cover) complete = download(rom, false, id, token, revision);
		if (banner) complete = download(rom, true, id, token, revision) && complete;
		if (!complete && cover) plazaArtwork(rom, title, revision);
		char readyPath[4096];
		complete = (!cover || SwitchArtworkPath(rom.c_str(), false, readyPath, sizeof(readyPath))) &&
		           (!banner || SwitchArtworkPath(rom.c_str(), true, readyPath, sizeof(readyPath)));
		message(complete ? "Artwork ready" : plazaError.empty() ? "Artwork unavailable or download failed" : plazaError.c_str());
		mutexLock(&lock); if (revision == generation) finished = true; mutexUnlock(&lock);
		if (!complete && attempt && !exact) {
			char path[4096];
			bool missingCover = cover && !SwitchArtworkPath(rom.c_str(), false, path, sizeof(path));
			bool missingBanner = banner && !SwitchArtworkPath(rom.c_str(), true, path, sizeof(path));
			if (missingCover || missingBanner) alternatives(rom, title, missingCover, missingBanner, revision);
		}
	}
}
}

extern "C" {
void SwitchArtworkGetState(SwitchArtworkState* state) {
	if (!state) return; *state = {}; if (!initialized) return;
	mutexLock(&lock);
	state->applying = applying || importPending || choosePending;
	state->resetId = resetSerial;
	state->resetDone = resetCompleted;
	state->resetSucceeded = resetSucceeded;
	state->bulk = bulk; state->bulkDone = bulkDone; state->bulkTotal = bulkTotal;
	state->busy = busy; state->needsChoice = needsChoice; state->searching = searching; state->finished = finished; state->percent = percent; state->revision = imageRevision; state->count = options.size(); snprintf(state->rom, sizeof(state->rom), "%s", choiceRom.empty() ? requested.c_str() : choiceRom.c_str()); snprintf(state->message, sizeof(state->message), "%s", status.c_str()); mutexUnlock(&lock);
}
bool SwitchArtworkGetOption(size_t index, SwitchArtworkOption* option) {
	if (!initialized || !option) return false; mutexLock(&lock); bool ok = index < options.size(); if (ok) *option = options[index].view; mutexUnlock(&lock); return ok;
}
bool SwitchArtworkOpen(const char* rom, bool banner) {
	if (!initialized || !rom || !*rom || strlen(rom) >= 4096) return false;
	mutexLock(&lock);
	searchBest = false;
	choiceRom = requested = rom;
	choiceCovers = !banner;
	choiceBanners = banner;
	suspended = needsChoice = searching = searchPending = choosePending = testKey = false;
	finished = false;
	pending.clear();
	discardOptions();
	importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	return true;
}
void SwitchArtworkSetSource(SwitchArtworkSource source) {
	if (!initialized || source < SWITCH_ARTWORK_AUTO || source > SWITCH_ARTWORK_ROMHACKPLAZA) return;
	mutexLock(&lock);
	if (!applying && !importPending && !choosePending && choiceSource != source) {
		choiceSource = source;
		searchBest = false;
		++generation;
		searchPending = searching = needsChoice = false;
		finished = false;
		discardOptions();
		condvarWakeAll(&changed);
	}
	mutexUnlock(&lock);
}
SwitchArtworkSource SwitchArtworkGetSource(void) {
	if (!initialized) return SWITCH_ARTWORK_AUTO;
	mutexLock(&lock); SwitchArtworkSource result = choiceSource; mutexUnlock(&lock); return result;
}
bool SwitchArtworkImport(const char* rom, const char* path, bool banner) {
	if (!initialized || !rom || !*rom || strlen(rom) >= 4096 || !path || !*path || strlen(path) >= 4096) return false;
	mutexLock(&lock);
	if (applying || importPending || choosePending) { mutexUnlock(&lock); return false; }
	importRom = choiceRom = requested = rom; importPath = path; importBanner = banner;
	importPending = applying = true; finished = false; percent = 0;
	searchPending = choosePending = searching = needsChoice = testKey = false; pending.clear();
	status = banner ? "Importing banner..." : "Importing cover...";
	cancelBulkLocked(); ++generation; condvarWakeAll(&changed); mutexUnlock(&lock);
	return true;
}
bool SwitchArtworkReset(const char* rom, bool banner) {
	if (!initialized || !rom || !*rom || strlen(rom) >= 4096) return false;
	mutexLock(&lock);
	if (resetPending) { mutexUnlock(&lock); return false; }
	resetRom = requested = rom;
	resetBanner = banner;
	resetPending = true;
	++resetSerial;
	needsChoice = searching = searchPending = choosePending = testKey = false;
	finished = false;
	percent = 0;
	status = banner ? "Restoring original banner..." : "Restoring original cover...";
	pending.clear();
	discardOptions();
	importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	return true;
}
void SwitchArtworkSearch(const char* title) {
	if (!initialized || !title || !*title || strlen(title) > 512) return;
	mutexLock(&lock);
	bool plaza = choiceSource == SWITCH_ARTWORK_ROMHACKPLAZA || (choiceSource == SWITCH_ARTWORK_AUTO && key.empty());
	bool available = plaza ? SwitchPlazaHasKey() : !key.empty();
	if (!choiceRom.empty() && available) {
		searchBest = false;
		searchTitle = title;
		needsChoice = choosePending = false;
		searching = searchPending = true;
		finished = false;
		percent = 0;
		status = plaza ? "Searching Romhack Plaza main images..." : "Searching SteamGridDB...";
		discardOptions();
		pending.clear();
		importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation;
		condvarWakeAll(&changed);
	} else if (!available) {
		finished = true;
		status = plaza ? "Add a Romhack Plaza API key in Settings first." : "Add a SteamGridDB API key in Settings > Artwork to search.";
	}
	mutexUnlock(&lock);
}
void SwitchArtworkChoose(size_t index) {
	if (!initialized) return; mutexLock(&lock); if (!applying && !importPending && !choosePending && index < options.size()) { applying = true; chosen = index; choosePending = true; searchPending = searching = needsChoice = false; finished = false; importPending = false; cancelBulkLocked(); ++generation; condvarWakeAll(&changed); } mutexUnlock(&lock);
}
void SwitchArtworkCancelChoice(void) {
	if (!initialized) return; mutexLock(&lock); importPending = searchBest = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; searchPending = choosePending = searching = needsChoice = false; finished = true; discardOptions(); pending.clear(); status = "Artwork selection cancelled"; condvarWakeAll(&changed); mutexUnlock(&lock);
}
void SwitchArtworkInit(const char* path) {
	if (initialized || !path || !*path) return;
	mutexInit(&lock);
	condvarInit(&changed);
	suspended = false;
	directory = path;
	mkdir(directory.c_str(), 0700);
	char buffer[129] = {};
	FILE* file = fopen((directory + "/steamgriddb.key").c_str(), "rb");
	if (file) { size_t n = fread(buffer, 1, 128, file); fclose(file); buffer[n] = 0; key = buffer; for (unsigned char c : key) if (!std::isalnum(c)) { key.clear(); break; } }
	if (curl_global_init(CURL_GLOBAL_DEFAULT)) return;
	running = true;
	if (R_FAILED(threadCreate(&worker, work, nullptr, nullptr, 128 * 1024, 0x3B, -2))) { running = false; curl_global_cleanup(); return; }
	if (R_FAILED(threadStart(&worker))) { threadClose(&worker); running = false; curl_global_cleanup(); return; }
	initialized = true;
}
void SwitchArtworkSuspend(void) {
	if (!initialized) return;
	mutexLock(&lock); suspended = true; pending.clear(); testKey = false; requested.clear(); importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation;
	searchPending = choosePending = needsChoice = searching = false; discardOptions();
	mutexUnlock(&lock);
}
void SwitchArtworkDeinit(void) {
	if (!initialized) return;
	mutexLock(&lock); running = false; discardOptions(); importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; condvarWakeAll(&changed); mutexUnlock(&lock);
	threadWaitForExit(&worker); threadClose(&worker);
	curl_global_cleanup(); key.clear(); initialized = false;
}
bool SwitchArtworkSetKey(const char* value) {
	if (!initialized || !value || strlen(value) > 128) return false;
	for (const unsigned char* c = (const unsigned char*) value; *c; ++c) if (!std::isalnum(*c)) return false;
	std::string destination = directory + "/steamgriddb.key", temporary = destination + ".tmp";
	if (*value) {
		FILE* file = fopen(temporary.c_str(), "wb");
		if (!file) return false;
		bool ok = fwrite(value, 1, strlen(value), file) == strlen(value);
		if (fclose(file)) ok = false;
		if (!ok || rename(temporary.c_str(), destination.c_str())) { remove(temporary.c_str()); return false; }
		chmod(destination.c_str(), 0600);
	} else if (exists(destination) && remove(destination.c_str())) return false;
	mutexLock(&lock); key = value; requested.clear(); pending.clear(); needsChoice = searching = searchPending = choosePending = false; discardOptions(); importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; condvarWakeAll(&changed); mutexUnlock(&lock);
	return true;
}
bool SwitchArtworkHasKey(void) { if (!initialized) return false; mutexLock(&lock); bool result = !key.empty(); mutexUnlock(&lock); return result; }
void SwitchArtworkSetDownloads(bool cover, bool banner) {
	if (!initialized) return;
	mutexLock(&lock);
	if (covers != cover || banners != banner) { importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; searchPending = choosePending = needsChoice = searching = false; discardOptions(); }
	covers = cover; banners = banner; requested.clear(); suspended = false;
	status = cover || banner ? "Artwork ready" : "Artwork downloads disabled";
	condvarWakeAll(&changed);
	if (!cover && !banner) pending.clear();
	mutexUnlock(&lock);
}
void SwitchArtworkRequest(const char* rom) {
	if (!initialized || !rom) return;
	mutexLock(&lock); if (!bulk && requested != rom) { requested = rom; pending = rom; choiceRom.clear(); pendingAttempt = false; needsChoice = searching = finished = searchPending = choosePending = false; discardOptions(); importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; condvarWakeAll(&changed); } mutexUnlock(&lock);
}
bool SwitchArtworkAttempt(const char* rom) {
	return SwitchArtworkDownloadBest(rom, nullptr);
}
static bool queueArtwork(const char* rom, const char* title, bool exact) {
	if (!initialized || !rom || !*rom || strlen(rom) >= 4096) return false;
	if (title && strlen(title) > 512) return false;
	mutexLock(&lock);
	if (key.empty() && !SwitchPlazaHasKey()) { status = "Add a SteamGridDB or Romhack Plaza API key to download artwork."; mutexUnlock(&lock); return false; }
	if (choiceRom != rom) discardOptions();
	requested = pending = choiceRom = rom;
	searchTitle = title ? title : "";
	pendingAttempt = true; pendingExact = exact; suspended = false;
	needsChoice = searching = finished = searchPending = choosePending = testKey = false;
	percent = 0; status = "Looking for artwork...";
	importPending = false; applying = true; cancelBulkLocked(); ++generation; condvarWakeAll(&changed);
	mutexUnlock(&lock);
	return true;
}
bool SwitchArtworkDownloadBest(const char* rom, const char* title) { return queueArtwork(rom, title, false); }
bool SwitchArtworkDownloadBestFromSource(const char* rom, const char* title, SwitchArtworkSource source) {
	if (!initialized || !rom || !title || !*title || strlen(title) > 512 || source < SWITCH_ARTWORK_AUTO || source > SWITCH_ARTWORK_ROMHACKPLAZA) return false;
	mutexLock(&lock);
	bool plaza = source == SWITCH_ARTWORK_ROMHACKPLAZA || (source == SWITCH_ARTWORK_AUTO && key.empty());
	bool available = plaza ? SwitchPlazaHasKey() : !key.empty();
	bool ok = available && choiceRom == rom && !applying && !importPending && !choosePending;
	if (ok) {
		choiceSource = source; searchTitle = title; searchBest = searchPending = searching = true;
		needsChoice = finished = false; percent = 0; pending.clear(); discardOptions();
		status = plaza ? "Finding the best Romhack Plaza main image..." : "Finding the best SteamGridDB artwork...";
		cancelBulkLocked(); ++generation; condvarWakeAll(&changed);
	} else if (!available) {
		finished = true; status = plaza ? "Add a Romhack Plaza API key in Settings first." : "Add a SteamGridDB API key in Settings > Artwork first.";
	}
	mutexUnlock(&lock); return ok;
}
bool SwitchArtworkDownloadExact(const char* rom, const char* title) { return queueArtwork(rom, title, true); }
bool SwitchArtworkDownloadMissing(const char* path) {
	if (!initialized || !path || !*path || strlen(path) >= 4096) return false;
	mutexLock(&lock);
	if (key.empty() && !SwitchPlazaHasKey()) { status = "Add a SteamGridDB or Romhack Plaza API key to download artwork."; mutexUnlock(&lock); return false; }
	cancelBulkLocked(); ++generation;
	bulk = bulkScan = true; bulkDone = bulkTotal = 0; bulkDirectory = path;
	bulkPartial = false;
	pending.clear(); pendingAttempt = testKey = suspended = false;
	searchPending = choosePending = searching = needsChoice = importPending = false;
	if (!busy) applying = false;
	finished = false; percent = 0; discardOptions();
	status = "Scanning library for missing artwork...";
	condvarWakeAll(&changed); mutexUnlock(&lock); return true;
}
void SwitchArtworkCancelBulk(void) {
	if (!initialized) return;
	mutexLock(&lock);
	if (bulk) { cancelBulkLocked(); ++generation; finished = true; status = "Missing artwork download cancelled"; condvarWakeAll(&changed); }
	mutexUnlock(&lock);
}
bool SwitchArtworkPath(const char* rom, bool banner, char* out, size_t size) {
	if (!rom || !out || !size) return false;
	std::string selected = selectionPath(rom, banner);
	if (!exists(selected) && exists(selected + ".bak")) selected += ".bak";
	if (exists(selected) && selected.size() < size) {
		memcpy(out, selected.c_str(), selected.size() + 1);
		return true;
	}
	std::string path = rom;
	size_t slash = path.find_last_of("/\\"), dot = path.find_last_of('.');
	if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) path.resize(dot);
	path += banner ? ".banner.png" : ".cover.png";
	if (!exists(path)) path = cachePath(rom, banner);
	if (!exists(path) || path.size() >= size) { out[0] = 0; return false; }
	memcpy(out, path.c_str(), path.size() + 1); return true;
}
void SwitchArtworkTest(void) { if (!initialized) return; mutexLock(&lock); if (key.empty()) { status = "Enter an API key first"; mutexUnlock(&lock); return; } testKey = true; importPending = false; if (!busy) applying = false; cancelBulkLocked(); ++generation; condvarWakeAll(&changed); mutexUnlock(&lock); }
void SwitchArtworkStatus(char* out, size_t size) { if (!out || !size) return; if (!initialized) { snprintf(out, size, "Artwork service unavailable"); return; } mutexLock(&lock); snprintf(out, size, "%s", status.c_str()); mutexUnlock(&lock); }
}
