// Host test for the RetroAchievements module (ra/ra_dc.cpp + rcheevos).
//
//   ra_test [--wav file] [--gameid] [image.gdi ...]
//   ra_test --flow dry|live image.gdi tests/ra_fake_curl.sh
//
// Always: memory callback mapping (RA address -> DC system RAM), dry-run
// fake responses parsed by rcheevos' own response parsers, chime loader.
// Each image given: rcheevos Dreamcast hash (.gdi via the default cdreader).
// --gameid: also asks retroachievements.org which game each hash is
// (r=gameid, no login; one request per image, through ra_http + curl).

#include "ra_dc.h"
#include "ra_http.h"
#include "ra_osd.h"   // ra_list_data

#include "rc_api_runtime.h"
#include "rc_api_info.h"
#include "rc_consoles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_memory()
{
	const uint32_t size = 16 * 1024 * 1024;
	uint8_t probe[8];

	CHECK(ra_test_read_memory(0, probe, 1) == 0);   // before ra_set_ram

	std::vector<uint8_t> ram(size);
	for (uint32_t i = 0; i < size; i++)
		ram[i] = (uint8_t)(i * 7 + (i >> 8) + 3);
	// the SH4 stores 0x11223344 at 0x8C000100 -> little-endian bytes in RAM
	ram[0x100] = 0x44; ram[0x101] = 0x33; ram[0x102] = 0x22; ram[0x103] = 0x11;
	ra_set_ram(ram.data(), size);

	CHECK(ra_test_read_memory(0, probe, 1) == 1 && probe[0] == ram[0]);
	CHECK(ra_test_read_memory(0x123456, probe, 4) == 4 && !memcmp(probe, &ram[0x123456], 4));
	CHECK(ra_test_read_memory(0xABCDE, probe, 2) == 2 && !memcmp(probe, &ram[0xABCDE], 2));
	CHECK(ra_test_read_memory(0x100, probe, 4) == 4);
	uint32_t v = probe[0] | (probe[1] << 8) | (probe[2] << 16) | ((uint32_t)probe[3] << 24);
	CHECK(v == 0x11223344);   // rcheevos assembles multi-byte reads little-endian
	CHECK(ra_test_read_memory(0xFFFFFF, probe, 1) == 1 && probe[0] == ram[0xFFFFFF]);
	CHECK(ra_test_read_memory(0xFFFFFE, probe, 4) == 2 && !memcmp(probe, &ram[0xFFFFFE], 2));   // partial at the end
	CHECK(ra_test_read_memory(0x1000000, probe, 1) == 0);   // past the 16 MB region
	CHECK(ra_test_read_memory(0xFFFFFFFF, probe, 4) == 0);
	CHECK(ra_test_read_memory(0x0C000000, probe, 1) == 0);  // physical addresses are not RA addresses

	ra_set_ram(nullptr, 0);
	printf("memory mapping: ok\n");
}

static void test_dryrun()
{
	char* body = ra_test_dryrun_response("r=awardachievement&u=user&t=TOKEN&a=123456&h=0&m=abcdef&v=0123");
	CHECK(body != NULL);
	if (body) {
		rc_api_server_response_t sr;
		memset(&sr, 0, sizeof(sr));
		sr.body = body;
		sr.body_length = strlen(body);
		sr.http_status_code = 200;
		rc_api_award_achievement_response_t resp;
		int r = rc_api_process_award_achievement_server_response(&resp, &sr);
		CHECK(r == RC_OK);
		CHECK(resp.response.succeeded);
		CHECK(resp.awarded_achievement_id == 123456);
		CHECK(resp.achievements_remaining == (uint32_t)-1);
		rc_api_destroy_award_achievement_response(&resp);
		printf("dry run award: %s\n", body);
		free(body);
	}

	body = ra_test_dryrun_response("r=submitlbentry&u=user&t=TOKEN&i=77&s=12345&m=abcdef&v=0123");
	CHECK(body != NULL);
	if (body) {
		rc_api_server_response_t sr;
		memset(&sr, 0, sizeof(sr));
		sr.body = body;
		sr.body_length = strlen(body);
		sr.http_status_code = 200;
		rc_api_submit_lboard_entry_response_t resp;
		int r = rc_api_process_submit_lboard_entry_server_response(&resp, &sr);
		CHECK(r == RC_OK);
		CHECK(resp.submitted_score == 12345);
		CHECK(resp.new_rank == 1 && resp.num_entries == 1);
		rc_api_destroy_submit_lboard_entry_response(&resp);
		printf("dry run leaderboard: %s\n", body);
		free(body);
	}

	CHECK(ra_test_dryrun_response("r=login2&u=user&p=secret") == NULL);
	CHECK(ra_test_dryrun_response("r=startsession&u=user&t=TOKEN&g=1") == NULL);
	CHECK(ra_test_dryrun_response("r=ping&u=user&t=TOKEN&g=1") == NULL);
	CHECK(ra_test_dryrun_response(NULL) == NULL);
	printf("dry run: ok\n");
}

