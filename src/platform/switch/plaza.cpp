/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "plaza.h"
#include "artwork.h"
#include <romhackplaza/client.h>
#include <patch.h>
#include <mgba-util/sha1.h>
#include <switch.h>
#include <zlib.h>
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace {
Mutex lock;
CondVar changed;
Thread worker;
bool initialized, running;
unsigned generation;
int pending;
size_t selectedEntry, selectedFile;
std::string key, keyPath, directory;
SwitchPlazaState state{};
struct Candidate { romhackplaza::Entry entry; std::string rom; uint32_t crc; std::string sha; };
std::vector<Candidate> candidates;
std::vector<romhackplaza::File> files;
std::vector<std::string> scannedRoms;
size_t scanCursor;
std::map<std::string, std::vector<romhackplaza::Entry>> searchCache;
std::map<uint64_t, romhackplaza::Entry> detailCache;
bool current(unsigned revision) { mutexLock(&lock); bool ok = running && revision == generation; mutexUnlock(&lock); return ok; }
void message(unsigned revision, const std::string& text) { mutexLock(&lock); if (revision == generation) snprintf(state.message, sizeof(state.message), "%s", text.c_str()); mutexUnlock(&lock); }
bool progress(unsigned revision, uint64_t now, uint64_t total) { mutexLock(&lock); bool ok = running && revision == generation; if (ok) state.percent = total ? std::min(uint64_t(100), now * 100 / total) : 0; mutexUnlock(&lock); return ok; }
std::string lower(std::string value) { for (char& c : value) c = std::tolower(static_cast<unsigned char>(c)); return value; }
std::string plainText(const std::string& html) {
	std::string out; bool tag = false;
	for (size_t i = 0; i < html.size(); ++i) {
		char c = html[i];
		if (c == '<') { tag = true; if (!out.empty() && out.back() != ' ') out += ' '; continue; }
		if (c == '>') { tag = false; continue; }
		if (tag) continue;
		if (c == '&') {
			size_t end = html.find(';', i + 1);
			if (end != std::string::npos && end - i < 12) {
				std::string entity = html.substr(i, end - i + 1);
				if (entity == "&amp;") c = '&'; else if (entity == "&lt;") c = '<'; else if (entity == "&gt;") c = '>'; else if (entity == "&quot;") c = '"'; else if (entity == "&#39;" || entity == "&apos;") c = '\''; else if (entity == "&nbsp;") c = ' '; else { out += entity; i = end; continue; }
				i = end;
			}
		}
		if (std::isspace(static_cast<unsigned char>(c))) c = ' ';
		if (c != ' ' || (!out.empty() && out.back() != ' ')) out += c;
	}
	return out;
}
void textCopy(char* out, size_t size, const std::string& text) {
	size_t length = std::min(size - 1, text.size());
	while (length < text.size() && length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) --length;
	memcpy(out, text.data(), length); out[length] = 0;
}
std::string title(const std::string& path) { size_t pos = path.find_last_of("/\\"); std::string out = path.substr(pos == std::string::npos ? 0 : pos + 1); pos = out.find_last_of('.'); if (pos != std::string::npos) out.resize(pos); pos = out.find_first_of("(["); if (pos != std::string::npos) out.resize(pos); for (char& c : out) if (c == '_') c = ' '; while (!out.empty() && out.back() == ' ') out.pop_back(); return out; }
bool hashFile(const std::string& path, unsigned revision, uint32_t& crc, std::string& sha) {
	FILE* f = fopen(path.c_str(), "rb"); if (!f) return false;
	SHA1Context context; sha1Init(&context); crc = ::crc32(0, Z_NULL, 0); uint8_t buffer[65536]; size_t bytes, total = 0; bool ok = true;
	while ((bytes = fread(buffer, 1, sizeof(buffer), f))) { total += bytes; if (total > romhackplaza::MaxRomSize || !current(revision)) { ok = false; break; } sha1Update(&context, buffer, bytes); crc = ::crc32(crc, buffer, bytes); }
	if (ferror(f) || !total) ok = false;
	fclose(f); if (!ok) return false;
	uint8_t digest[20]; sha1Finalize(digest, &context); char hex[41]; for (unsigned i = 0; i < 20; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]); sha = hex; return true;
}
bool scan(const std::string& path, unsigned depth, unsigned revision, unsigned& visited, std::vector<std::string>& roms) {
	if (depth > 16 || visited >= 100000 || !current(revision)) return false;
	DIR* d = opendir(path.c_str()); if (!d) return false; bool complete = true;
	while (dirent* e = readdir(d)) { if (++visited > 100000 || !current(revision)) { complete = false; break; } if (e->d_name[0] == '.') continue;
		std::string child = path + (path.back() == '/' ? "" : "/") + e->d_name; struct stat st; if (child.size() >= 4096 || lstat(child.c_str(), &st)) { complete = false; continue; }
		if (S_ISDIR(st.st_mode)) { if (!scan(child, depth + 1, revision, visited, roms)) complete = false; } else if (S_ISREG(st.st_mode)) { size_t dot = child.find_last_of('.'); std::string ext = dot == std::string::npos ? "" : lower(child.substr(dot)); if (ext == ".gba" || ext == ".gb" || ext == ".gbc" || ext == ".agb" || ext == ".sgb") roms.push_back(child); }
	} closedir(d); return complete;
}
void work(void*) {
	mutexLock(&lock);
	while (running) {
		while (running && !pending) condvarWait(&changed, &lock);
		if (!running) break;
		int job = pending; pending = 0; unsigned revision = generation; std::string token = key, root = directory; size_t ei = selectedEntry, fi = selectedFile; Candidate selected; romhackplaza::File file;
		if (job != 1 && ei < candidates.size()) selected = candidates[ei];
		if (job == 3 && fi < files.size()) file = files[fi];
		state.busy = true; state.percent = 0; mutexUnlock(&lock);
		try {
			romhackplaza::Client client(token); auto callback = [revision](uint64_t n, uint64_t total) { return progress(revision, n, total); };
			if (job == 1) {
				message(revision, "Scanning uncompressed GB/GBC/GBA ROMs..."); unsigned visited = 0; bool complete = true;
				if (scannedRoms.empty()) complete = scan(root, 0, revision, visited, scannedRoms);
				for (; scanCursor < scannedRoms.size() && current(revision); ++scanCursor) {
					const std::string& rom = scannedRoms[scanCursor]; uint32_t crc; std::string sha; if (!hashFile(rom, revision, crc, sha)) continue;
					std::string query = title(rom); message(revision, "Finding verified patches for " + query + "...");
					if (!searchCache.count(query)) searchCache[query] = client.search(query, callback);
					for (const auto& result : searchCache[query]) { if (!current(revision)) break; if (!detailCache.count(result.id)) detailCache[result.id] = client.details(result.id, callback); const auto& entry = detailCache[result.id]; bool matches = false; char hex[9]; snprintf(hex, sizeof(hex), "%08x", crc);
						for (const auto& hash : entry.hashes) if (lower(hash.verified).find("no-intro") != std::string::npos && hash.sha1.size() == 40 && lower(hash.sha1) == sha && lower(hash.crc32) == hex) matches = true;
						if (!matches) continue;
						mutexLock(&lock); if (revision == generation && std::none_of(candidates.begin(), candidates.end(), [&entry](const Candidate& c) { return c.entry.id == entry.id; })) { candidates.push_back({entry, rom, crc, sha}); state.count = candidates.size(); ++state.revision; } mutexUnlock(&lock);
					}
					searchCache.clear(); detailCache.clear();
				}
				message(revision, complete ? "Scan finished. Only exact base-ROM hash matches are listed; archives must be extracted first." : "Partial scan finished; some folders were skipped. Archives must be extracted first.");
			} else if (job == 2) {
				message(revision, "Loading patch downloads..."); auto available = client.files(selected.entry.id, callback);
				available.erase(std::remove_if(available.begin(), available.end(), [](const romhackplaza::File& file) { size_t dot = file.filename.find_last_of('.'); std::string ext = dot == std::string::npos ? "" : lower(file.filename.substr(dot)); return (ext != ".ips" && ext != ".bps" && ext != ".ups" && ext != ".zip") || file.size > 64 * 1024 * 1024; }), available.end());
				mutexLock(&lock); if (revision == generation) { files = available; state.fileCount = files.size(); state.filesEntry = ei; ++state.revision; } mutexUnlock(&lock); message(revision, available.empty() ? "No supported patch files (IPS/BPS/UPS/ZIP) available." : "Choose a patch file. Downloads are limited to 5 per minute.");
			} else {
				uint32_t crc; std::string sha; if (!hashFile(selected.rom, revision, crc, sha) || crc != selected.crc || sha != selected.sha) throw std::runtime_error("The original ROM changed; scan again before installing.");
				message(revision, "Downloading patch (5 downloads per minute)..."); auto download = client.download(file.url, 64 * 1024 * 1024, callback);
				if (file.size && download.size() != file.size) throw std::runtime_error("Downloaded patch size does not match the file listing.");
				message(revision, "Verifying and applying patch to a separate copy..."); std::string installed, error;
				if (!romhackplaza::InstallPatch(selected.rom, download, root, selected.entry.title, selected.entry.id, crc, installed, error, callback)) throw std::runtime_error(error);
				mutexLock(&lock); snprintf(state.installedPath, sizeof(state.installedPath), "%s", installed.c_str()); ++state.installedRevision; mutexUnlock(&lock);
				SwitchArtworkDownloadExact(installed.c_str(), selected.entry.title.c_str()); message(revision, "Installed successfully. The original ROM was not changed.");
			}
		} catch (const romhackplaza::RateLimitError& e) { message(revision, std::string(e.what()) + " Retry after " + std::to_string(e.retrySeconds) + " seconds; no automatic retry."); }
		catch (const std::exception& e) { message(revision, e.what()); }
		mutexLock(&lock); state.busy = false; ++state.revision;
	}
	mutexUnlock(&lock);
}
}
bool SwitchPlazaArtwork(const char* query, std::vector<unsigned char>& image, const std::function<bool()>& alive, std::string& error) {
	error.clear();
	if (!initialized || !query || !*query) return false;
	mutexLock(&lock); std::string token = key; mutexUnlock(&lock); if (token.empty()) return false;
	auto normalize = [](const std::string& input) { std::string result; for (unsigned char c : input) if (std::isalnum(c)) result += std::tolower(c); return result; };
	try {
		romhackplaza::Client client(token); auto progress = [&alive](uint64_t, uint64_t) { return alive(); };
		uint64_t match = 0;
		for (const auto& entry : client.search(query, progress)) {
			if (!alive()) return false;
			if (normalize(entry.title) != normalize(query)) continue;
			if (match && match != entry.id) { error = "Romhack Plaza artwork title is ambiguous; choose or import artwork manually."; return false; }
			match = entry.id;
		}
		if (!match) return false;
		auto detail = client.details(match, progress);
		if (detail.mainImage.empty()) return false;
		image = client.download(detail.mainImage, 8 * 1024 * 1024, progress, false);
		return alive() && !image.empty();
	} catch (const std::exception& e) { error = e.what(); return false; }
	return false;
}
bool SwitchPlazaArtworkOptions(const char* query, const std::function<bool()>& alive, const std::function<void(const std::string&, const std::vector<unsigned char>&)>& result, std::string& error) {
	error.clear();
	if (!initialized || !query || !*query) return false;
	mutexLock(&lock); std::string token = key; mutexUnlock(&lock);
	if (token.empty()) { error = "Add a Romhack Plaza API key in Settings first."; return false; }
	try {
		romhackplaza::Client client(token);
		auto progress = [&alive](uint64_t, uint64_t) { return alive(); };
		auto entries = client.search(query, progress);
		auto normalize = [](const std::string& title) { std::string result; for (unsigned char c : title) if (std::isalnum(c)) result += std::tolower(c); return result; };
		std::string needle = normalize(query);
		std::stable_sort(entries.begin(), entries.end(), [&](const romhackplaza::Entry& a, const romhackplaza::Entry& b) { return (normalize(a.title) == needle) > (normalize(b.title) == needle); });
		unsigned count = 0;
		for (const auto& entry : entries) {
			if (!alive()) return false;
			if (count++ >= 6) break;
			auto detail = client.details(entry.id, progress);
			if (detail.mainImage.empty()) continue;
			auto image = client.download(detail.mainImage, 8 * 1024 * 1024, progress, false);
			if (!alive()) return false;
			if (!image.empty()) result(plainText(entry.title), image);
		}
		return alive();
	} catch (const std::exception& e) { error = e.what(); return false; }
}
extern "C" {
void SwitchPlazaInit(const char* config) { if (initialized || !config || curl_global_init(CURL_GLOBAL_DEFAULT)) return; mutexInit(&lock); condvarInit(&changed); keyPath = std::string(config) + "/romhackplaza.key"; mkdir("sdmc:/mGBA", 0777); if (!romhackplaza::ReadApiKeyFile(config, key)) { FILE* f = fopen(keyPath.c_str(), "rb"); if (f) { char data[1025]{}; size_t n = fread(data, 1, 1024, f); fclose(f); if (n < 1024) key.assign(data, n); } } running = true; snprintf(state.message, sizeof(state.message), key.empty() ? "Add a Romhack Plaza API key, then scan your ROM folder. Downloads: 5 per minute." : "API key loaded. Scan your ROM folder. Downloads: 5 per minute."); if (R_FAILED(threadCreate(&worker, work, nullptr, nullptr, 128 * 1024, 0x3B, -2))) { running = false; curl_global_cleanup(); return; } if (R_FAILED(threadStart(&worker))) { threadClose(&worker); running = false; curl_global_cleanup(); return; } initialized = true; }
void SwitchPlazaDeinit(void) { if (!initialized) return; mutexLock(&lock); running = false; ++generation; condvarWakeAll(&changed); mutexUnlock(&lock); threadWaitForExit(&worker); threadClose(&worker); candidates.clear(); files.clear(); key.clear(); initialized = false; curl_global_cleanup(); }
bool SwitchPlazaHasKey(void) { if (!initialized) return false; mutexLock(&lock); bool present = !key.empty(); mutexUnlock(&lock); return present; }
bool SwitchPlazaSetKey(const char* value) {
	if (!initialized || !value || strlen(value) > 1023) return false;
	for (const char* p = value; *p; ++p) if (static_cast<unsigned char>(*p) < 33 || static_cast<unsigned char>(*p) > 126) return false;
	std::string temp = keyPath + ".tmp", backup = keyPath + ".bak";
	FILE* f = fopen(temp.c_str(), "wb"); if (!f) return false; chmod(temp.c_str(), 0600);
	bool ok = fwrite(value, 1, strlen(value), f) == strlen(value); if (fclose(f)) ok = false;
	struct stat st; bool old = !lstat(keyPath.c_str(), &st);
	if (ok && old) { remove(backup.c_str()); ok = !rename(keyPath.c_str(), backup.c_str()); }
	if (ok && rename(temp.c_str(), keyPath.c_str())) { ok = false; if (old) rename(backup.c_str(), keyPath.c_str()); }
	remove(temp.c_str()); if (ok) { remove(backup.c_str()); mutexLock(&lock); key = value; mutexUnlock(&lock); } return ok;
}
bool SwitchPlazaStartScan(const char* path) { if (!initialized || !path || !*path || strlen(path) >= 4096) return false; mutexLock(&lock); bool ok = !state.busy && !pending && !key.empty(); if (ok) { if (directory != path || scanCursor >= scannedRoms.size()) { scannedRoms.clear(); scanCursor = 0; candidates.clear(); searchCache.clear(); detailCache.clear(); state.count = 0; } directory = path; files.clear(); state.fileCount = 0; pending = 1; ++generation; state.busy = true; condvarWakeAll(&changed); } else if (key.empty()) snprintf(state.message, sizeof(state.message), "Enter a Romhack Plaza API key first."); mutexUnlock(&lock); return ok; }
void SwitchPlazaCancel(void) { if (!initialized) return; mutexLock(&lock); ++generation; if (pending) state.busy = false; pending = 0; snprintf(state.message, sizeof(state.message), "Cancelling; original ROMs are unchanged."); mutexUnlock(&lock); }
void SwitchPlazaGetState(SwitchPlazaState* out) { if (!out) return; *out = {}; if (!initialized) return; mutexLock(&lock); *out = state; mutexUnlock(&lock); }
bool SwitchPlazaGetEntry(size_t index, SwitchPlazaEntry* out) { if (!initialized || !out) return false; mutexLock(&lock); bool ok = index < candidates.size(); if (ok) { *out = {}; const auto& c = candidates[index]; out->id = c.entry.id; textCopy(out->title, sizeof(out->title), plainText(c.entry.title)); textCopy(out->description, sizeof(out->description), plainText(c.entry.description)); snprintf(out->baseRom, sizeof(out->baseRom), "%s", c.rom.c_str()); } mutexUnlock(&lock); return ok; }
bool SwitchPlazaRequestFiles(size_t index) { if (!initialized) return false; mutexLock(&lock); bool ok = !state.busy && !pending && index < candidates.size(); if (ok) { selectedEntry = index; files.clear(); state.fileCount = 0; state.filesEntry = index; pending = 2; state.busy = true; ++generation; condvarWakeAll(&changed); } mutexUnlock(&lock); return ok; }
bool SwitchPlazaGetFile(size_t index, SwitchPlazaFile* out) { if (!initialized || !out) return false; mutexLock(&lock); bool ok = index < files.size(); if (ok) { snprintf(out->filename, sizeof(out->filename), "%s", files[index].filename.c_str()); out->size = files[index].size; } mutexUnlock(&lock); return ok; }
bool SwitchPlazaInstall(size_t ei, size_t fi) { if (!initialized) return false; mutexLock(&lock); bool ok = !state.busy && !pending && ei < candidates.size() && fi < files.size() && state.filesEntry == ei; if (ok) { selectedEntry = ei; selectedFile = fi; pending = 3; state.busy = true; ++generation; condvarWakeAll(&changed); } mutexUnlock(&lock); return ok; }
}
