// ra_http.cpp -- asynchronous HTTP for RetroAchievements in minicast
//
// Adapted from the MiSTer RetroAchievements fork of Main_MiSTer
// (ra_http.cpp): the worker thread, the two fixed-size ring queues, the
// curl flags (-k, -L, --max-time, -w '\n%{http_code}' status trailer) and the
// status/body split are that code. Changed for minicast:
//   - curl is started with posix_spawnp() and an argv array instead of
//     popen() + a shell command line: no quoting, and the POST body goes in
//     through a stdin pipe instead of a `printf '<body>' |` (which put the
//     password / token on a command line visible in ps).
//   - the child gets an empty signal mask and default dispositions (every
//     minicast helper thread blocks all signals, and spawned children would
//     inherit that), and every inherited descriptor above 2 is closed (some
//     /dev/mem mappings in minicast are opened without O_CLOEXEC).
//   - the worker blocks all signals (the polly2 module SIGUSR1s /dev/polly2
//     holders; SIGTERM is sigwait()ed by main.cpp), runs on cpu0 (cpu1 is the
//     SH4 thread) at nice 5, and curl inherits both, so a TLS handshake never
//     competes with the JIT.

#include "ra_http.h"
#include "ra_dc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>

extern char** environ;

#define HTTP_LOG(fmt, ...) ra_log_debug("HTTP: " fmt, ##__VA_ARGS__)

struct ra_http_req {
	char* url;
	char* post_data;
	char* content_type;
	ra_http_callback_t callback;
	void* callback_data;
};

struct ra_http_done {
	ra_http_resp resp;
	ra_http_callback_t callback;
	void* callback_data;
};

// ---------------------------------------------------------------------------
// Thread-safe queues (fixed-size rings, as in the fork)
// ---------------------------------------------------------------------------

static const int QUEUE_CAP = 16;

static ra_http_req s_req_queue[QUEUE_CAP];
static int s_req_head = 0, s_req_tail = 0;
static pthread_mutex_t s_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_req_cond = PTHREAD_COND_INITIALIZER;
static int s_executing = 0;          // worker is running curl (under s_req_mutex)

static ra_http_done s_resp_queue[QUEUE_CAP];
static int s_resp_head = 0, s_resp_tail = 0;
static pthread_mutex_t s_resp_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_t s_worker;
static bool s_inited = false;
static char s_user_agent[256] = "minicast";

// ---------------------------------------------------------------------------
// curl execution
// ---------------------------------------------------------------------------

struct buf {
	char* data;
	size_t len;
	size_t cap;
};

static void buf_init(buf* b)
{
	b->cap = 4096;
	b->data = (char*)malloc(b->cap);
	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
}

static void buf_append(buf* b, const char* chunk, size_t n)
{
	if (!b->data)
		return;
	while (b->len + n + 1 > b->cap) {
		b->cap *= 2;
		char* tmp = (char*)realloc(b->data, b->cap);
		if (!tmp) {
			free(b->data);
			b->data = NULL;
			return;
		}
		b->data = tmp;
	}
	memcpy(b->data + b->len, chunk, n);
	b->len += n;
	b->data[b->len] = '\0';
}

// Close every descriptor the child would otherwise inherit (anything above
// stderr that is still open in this process when the request starts).
static void add_close_inherited(posix_spawn_file_actions_t* fa, int keep0, int keep1)
{
	DIR* d = opendir("/proc/self/fd");
	if (!d)
		return;
	int dfd = dirfd(d);
	while (struct dirent* e = readdir(d)) {
		char* end;
		long fd = strtol(e->d_name, &end, 10);
		if (*end || end == e->d_name || fd <= 2 || fd == dfd || fd == keep0 || fd == keep1)
			continue;
		// already-closed fds are ignored by glibc's close action (EBADF)
		posix_spawn_file_actions_addclose(fa, (int)fd);
	}
	closedir(d);
}

static const char* curl_binary(void)
{
	const char* c = getenv("MINICAST_RA_CURL");
	return (c && c[0]) ? c : "curl";
}

static void fail_resp(ra_http_resp* resp, const char* why)
{
	resp->http_status = 0;
	resp->body = strdup(why);
	resp->body_len = resp->body ? strlen(resp->body) : 0;
}

