/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "ra-http.h"
#include <switch.h>
#include <curl/curl.h>
#include <deque>
#include <memory>
#include <string>

namespace {
struct Request {
	std::string url, post, type, agent, body;
	bool hasPost;
	rc_client_server_callback_t callback;
	void* userdata;
	int status = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
};
Mutex lock;
CondVar changed;
Thread worker;
bool initialized, running;
size_t outstanding;
std::deque<std::unique_ptr<Request>> pending, complete;

void deliver(Request& request) {
	rc_api_server_response_t response = {};
	response.body = request.body.c_str();
	response.body_length = request.body.size();
	response.http_status_code = request.status;
	request.callback(&response, request.userdata);
}

size_t receive(char* data, size_t size, size_t count, void* userdata) {
	Request* request = static_cast<Request*>(userdata);
	const size_t limit = 8 * 1024 * 1024;
	if (size && count > (limit - request->body.size()) / size) return 0;
	request->body.append(data, size * count);
	return size * count;
}

int progress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	mutexLock(&lock);
	bool cancelled = !running;
	mutexUnlock(&lock);
	return cancelled;
}

void work(void*) {
	for (;;) {
		mutexLock(&lock);
		while (running && pending.empty()) condvarWait(&changed, &lock);
		if (!running) { mutexUnlock(&lock); return; }
		auto request = std::move(pending.front());
		pending.pop_front();
		mutexUnlock(&lock);
		CURL* curl = curl_easy_init();
		if (curl) {
			curl_easy_setopt(curl, CURLOPT_URL, request->url.c_str());
			curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
			curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
			curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
			curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
			curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
			curl_easy_setopt(curl, CURLOPT_USERAGENT, request->agent.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, request.get());
			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
			curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
			curl_slist* headers = nullptr;
			if (request->hasPost) {
				std::string type = "Content-Type: " + request->type;
				headers = curl_slist_append(headers, type.c_str());
				curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post.c_str());
				curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request->post.size()));
			}
			if (curl_easy_perform(curl) == CURLE_OK) {
				long status;
				curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
				request->status = static_cast<int>(status);
			} else request->body = "Network request failed";
			curl_slist_free_all(headers);
			curl_easy_cleanup(curl);
		}
		mutexLock(&lock);
		complete.emplace_back(std::move(request));
		mutexUnlock(&lock);
	}
}
}

bool SwitchRAHttpInit(void) {
	if (initialized) return true;
	mutexInit(&lock);
	condvarInit(&changed);
	if (curl_global_init(CURL_GLOBAL_DEFAULT)) return false;
	running = true;
	if (R_FAILED(threadCreate(&worker, work, nullptr, nullptr, 128 * 1024, 0x3B, -2))) {
		running = false; curl_global_cleanup(); return false;
	}
	if (R_FAILED(threadStart(&worker))) {
		threadClose(&worker); running = false; curl_global_cleanup(); return false;
	}
	initialized = true;
	return true;
}

void SwitchRAHttpSend(const rc_api_request_t* source, rc_client_server_callback_t callback, void* userdata, const char* userAgent) {
	if (!callback) return;
	auto request = std::unique_ptr<Request>(new Request);
	request->callback = callback;
	request->userdata = userdata;
	if (initialized) {
		request->url = source->url ? source->url : "";
		request->hasPost = source->post_data != nullptr;
		request->post = source->post_data ? source->post_data : "";
		request->type = source->content_type ? source->content_type : "application/x-www-form-urlencoded";
		request->agent = userAgent ? userAgent : "mGBA";
		mutexLock(&lock);
		if (running && outstanding < 32) {
			++outstanding;
			pending.emplace_back(std::move(request));
			condvarWakeAll(&changed);
			mutexUnlock(&lock);
			return;
		}
		mutexUnlock(&lock);
	}
	request->body = "Network service unavailable";
	deliver(*request);
}

void SwitchRAHttpPump(void) {
	if (!initialized) return;
	std::deque<std::unique_ptr<Request>> ready;
	mutexLock(&lock);
	ready.swap(complete);
	outstanding -= ready.size();
	mutexUnlock(&lock);
	for (auto& request : ready) deliver(*request);
}

void SwitchRAHttpDeinit(void) {
	if (!initialized) return;
	mutexLock(&lock);
	running = false;
	condvarWakeAll(&changed);
	mutexUnlock(&lock);
	threadWaitForExit(&worker);
	threadClose(&worker);
	SwitchRAHttpPump();
	std::deque<std::unique_ptr<Request>> cancelled;
	cancelled.swap(pending);
	outstanding = 0;
	for (auto& request : cancelled) {
		request->body = "Network service stopped";
		deliver(*request);
	}
	initialized = false;
	curl_global_cleanup();
}
