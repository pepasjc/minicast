#pragma once

// Asynchronous HTTP for RetroAchievements in minicast.
//
// Adapted from the MiSTer RetroAchievements fork of Main_MiSTer
// (ra_http.h / ra_http.cpp, odelot's MiSTer_RA work): same worker thread +
// request/response queues, same "system curl" transport (minicast is a
// static build with no TLS library; MiSTer ships /usr/bin/curl, run with -k
// because its CA bundle is outdated). See ra_http.cpp for what changed.
//
// Thread model:
//   RA thread:     ra_http_request() enqueues, ra_http_poll() dispatches the
//                  completed callbacks (rc_client callbacks stay on it)
//   worker thread: dequeues requests, runs curl, enqueues responses

#include <stddef.h>

// Response handed to the callback (valid only during the callback).
struct ra_http_resp {
	int http_status;        // 0 = curl failed / no status
	char* body;
	size_t body_len;
};

typedef void (*ra_http_callback_t)(const ra_http_resp* resp, void* callback_data);

// Start the worker thread (pinned to cpu0). Idempotent.
void ra_http_init(void);

// Enqueue a request. post_data == NULL -> GET. The callback runs from
// ra_http_poll() on the polling thread.
void ra_http_request(const char* url, const char* post_data,
	const char* content_type, ra_http_callback_t callback, void* callback_data);

// Queue a locally made response (dry run): dispatched by the next
// ra_http_poll() exactly like a real one. Takes ownership of body (malloc'd).
void ra_http_inject_response(int http_status, char* body,
	ra_http_callback_t callback, void* callback_data);

// Dispatch completed responses. Returns the number of callbacks run.
int ra_http_poll(void);

// Requests queued or executing + responses not yet dispatched.
int ra_http_pending(void);

// User-Agent for every request. Call before the first request.
void ra_http_set_user_agent(const char* user_agent);