static void execute_request(const ra_http_req* req, ra_http_resp* resp)
{
	resp->http_status = 0;
	resp->body = NULL;
	resp->body_len = 0;

	const bool post = req->post_data && req->post_data[0];

	// Log with the secrets masked: &t= (token) and &p= (password)
	if (post) {
		char masked[160];
		snprintf(masked, sizeof(masked), "%s", req->post_data);
		static const char* keys[] = { "&t=", "&p=", NULL };
		for (int k = 0; keys[k]; k++) {
			char* pos = strstr(masked, keys[k]);
			if (pos) {
				char* val = pos + 3;
				char* end = val;
				while (*end && *end != '&')
					end++;
				memmove(val + 3, end, strlen(end) + 1);
				memcpy(val, "***", 3);
			}
		}
		HTTP_LOG("POST %s [%s%s]", req->url, masked, strlen(req->post_data) >= sizeof(masked) - 1 ? "..." : "");
	} else {
		HTTP_LOG("GET %s", req->url);
	}

	const char* ct = (req->content_type && req->content_type[0])
		? req->content_type : "application/x-www-form-urlencoded";
	char ct_hdr[160];
	snprintf(ct_hdr, sizeof(ct_hdr), "Content-Type: %s", ct);

	const char* argv[24];
	int a = 0;
	argv[a++] = curl_binary();
	argv[a++] = "-sS";                 // quiet, but errors still go to stderr (the game log)
	argv[a++] = "-k";                  // MiSTer's CA bundle is outdated
	argv[a++] = "-L";
	argv[a++] = "--connect-timeout";
	argv[a++] = "10";
	argv[a++] = "--max-time";
	argv[a++] = "30";
	argv[a++] = "-w";
	argv[a++] = "\n%{http_code}";
	argv[a++] = "-A";
	argv[a++] = s_user_agent;
	if (post) {
		argv[a++] = "-X";
		argv[a++] = "POST";
		argv[a++] = "-H";
		argv[a++] = ct_hdr;
		argv[a++] = "--data-binary";
		argv[a++] = "@-";
	}
	argv[a++] = req->url;
	argv[a] = NULL;

	int out[2] = { -1, -1 }, in[2] = { -1, -1 };
	if (pipe2(out, O_CLOEXEC) != 0) {
		fail_resp(resp, "pipe failed");
		return;
	}
	if (post && pipe2(in, O_CLOEXEC) != 0) {
		close(out[0]);
		close(out[1]);
		fail_resp(resp, "pipe failed");
		return;
	}

	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	if (post)
		posix_spawn_file_actions_adddup2(&fa, in[0], 0);
	else
		posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, out[1], 1);
	add_close_inherited(&fa, -1, -1);   // the pipe ends are O_CLOEXEC already

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	sigset_t none, all;
	sigemptyset(&none);
	sigfillset(&all);
	posix_spawnattr_setsigmask(&attr, &none);
	posix_spawnattr_setsigdefault(&attr, &all);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

	pid_t pid = -1;
	int err = posix_spawnp(&pid, argv[0], &fa, &attr, (char* const*)argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	posix_spawnattr_destroy(&attr);
	close(out[1]);
	if (post)
		close(in[0]);

	if (err != 0) {
		HTTP_LOG("ERROR: spawning %s failed: %s", argv[0], strerror(err));
		close(out[0]);
		if (post)
			close(in[1]);
		fail_resp(resp, "curl spawn failed");
		return;
	}

	if (post) {
		// Small bodies (< 64 KB pipe buffer), and curl reads all of @- before
		// connecting. SIGPIPE is blocked in this thread: EPIPE if curl died.
		const char* p = req->post_data;
		size_t left = strlen(p);
		while (left) {
			ssize_t w = write(in[1], p, left);
			if (w < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			p += w;
			left -= (size_t)w;
		}
		close(in[1]);
	}

	buf output;
	buf_init(&output);
	char chunk[4096];
	for (;;) {
		ssize_t n = read(out[0], chunk, sizeof(chunk));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		buf_append(&output, chunk, (size_t)n);
	}
	close(out[0]);

	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

	if (!output.data || output.len == 0) {
		HTTP_LOG("ERROR: curl returned no output (exit=%d)", exit_code);
		free(output.data);
		fail_resp(resp, "curl returned no output");
		return;
	}

	// HTTP status is the last line (-w '\n%{http_code}')
	char* last_nl = NULL;
	for (size_t i = output.len; i > 0; i--) {
		if (output.data[i - 1] == '\n') {
			last_nl = output.data + i - 1;
			break;
		}
	}
	if (last_nl && last_nl < output.data + output.len - 1) {
		resp->http_status = atoi(last_nl + 1);
		*last_nl = '\0';
		resp->body_len = (size_t)(last_nl - output.data);
	} else {
		resp->http_status = 0;
		resp->body_len = output.len;
	}
	resp->body = output.data;   // ownership transferred

	HTTP_LOG("response: HTTP %d, %zu bytes (curl exit %d)", resp->http_status, resp->body_len, exit_code);
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

static void* http_worker(void*)
{
	sigset_t all;
	sigfillset(&all);
	pthread_sigmask(SIG_BLOCK, &all, NULL);

	// cpu0 (the SH4 thread owns cpu1), nice 5: curl children inherit both
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(0, &cpuset);
	sched_setaffinity(0, sizeof(cpuset), &cpuset);
	setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 5);

	for (;;) {
		pthread_mutex_lock(&s_req_mutex);
		while (s_req_head == s_req_tail)
			pthread_cond_wait(&s_req_cond, &s_req_mutex);
		ra_http_req req = s_req_queue[s_req_tail % QUEUE_CAP];
		s_req_tail++;
		s_executing = 1;
		pthread_mutex_unlock(&s_req_mutex);

		ra_http_done done;
		memset(&done, 0, sizeof(done));
		execute_request(&req, &done.resp);
		done.callback = req.callback;
		done.callback_data = req.callback_data;

		free(req.url);
		free(req.post_data);
		free(req.content_type);

		pthread_mutex_lock(&s_resp_mutex);
		if (s_resp_head - s_resp_tail < QUEUE_CAP) {
			s_resp_queue[s_resp_head % QUEUE_CAP] = done;
			s_resp_head++;
		} else {
			HTTP_LOG("WARNING: response queue full, dropping response");
			free(done.resp.body);
		}
		pthread_mutex_unlock(&s_resp_mutex);

		pthread_mutex_lock(&s_req_mutex);
		s_executing = 0;
		pthread_mutex_unlock(&s_req_mutex);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void ra_http_init(void)
{
	if (s_inited)
		return;
	// The worker lives until the process exits: it may sit in a 30 s curl
	// when minicast quits, and _exit() takes it down with everything else.
	pthread_t th;
	if (pthread_create(&th, NULL, http_worker, NULL) != 0) {
		ra_log("HTTP worker thread failed to start");
		return;
	}
	pthread_detach(th);
	s_worker = th;
	s_inited = true;
}

static void queue_error_response(const char* why, ra_http_callback_t callback, void* callback_data)
{
	ra_http_done err;
	memset(&err, 0, sizeof(err));
	fail_resp(&err.resp, why);
	err.callback = callback;
	err.callback_data = callback_data;
	pthread_mutex_lock(&s_resp_mutex);
	if (s_resp_head - s_resp_tail < QUEUE_CAP) {
		s_resp_queue[s_resp_head % QUEUE_CAP] = err;
		s_resp_head++;
	} else {
		free(err.resp.body);
	}
	pthread_mutex_unlock(&s_resp_mutex);
}

void ra_http_inject_response(int http_status, char* body,
	ra_http_callback_t callback, void* callback_data)
{
	ra_http_done d;
	memset(&d, 0, sizeof(d));
	d.resp.http_status = http_status;
	d.resp.body = body;
	d.resp.body_len = body ? strlen(body) : 0;
	d.callback = callback;
	d.callback_data = callback_data;
	pthread_mutex_lock(&s_resp_mutex);
	if (s_resp_head - s_resp_tail < QUEUE_CAP) {
		s_resp_queue[s_resp_head % QUEUE_CAP] = d;
		s_resp_head++;
		d.callback = NULL;
	}
	pthread_mutex_unlock(&s_resp_mutex);
	if (d.callback) {
		// queue full: answer right away rather than leave rc_client waiting
		d.callback(&d.resp, d.callback_data);
		free(body);
	}
}

void ra_http_request(const char* url, const char* post_data,
	const char* content_type, ra_http_callback_t callback, void* callback_data)
{
	if (!s_inited) {
		// the caller must still get its callback, from the next poll
		queue_error_response("HTTP not initialized", callback, callback_data);
		return;
	}

	pthread_mutex_lock(&s_req_mutex);
	if (s_req_head - s_req_tail >= QUEUE_CAP) {
		pthread_mutex_unlock(&s_req_mutex);
		HTTP_LOG("WARNING: request queue full, rejecting %s", url);
		queue_error_response("Request queue full", callback, callback_data);
		return;
	}
	ra_http_req* r = &s_req_queue[s_req_head % QUEUE_CAP];
	r->url = strdup(url ? url : "");
	r->post_data = post_data ? strdup(post_data) : NULL;
	r->content_type = content_type ? strdup(content_type) : NULL;
	r->callback = callback;
	r->callback_data = callback_data;
	s_req_head++;
	pthread_cond_signal(&s_req_cond);
	pthread_mutex_unlock(&s_req_mutex);
}

int ra_http_poll(void)
{
	int dispatched = 0;
	pthread_mutex_lock(&s_resp_mutex);
	while (s_resp_head != s_resp_tail) {
		ra_http_done done = s_resp_queue[s_resp_tail % QUEUE_CAP];
		s_resp_tail++;
		pthread_mutex_unlock(&s_resp_mutex);

		if (done.callback)
			done.callback(&done.resp, done.callback_data);
		free(done.resp.body);
		dispatched++;

		pthread_mutex_lock(&s_resp_mutex);
	}
	pthread_mutex_unlock(&s_resp_mutex);
	return dispatched;
}

int ra_http_pending(void)
{
	pthread_mutex_lock(&s_req_mutex);
	int req_pending = (s_req_head - s_req_tail) + s_executing;
	pthread_mutex_unlock(&s_req_mutex);

	pthread_mutex_lock(&s_resp_mutex);
	int resp_pending = s_resp_head - s_resp_tail;
	pthread_mutex_unlock(&s_resp_mutex);

	return req_pending + resp_pending;
}

void ra_http_set_user_agent(const char* user_agent)
{
	if (user_agent && user_agent[0])
		snprintf(s_user_agent, sizeof(s_user_agent), "%s", user_agent);
}