static void test_chime(const char* wav)
{
	int frames = ra_test_load_chime(wav, 70);
	CHECK(frames > 0);
	printf("chime %s: %d frames @48 kHz (%.3f s)\n", wav, frames, frames / 48000.0);

	// mixer: saturates and stops after the chime
	RaChimeMixer m;
	m.poll();
	CHECK(!m.active());
	ra_chime_seq.fetch_add(1);
	m.poll();
	CHECK(m.active() && m.len == (uint32_t)frames);
	uint32_t n = 0;
	while (m.active()) {
		uint32_t w = m.mix(0x7FFF7FFF);   // full-scale input: must not wrap
		int16_t l = (int16_t)(w & 0xFFFF), r = (int16_t)(w >> 16);
		if (l < 0 || r < 0) {
			CHECK(!"mix wrapped");
			break;
		}
		n++;
	}
	CHECK(n == (uint32_t)frames);
}

struct GameIdResult {
	int done;
	int status;
	char body[256];
};

static void gameid_done(const ra_http_resp* resp, void* ud)
{
	GameIdResult* r = (GameIdResult*)ud;
	r->status = resp->http_status;
	snprintf(r->body, sizeof(r->body), "%.*s", (int)resp->body_len, resp->body ? resp->body : "");
	r->done = 1;
}

static void lookup_game(const char* hash)
{
	rc_api_resolve_hash_request_t req;
	memset(&req, 0, sizeof(req));
	req.game_hash = hash;
	rc_api_request_t api;
	if (rc_api_init_resolve_hash_request(&api, &req) != RC_OK) {
		printf("gameid: request build failed\n");
		return;
	}
	GameIdResult r;
	memset(&r, 0, sizeof(r));
	ra_http_request(api.url, api.post_data, api.content_type, gameid_done, &r);
	for (int i = 0; i < 400 && !r.done; i++) {
		ra_http_poll();
		usleep(100000);
	}
	printf("gameid %s -> HTTP %d %s\n", hash, r.status, r.done ? r.body : "(timeout)");
	rc_api_destroy_request(&api);
}

// ---------------------------------------------------------------------------
// --flow dry|live <image> <fake curl>: the real RA thread end to end against
// tests/ra_fake_curl.sh (login -> hash -> load -> do_frame per signaled
// frame -> two unlocks -> shutdown flush). dry: submissions must never reach
// the "server"; live: they must, even though the fake server takes 1 s per
// award and shutdown is requested right after the second unlock.
// ---------------------------------------------------------------------------

static int count_lines(const char* path, const char* needle)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return 0;
	char line[1024];
	int n = 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, needle))
			n++;
	fclose(f);
	return n;
}

static bool run_frames_until(int ms, bool (*cond)())
{
	for (int t = 0; t < ms; t += 16) {
		ra_frame_hook(RA_FRAME_VBLANK);   // what spg.cpp does every vblank
		if (cond())
			return true;
		usleep(16000);
	}
	return cond();
}

static int unlocks_seen = 0;
static bool game_loaded = false;
static void drain_notifications()
{
	ra_notification_t n;
	while (ra_notification_pop(&n)) {
		printf("notification #%u kind=%d id=%u pts=%u badge=%s title=\"%s\" text=\"%s\"\n",
			n.seq, n.kind, n.id, n.points, n.badge, n.title, n.text);
		if (n.kind == RA_NOTIF_ACHIEVEMENT)
			unlocks_seen++;
		if (n.kind == RA_NOTIF_GAME_LOADED)
			game_loaded = true;
	}
}

static int flow_test(bool dry, const char* image, const char* fake_curl)
{
	char dir[] = "/tmp/ra_flow_XXXXXX";
	if (!mkdtemp(dir))
		return 1;
	char cfg[256], unlog[256], curllog[256];
	snprintf(cfg, sizeof(cfg), "%s/retroachievements.cfg", dir);
	snprintf(unlog, sizeof(unlog), "%s/logs/ra_unlocks.log", dir);
	snprintf(curllog, sizeof(curllog), "%s/curl.log", dir);
	FILE* f = fopen(cfg, "w");
	fprintf(f, "# test\nusername=tester\npassword=secret\nhardcore=1\ndebug=0\n");
	fclose(f);
	setenv("MINICAST_RA_CFG", cfg, 1);
	setenv("MINICAST_RA_CURL", fake_curl, 1);
	setenv("MINICAST_RA_UNLOCK_LOG", unlog, 1);
	setenv("MINICAST_RA_WAV", "/nonexistent.wav", 1);
	setenv("MINICAST_RA_DRYRUN", dry ? "1" : "0", 1);
	setenv("FAKE_CURL_LOG", curllog, 1);

	const uint32_t size = 16 * 1024 * 1024;
	std::vector<uint8_t> ram(size, 0);

	ra_init(image);
	ra_set_ram(ram.data(), size);   // as libswirl's Init does, after ra_init

	CHECK(run_frames_until(20000, [] { drain_notifications(); return game_loaded; }));
	CHECK(ra_frame_source.load() == RA_FRAME_VBLANK);
	CHECK(ra_game_ready());

	// achievement list snapshot for the overlay: nothing unlocked yet
	CHECK(ra_list_request());
	ra_list_data* list = NULL;
	for (int i = 0; i < 100 && !list; i++) {
		usleep(10000);
		list = ra_list_take();
	}
	CHECK(list && list->count == 2 && list->num_unlocked == 0 && list->points_total == 15);
	if (list) {
		CHECK(!strcmp(list->game, "Fake Game"));
		CHECK(!strcmp(list->items[0].title, "Byte") || !strcmp(list->items[0].title, "Dword"));
		ra_list_free(list);
	}

	// conditions false for a while (achievements must see false before true)
	run_frames_until(300, [] { return false; });
	ram[0x100] = 1;
	CHECK(run_frames_until(3000, [] { drain_notifications(); return unlocks_seen >= 1; }));
	// one unlocked: it sorts first
	CHECK(ra_list_request());
	list = NULL;
	for (int i = 0; i < 100 && !list; i++) {
		usleep(10000);
		list = ra_list_take();
	}
	CHECK(list && list->count == 2 && list->num_unlocked == 1 && list->points_unlocked == 5);
	if (list) {
		CHECK(list->items[0].unlocked && list->items[0].id == 101 && !strcmp(list->items[0].title, "Byte"));
		CHECK(!list->items[1].unlocked && list->items[1].id == 102);
		CHECK(!strcmp(list->items[1].desc, "RA 0x200 becomes 0x11223344 (little-endian)"));
		ra_list_free(list);
	}
	ram[0x200] = 0x44; ram[0x201] = 0x33; ram[0x202] = 0x22; ram[0x203] = 0x11;
	CHECK(run_frames_until(3000, [] { drain_notifications(); return unlocks_seen >= 2; }));

	// quit right after the unlock: the (slow) award must still go out
	ra_shutdown(3000);
	ra_frame_hook(RA_FRAME_VBLANK);   // after shutdown: must be a no-op
	CHECK(ra_frame_source.load() == RA_FRAME_OFF);

	CHECK(unlocks_seen == 2);
	CHECK(count_lines(unlog, "achievement 101") == 1);
	CHECK(count_lines(unlog, "achievement 102") == 1);
	CHECK(count_lines(curllog, "login2") == 1);
	CHECK(count_lines(curllog, "achievementsets") == 1);
	int awards = count_lines(curllog, "awardachievement");
	printf("flow %s: awards that reached the server: %d\n", dry ? "dry" : "live", awards);
	CHECK(awards == (dry ? 0 : 2));
	if (dry)
		CHECK(count_lines(unlog, "DRY RUN") == 2);
	printf("---- %s\n", unlog);
	fflush(stdout);
	if (system((std::string("cat '") + unlog + "'; echo ---- curl log; cat '" + curllog + "'").c_str()) != 0)
		printf("(could not show the logs)\n");
	printf(failures ? "flow %s: %d FAILURE(S)\n" : "flow %s: ok\n", dry ? "dry" : "live", failures);
	return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
	const char* wav = NULL;
	bool gameid = false;
	std::vector<const char*> images;
	if (argc == 5 && !strcmp(argv[1], "--flow"))
		return flow_test(!strcmp(argv[2], "dry"), argv[3], argv[4]);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--wav") && i + 1 < argc)
			wav = argv[++i];
		else if (!strcmp(argv[i], "--gameid"))
			gameid = true;
		else
			images.push_back(argv[i]);
	}

	test_memory();
	test_dryrun();
	if (wav)
		test_chime(wav);

	if (gameid) {
		ra_http_init();
		ra_http_set_user_agent("DreamSTer_MiSTer/0.1 rcheevos/12.4 (host test)");
	}
	for (const char* img : images) {
		char hash[33];
		int ok = ra_test_hash(img, hash);
		printf("hash %s: %s\n", ok ? hash : "FAILED", img);
		CHECK(ok);
		if (ok && gameid)
			lookup_game(hash);
	}

	printf(failures ? "%d FAILURE(S)\n" : "all ok\n", failures);
	return failures ? 1 : 0;
}
