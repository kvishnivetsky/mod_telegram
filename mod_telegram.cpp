/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2024, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 *
 * mod_telegram.cpp -- Telegram Endpoint Module
 *
 * Bridges FreeSWITCH calls to/from Telegram voice calls via TDLib.
 * Dial string format: telegram/<profile>/<user_id>
 * Config file:        autoload_configs/telegram.conf.xml
 */
#include <switch.h>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.hpp>
#include <rtc_base/logging.h>
#include <tgcalls/Instance.h>
#include <tgcalls/InstanceImpl.h>
#include <tgcalls/v2/InstanceV2Impl.h>
#include <tgcalls/v2/InstanceV2ReferenceImpl.h>
#include <tgcalls/FakeAudioDeviceModule.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <cinttypes>
#include <unordered_map>
#include <string>

#define DC_SERVER_TEST "149.154.167.40:443"
#define DC_SERVER_PROD "149.154.167.50:443"

/* Audio format shared by the FreeSWITCH codec and the tgcalls ADM. */
#define TG_AUDIO_RATE     48000
#define TG_FRAME_MS       20
#define TG_FRAME_SAMPLES  (TG_AUDIO_RATE * TG_FRAME_MS / 1000)   /* 960  */
#define TG_FRAME_BYTES    (TG_FRAME_SAMPLES * 2)                   /* 1920 */
#define TG_ADM_SAMPLES    (TG_AUDIO_RATE * 10 / 1000)             /* 480, 10 ms ADM tick */
#define TG_ADM_BYTES      (TG_ADM_SAMPLES * 2)                    /* 960 bytes */

#ifdef _WIN32
#  include <windows.h>
#  define TG_PATH_MAX MAX_PATH
#else
#  include <limits.h>
#  define TG_PATH_MAX PATH_MAX
#endif

/* Module entry points must have C linkage so FreeSWITCH can locate them via dlsym. */
SWITCH_BEGIN_EXTERN_C

SWITCH_MODULE_LOAD_FUNCTION(mod_telegram_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_telegram_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_telegram_runtime);
SWITCH_MODULE_DEFINITION(mod_telegram, mod_telegram_load, mod_telegram_shutdown, mod_telegram_runtime);

SWITCH_END_EXTERN_C

static switch_endpoint_interface_t *telegram_endpoint_interface;
static switch_memory_pool_t        *module_pool = NULL;
static int                          running     = 1;
static td::ClientManager           *telegram_client_manager = nullptr;

/* Monotonic counter for TDLib request IDs — 0 means fire-and-forget. */
static std::atomic<uint64_t> tg_next_request_id{1};
static uint64_t tg_new_request_id() { return tg_next_request_id.fetch_add(1); }

/* ── Logging hooks ──────────────────────────────────────────────────────────── */

static switch_log_level_t tg_tdlib_level(int v)
{
	if (v <= 0) return SWITCH_LOG_CRIT;
	if (v == 1) return SWITCH_LOG_ERROR;
	if (v == 2) return SWITCH_LOG_WARNING;
	if (v == 3) return SWITCH_LOG_INFO;
	return SWITCH_LOG_DEBUG;
}

static switch_log_level_t tg_rtc_level(rtc::LoggingSeverity s)
{
	switch (s) {
	case rtc::LS_ERROR:   return SWITCH_LOG_ERROR;
	case rtc::LS_WARNING: return SWITCH_LOG_WARNING;
	case rtc::LS_INFO:    return SWITCH_LOG_INFO;
	default:              return SWITCH_LOG_DEBUG;
	}
}

static void tdlib_log_callback(int verbosity_level, const char *message)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, tg_tdlib_level(verbosity_level), "[TDLib] %s", message);
}

class TelegramRtcLogSink : public rtc::LogSink {
public:
	void OnLogMessage(const std::string &message) override {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "[tgcalls] %s", message.c_str());
	}
	void OnLogMessage(const std::string &message, rtc::LoggingSeverity severity) override {
		switch_log_printf(SWITCH_CHANNEL_LOG, tg_rtc_level(severity), "[tgcalls] %s", message.c_str());
	}
};
static TelegramRtcLogSink g_rtc_log_sink;

/* ── Channel flags ──────────────────────────────────────────────────────────── */

typedef enum {
	TFLAG_IO       = (1 << 0),
	TFLAG_INBOUND  = (1 << 1),
	TFLAG_OUTBOUND = (1 << 2),
	TFLAG_DTMF     = (1 << 3),
	TFLAG_VOICE    = (1 << 4),
	TFLAG_HANGUP   = (1 << 5),
	TFLAG_LINEAR   = (1 << 6),
	TFLAG_CODEC    = (1 << 7),
	TFLAG_BREAK    = (1 << 8)
} TFLAGS;

/* ── Profile ────────────────────────────────────────────────────────────────── */

typedef enum {
	TG_PROFILE_STATUS_DOWN,
	TG_PROFILE_STATUS_CONNECTING,
	TG_PROFILE_STATUS_UP,
	TG_PROFILE_STATUS_ERROR
} tg_profile_status_t;

typedef struct {
	switch_bool_t                tg_profile_enabled;
	char                        *tg_profile_name;
	tg_profile_status_t          tg_profile_status;
	char                        *tg_profile_server;
	char                        *tg_profile_api_id;
	char                        *tg_profile_api_hash;
	char                        *tg_profile_database_dir;
	char                        *tg_profile_files_dir;
	/* NULL = use TDLib's allow_p2p; "true"/"false" = override per-profile */
	char                        *tg_profile_allow_p2p;
	/* Optional: colon-separated list of allowed tgcalls versions, e.g. "2.7.7:5.0.0".
	 * NULL = all available versions allowed. Overridable per-call via telegram_versions. */
	char                        *tg_profile_versions;
	/* Optional: auto-submit phone number and 2FA password during authentication */
	char                        *tg_profile_login;
	char                        *tg_profile_password;
	/* dest_proto passed to switch_core_chat_deliver for inbound text messages (default: "telegram") */
	char                        *tg_inbound_dest_proto;
	td::ClientManager::ClientId  tg_client_id;
	switch_hash_t               *calls;
	switch_mutex_t              *calls_mutex;
	char                         pending_outgoing_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
} tg_profile_t;

typedef tg_profile_t *tg_profile_p;

static void tg_profile_set_default_dirs(tg_profile_t *profile)
{
	char path[TG_PATH_MAX];

	if (!profile->tg_profile_database_dir) {
		snprintf(path, sizeof(path), "%s/telegram/%s",
				 SWITCH_GLOBAL_dirs.db_dir, profile->tg_profile_name);
		profile->tg_profile_database_dir = strdup(path);
	}
	if (!profile->tg_profile_files_dir) {
		snprintf(path, sizeof(path), "%s/telegram/%s/files",
				 SWITCH_GLOBAL_dirs.db_dir, profile->tg_profile_name);
		profile->tg_profile_files_dir = strdup(path);
	}

	switch_dir_make_recursive(profile->tg_profile_database_dir, SWITCH_DEFAULT_DIR_PERMS, module_pool);
	switch_dir_make_recursive(profile->tg_profile_files_dir,    SWITCH_DEFAULT_DIR_PERMS, module_pool);
}

static void tg_profile_free(tg_profile_t *p)
{
	if (!p) return;
	if (p->calls) {
		switch_hash_index_t *hi;
		const void          *key;
		void                *val;
		for (hi = switch_core_hash_first(p->calls); hi; hi = switch_core_hash_next(&hi)) {
			switch_core_hash_this(hi, &key, NULL, &val);
			free(val);
		}
		switch_core_hash_destroy(&p->calls);
	}
	switch_safe_free(p->tg_profile_name);
	switch_safe_free(p->tg_profile_server);
	switch_safe_free(p->tg_profile_api_id);
	switch_safe_free(p->tg_profile_api_hash);
	switch_safe_free(p->tg_profile_database_dir);
	switch_safe_free(p->tg_profile_files_dir);
	switch_safe_free(p->tg_profile_allow_p2p);
	switch_safe_free(p->tg_profile_versions);
	switch_safe_free(p->tg_profile_login);
	switch_safe_free(p->tg_profile_password);
	switch_safe_free(p->tg_inbound_dest_proto);
	free(p);
}

/* ── Globals ────────────────────────────────────────────────────────────────── */

static struct {
	int              debug;
	char            *server;
	char            *dialplan;
	char            *context;
	char            *destination;
	unsigned int     flags;
	int              calls;
	switch_mutex_t  *mutex;
	switch_hash_t   *profiles;
	switch_mutex_t  *profiles_mutex;
} globals;

/* ── Pending file downloads ─────────────────────────────────────────────────── */

struct tg_pending_download {
	char    profile_name[64];
	int64_t chat_id;
	int64_t from_user_id;
	int64_t msg_id;
	int32_t file_id;
	char    file_type[32];       /* "document" "photo" "audio" "video" "voice" "video_note" */
	char    file_name[512];      /* original filename for documents */
	char    caption[1024];       /* caption text */
	char    mime_type[128];
	char    audio_title[256];
	char    audio_performer[256];
	int32_t duration;            /* seconds */
	int32_t width;
	int32_t height;
};

static std::unordered_map<std::string, tg_pending_download> tg_pending_downloads;
static switch_mutex_t  *tg_pending_downloads_mutex = nullptr;
static switch_core_db_t *tg_dl_db                 = nullptr;
static switch_mutex_t   *tg_dl_db_mutex           = nullptr;

/* ── Inline query flow tracker ───────────────────────────────────────────────── */
struct tg_inline_flow {
	uint32_t    tg_client_id;
	int64_t     chat_id;
	std::string query;
	enum { SEARCHING, QUERYING } phase;
};
static std::unordered_map<uint64_t, tg_inline_flow> tg_inline_flows;
static switch_mutex_t *tg_inline_flows_mutex = nullptr;

/* ── Profile lookup helpers ─────────────────────────────────────────────────── */

static tg_profile_t *tg_find_profile(const char *name)
{
	tg_profile_t *p = NULL;
	switch_mutex_lock(globals.profiles_mutex);
	p = (tg_profile_t *)switch_core_hash_find(globals.profiles, name);
	switch_mutex_unlock(globals.profiles_mutex);
	return p;
}

static tg_profile_t *tg_find_profile_by_client_id(td::ClientManager::ClientId client_id)
{
	tg_profile_t      *found = NULL;
	switch_hash_index_t *hi;
	const void          *key;
	void                *val;

	switch_mutex_lock(globals.profiles_mutex);
	for (hi = switch_core_hash_first(globals.profiles); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &key, NULL, &val);
		tg_profile_t *p = (tg_profile_t *)val;
		if (p->tg_client_id == client_id) {
			found = p;
			switch_core_hash_next(&hi); /* free iterator */
			break;
		}
	}
	switch_mutex_unlock(globals.profiles_mutex);
	return found;
}

/* ── Pending download DB helpers ────────────────────────────────────────────── */

/* Escape a string for use inside single-quoted SQL literals. */
static std::string tg_sql_str(const char *s)
{
	if (!s) s = "";
	std::string r = "'";
	for (const char *p = s; *p; ++p) {
		if (*p == '\'') r += "''";
		else             r += *p;
	}
	return r + "'";
}

static const char *TG_DL_CREATE_SQL =
	"CREATE TABLE IF NOT EXISTS tg_pending_downloads ("
	"  map_key         TEXT PRIMARY KEY,"
	"  profile_name    TEXT NOT NULL DEFAULT '',"
	"  chat_id         INTEGER NOT NULL DEFAULT 0,"
	"  from_user_id    INTEGER NOT NULL DEFAULT 0,"
	"  msg_id          INTEGER NOT NULL DEFAULT 0,"
	"  file_id         INTEGER NOT NULL DEFAULT 0,"
	"  file_type       TEXT NOT NULL DEFAULT '',"
	"  file_name       TEXT NOT NULL DEFAULT '',"
	"  caption         TEXT NOT NULL DEFAULT '',"
	"  mime_type       TEXT NOT NULL DEFAULT '',"
	"  audio_title     TEXT NOT NULL DEFAULT '',"
	"  audio_performer TEXT NOT NULL DEFAULT '',"
	"  duration        INTEGER NOT NULL DEFAULT 0,"
	"  width           INTEGER NOT NULL DEFAULT 0,"
	"  height          INTEGER NOT NULL DEFAULT 0"
	");";

static void tg_db_open(void)
{
	char path[TG_PATH_MAX];
	snprintf(path, sizeof(path), "%s%smod_telegram.db",
	         SWITCH_GLOBAL_dirs.db_dir, SWITCH_PATH_SEPARATOR);

	if (switch_core_db_open(path, &tg_dl_db) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram: failed to open download DB at %s\n", path);
		tg_dl_db = nullptr;
		return;
	}

	char *err = nullptr;
	switch_core_db_exec(tg_dl_db, TG_DL_CREATE_SQL, nullptr, nullptr, &err);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram: DB create table error: %s\n", err);
		switch_core_db_free(err);
	}
}

static void tg_db_close(void)
{
	if (tg_dl_db) {
		switch_core_db_close(tg_dl_db);
		tg_dl_db = nullptr;
	}
}

static void tg_db_insert(const char *key, const tg_pending_download &pd)
{
	if (!tg_dl_db) return;

	char sql[4096];
	snprintf(sql, sizeof(sql),
		"INSERT OR REPLACE INTO tg_pending_downloads VALUES ("
		"%s, %s, %" PRId64 ", %" PRId64 ", %" PRId64 ", %d, "
		"%s, %s, %s, %s, %s, %s, %d, %d, %d);",
		tg_sql_str(key).c_str(),
		tg_sql_str(pd.profile_name).c_str(),
		pd.chat_id, pd.from_user_id, pd.msg_id, pd.file_id,
		tg_sql_str(pd.file_type).c_str(),
		tg_sql_str(pd.file_name).c_str(),
		tg_sql_str(pd.caption).c_str(),
		tg_sql_str(pd.mime_type).c_str(),
		tg_sql_str(pd.audio_title).c_str(),
		tg_sql_str(pd.audio_performer).c_str(),
		pd.duration, pd.width, pd.height);

	char *err = nullptr;
	switch_mutex_lock(tg_dl_db_mutex);
	switch_core_db_exec(tg_dl_db, sql, nullptr, nullptr, &err);
	switch_mutex_unlock(tg_dl_db_mutex);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram: DB insert error: %s\n", err);
		switch_core_db_free(err);
	}
}

static void tg_db_delete(const char *key)
{
	if (!tg_dl_db) return;

	char sql[256];
	snprintf(sql, sizeof(sql),
		"DELETE FROM tg_pending_downloads WHERE map_key=%s;",
		tg_sql_str(key).c_str());

	char *err = nullptr;
	switch_mutex_lock(tg_dl_db_mutex);
	switch_core_db_exec(tg_dl_db, sql, nullptr, nullptr, &err);
	switch_mutex_unlock(tg_dl_db_mutex);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram: DB delete error: %s\n", err);
		switch_core_db_free(err);
	}
}

/* Callback for SELECT during startup recovery — populates in-memory map only. */
static int tg_db_load_row(void *unused, int argc, char **argv, char **col_names)
{
	if (argc < 15 || !argv[0] || !argv[0][0]) return 0;

	tg_pending_download pd = {};
#define SCOL(dst, idx) if (argv[idx]) strncpy(dst, argv[idx], sizeof(dst) - 1)
#define ICOL(dst, idx) if (argv[idx]) dst = (decltype(dst))atoll(argv[idx])
	SCOL(pd.profile_name,    1);
	ICOL(pd.chat_id,         2);
	ICOL(pd.from_user_id,    3);
	ICOL(pd.msg_id,          4);
	ICOL(pd.file_id,         5);
	SCOL(pd.file_type,       6);
	SCOL(pd.file_name,       7);
	SCOL(pd.caption,         8);
	SCOL(pd.mime_type,       9);
	SCOL(pd.audio_title,    10);
	SCOL(pd.audio_performer,11);
	ICOL(pd.duration,       12);
	ICOL(pd.width,          13);
	ICOL(pd.height,         14);
#undef SCOL
#undef ICOL

	switch_mutex_lock(tg_pending_downloads_mutex);
	tg_pending_downloads[argv[0]] = pd;
	switch_mutex_unlock(tg_pending_downloads_mutex);
	return 0;
}

/* Load persisted rows into map and re-issue downloadFile for each profile's entries. */
static void tg_db_recover_downloads(void)
{
	if (!tg_dl_db) return;

	/* Phase 1: populate in-memory map from DB. */
	char *err = nullptr;
	switch_mutex_lock(tg_dl_db_mutex);
	switch_core_db_exec(tg_dl_db,
		"SELECT map_key,profile_name,chat_id,from_user_id,msg_id,file_id,"
		"file_type,file_name,caption,mime_type,audio_title,audio_performer,"
		"duration,width,height FROM tg_pending_downloads;",
		tg_db_load_row, nullptr, &err);
	switch_mutex_unlock(tg_dl_db_mutex);
	if (err) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram: DB load error: %s\n", err);
		switch_core_db_free(err);
		return;
	}

	/* Phase 2: re-issue downloadFile for each entry whose profile is up. */
	switch_mutex_lock(tg_pending_downloads_mutex);
	for (auto &kv : tg_pending_downloads) {
		tg_pending_download &pd = kv.second;

		switch_mutex_lock(globals.profiles_mutex);
		tg_profile_t *profile = (tg_profile_t *)switch_core_hash_find(
			globals.profiles, pd.profile_name);
		switch_mutex_unlock(globals.profiles_mutex);

		if (!profile || !profile->tg_client_id) continue;

		auto req = td::td_api::make_object<td::td_api::downloadFile>();
		req->file_id_     = pd.file_id;
		req->priority_    = 1;
		req->offset_      = 0;
		req->limit_       = 0;
		req->synchronous_ = false;
		telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: recovery re-issued downloadFile for file_id=%d type=%s\n",
			pd.profile_name, pd.file_id, pd.file_type);
	}
	switch_mutex_unlock(tg_pending_downloads_mutex);
}

/* ── TDLib auth state machine ───────────────────────────────────────────────── */

static void tg_profile_send_parameters(tg_profile_t *profile)
{
	auto params = td::td_api::make_object<td::td_api::setTdlibParameters>();
	params->use_test_dc_            = (strcmp(profile->tg_profile_server, DC_SERVER_PROD) != 0);
	params->database_directory_     = profile->tg_profile_database_dir;
	params->files_directory_        = profile->tg_profile_files_dir;
	params->use_file_database_      = true;
	params->use_chat_info_database_ = true;
	params->use_message_database_   = true;
	params->use_secret_chats_       = false;
	params->api_id_                 = atoi(profile->tg_profile_api_id);
	params->api_hash_               = profile->tg_profile_api_hash;
	params->system_language_code_   = "en";
	params->device_model_           = "FreeSWITCH";
	params->application_version_    = "1.0";
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(params));
}

static void tg_handle_auth_state(tg_profile_t *profile, td::td_api::AuthorizationState &state)
{
	switch (state.get_id()) {
	case td::td_api::authorizationStateWaitTdlibParameters::ID:
		profile->tg_profile_status = TG_PROFILE_STATUS_CONNECTING;
		tg_profile_send_parameters(profile);
		break;

	case td::td_api::authorizationStateWaitPhoneNumber::ID:
		if (!zstr(profile->tg_profile_login)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: auto-submitting phone number from profile config\n",
				profile->tg_profile_name);
			telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
				td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(
					profile->tg_profile_login, nullptr));
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: waiting for phone number — "
				"run: telegram_login %s <phone>\n",
				profile->tg_profile_name, profile->tg_profile_name);
		}
		break;

	case td::td_api::authorizationStateWaitCode::ID:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: waiting for auth code — "
			"run: telegram_code %s <code>\n",
			profile->tg_profile_name, profile->tg_profile_name);
		break;

	case td::td_api::authorizationStateWaitPassword::ID:
		if (!zstr(profile->tg_profile_password)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: auto-submitting 2FA password from profile config\n",
				profile->tg_profile_name);
			telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
				td::td_api::make_object<td::td_api::checkAuthenticationPassword>(
					profile->tg_profile_password));
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: waiting for 2FA password — "
				"run: telegram_password %s <password>\n",
				profile->tg_profile_name, profile->tg_profile_name);
		}
		break;

	case td::td_api::authorizationStateReady::ID:
		profile->tg_profile_status = TG_PROFILE_STATUS_UP;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: connected and ready\n", profile->tg_profile_name);
		break;

	case td::td_api::authorizationStateLoggingOut::ID:
		profile->tg_profile_status = TG_PROFILE_STATUS_CONNECTING;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: logging out\n", profile->tg_profile_name);
		break;

	case td::td_api::authorizationStateClosing::ID:
		profile->tg_profile_status = TG_PROFILE_STATUS_DOWN;
		break;

	case td::td_api::authorizationStateClosed::ID:
		profile->tg_profile_status = TG_PROFILE_STATUS_DOWN;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: closed\n", profile->tg_profile_name);
		break;

	default:
		break;
	}
}

/* ── Call media types ───────────────────────────────────────────────────────── */

struct tg_call_entry {
	char session_uuid[SWITCH_UUID_FORMATTED_LENGTH + 1];
};

/* Receives decoded Telegram audio and writes it into a ring buffer. */
class TelegramRenderer : public tgcalls::FakeAudioDeviceModule::Renderer {
public:
	switch_buffer_t *playout_buf = nullptr;
	std::mutex       playout_mtx;

	bool Render(const tgcalls::AudioFrame &frame) override {
		std::lock_guard<std::mutex> lk(playout_mtx);
		if (!playout_buf) return true;
		{
			static std::atomic<uint32_t> render_count{0};
			uint32_t cnt = ++render_count;
			if (cnt % 500 == 1) {
				int16_t maxval = 0;
				size_t total = frame.num_samples * frame.num_channels;
				for (size_t i = 0; i < total; i++) {
					int16_t v = frame.audio_samples[i];
					if (v < 0) v = -v;
					if (v > maxval) maxval = v;
				}
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					"tgcalls Render: call#%u ch=%zu samples=%zu max_sample=%d\n",
					cnt, frame.num_channels, frame.num_samples, (int)maxval);
			}
		}
		if (frame.num_channels == 1) {
			switch_buffer_write(playout_buf, frame.audio_samples,
				frame.num_samples * sizeof(int16_t));
		} else {
			/* Downmix to mono */
			std::vector<int16_t> mono(frame.num_samples);
			for (size_t i = 0; i < frame.num_samples; i++) {
				int32_t sum = 0;
				for (size_t c = 0; c < frame.num_channels; c++)
					sum += frame.audio_samples[i * frame.num_channels + c];
				mono[i] = (int16_t)(sum / (int32_t)frame.num_channels);
			}
			switch_buffer_write(playout_buf, mono.data(),
				frame.num_samples * sizeof(int16_t));
		}
		return true;
	}
};

/* Provides ADM audio for WebRTC-based instances (e.g. InstanceV2ReferenceImpl).
 * capture_buf is fed by channel_write_frame; Record() drains it 10ms at a time.
 * InstanceImpl-based instances ignore the ADM and use addExternalAudioSamples instead. */
class TelegramRecorder : public tgcalls::FakeAudioDeviceModule::Recorder {
public:
	switch_buffer_t *capture_buf = nullptr;
	std::mutex       capture_mtx;
	int16_t          pcm_buf[TG_ADM_SAMPLES] = {};

	tgcalls::AudioFrame Record() override {
		std::lock_guard<std::mutex> lk(capture_mtx);
		if (capture_buf && switch_buffer_inuse(capture_buf) >= TG_ADM_BYTES) {
			switch_buffer_read(capture_buf, pcm_buf, TG_ADM_BYTES);
		} else {
			memset(pcm_buf, 0, TG_ADM_BYTES);
		}
		return {pcm_buf, TG_ADM_SAMPLES, sizeof(int16_t), 1, TG_AUDIO_RATE, 0, 0};
	}
};

struct TelegramCallMedia {
	std::unique_ptr<tgcalls::Instance> instance;
	std::shared_ptr<TelegramRenderer>  renderer;
	std::shared_ptr<TelegramRecorder>  recorder;
	switch_buffer_t                   *playout_buf = nullptr;
	switch_buffer_t                   *capture_buf = nullptr;
	std::atomic<bool>                  stopping{false};
};

/* ── Private channel object ─────────────────────────────────────────────────── */

struct private_object {
	unsigned int           flags;
	switch_codec_t         read_codec;
	switch_codec_t         write_codec;
	switch_frame_t         read_frame;
	unsigned char          databuf[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_core_session_t *session;
	switch_caller_profile_t *caller_profile;
	switch_mutex_t        *mutex;
	switch_mutex_t        *flag_mutex;
	char                  *chat_id;
	int32_t                tg_call_id;
	TelegramCallMedia     *call_media;
	char                   tg_profile_name[64];
};
typedef struct private_object private_t;

SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_dialplan, globals.dialplan);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_context,  globals.context);
SWITCH_DECLARE_GLOBAL_STRING_FUNC(set_global_server,   globals.server);

/* ── Forward declarations ───────────────────────────────────────────────────── */

static td::td_api::object_ptr<td::td_api::callProtocol> tg_make_protocol();

static switch_status_t   channel_on_init(switch_core_session_t *session);
static switch_status_t   channel_on_hangup(switch_core_session_t *session);
static switch_status_t   channel_on_destroy(switch_core_session_t *session);
static switch_status_t   channel_on_routing(switch_core_session_t *session);
static switch_status_t   channel_on_execute(switch_core_session_t *session);
static switch_status_t   channel_on_exchange_media(switch_core_session_t *session);
static switch_status_t   channel_on_soft_execute(switch_core_session_t *session);
static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session,
													  switch_event_t *var_event,
													  switch_caller_profile_t *outbound_profile,
													  switch_core_session_t **new_session,
													  switch_memory_pool_t **pool,
													  switch_originate_flag_t flags,
													  switch_call_cause_t *cancel_cause);
static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame,
										   switch_io_flag_t flags, int stream_id);
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame,
											switch_io_flag_t flags, int stream_id);
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig);

static void tech_init(private_t *tech_pvt, switch_core_session_t *session)
{
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);
	tech_pvt->read_frame.data   = tech_pvt->databuf;
	tech_pvt->read_frame.buflen = sizeof(tech_pvt->databuf);
	switch_mutex_init(&tech_pvt->mutex,      SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&tech_pvt->flag_mutex, SWITCH_MUTEX_NESTED, pool);
	switch_core_session_set_private(session, tech_pvt);
	tech_pvt->session = session;

	if (switch_core_codec_init(&tech_pvt->read_codec, "L16", NULL, NULL,
				TG_AUDIO_RATE, TG_FRAME_MS, 1,
				SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
				NULL, pool) == SWITCH_STATUS_SUCCESS) {
		switch_core_session_set_read_codec(session, &tech_pvt->read_codec);
	}
	if (switch_core_codec_init(&tech_pvt->write_codec, "L16", NULL, NULL,
				TG_AUDIO_RATE, TG_FRAME_MS, 1,
				SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
				NULL, pool) == SWITCH_STATUS_SUCCESS) {
		switch_core_session_set_write_codec(session, &tech_pvt->write_codec);
	}
	tech_pvt->read_frame.rate    = TG_AUDIO_RATE;
	tech_pvt->read_frame.codec   = &tech_pvt->read_codec;
}

/* ── Channel state handlers ─────────────────────────────────────────────────── */

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch_set_flag_locked(tech_pvt, TFLAG_IO);

	switch_mutex_lock(globals.mutex);
	globals.calls++;
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_routing(switch_core_session_t *session)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
					  "%s CHANNEL ROUTING\n", switch_channel_get_name(channel));

	/* For outbound channels, block here until the Telegram call is answered
	 * (tg_start_media calls switch_channel_mark_answered when callStateReady fires).
	 * Without this wait, CS_EXECUTE starts immediately and the dialplan's media
	 * apps run before the call is established. */
	if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
		while (switch_channel_up_nosig(channel) &&
			   !switch_channel_test_flag(channel, CF_ANSWERED) &&
			   !switch_channel_test_flag(channel, CF_EARLY_MEDIA)) {
			switch_yield(100000);
		}
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_execute(switch_core_session_t *session)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
					  "%s CHANNEL EXECUTE\n", switch_channel_get_name(channel));
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	private_t *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(switch_core_session_get_channel(session) != NULL);

	if (tech_pvt) {
		if (switch_core_codec_ready(&tech_pvt->read_codec))
			switch_core_codec_destroy(&tech_pvt->read_codec);
		if (switch_core_codec_ready(&tech_pvt->write_codec))
			switch_core_codec_destroy(&tech_pvt->write_codec);
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch_clear_flag_locked(tech_pvt, TFLAG_IO);
	switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
					  "%s CHANNEL HANGUP\n", switch_channel_get_name(channel));

	if (tech_pvt->tg_call_id && tech_pvt->tg_profile_name[0]) {
		tg_profile_t *profile = tg_find_profile(tech_pvt->tg_profile_name);
		if (profile) {
			/* Remove from profile's calls hash */
			char id_str[24];
			snprintf(id_str, sizeof(id_str), "%d", tech_pvt->tg_call_id);
			switch_mutex_lock(profile->calls_mutex);
			tg_call_entry *entry = (tg_call_entry*)switch_core_hash_delete(
				profile->calls, id_str);
			switch_mutex_unlock(profile->calls_mutex);
			free(entry);

			/* Tell TDLib to drop the call */
			if (profile->tg_client_id) {
				auto discard = td::td_api::make_object<td::td_api::discardCall>();
				discard->call_id_         = tech_pvt->tg_call_id;
				discard->is_disconnected_ = false;
				discard->invite_link_     = "";
				discard->duration_        = 0;
				discard->is_video_        = false;
				discard->connection_id_   = 0;
				telegram_client_manager->send(profile->tg_client_id,
					tg_new_request_id(), std::move(discard));
			}
		}
	}

	/* Stop tgcalls media asynchronously */
	if (tech_pvt->call_media) {
		TelegramCallMedia *cm = tech_pvt->call_media;
		tech_pvt->call_media  = nullptr;
		{
			std::lock_guard<std::mutex> lk(cm->renderer->playout_mtx);
			cm->renderer->playout_buf = nullptr;
		}
		{
			std::lock_guard<std::mutex> lk(cm->recorder->capture_mtx);
			cm->recorder->capture_buf = nullptr;
		}
		if (!cm->stopping.exchange(true)) {
			tgcalls::Instance *raw = cm->instance.release();
			raw->stop([raw, cm](tgcalls::FinalState) {
				if (cm->playout_buf)  switch_buffer_destroy(&cm->playout_buf);
				if (cm->capture_buf)  switch_buffer_destroy(&cm->capture_buf);
				delete raw;
				delete cm;
			});
		}
	}

	switch_mutex_lock(globals.mutex);
	globals.calls--;
	if (globals.calls < 0) globals.calls = 0;
	switch_mutex_unlock(globals.mutex);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch (sig) {
	case SWITCH_SIG_KILL:
		switch_clear_flag_locked(tech_pvt, TFLAG_IO);
		switch_clear_flag_locked(tech_pvt, TFLAG_VOICE);
		switch_channel_hangup(channel, SWITCH_CAUSE_NORMAL_CLEARING);
		break;
	case SWITCH_SIG_BREAK:
		switch_set_flag_locked(tech_pvt, TFLAG_BREAK);
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_exchange_media(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CHANNEL LOOPBACK\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_soft_execute(switch_core_session_t *session)
{
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "CHANNEL TRANSMIT\n");
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_send_dtmf(switch_core_session_t *session, const switch_dtmf_t *dtmf)
{
	private_t *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(tech_pvt != NULL);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame,
										   switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_byte_t    *data;
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	tech_pvt->read_frame.flags = SFF_NONE;
	*frame = NULL;

	/* Wait up to 30ms for a full frame from the tgcalls playout buffer */
	for (int i = 0; i < 30 && switch_test_flag(tech_pvt, TFLAG_IO); i++) {
		if (switch_test_flag(tech_pvt, TFLAG_BREAK)) {
			switch_clear_flag(tech_pvt, TFLAG_BREAK);
			goto cng;
		}
		if (tech_pvt->call_media) {
			TelegramCallMedia *cm = tech_pvt->call_media;
			std::lock_guard<std::mutex> lk(cm->renderer->playout_mtx);
			if (cm->playout_buf && switch_buffer_inuse(cm->playout_buf) >= TG_FRAME_BYTES) {
				switch_buffer_read(cm->playout_buf, tech_pvt->read_frame.data, TG_FRAME_BYTES);
				tech_pvt->read_frame.datalen = TG_FRAME_BYTES;
				tech_pvt->read_frame.samples = TG_FRAME_SAMPLES;
				*frame = &tech_pvt->read_frame;
				return SWITCH_STATUS_SUCCESS;
			}
		}
		switch_yield(1000);
	}

	if (!switch_test_flag(tech_pvt, TFLAG_IO))
		return SWITCH_STATUS_FALSE;

  cng:
	{
		static std::atomic<uint32_t> cng_count{0};
		uint32_t c = ++cng_count;
		if (c % 50 == 1)
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"channel_read_frame: returning CNG #%u (playout_buf empty)\n", c);
	}
	data    = static_cast<switch_byte_t *>(tech_pvt->read_frame.data);
	data[0] = 65;
	data[1] = 0;
	tech_pvt->read_frame.datalen = 2;
	tech_pvt->read_frame.flags   = SFF_CNG;
	*frame = &tech_pvt->read_frame;
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame,
											switch_io_flag_t flags, int stream_id)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	if (!switch_test_flag(tech_pvt, TFLAG_IO))
		return SWITCH_STATUS_FALSE;

	if (frame->flags & SFF_CNG)
		return SWITCH_STATUS_SUCCESS;

	static std::atomic<uint32_t> wf_total{0}, wf_sent{0}, wf_dropped{0};
	++wf_total;

	if (tech_pvt->call_media && tech_pvt->call_media->instance
			&& !tech_pvt->call_media->stopping && frame->datalen > 0) {
		++wf_sent;
		const uint8_t *p = static_cast<const uint8_t*>(frame->data);
		/* Feed ADM recorder for WebRTC-based instances (e.g. InstanceV2ReferenceImpl).
		 * InstanceImpl ignores the ADM and uses addExternalAudioSamples below. */
		{
			auto &rec = tech_pvt->call_media->recorder;
			std::lock_guard<std::mutex> lk(rec->capture_mtx);
			if (rec->capture_buf)
				switch_buffer_write(rec->capture_buf, p, frame->datalen);
		}
		tech_pvt->call_media->instance->addExternalAudioSamples(
			std::vector<uint8_t>(p, p + frame->datalen));
	} else {
		++wf_dropped;
	}

	uint32_t t = wf_total.load();
	if (t % 500 == 1) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
			"write_frame #%u: sent=%u dropped=%u call_media=%p\n",
			t, wf_sent.load(), wf_dropped.load(), (void*)tech_pvt->call_media);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_answer_channel(switch_core_session_t *session)
{
	switch_assert(switch_core_session_get_channel(session) != NULL);
	switch_assert(switch_core_session_get_private(session) != NULL);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session,
												switch_core_session_message_t *msg)
{
	switch_channel_t *channel  = switch_core_session_get_channel(session);
	private_t        *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(channel  != NULL);
	switch_assert(tech_pvt != NULL);

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_ANSWER:
		channel_answer_channel(session);
		break;
	default:
		break;
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session,
													  switch_event_t *var_event,
													  switch_caller_profile_t *outbound_profile,
													  switch_core_session_t **new_session,
													  switch_memory_pool_t **pool,
													  switch_originate_flag_t flags,
													  switch_call_cause_t *cancel_cause)
{
	if (!outbound_profile) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "No caller profile\n");
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	/* Dial string: telegram/<profile>/<user_id> — destination_number = "<profile>/<user_id>" */
	char  dest[256];
	strncpy(dest, outbound_profile->destination_number, sizeof(dest) - 1);
	dest[sizeof(dest) - 1] = '\0';
	char *slash = strchr(dest, '/');
	if (!slash) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Invalid dial string '%s'. Use: telegram/<profile>/<user_id>\n",
			outbound_profile->destination_number);
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}
	*slash = '\0';
	const char *profile_name = dest;
	int64_t     user_id      = (int64_t)atoll(slash + 1);

	tg_profile_t *profile;
	switch_mutex_lock(globals.profiles_mutex);
	profile = (tg_profile_t *)switch_core_hash_find(globals.profiles, profile_name);
	if (!profile || !profile->tg_client_id || profile->tg_profile_status != TG_PROFILE_STATUS_UP) {
		switch_mutex_unlock(globals.profiles_mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Profile '%s' not found or not ready\n", profile_name);
		return SWITCH_CAUSE_NETWORK_OUT_OF_ORDER;
	}

	*new_session = switch_core_session_request(telegram_endpoint_interface,
												SWITCH_CALL_DIRECTION_OUTBOUND, flags, pool);
	if (!*new_session) {
		switch_mutex_unlock(globals.profiles_mutex);
		return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	}

	{
		private_t               *tech_pvt;
		switch_channel_t        *channel;
		switch_caller_profile_t *caller_profile;
		char                     name[128];

		switch_core_session_add_stream(*new_session, NULL);

		tech_pvt = static_cast<private_t *>(switch_core_session_alloc(*new_session, sizeof(private_t)));
		if (!tech_pvt) {
			switch_mutex_unlock(globals.profiles_mutex);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(*new_session), SWITCH_LOG_CRIT,
							  "Memory allocation failure\n");
			switch_core_session_destroy(new_session);
			return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
		}

		channel = switch_core_session_get_channel(*new_session);
		tech_init(tech_pvt, *new_session);
		strncpy(tech_pvt->tg_profile_name, profile_name, sizeof(tech_pvt->tg_profile_name) - 1);

		snprintf(name, sizeof(name), "telegram/%s", outbound_profile->destination_number);
		switch_channel_set_name(channel, name);

		caller_profile = switch_caller_profile_clone(*new_session, outbound_profile);
		switch_channel_set_caller_profile(channel, caller_profile);
		tech_pvt->caller_profile = caller_profile;

		/* Store pending UUID so the runtime thread can match the updateCall */
		strncpy(profile->pending_outgoing_uuid,
			switch_core_session_get_uuid(*new_session),
			SWITCH_UUID_FORMATTED_LENGTH);

		telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
			td::td_api::make_object<td::td_api::createCall>(user_id, tg_make_protocol(), false));

		switch_mutex_unlock(globals.profiles_mutex);

		switch_set_flag_locked(tech_pvt, TFLAG_OUTBOUND);
		switch_channel_set_state(channel, CS_INIT);
	}
	return SWITCH_CAUSE_SUCCESS;
}

static switch_status_t channel_receive_event(switch_core_session_t *session, switch_event_t *event)
{
	struct private_object *tech_pvt = static_cast<private_t *>(switch_core_session_get_private(session));
	switch_assert(tech_pvt != NULL);
	return SWITCH_STATUS_SUCCESS;
}

static switch_state_handler_table_t telegram_state_handlers = {
	/*.on_init           */ channel_on_init,
	/*.on_routing        */ channel_on_routing,
	/*.on_execute        */ channel_on_execute,
	/*.on_hangup         */ channel_on_hangup,
	/*.on_exchange_media */ channel_on_exchange_media,
	/*.on_soft_execute   */ channel_on_soft_execute,
	/*.on_consume_media  */ NULL,
	/*.on_hibernate      */ NULL,
	/*.on_reset          */ NULL,
	/*.on_park           */ NULL,
	/*.on_reporting      */ NULL,
	/*.on_destroy        */ channel_on_destroy
};

static switch_io_routines_t telegram_io_routines = {
	/*.outgoing_channel  */ channel_outgoing_channel,
	/*.read_frame        */ channel_read_frame,
	/*.write_frame       */ channel_write_frame,
	/*.kill_channel      */ channel_kill_channel,
	/*.send_dtmf         */ channel_send_dtmf,
	/*.receive_message   */ channel_receive_message,
	/*.receive_event     */ channel_receive_event
};

/* ── ESL API commands ───────────────────────────────────────────────────────── */

static const char *tg_status_str(tg_profile_status_t st)
{
	switch (st) {
	case TG_PROFILE_STATUS_DOWN:       return "DOWN";
	case TG_PROFILE_STATUS_CONNECTING: return "CONNECTING";
	case TG_PROFILE_STATUS_UP:         return "UP";
	case TG_PROFILE_STATUS_ERROR:      return "ERROR";
	default:                           return "UNKNOWN";
	}
}

static void tg_profile_print_detail(tg_profile_t *p, switch_stream_handle_t *stream)
{
	stream->write_function(stream,
		"==================== %s ====================\n"
		"  Status   : %s\n"
		"  Enabled  : %s\n"
		"  Server   : %s\n"
		"  API ID   : %s\n"
		"  API Hash : %s\n"
		"  DB dir   : %s\n"
		"  Files dir: %s\n"
		"  Allow P2P: %s\n"
		"  Versions : %s\n"
		"  Login    : %s\n"
		"  Password : %s\n"
		"  Client ID: %d\n"
		"==================================================\n",
		p->tg_profile_name,
		tg_status_str(p->tg_profile_status),
		p->tg_profile_enabled ? "yes" : "no",
		p->tg_profile_server        ? p->tg_profile_server        : "(default)",
		p->tg_profile_api_id        ? p->tg_profile_api_id        : "",
		p->tg_profile_api_hash      ? p->tg_profile_api_hash      : "",
		p->tg_profile_database_dir  ? p->tg_profile_database_dir  : "",
		p->tg_profile_files_dir     ? p->tg_profile_files_dir     : "",
		p->tg_profile_allow_p2p     ? p->tg_profile_allow_p2p     : "(from TDLib)",
		p->tg_profile_versions      ? p->tg_profile_versions      : "(all)",
		p->tg_profile_login         ? p->tg_profile_login         : "(not set)",
		p->tg_profile_password      ? "***"                        : "(not set)",
		(int)p->tg_client_id);
}

SWITCH_STANDARD_API(telegram_status_api)
{
	if (!zstr(cmd)) {
		tg_profile_t *p;
		switch_mutex_lock(globals.profiles_mutex);
		p = tg_find_profile(cmd);
		if (p) {
			tg_profile_print_detail(p, stream);
		} else {
			stream->write_function(stream, "-ERR Profile not found: %s\n", cmd);
		}
		switch_mutex_unlock(globals.profiles_mutex);
	} else {
		switch_hash_index_t *hi;
		const void          *key;
		void                *val;

		stream->write_function(stream, "%-20s %s\n", "Profile", "Status");
		stream->write_function(stream, "%-20s %s\n", "-------", "------");

		switch_mutex_lock(globals.profiles_mutex);
		for (hi = switch_core_hash_first(globals.profiles); hi; hi = switch_core_hash_next(&hi)) {
			switch_core_hash_this(hi, &key, NULL, &val);
			tg_profile_t *p = (tg_profile_t *)val;
			stream->write_function(stream, "%-20s %s%s\n",
				p->tg_profile_name,
				tg_status_str(p->tg_profile_status),
				p->tg_profile_enabled ? "" : " (disabled)");
		}
		switch_mutex_unlock(globals.profiles_mutex);
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_login_api)
{
	char         *argv[2] = {0};
	char         *dup;
	int           argc;
	tg_profile_t *profile;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: telegram_login <profile> <phone>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	dup  = strdup(cmd);
	argc = switch_separate_string(dup, ' ', argv, 2);

	if (argc < 2 || zstr(argv[0]) || zstr(argv[1])) {
		stream->write_function(stream, "-ERR Usage: telegram_login <profile> <phone>\n");
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	if (!(profile = tg_find_profile(argv[0]))) {
		stream->write_function(stream, "-ERR Profile not found: %s\n", argv[0]);
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
		td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(argv[1], nullptr));
	stream->write_function(stream, "+OK Phone number sent for profile %s\n", argv[0]);
	free(dup);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_code_api)
{
	char         *argv[2] = {0};
	char         *dup;
	int           argc;
	tg_profile_t *profile;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: telegram_code <profile> <code>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	dup  = strdup(cmd);
	argc = switch_separate_string(dup, ' ', argv, 2);

	if (argc < 2 || zstr(argv[0]) || zstr(argv[1])) {
		stream->write_function(stream, "-ERR Usage: telegram_code <profile> <code>\n");
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	if (!(profile = tg_find_profile(argv[0]))) {
		stream->write_function(stream, "-ERR Profile not found: %s\n", argv[0]);
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
		td::td_api::make_object<td::td_api::checkAuthenticationCode>(argv[1]));
	stream->write_function(stream, "+OK Auth code sent for profile %s\n", argv[0]);
	free(dup);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_password_api)
{
	char         *argv[2] = {0};
	char         *dup;
	int           argc;
	tg_profile_t *profile;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: telegram_password <profile> <password>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	dup  = strdup(cmd);
	argc = switch_separate_string(dup, ' ', argv, 2);

	if (argc < 2 || zstr(argv[0]) || zstr(argv[1])) {
		stream->write_function(stream, "-ERR Usage: telegram_password <profile> <password>\n");
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	if (!(profile = tg_find_profile(argv[0]))) {
		stream->write_function(stream, "-ERR Profile not found: %s\n", argv[0]);
		free(dup);
		return SWITCH_STATUS_SUCCESS;
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
		td::td_api::make_object<td::td_api::checkAuthenticationPassword>(argv[1]));
	stream->write_function(stream, "+OK Password sent for profile %s\n", argv[0]);
	free(dup);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_start_api)
{
	tg_profile_t *profile;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: telegram_start <profile>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_lock(globals.profiles_mutex);
	if (!(profile = tg_find_profile(cmd))) {
		stream->write_function(stream, "-ERR Profile not found: %s\n", cmd);
		switch_mutex_unlock(globals.profiles_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	if (profile->tg_client_id != 0) {
		stream->write_function(stream, "-ERR Profile %s is already running\n", cmd);
		switch_mutex_unlock(globals.profiles_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	profile->tg_client_id = telegram_client_manager->create_client_id();
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
		td::td_api::make_object<td::td_api::getOption>("version"));
	switch_mutex_unlock(globals.profiles_mutex);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram profile [%s]: started (client_id=%d)\n", cmd, (int)profile->tg_client_id);
	stream->write_function(stream, "+OK Profile %s started\n", cmd);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_stop_api)
{
	tg_profile_t *profile;

	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: telegram_stop <profile>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_lock(globals.profiles_mutex);
	if (!(profile = tg_find_profile(cmd))) {
		stream->write_function(stream, "-ERR Profile not found: %s\n", cmd);
		switch_mutex_unlock(globals.profiles_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	if (profile->tg_client_id == 0) {
		stream->write_function(stream, "-ERR Profile %s is not running\n", cmd);
		switch_mutex_unlock(globals.profiles_mutex);
		return SWITCH_STATUS_SUCCESS;
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
		td::td_api::make_object<td::td_api::close>());
	profile->tg_profile_status = TG_PROFILE_STATUS_DOWN;
	profile->tg_client_id = 0;
	switch_mutex_unlock(globals.profiles_mutex);
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram profile [%s]: stopped\n", cmd);
	stream->write_function(stream, "+OK Profile %s stopped\n", cmd);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_react <profile> <chat_id> <message_id> <emoji>
 * Sends a reaction to a Telegram message. Use an empty string "" as emoji to remove all reactions. */
SWITCH_STANDARD_API(tg_react_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_react <profile> <chat_id> <message_id> <emoji>\n");
		return SWITCH_STATUS_SUCCESS;
	}

	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);

	if (argc < 4) {
		stream->write_function(stream, "-ERR Usage: tg_react <profile> <chat_id> <message_id> <emoji>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	const char *profile_name = argv[0];
	int64_t chat_id    = (int64_t)atoll(argv[1]);
	int64_t message_id = (int64_t)atoll(argv[2]);
	const char *emoji  = argv[3];

	switch_mutex_lock(globals.profiles_mutex);
	tg_profile_t *profile = tg_find_profile(profile_name);
	switch_mutex_unlock(globals.profiles_mutex);

	if (!profile || !profile->tg_client_id || profile->tg_profile_status != TG_PROFILE_STATUS_UP) {
		stream->write_function(stream, "-ERR Profile '%s' not found or not ready\n", profile_name);
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	auto req = td::td_api::make_object<td::td_api::addMessageReaction>();
	req->chat_id_    = chat_id;
	req->message_id_ = message_id;
	req->is_big_     = false;
	req->update_recent_reactions_ = false;

	if (!zstr(emoji) && strcmp(emoji, "\"\"") != 0) {
		auto rt = td::td_api::make_object<td::td_api::reactionTypeEmoji>();
		rt->emoji_ = emoji;
		req->reaction_type_ = std::move(rt);
	}

	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sent reaction '%s' to message %" PRId64 " in chat %" PRId64 "\n",
		profile_name, emoji, message_id, chat_id);

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* Helper: look up a profile and validate it is UP. Returns NULL and writes -ERR on failure. */
static tg_profile_t *tg_api_get_profile(switch_stream_handle_t *stream, const char *profile_name)
{
	switch_mutex_lock(globals.profiles_mutex);
	tg_profile_t *profile = tg_find_profile(profile_name);
	switch_mutex_unlock(globals.profiles_mutex);
	if (!profile || !profile->tg_client_id || profile->tg_profile_status != TG_PROFILE_STATUS_UP) {
		stream->write_function(stream, "-ERR Profile '%s' not found or not ready\n", profile_name);
		return NULL;
	}
	return profile;
}

/* tg_send_photo <profile> <chat_id> <file_path> [caption]
 * Sends a photo to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_photo_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_photo <profile> <chat_id> <file_path> [caption]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_photo <profile> <chat_id> <file_path> [caption]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto photo = td::td_api::make_object<td::td_api::inputMessagePhoto>();
	auto file  = td::td_api::make_object<td::td_api::inputFileLocal>();
	file->path_ = argv[2];
	photo->photo_ = std::move(file);
	if (argc >= 4 && !zstr(argv[3])) {
		auto caption = td::td_api::make_object<td::td_api::formattedText>();
		caption->text_ = argv[3];
		photo->caption_ = std::move(caption);
	}

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(photo);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending photo '%s' to chat %" PRId64 "\n", argv[0], argv[2], chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_document <profile> <chat_id> <file_path> [caption]
 * Sends a document/file to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_document_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_document <profile> <chat_id> <file_path> [caption]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_document <profile> <chat_id> <file_path> [caption]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto doc  = td::td_api::make_object<td::td_api::inputMessageDocument>();
	auto file = td::td_api::make_object<td::td_api::inputFileLocal>();
	file->path_ = argv[2];
	doc->document_ = std::move(file);
	if (argc >= 4 && !zstr(argv[3])) {
		auto caption = td::td_api::make_object<td::td_api::formattedText>();
		caption->text_ = argv[3];
		doc->caption_ = std::move(caption);
	}

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(doc);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending document '%s' to chat %" PRId64 "\n", argv[0], argv[2], chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_audio <profile> <chat_id> <file_path> [duration_sec] [title] [performer]
 * Sends an audio file to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_audio_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_audio <profile> <chat_id> <file_path> [duration] [title] [performer]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[6] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 6);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_audio <profile> <chat_id> <file_path> [duration] [title] [performer]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto audio = td::td_api::make_object<td::td_api::inputMessageAudio>();
	auto file  = td::td_api::make_object<td::td_api::inputFileLocal>();
	file->path_      = argv[2];
	audio->audio_    = std::move(file);
	audio->duration_ = (argc >= 4 && argv[3]) ? atoi(argv[3]) : 0;
	if (argc >= 5 && !zstr(argv[4])) audio->title_     = argv[4];
	if (argc >= 6 && !zstr(argv[5])) audio->performer_ = argv[5];

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(audio);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending audio '%s' to chat %" PRId64 "\n", argv[0], argv[2], chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_video <profile> <chat_id> <file_path> [duration_sec] [width] [height] [caption]
 * Sends a video to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_video_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_video <profile> <chat_id> <file_path> [duration] [width] [height] [caption]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[7] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 7);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_video <profile> <chat_id> <file_path> [duration] [width] [height] [caption]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto video = td::td_api::make_object<td::td_api::inputMessageVideo>();
	auto file  = td::td_api::make_object<td::td_api::inputFileLocal>();
	file->path_      = argv[2];
	video->video_    = std::move(file);
	video->duration_ = (argc >= 4 && argv[3]) ? atoi(argv[3]) : 0;
	video->width_    = (argc >= 5 && argv[4]) ? atoi(argv[4]) : 0;
	video->height_   = (argc >= 6 && argv[5]) ? atoi(argv[5]) : 0;
	if (argc >= 7 && !zstr(argv[6])) {
		auto caption = td::td_api::make_object<td::td_api::formattedText>();
		caption->text_ = argv[6];
		video->caption_ = std::move(caption);
	}

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(video);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending video '%s' to chat %" PRId64 "\n", argv[0], argv[2], chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_contact <profile> <chat_id> <phone> <first_name> [last_name]
 * Sends a contact card to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_contact_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_contact <profile> <chat_id> <phone> <first_name> [last_name]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[5] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 5);
	if (argc < 4) {
		stream->write_function(stream, "-ERR Usage: tg_send_contact <profile> <chat_id> <phone> <first_name> [last_name]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto contact = td::td_api::make_object<td::td_api::contact>();
	contact->phone_number_ = argv[2];
	contact->first_name_   = argv[3];
	if (argc >= 5 && !zstr(argv[4])) contact->last_name_ = argv[4];

	auto msg = td::td_api::make_object<td::td_api::inputMessageContact>();
	msg->contact_ = std::move(contact);

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(msg);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending contact '%s %s' to chat %" PRId64 "\n",
		argv[0], argv[3], argc >= 5 ? argv[4] : "", chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_location <profile> <chat_id> <latitude> <longitude>
 * Sends a map location to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_location_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_location <profile> <chat_id> <latitude> <longitude>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 4) {
		stream->write_function(stream, "-ERR Usage: tg_send_location <profile> <chat_id> <latitude> <longitude>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);
	auto loc = td::td_api::make_object<td::td_api::location>();
	loc->latitude_  = atof(argv[2]);
	loc->longitude_ = atof(argv[3]);

	auto msg = td::td_api::make_object<td::td_api::inputMessageLocation>();
	msg->location_ = std::move(loc);

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(msg);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending location (%s, %s) to chat %" PRId64 "\n",
		argv[0], argv[2], argv[3], chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_recognize_speech <profile> <chat_id> <message_id>
 * Requests Telegram server-side speech recognition on a voice/video-note message.
 * Result arrives as TELEGRAM::SPEECH_RECOGNITION CUSTOM event. */
SWITCH_STANDARD_API(tg_recognize_speech_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_recognize_speech <profile> <chat_id> <message_id>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[3] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 3);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_recognize_speech <profile> <chat_id> <message_id>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id    = (int64_t)atoll(argv[1]);
	int64_t message_id = (int64_t)atoll(argv[2]);

	auto req = td::td_api::make_object<td::td_api::recognizeSpeech>();
	req->chat_id_    = chat_id;
	req->message_id_ = message_id;
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: requested speech recognition for message %" PRId64 " in chat %" PRId64 "\n",
		argv[0], message_id, chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_text <profile> <chat_id> [reply_to:<msg_id>] <text...>
 * Sends a plain text message to a Telegram chat. Remaining argv tokens after
 * profile/chat_id (and optional reply_to:) are joined with spaces as the body. */
SWITCH_STANDARD_API(tg_send_text_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_text <profile> <chat_id> [reply_to:<id>] <text>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	/* Split into at most 4 fields: profile chat_id [reply_to:X] rest-of-text */
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_text <profile> <chat_id> [reply_to:<id>] <text>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	int64_t reply_to_id = 0;
	int text_idx = 2;
	if (argc > 2 && argv[2] && strncmp(argv[2], "reply_to:", 9) == 0) {
		reply_to_id = (int64_t)atoll(argv[2] + 9);
		text_idx = 3;
	}
	if (text_idx >= argc || zstr(argv[text_idx])) {
		stream->write_function(stream, "-ERR No text provided\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	auto text_obj = td::td_api::make_object<td::td_api::formattedText>();
	text_obj->text_ = argv[text_idx];

	auto msg_text = td::td_api::make_object<td::td_api::inputMessageText>();
	msg_text->text_ = std::move(text_obj);

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(msg_text);
	if (reply_to_id) {
		auto reply_to = td::td_api::make_object<td::td_api::inputMessageReplyToMessage>();
		reply_to->message_id_ = reply_to_id;
		req->reply_to_ = std::move(reply_to);
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_delete_messages <profile> <chat_id> <msg_id1> [msg_id2 ...]
 * Deletes up to 20 messages (for all users) from a Telegram chat. */
SWITCH_STANDARD_API(tg_delete_messages_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_delete_messages <profile> <chat_id> <msg_id1> [msg_id2 ...]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	const int MAX_IDS = 22; /* profile + chat_id + up to 20 message ids */
	char *argv[MAX_IDS] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, MAX_IDS);
	if (argc < 3) {
		stream->write_function(stream, "-ERR At least one message id required\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	auto req = td::td_api::make_object<td::td_api::deleteMessages>();
	req->chat_id_ = chat_id;
	req->revoke_  = true;
	for (int i = 2; i < argc && argv[i]; ++i)
		req->message_ids_.push_back((int64_t)atoll(argv[i]));
	int n = (int)req->message_ids_.size();

	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	stream->write_function(stream, "+OK deleted %d messages\n", n);
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_edit_message_text <profile> <chat_id> <msg_id> <new_text>
 * Edits the text of an existing outgoing message. */
SWITCH_STANDARD_API(tg_edit_message_text_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_edit_message_text <profile> <chat_id> <msg_id> <new_text>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 4 || zstr(argv[3])) {
		stream->write_function(stream, "-ERR Usage: tg_edit_message_text <profile> <chat_id> <msg_id> <new_text>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id    = (int64_t)atoll(argv[1]);
	int64_t message_id = (int64_t)atoll(argv[2]);

	auto fmt = td::td_api::make_object<td::td_api::formattedText>();
	fmt->text_ = argv[3];

	auto content = td::td_api::make_object<td::td_api::inputMessageText>();
	content->text_ = std::move(fmt);

	auto req = td::td_api::make_object<td::td_api::editMessageText>();
	req->chat_id_               = chat_id;
	req->message_id_            = message_id;
	req->input_message_content_ = std::move(content);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_edit_message_caption <profile> <chat_id> <msg_id> <new_caption>
 * Edits the caption of an existing outgoing photo/video/document message. */
SWITCH_STANDARD_API(tg_edit_message_caption_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_edit_message_caption <profile> <chat_id> <msg_id> <new_caption>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 4 || zstr(argv[3])) {
		stream->write_function(stream, "-ERR Usage: tg_edit_message_caption <profile> <chat_id> <msg_id> <new_caption>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id    = (int64_t)atoll(argv[1]);
	int64_t message_id = (int64_t)atoll(argv[2]);

	auto fmt = td::td_api::make_object<td::td_api::formattedText>();
	fmt->text_ = argv[3];

	auto req = td::td_api::make_object<td::td_api::editMessageCaption>();
	req->chat_id_    = chat_id;
	req->message_id_ = message_id;
	req->caption_    = std::move(fmt);
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_inline_result <profile> <chat_id> <bot_username> [query]
 * Queries a bot's inline mode and sends its first result to the chat.
 * Flow: searchPublicChat → getInlineQueryResults → sendInlineQueryResultMessage */
SWITCH_STANDARD_API(tg_send_inline_result_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_inline_result <profile> <chat_id> <bot_username> [query]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[4] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 4);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_inline_result <profile> <chat_id> <bot_username> [query]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	/* Strip leading @ if present */
	const char *username = argv[2];
	if (username[0] == '@') username++;

	tg_inline_flow flow;
	flow.tg_client_id = profile->tg_client_id;
	flow.chat_id      = chat_id;
	flow.query        = (argc >= 4 && argv[3]) ? argv[3] : "";
	flow.phase        = tg_inline_flow::SEARCHING;

	uint64_t req_id = tg_new_request_id();
	switch_mutex_lock(tg_inline_flows_mutex);
	tg_inline_flows[req_id] = flow;
	switch_mutex_unlock(tg_inline_flows_mutex);

	auto req = td::td_api::make_object<td::td_api::searchPublicChat>();
	req->username_ = username;
	telegram_client_manager->send(profile->tg_client_id, req_id, std::move(req));

	stream->write_function(stream, "+OK searching @%s\n", username);
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_clear_chat_history <profile> <chat_id>
 * Deletes all messages in a chat for both sides (revoke=true). */
SWITCH_STANDARD_API(tg_clear_chat_history_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_clear_chat_history <profile> <chat_id>\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[2] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 2);
	if (argc < 2) {
		stream->write_function(stream, "-ERR Usage: tg_clear_chat_history <profile> <chat_id>\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	auto req = td::td_api::make_object<td::td_api::deleteChatHistory>();
	req->chat_id_              = chat_id;
	req->remove_from_chat_list_ = false;
	req->revoke_               = true;
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_voice <profile> <chat_id> [reply_to:<id>] <file_path> [duration]
 * Sends a file as a voice note (OGA/OGG) to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_voice_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_voice <profile> <chat_id> [reply_to:<id>] <file_path> [duration]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[5] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 5);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_voice <profile> <chat_id> [reply_to:<id>] <file_path> [duration]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	int64_t reply_to_id = 0;
	int file_idx = 2;
	if (argc > 2 && argv[2] && strncmp(argv[2], "reply_to:", 9) == 0) {
		reply_to_id = (int64_t)atoll(argv[2] + 9);
		file_idx = 3;
	}
	if (file_idx >= argc || zstr(argv[file_idx])) {
		stream->write_function(stream, "-ERR No file path provided\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	auto input_file = td::td_api::make_object<td::td_api::inputFileLocal>();
	input_file->path_ = argv[file_idx];

	int32_t duration = 0;
	if (file_idx + 1 < argc && argv[file_idx + 1]) duration = atoi(argv[file_idx + 1]);

	auto voice = td::td_api::make_object<td::td_api::inputMessageVoiceNote>();
	voice->voice_note_ = std::move(input_file);
	voice->duration_   = duration;

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(voice);
	if (reply_to_id) {
		auto reply_to = td::td_api::make_object<td::td_api::inputMessageReplyToMessage>();
		reply_to->message_id_ = reply_to_id;
		req->reply_to_ = std::move(reply_to);
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_video_note <profile> <chat_id> [reply_to:<id>] <file_path> [duration] [length]
 * Sends a file as a round video note to a Telegram chat. */
SWITCH_STANDARD_API(tg_send_video_note_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_video_note <profile> <chat_id> [reply_to:<id>] <file_path> [duration] [length]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	char *argv[6] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 6);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_video_note <profile> <chat_id> [reply_to:<id>] <file_path> [duration] [length]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id = (int64_t)atoll(argv[1]);

	int64_t reply_to_id = 0;
	int file_idx = 2;
	if (argc > 2 && argv[2] && strncmp(argv[2], "reply_to:", 9) == 0) {
		reply_to_id = (int64_t)atoll(argv[2] + 9);
		file_idx = 3;
	}
	if (file_idx >= argc || zstr(argv[file_idx])) {
		stream->write_function(stream, "-ERR No file path provided\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	auto input_file = td::td_api::make_object<td::td_api::inputFileLocal>();
	input_file->path_ = argv[file_idx];

	int32_t duration = 0, length = 240;
	if (file_idx + 1 < argc && argv[file_idx + 1]) duration = atoi(argv[file_idx + 1]);
	if (file_idx + 2 < argc && argv[file_idx + 2]) length   = atoi(argv[file_idx + 2]);

	auto vn = td::td_api::make_object<td::td_api::inputMessageVideoNote>();
	vn->video_note_  = std::move(input_file);
	vn->duration_    = duration;
	vn->length_      = length;

	auto req = td::td_api::make_object<td::td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(vn);
	if (reply_to_id) {
		auto reply_to = td::td_api::make_object<td::td_api::inputMessageReplyToMessage>();
		reply_to->message_id_ = reply_to_id;
		req->reply_to_ = std::move(reply_to);
	}
	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));

	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

/* tg_send_album <profile> <chat_id> <file1> [file2] ... [fileN]
 * Sends up to 10 photos/videos as a single media album message.
 * File type is inferred from extension: jpg/jpeg/png/webp/gif → photo, else → document. */
SWITCH_STANDARD_API(tg_send_album_api)
{
	if (zstr(cmd)) {
		stream->write_function(stream, "-ERR Usage: tg_send_album <profile> <chat_id> <file1> [file2 ...]\n");
		return SWITCH_STATUS_SUCCESS;
	}
	/* Up to 10 files + profile + chat_id = 12 slots max. */
	const int MAX_ALBUM = 10;
	char *argv[12] = {0};
	char *lbuf = strdup(cmd);
	int argc = switch_separate_string(lbuf, ' ', argv, 12);
	if (argc < 3) {
		stream->write_function(stream, "-ERR Usage: tg_send_album <profile> <chat_id> <file1> [file2 ...]\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}
	tg_profile_t *profile = tg_api_get_profile(stream, argv[0]);
	if (!profile) { switch_safe_free(lbuf); return SWITCH_STATUS_SUCCESS; }

	int64_t chat_id    = (int64_t)atoll(argv[1]);
	int64_t reply_to_id = 0;
	int     file_start = 2;

	/* Optional reply_to:<message_id> before the first file path. */
	if (argc > 2 && argv[2] && strncmp(argv[2], "reply_to:", 9) == 0) {
		reply_to_id = (int64_t)atoll(argv[2] + 9);
		file_start  = 3;
	}

	int n_files = argc - file_start;
	if (n_files > MAX_ALBUM) n_files = MAX_ALBUM;

	auto req = td::td_api::make_object<td::td_api::sendMessageAlbum>();
	req->chat_id_ = chat_id;
	if (reply_to_id) {
		auto reply_to = td::td_api::make_object<td::td_api::inputMessageReplyToMessage>();
		reply_to->message_id_ = reply_to_id;
		req->reply_to_ = std::move(reply_to);
	}

	for (int i = 0; i < n_files; i++) {
		const char *path = argv[file_start + i];
		if (!path || !*path) continue;

		/* Infer type from extension. */
		const char *dot = strrchr(path, '.');
		bool is_photo = false;
		if (dot) {
			const char *ext = dot + 1;
			is_photo = (!strcasecmp(ext, "jpg")  ||
			            !strcasecmp(ext, "jpeg") ||
			            !strcasecmp(ext, "png")  ||
			            !strcasecmp(ext, "webp") ||
			            !strcasecmp(ext, "gif"));
		}

		auto file = td::td_api::make_object<td::td_api::inputFileLocal>();
		file->path_ = path;

		if (is_photo) {
			auto photo = td::td_api::make_object<td::td_api::inputMessagePhoto>();
			photo->photo_ = std::move(file);
			req->input_message_contents_.push_back(std::move(photo));
		} else {
			auto doc = td::td_api::make_object<td::td_api::inputMessageDocument>();
			doc->document_ = std::move(file);
			req->input_message_contents_.push_back(std::move(doc));
		}
	}

	if (req->input_message_contents_.empty()) {
		stream->write_function(stream, "-ERR No valid files provided\n");
		switch_safe_free(lbuf);
		return SWITCH_STATUS_SUCCESS;
	}

	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(req));
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram [%s]: sending album of %d item(s) to chat %" PRId64 "\n",
		argv[0], n_files, chat_id);
	stream->write_function(stream, "+OK\n");
	switch_safe_free(lbuf);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_STANDARD_API(telegram_reload_api)
{
	char         *cf = const_cast<char *>("telegram.conf");
	switch_xml_t  cfg, xml, settings_xml, param, profiles_xml, profile_xml;
	int           updated = 0, created = 0;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		stream->write_function(stream, "-ERR Cannot open %s\n", cf);
		return SWITCH_STATUS_SUCCESS;
	}

	/* ── Global settings ── */
	if ((settings_xml = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings_xml, "param"); param; param = param->next) {
			const char *var = switch_xml_attr_soft(param, "name");
			const char *val = switch_xml_attr_soft(param, "value");
			if (!strcmp(var, "debug")) {
				globals.debug = atoi(val);
			} else if (!strcmp(var, "server")) {
				switch_safe_free(globals.server);
				globals.server = strdup(val);
			} else if (!strcmp(var, "dialplan")) {
				switch_safe_free(globals.dialplan);
				globals.dialplan = strdup(val);
			} else if (!strcmp(var, "context")) {
				switch_safe_free(globals.context);
				globals.context = strdup(val);
			}
		}
	}

	/* ── Profiles ── */
	if ((profiles_xml = switch_xml_child(cfg, "profiles"))) {
		for (profile_xml = switch_xml_child(profiles_xml, "profile");
			 profile_xml; profile_xml = profile_xml->next) {

			const char *name        = switch_xml_attr(profile_xml, "name");
			const char *enabled_str = switch_xml_attr(profile_xml, "enabled");
			if (!name) continue;

			const char *p_api_id = nullptr, *p_api_hash  = nullptr;
			const char *p_server = nullptr, *p_db_dir    = nullptr, *p_files_dir = nullptr;
			const char *p_allow_p2p = nullptr, *p_versions = nullptr;
			const char *p_login     = nullptr, *p_password  = nullptr;
			const char *p_ev_dest = nullptr;

			for (param = switch_xml_child(profile_xml, "param"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "value");
				if      (!strcmp(var, "api-id"))             p_api_id    = val;
				else if (!strcmp(var, "api-hash"))           p_api_hash  = val;
				else if (!strcmp(var, "server"))             p_server    = val;
				else if (!strcmp(var, "database-dir"))       p_db_dir    = val;
				else if (!strcmp(var, "files-dir"))          p_files_dir = val;
				else if (!strcmp(var, "allow-p2p"))          p_allow_p2p = val;
				else if (!strcmp(var, "versions"))           p_versions  = val;
				else if (!strcmp(var, "login"))              p_login     = val;
				else if (!strcmp(var, "password"))           p_password  = val;
				else if (!strcmp(var, "inbound-dest-proto")) p_ev_dest   = val;
			}

			switch_mutex_lock(globals.profiles_mutex);
			tg_profile_t *existing = tg_find_profile(name);

			if (existing) {
				/* Update hot-swappable settings under calls_mutex */
				switch_mutex_lock(existing->calls_mutex);
#define TG_RELOAD_STR(field, newval) do {                                     \
	const char *_n = (newval);                                                \
	if (!_n) { switch_safe_free(existing->field); }                           \
	else if (!existing->field || strcmp(existing->field, _n)) {               \
		switch_safe_free(existing->field); existing->field = strdup(_n);      \
	}                                                                         \
} while (0)
				TG_RELOAD_STR(tg_profile_allow_p2p,        p_allow_p2p);
				TG_RELOAD_STR(tg_profile_versions,         p_versions);
				TG_RELOAD_STR(tg_profile_login,            p_login);
				TG_RELOAD_STR(tg_profile_password,         p_password);
				TG_RELOAD_STR(tg_inbound_dest_proto,       p_ev_dest);
#undef TG_RELOAD_STR
				switch_mutex_unlock(existing->calls_mutex);

				/* Warn about fields that require a profile restart to take effect */
				auto warn_restart = [&](const char *field, const char *cur, const char *nv) {
					if (nv && (!cur || strcmp(cur, nv)))
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							"Telegram reload [%s]: '%s' changed — "
							"requires telegram_stop + telegram_start\n", name, field);
				};
				warn_restart("api-id",       existing->tg_profile_api_id,       p_api_id);
				warn_restart("api-hash",     existing->tg_profile_api_hash,     p_api_hash);
				warn_restart("server",       existing->tg_profile_server,       p_server);
				warn_restart("database-dir", existing->tg_profile_database_dir, p_db_dir);
				warn_restart("files-dir",    existing->tg_profile_files_dir,    p_files_dir);

				switch_mutex_unlock(globals.profiles_mutex);
				stream->write_function(stream, "+OK Profile %s: settings updated\n", name);
				updated++;
			} else {
				/* New profile: create and optionally start */
				switch_bool_t enabled = (!enabled_str || strcmp(enabled_str, "false") != 0)
										? SWITCH_TRUE : SWITCH_FALSE;
				tg_profile_t *profile = (tg_profile_t *)calloc(1, sizeof(tg_profile_t));
				if (!profile) {
					switch_mutex_unlock(globals.profiles_mutex);
					stream->write_function(stream, "-ERR Out of memory for profile %s\n", name);
					continue;
				}
				profile->tg_profile_name    = strdup(name);
				profile->tg_profile_enabled = enabled;
				profile->tg_profile_status  = TG_PROFILE_STATUS_DOWN;
				if (p_api_id)    profile->tg_profile_api_id       = strdup(p_api_id);
				if (p_api_hash)  profile->tg_profile_api_hash     = strdup(p_api_hash);
				profile->tg_profile_server  = p_server ? strdup(p_server) : strdup(globals.server);
				if (p_db_dir)    profile->tg_profile_database_dir = strdup(p_db_dir);
				if (p_files_dir) profile->tg_profile_files_dir    = strdup(p_files_dir);
				if (p_allow_p2p) profile->tg_profile_allow_p2p   = strdup(p_allow_p2p);
				if (p_versions)  profile->tg_profile_versions     = strdup(p_versions);
				if (p_login)     profile->tg_profile_login           = strdup(p_login);
				if (p_password)  profile->tg_profile_password        = strdup(p_password);
				if (p_ev_dest)   profile->tg_inbound_dest_proto      = strdup(p_ev_dest);

				tg_profile_set_default_dirs(profile);
				switch_core_hash_init(&profile->calls);
				switch_mutex_init(&profile->calls_mutex, SWITCH_MUTEX_NESTED, module_pool);
				switch_core_hash_insert(globals.profiles, name, profile);

				if (enabled) {
					profile->tg_client_id = telegram_client_manager->create_client_id();
					telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
						td::td_api::make_object<td::td_api::getOption>("version"));
				}

				switch_mutex_unlock(globals.profiles_mutex);
				stream->write_function(stream, "+OK Profile %s: created%s\n", name,
									   enabled ? " and started" : " (disabled)");
				created++;
			}
		}
	}

	switch_xml_free(xml);
	stream->write_function(stream, "+OK Reload complete: %d updated, %d created\n",
						   updated, created);
	return SWITCH_STATUS_SUCCESS;
}

/* ── Config loader ──────────────────────────────────────────────────────────── */

static switch_status_t load_config(void)
{
	char           *cf = const_cast<char *>("telegram.conf");
	switch_xml_t    cfg, xml, settings, param, profiles_xml, profile_xml;

	/* Reset scalar settings without touching mutex / hash pointers. */
	switch_safe_free(globals.server);
	switch_safe_free(globals.dialplan);
	switch_safe_free(globals.context);
	globals.debug = 0;
	globals.flags = 0;

	/* Drop existing profiles. */
	if (globals.profiles) {
		switch_hash_index_t *hi;
		const void          *key;
		void                *val;
		switch_mutex_lock(globals.profiles_mutex);
		for (hi = switch_core_hash_first(globals.profiles); hi; hi = switch_core_hash_next(&hi)) {
			switch_core_hash_this(hi, &key, NULL, &val);
			tg_profile_free((tg_profile_t *)val);
		}
		switch_core_hash_destroy(&globals.profiles);
		switch_mutex_unlock(globals.profiles_mutex);
	}
	switch_core_hash_init(&globals.profiles);

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		return SWITCH_STATUS_TERM;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = const_cast<char *>(switch_xml_attr_soft(param, "name"));
			char *val = const_cast<char *>(switch_xml_attr_soft(param, "value"));

			if (!strcmp(var, "debug")) {
				globals.debug = atoi(val);
			} else if (!strcmp(var, "server")) {
				set_global_server(val);
			} else if (!strcmp(var, "dialplan")) {
				set_global_dialplan(val);
			} else if (!strcmp(var, "context")) {
				set_global_context(val);
			}
		}
	}

	if (!globals.server)   set_global_server(DC_SERVER_TEST);
	if (!globals.dialplan) set_global_dialplan("XML");
	if (!globals.context)  set_global_context("default");

	if ((profiles_xml = switch_xml_child(cfg, "profiles"))) {
		for (profile_xml = switch_xml_child(profiles_xml, "profile");
			 profile_xml; profile_xml = profile_xml->next) {

			const char *name        = switch_xml_attr(profile_xml, "name");
			const char *enabled_str = switch_xml_attr(profile_xml, "enabled");

			if (!name) continue;

			tg_profile_t *profile = (tg_profile_t *)calloc(1, sizeof(tg_profile_t));
			if (!profile) continue;

			profile->tg_profile_name    = strdup(name);
			profile->tg_profile_enabled = (!enabled_str || strcmp(enabled_str, "false") != 0)
										  ? SWITCH_TRUE : SWITCH_FALSE;
			profile->tg_profile_status  = TG_PROFILE_STATUS_DOWN;

			for (param = switch_xml_child(profile_xml, "param"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "value");

				if (!strcmp(var, "api-id")) {
					profile->tg_profile_api_id   = strdup(val);
				} else if (!strcmp(var, "api-hash")) {
					profile->tg_profile_api_hash = strdup(val);
				} else if (!strcmp(var, "server")) {
					profile->tg_profile_server   = strdup(val);
				} else if (!strcmp(var, "database-dir")) {
					profile->tg_profile_database_dir = strdup(val);
				} else if (!strcmp(var, "files-dir")) {
					profile->tg_profile_files_dir    = strdup(val);
				} else if (!strcmp(var, "allow-p2p")) {
					profile->tg_profile_allow_p2p    = strdup(val);
				} else if (!strcmp(var, "versions")) {
					profile->tg_profile_versions     = strdup(val);
				} else if (!strcmp(var, "login")) {
					profile->tg_profile_login        = strdup(val);
				} else if (!strcmp(var, "password")) {
					profile->tg_profile_password     = strdup(val);
				} else if (!strcmp(var, "inbound-dest-proto")) {
					profile->tg_inbound_dest_proto      = strdup(val);
				}
			}

			if (!profile->tg_profile_server)
				profile->tg_profile_server = strdup(globals.server);

			tg_profile_set_default_dirs(profile);

			switch_core_hash_init(&profile->calls);
			switch_mutex_init(&profile->calls_mutex, SWITCH_MUTEX_NESTED, module_pool);

			switch_mutex_lock(globals.profiles_mutex);
			switch_core_hash_insert(globals.profiles, name, profile);
			switch_mutex_unlock(globals.profiles_mutex);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram profile [%s] loaded (%s)\n", name,
				profile->tg_profile_enabled ? "enabled" : "disabled");
		}
	}

	switch_xml_free(xml);
	return SWITCH_STATUS_SUCCESS;
}

/* ── tgcalls helpers ────────────────────────────────────────────────────────── */

static td::td_api::object_ptr<td::td_api::callProtocol> tg_make_protocol()
{
	auto proto = td::td_api::make_object<td::td_api::callProtocol>();
	proto->udp_p2p_       = true;
	proto->udp_reflector_ = true;
	proto->min_layer_     = 65;
	proto->max_layer_     = tgcalls::Meta::MaxLayer();
	for (auto &v : tgcalls::Meta::Versions())
		proto->library_versions_.push_back(v);
	return proto;
}

static TelegramCallMedia *tg_start_media(
	tg_profile_t               *profile,
	int32_t                     call_id,
	td::td_api::callStateReady &ready,
	bool                        is_outgoing,
	const char                 *session_uuid)
{
	/* Encryption key (bytes → uint8_t array) */
	auto key_bytes = std::make_shared<std::array<uint8_t, tgcalls::EncryptionKey::kSize>>();
	memset(key_bytes->data(), 0, tgcalls::EncryptionKey::kSize);
	{
		const std::string &k = ready.encryption_key_;
		memcpy(key_bytes->data(),
			reinterpret_cast<const uint8_t*>(k.data()),
			std::min(k.size(), (size_t)tgcalls::EncryptionKey::kSize));
	}
	tgcalls::EncryptionKey enc_key(key_bytes, is_outgoing);

	/* RTC servers */
	std::vector<tgcalls::RtcServer> rtc_servers;
	for (auto &srv : ready.servers_) {
		if (!srv || !srv->type_) continue;
		if (srv->type_->get_id() != td::td_api::callServerTypeWebrtc::ID) continue;
		auto &wrtc = static_cast<td::td_api::callServerTypeWebrtc&>(*srv->type_);

		tgcalls::RtcServer r;
		r.host     = srv->ip_address_;
		r.port     = (uint16_t)srv->port_;
		r.login    = wrtc.username_;
		r.password = wrtc.password_;
		r.isTurn   = wrtc.supports_turn_;
		rtc_servers.push_back(r);

		if (!srv->ipv6_address_.empty()) {
			r.host = srv->ipv6_address_;
			rtc_servers.push_back(r);
		}
	}

	/* Resolve effective P2P and version settings:
	 *   channel var telegram_versions / tg_allow_p2p
	 *   > profile config versions / allow-p2p
	 *   > TDLib callStateReady defaults */
	bool effective_allow_p2p = ready.allow_p2p_;
	std::vector<std::string> allowed_versions;  /* empty = all supported */
	{
		/* Snapshot mutable profile settings under calls_mutex before the
		 * session locate (which may block) so telegram_reload cannot race. */
		char *snap_allow_p2p = nullptr, *snap_versions = nullptr;
		switch_mutex_lock(profile->calls_mutex);
		if (profile->tg_profile_allow_p2p) snap_allow_p2p = strdup(profile->tg_profile_allow_p2p);
		if (profile->tg_profile_versions)  snap_versions  = strdup(profile->tg_profile_versions);
		switch_mutex_unlock(profile->calls_mutex);

		const char *v_str = nullptr;
		switch_core_session_t *tmp = switch_core_session_locate(session_uuid);
		if (tmp) {
			switch_channel_t *tmp_ch = switch_core_session_get_channel(tmp);
			const char *ch_p2p = switch_channel_get_variable(tmp_ch, "tg_allow_p2p");
			if (!zstr(ch_p2p)) {
				effective_allow_p2p = switch_true(ch_p2p);
			} else if (!zstr(snap_allow_p2p)) {
				effective_allow_p2p = switch_true(snap_allow_p2p);
			}
			v_str = switch_channel_get_variable(tmp_ch, "telegram_versions");
			if (zstr(v_str)) v_str = snap_versions;
			switch_core_session_rwunlock(tmp);
		} else {
			if (!zstr(snap_allow_p2p))
				effective_allow_p2p = switch_true(snap_allow_p2p);
			v_str = snap_versions;
		}
		if (!zstr(v_str)) {
			char *dup = strdup(v_str);
			char *tok = strtok(dup, ":");
			while (tok) {
				while (*tok == ' ') tok++;
				if (*tok) allowed_versions.push_back(std::string(tok));
				tok = strtok(nullptr, ":");
			}
			free(dup);
		}
		switch_safe_free(snap_allow_p2p);
		switch_safe_free(snap_versions);
	}
	{
		std::string vf_log;
		if (allowed_versions.empty()) {
			vf_log = "(all)";
		} else {
			for (auto &v : allowed_versions) { if (!vf_log.empty()) vf_log += ":"; vf_log += v; }
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram: call %d P2P=%s (tdlib=%s profile=%s) versions_filter=%s\n",
			call_id,
			effective_allow_p2p ? "yes" : "no",
			ready.allow_p2p_ ? "yes" : "no",
			profile->tg_profile_allow_p2p ? profile->tg_profile_allow_p2p : "(unset)",
			vf_log.c_str());
	}

	/* Pick tgcalls version */
	std::string version;
	if (ready.protocol_) {
		std::string phone_versions;
		for (auto &v : ready.protocol_->library_versions_) {
			if (!phone_versions.empty()) phone_versions += ",";
			phone_versions += v;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram: call %d callStateReady protocol: phone_versions=[%s] "
			"min_layer=%d max_layer=%d udp_p2p=%d udp_reflector=%d allow_p2p=%d "
			"servers=%zu\n",
			call_id,
			phone_versions.c_str(),
			ready.protocol_->min_layer_,
			ready.protocol_->max_layer_,
			(int)ready.protocol_->udp_p2p_,
			(int)ready.protocol_->udp_reflector_,
			(int)ready.allow_p2p_,
			ready.servers_.size());

		auto supported = tgcalls::Meta::Versions();
		/* Filter to allowed_versions if specified via telegram_versions var or profile */
		if (!allowed_versions.empty()) {
			auto it = supported.begin();
			while (it != supported.end()) {
				if (std::find(allowed_versions.begin(), allowed_versions.end(), *it)
						== allowed_versions.end())
					it = supported.erase(it);
				else
					++it;
			}
		}
		for (auto &v : ready.protocol_->library_versions_) {
			if (std::find(supported.begin(), supported.end(), v) != supported.end()) {
				version = v;
				break;
			}
		}
	}
	if (version.empty()) {
		/* Fall back: pick highest allowed version we can instantiate */
		auto candidates = tgcalls::Meta::Versions();
		if (!allowed_versions.empty()) {
			auto it = candidates.begin();
			while (it != candidates.end()) {
				if (std::find(allowed_versions.begin(), allowed_versions.end(), *it)
						== allowed_versions.end())
					it = candidates.erase(it);
				else
					++it;
			}
		}
		if (!candidates.empty())
			version = candidates.back();
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
		"Telegram: call %d selected tgcalls version=[%s]\n", call_id, version.c_str());

	/* Audio device module */
	auto renderer = std::make_shared<TelegramRenderer>();
	auto recorder = std::make_shared<TelegramRecorder>();
	tgcalls::FakeAudioDeviceModule::Options adm_opts;
	adm_opts.samples_per_sec = TG_AUDIO_RATE;
	adm_opts.num_channels    = 1;
	auto adm_creator = tgcalls::FakeAudioDeviceModule::Creator(renderer, recorder, adm_opts);

	/* Capture for lambdas */
	int32_t                     cid       = call_id;
	td::ClientManager::ClientId client_id = profile->tg_client_id;

	/* Config */
	tgcalls::Config cfg;
	cfg.initializationTimeout = 60.0;
	cfg.receiveTimeout        = 20.0;
	cfg.enableAEC             = false;
	cfg.enableNS              = false;
	cfg.enableAGC             = false;
	cfg.enableP2P             = effective_allow_p2p;
	cfg.customParameters      = ready.config_;
	if (ready.protocol_) cfg.maxApiLayer = ready.protocol_->max_layer_;

	/* Descriptor — must be aggregate-initialized because EncryptionKey has no default ctor */
	tgcalls::Descriptor desc{
		version,
		std::move(cfg),
		{},                         /* persistentState */
		{},                         /* endpoints */
		nullptr,                    /* proxy */
		std::move(rtc_servers),
		tgcalls::NetworkType{},
		std::move(enc_key),
		{},                         /* mediaDevicesConfig */
		nullptr,                    /* videoCapture */
		[cid](tgcalls::State st) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"tgcalls call %d state: %d\n", cid, (int)st);
		},
		nullptr,                    /* signalBarsUpdated */
		[cid](float level) {
			static std::atomic<uint32_t> al_count{0};
			uint32_t c = ++al_count;
			if (c % 10 == 1)
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
					"tgcalls audio level #%u call %d: %.4f\n", c, cid, level);
		},
		nullptr,                    /* remoteBatteryLevelIsLowUpdated */
		[cid](tgcalls::AudioState audio, tgcalls::VideoState video) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"tgcalls call %d remote media state: audio=%d video=%d\n",
				cid, (int)audio, (int)video);
		},
		nullptr,                    /* remotePrefferedAspectRatioUpdated */
		[client_id, cid](const std::vector<uint8_t> &data) {
			auto req = td::td_api::make_object<td::td_api::sendCallSignalingData>();
			req->call_id_ = cid;
			req->data_    = std::string(data.begin(), data.end());
			telegram_client_manager->send(client_id, tg_new_request_id(), std::move(req));
		},
		std::move(adm_creator),     /* createAudioDeviceModule */
		nullptr,                    /* createWrappedAudioDeviceModule */
		{},                         /* initialInputDeviceId */
		{},                         /* initialOutputDeviceId */
		nullptr,                    /* directConnectionChannel */
	};

	TelegramCallMedia *cm = new TelegramCallMedia();
	cm->renderer = renderer;
	cm->recorder = recorder;
	switch_buffer_create_dynamic(&cm->playout_buf,
		TG_FRAME_BYTES, TG_FRAME_BYTES * 10, TG_FRAME_BYTES * 50);
	switch_buffer_create_dynamic(&cm->capture_buf,
		TG_FRAME_BYTES, TG_FRAME_BYTES * 10, TG_FRAME_BYTES * 50);
	renderer->playout_buf = cm->playout_buf;
	recorder->capture_buf = cm->capture_buf;

	cm->instance = tgcalls::Meta::Create(version, std::move(desc));
	if (!cm->instance) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"tgcalls: failed to create instance for call %d\n", cid);
		switch_buffer_destroy(&cm->playout_buf);
		switch_buffer_destroy(&cm->capture_buf);
		delete cm;
		return nullptr;
	}
	cm->instance->setNetworkType(tgcalls::NetworkType::WiFi);

	/* Attach media to the FS session */
	switch_core_session_t *sess = switch_core_session_locate(session_uuid);
	if (sess) {
		switch_channel_t *ch = switch_core_session_get_channel(sess);
		if (switch_channel_get_state(ch) < CS_HANGUP) {
			private_t *tp = static_cast<private_t*>(switch_core_session_get_private(sess));
			if (tp) {
				tp->tg_call_id = cid;
				tp->call_media = cm;
				/* switch_channel_answer() is a no-op for outbound channels; use
				 * switch_channel_mark_answered() to set CF_ANSWERED directly so
				 * channel_on_routing unblocks and media apps can run. */
				if (switch_test_flag(tp, TFLAG_OUTBOUND))
					switch_channel_mark_answered(ch);
			}
		} else {
			/* Session already hanging up — clean up asynchronously */
			switch_core_session_rwunlock(sess);
			tgcalls::Instance *raw = cm->instance.release();
			raw->stop([raw, cm](tgcalls::FinalState) {
				if (cm->playout_buf)  switch_buffer_destroy(&cm->playout_buf);
				if (cm->capture_buf)  switch_buffer_destroy(&cm->capture_buf);
				delete raw;
				delete cm;
			});
			return nullptr;
		}
		switch_core_session_rwunlock(sess);
	}
	return cm;
}

static void tg_handle_call_update(tg_profile_t *profile, td::td_api::updateCall &upd)
{
	if (!upd.call_ || !upd.call_->state_) return;

	auto    &call        = *upd.call_;
	int32_t  call_id     = call.id_;
	bool     is_outgoing = call.is_outgoing_;
	int64_t  peer_user   = call.user_id_;

	char id_str[24];
	snprintf(id_str, sizeof(id_str), "%d", call_id);

	switch (call.state_->get_id()) {

	case td::td_api::callStatePending::ID:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: call %d %s (peer=%" PRId64 ")\n",
			profile->tg_profile_name, call_id,
			is_outgoing ? "outgoing" : "incoming", peer_user);

		if (is_outgoing) {
			/* Match to the pending outbound FS session */
			char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
			switch_mutex_lock(globals.profiles_mutex);
			if (profile->pending_outgoing_uuid[0]) {
				strncpy(uuid, profile->pending_outgoing_uuid, SWITCH_UUID_FORMATTED_LENGTH);
				memset(profile->pending_outgoing_uuid, 0, sizeof(profile->pending_outgoing_uuid));
			}
			switch_mutex_unlock(globals.profiles_mutex);

			if (uuid[0]) {
				tg_call_entry *e = (tg_call_entry*)calloc(1, sizeof(tg_call_entry));
				strncpy(e->session_uuid, uuid, SWITCH_UUID_FORMATTED_LENGTH);
				switch_mutex_lock(profile->calls_mutex);
				switch_core_hash_insert(profile->calls, id_str, e);
				switch_mutex_unlock(profile->calls_mutex);

				switch_core_session_t *sess = switch_core_session_locate(uuid);
				if (sess) {
					private_t *tp = static_cast<private_t*>(switch_core_session_get_private(sess));
					if (tp) tp->tg_call_id = call_id;
					switch_core_session_rwunlock(sess);
				}
			}
		} else {
			/* Incoming call: create inbound FS session */
			switch_core_session_t *sess = switch_core_session_request(
				telegram_endpoint_interface,
				SWITCH_CALL_DIRECTION_INBOUND,
				SOF_NONE, NULL);
			if (!sess) {
				auto d = td::td_api::make_object<td::td_api::discardCall>();
				d->call_id_ = call_id;
				telegram_client_manager->send(profile->tg_client_id,
					tg_new_request_id(), std::move(d));
				break;
			}

			private_t *tp = static_cast<private_t*>(
				switch_core_session_alloc(sess, sizeof(private_t)));
			tech_init(tp, sess);
			tp->tg_call_id = call_id;
			strncpy(tp->tg_profile_name, profile->tg_profile_name,
				sizeof(tp->tg_profile_name) - 1);

			char caller_num[32], caller_name[48];
			snprintf(caller_num,  sizeof(caller_num),  "%" PRId64, peer_user);
			snprintf(caller_name, sizeof(caller_name), "TG:%s", caller_num);

			switch_channel_t *ch = switch_core_session_get_channel(sess);
			switch_caller_profile_t *cp = switch_caller_profile_new(
				switch_core_session_get_pool(sess),
				"telegram", globals.dialplan,
				caller_name, caller_num,
				NULL, NULL, NULL, NULL,
				(char*)modname, globals.context,
				profile->tg_profile_name);
			switch_channel_set_caller_profile(ch, cp);
			switch_set_flag_locked(tp, TFLAG_INBOUND);

			const char *uuid = switch_core_session_get_uuid(sess);
			tg_call_entry *e = (tg_call_entry*)calloc(1, sizeof(tg_call_entry));
			strncpy(e->session_uuid, uuid, SWITCH_UUID_FORMATTED_LENGTH);
			switch_mutex_lock(profile->calls_mutex);
			switch_core_hash_insert(profile->calls, id_str, e);
			switch_mutex_unlock(profile->calls_mutex);

			switch_channel_set_state(ch, CS_INIT);
			if (switch_core_session_thread_launch(sess) != SWITCH_STATUS_SUCCESS) {
				switch_mutex_lock(profile->calls_mutex);
				free(switch_core_hash_delete(profile->calls, id_str));
				switch_mutex_unlock(profile->calls_mutex);
				switch_core_session_destroy(&sess);
				auto d = td::td_api::make_object<td::td_api::discardCall>();
				d->call_id_ = call_id;
				telegram_client_manager->send(profile->tg_client_id,
					tg_new_request_id(), std::move(d));
				break;
			}

			telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(),
				td::td_api::make_object<td::td_api::acceptCall>(call_id, tg_make_protocol()));
		}
		break;

	case td::td_api::callStateExchangingKeys::ID:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Telegram [%s]: call %d exchanging keys\n",
			profile->tg_profile_name, call_id);
		break;

	case td::td_api::callStateReady::ID: {
		auto &ready = static_cast<td::td_api::callStateReady&>(*call.state_);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: call %d ready\n", profile->tg_profile_name, call_id);

		char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
		switch_mutex_lock(profile->calls_mutex);
		tg_call_entry *e = (tg_call_entry*)switch_core_hash_find(profile->calls, id_str);
		if (e) strncpy(uuid, e->session_uuid, SWITCH_UUID_FORMATTED_LENGTH);
		switch_mutex_unlock(profile->calls_mutex);

		if (uuid[0])
			tg_start_media(profile, call_id, ready, is_outgoing, uuid);
		else
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"Telegram [%s]: no session for ready call %d\n",
				profile->tg_profile_name, call_id);
		break;
	}

	case td::td_api::callStateDiscarded::ID:
	case td::td_api::callStateError::ID: {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
			"Telegram [%s]: call %d ended\n", profile->tg_profile_name, call_id);

		char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
		switch_mutex_lock(profile->calls_mutex);
		tg_call_entry *e = (tg_call_entry*)switch_core_hash_delete(profile->calls, id_str);
		if (e) {
			strncpy(uuid, e->session_uuid, SWITCH_UUID_FORMATTED_LENGTH);
			free(e);
		}
		switch_mutex_unlock(profile->calls_mutex);

		if (uuid[0]) {
			switch_core_session_t *sess = switch_core_session_locate(uuid);
			if (sess) {
				switch_channel_hangup(switch_core_session_get_channel(sess),
					SWITCH_CAUSE_NORMAL_CLEARING);
				switch_core_session_rwunlock(sess);
			}
		}
		break;
	}

	default:
		break;
	}
}

/* ── Chat (SMS) interface ───────────────────────────────────────────────────── */

static switch_status_t tg_chat_send(switch_event_t *event)
{
	const char *profile_name = switch_event_get_header(event, "to_host");
	const char *to_user      = switch_event_get_header(event, "to_user");
	const char *body         = switch_event_get_body(event);

	/* The fs_cli "chat" API command populates only the "to" header (user@host).
	 * Parse it as a fallback when to_user/to_host are absent. */
	char to_user_buf[64] = {0}, to_host_buf[128] = {0};
	if (zstr(profile_name) || zstr(to_user)) {
		const char *to = switch_event_get_header(event, "to");
		if (!zstr(to)) {
			const char *at = strchr(to, '@');
			if (at && (size_t)(at - to) < sizeof(to_user_buf)) {
				switch_snprintf(to_user_buf, at - to + 1, "%s", to);
				switch_snprintf(to_host_buf, sizeof(to_host_buf), "%s", at + 1);
				to_user      = to_user_buf;
				profile_name = to_host_buf;
			}
		}
	}

	if (zstr(profile_name) || zstr(to_user) || zstr(body)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram chat_send: missing to_host, to_user, or body\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_mutex_lock(globals.profiles_mutex);
	tg_profile_t *profile = tg_find_profile(profile_name);
	switch_mutex_unlock(globals.profiles_mutex);

	if (!profile || !profile->tg_client_id || profile->tg_profile_status != TG_PROFILE_STATUS_UP) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram chat_send: profile '%s' not found or not ready\n", profile_name);
		return SWITCH_STATUS_FALSE;
	}

	int64_t chat_id = (int64_t)atoll(to_user);
	if (!chat_id) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Telegram chat_send: invalid to_user '%s'\n", to_user);
		return SWITCH_STATUS_FALSE;
	}

	auto text_obj = td::td_api::make_object<td::td_api::formattedText>();
	text_obj->text_ = body;

	auto msg_text = td::td_api::make_object<td::td_api::inputMessageText>();
	msg_text->text_ = std::move(text_obj);

	const char *reply_hdr = switch_event_get_header(event, "Reply-To-Message-Id");
	int64_t reply_to_id = !zstr(reply_hdr) ? (int64_t)atoll(reply_hdr) : 0;

	auto send_req = td::td_api::make_object<td::td_api::sendMessage>();
	send_req->chat_id_               = chat_id;
	send_req->input_message_content_ = std::move(msg_text);
	if (reply_to_id) {
		auto reply_to = td::td_api::make_object<td::td_api::inputMessageReplyToMessage>();
		reply_to->message_id_ = reply_to_id;
		send_req->reply_to_   = std::move(reply_to);
	}

	telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(send_req));

	if (reply_to_id) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Telegram [%s]: sent text to chat %" PRId64 " reply_to=%" PRId64 "\n",
			profile_name, chat_id, reply_to_id);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Telegram [%s]: sent text to chat %" PRId64 "\n",
			profile_name, chat_id);
	}

	return SWITCH_STATUS_SUCCESS;
}

/* ── Module lifecycle ───────────────────────────────────────────────────────── */

SWITCH_MODULE_LOAD_FUNCTION(mod_telegram_load)
{
	switch_api_interface_t *api_interface;
	switch_hash_index_t    *hi;
	const void             *key;
	void                   *val;

	module_pool = pool;

	tgcalls::Register<tgcalls::InstanceImpl>();
	tgcalls::Register<tgcalls::InstanceV2Impl>();
	tgcalls::Register<tgcalls::InstanceV2ReferenceImpl>();

	telegram_client_manager = new td::ClientManager();
	td::ClientManager::set_log_message_callback(3, tdlib_log_callback);
	rtc::LogMessage::AddLogToStream(&g_rtc_log_sink, rtc::LS_INFO);

	switch_mutex_init(&globals.mutex,             SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&globals.profiles_mutex,    SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&tg_pending_downloads_mutex,SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&tg_dl_db_mutex,            SWITCH_MUTEX_NESTED, pool);
	switch_mutex_init(&tg_inline_flows_mutex,     SWITCH_MUTEX_NESTED, pool);
	switch_core_hash_init(&globals.profiles);

	tg_db_open();

	load_config();

	/* Register tab-completion hints: profile names as first argument for each command. */
	{
		static const char * const cmds[] = {
			"telegram_status", "telegram_login", "telegram_code",
			"telegram_password", "telegram_start", "telegram_stop", nullptr
		};
		switch_hash_index_t *phi;
		const void          *pkey;
		void                *pval;
		switch_mutex_lock(globals.profiles_mutex);
		for (phi = switch_core_hash_first(globals.profiles); phi; phi = switch_core_hash_next(&phi)) {
			switch_core_hash_this(phi, &pkey, NULL, &pval);
			for (int ci = 0; cmds[ci]; ci++) {
				char buf[128];
				snprintf(buf, sizeof(buf), "add %s %s", cmds[ci], (const char *)pkey);
				switch_console_set_complete(buf);
			}
		}
		switch_mutex_unlock(globals.profiles_mutex);
	}

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	telegram_endpoint_interface = static_cast<switch_endpoint_interface_t *>(
		switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE));
	telegram_endpoint_interface->interface_name = "telegram";
	telegram_endpoint_interface->io_routines    = &telegram_io_routines;
	telegram_endpoint_interface->state_handler  = &telegram_state_handlers;

	SWITCH_ADD_API(api_interface, "telegram_status",
				   "Show Telegram profile status",
				   telegram_status_api, "");
	SWITCH_ADD_API(api_interface, "telegram_login",
				   "Send phone number for Telegram authentication",
				   telegram_login_api, "<profile> <phone>");
	SWITCH_ADD_API(api_interface, "telegram_code",
				   "Send Telegram authentication code",
				   telegram_code_api, "<profile> <code>");
	SWITCH_ADD_API(api_interface, "telegram_password",
				   "Send Telegram 2FA password",
				   telegram_password_api, "<profile> <password>");
	SWITCH_ADD_API(api_interface, "telegram_start",
				   "Start a Telegram profile",
				   telegram_start_api, "<profile>");
	SWITCH_ADD_API(api_interface, "telegram_stop",
				   "Stop a Telegram profile",
				   telegram_stop_api, "<profile>");
	SWITCH_ADD_API(api_interface, "telegram_reload",
				   "Reload telegram.conf.xml settings without restarting profiles",
				   telegram_reload_api, "");
	SWITCH_ADD_API(api_interface, "tg_react",
				   "Send a reaction to a Telegram message",
				   tg_react_api, "<profile> <chat_id> <message_id> <emoji>");
	SWITCH_ADD_API(api_interface, "tg_send_photo",
				   "Send a photo to a Telegram chat",
				   tg_send_photo_api, "<profile> <chat_id> <file_path> [caption]");
	SWITCH_ADD_API(api_interface, "tg_send_document",
				   "Send a document/file to a Telegram chat",
				   tg_send_document_api, "<profile> <chat_id> <file_path> [caption]");
	SWITCH_ADD_API(api_interface, "tg_send_audio",
				   "Send an audio file to a Telegram chat",
				   tg_send_audio_api, "<profile> <chat_id> <file_path> [duration] [title] [performer]");
	SWITCH_ADD_API(api_interface, "tg_send_video",
				   "Send a video to a Telegram chat",
				   tg_send_video_api, "<profile> <chat_id> <file_path> [duration] [width] [height] [caption]");
	SWITCH_ADD_API(api_interface, "tg_send_contact",
				   "Send a contact card to a Telegram chat",
				   tg_send_contact_api, "<profile> <chat_id> <phone> <first_name> [last_name]");
	SWITCH_ADD_API(api_interface, "tg_send_location",
				   "Send a map location to a Telegram chat",
				   tg_send_location_api, "<profile> <chat_id> <latitude> <longitude>");
	SWITCH_ADD_API(api_interface, "tg_send_album",
				   "Send multiple photos/videos as a single album message",
				   tg_send_album_api, "<profile> <chat_id> [reply_to:<id>] <file1> [file2 ...]");
	SWITCH_ADD_API(api_interface, "tg_recognize_speech",
				   "Request Telegram speech recognition on a voice message",
				   tg_recognize_speech_api, "<profile> <chat_id> <message_id>");
	SWITCH_ADD_API(api_interface, "tg_send_text",
				   "Send a text message to a Telegram chat",
				   tg_send_text_api, "<profile> <chat_id> [reply_to:<id>] <text>");
	SWITCH_ADD_API(api_interface, "tg_send_voice",
				   "Send a voice note to a Telegram chat",
				   tg_send_voice_api, "<profile> <chat_id> [reply_to:<id>] <file_path> [duration]");
	SWITCH_ADD_API(api_interface, "tg_send_video_note",
				   "Send a round video note to a Telegram chat",
				   tg_send_video_note_api, "<profile> <chat_id> [reply_to:<id>] <file_path> [duration] [length]");
	SWITCH_ADD_API(api_interface, "tg_delete_messages",
				   "Delete messages from a Telegram chat",
				   tg_delete_messages_api, "<profile> <chat_id> <msg_id1> [msg_id2 ...]");
	SWITCH_ADD_API(api_interface, "tg_edit_message_text",
				   "Edit the text of an outgoing Telegram message",
				   tg_edit_message_text_api, "<profile> <chat_id> <msg_id> <new_text>");
	SWITCH_ADD_API(api_interface, "tg_edit_message_caption",
				   "Edit the caption of an outgoing Telegram photo/video/document",
				   tg_edit_message_caption_api, "<profile> <chat_id> <msg_id> <new_caption>");
	SWITCH_ADD_API(api_interface, "tg_clear_chat_history",
				   "Delete all messages in a Telegram chat for both sides",
				   tg_clear_chat_history_api, "<profile> <chat_id>");
	SWITCH_ADD_API(api_interface, "tg_send_inline_result",
				   "Query a bot inline and send its first result to a chat",
				   tg_send_inline_result_api, "<profile> <chat_id> <bot_username> [query]");

	switch_chat_interface_t *chat_interface;
	SWITCH_ADD_CHAT(chat_interface, "telegram", tg_chat_send);

	/* Start TDLib client for each enabled profile.
	 * A getOption("version") kick-starts the update stream so TDLib immediately
	 * sends the first updateAuthorizationState to the runtime thread. */
	switch_mutex_lock(globals.profiles_mutex);
	for (hi = switch_core_hash_first(globals.profiles); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &key, NULL, &val);
		tg_profile_t *p = (tg_profile_t *)val;
		if (p->tg_profile_enabled) {
			p->tg_client_id = telegram_client_manager->create_client_id();
			telegram_client_manager->send(p->tg_client_id, tg_new_request_id(),
				td::td_api::make_object<td::td_api::getOption>("version"));
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram profile [%s]: client started (id=%d)\n",
				p->tg_profile_name, (int)p->tg_client_id);
		}
	}
	switch_mutex_unlock(globals.profiles_mutex);

	/* Recover any downloads that were pending before the last restart. */
	tg_db_recover_downloads();

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Telegram endpoint loaded\n");
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_telegram_runtime)
{
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Telegram runtime thread started\n");

	while (running) {
		auto response = telegram_client_manager->receive(1.0);
		if (!response.object) continue;

		tg_profile_t *profile = tg_find_profile_by_client_id(response.client_id);
		if (!profile) continue;

		if (response.object->get_id() == td::td_api::error::ID) {
			auto &err = static_cast<td::td_api::error &>(*response.object);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				"Telegram [%s]: TDLib error %d: %s\n",
				profile->tg_profile_name, err.code_, err.message_.c_str());
			/* Clean up any pending inline flow waiting on this request */
			if (response.request_id != 0) {
				switch_mutex_lock(tg_inline_flows_mutex);
				tg_inline_flows.erase(response.request_id);
				switch_mutex_unlock(tg_inline_flows_mutex);
			}
			continue;
		}

		if (response.object->get_id() == td::td_api::updateAuthorizationState::ID) {
			auto &upd = static_cast<td::td_api::updateAuthorizationState &>(*response.object);
			if (upd.authorization_state_)
				tg_handle_auth_state(profile, *upd.authorization_state_);
			continue;
		}

		if (response.object->get_id() == td::td_api::updateCall::ID) {
			auto &upd = static_cast<td::td_api::updateCall &>(*response.object);
			tg_handle_call_update(profile, upd);
			continue;
		}

		if (response.object->get_id() == td::td_api::updateNewCallSignalingData::ID) {
			auto &sig = static_cast<td::td_api::updateNewCallSignalingData &>(*response.object);
			char  id_str[24];
			snprintf(id_str, sizeof(id_str), "%d", sig.call_id_);

			static std::atomic<int> _sig_seq{0};
			int seq = ++_sig_seq;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram: updateNewCallSignalingData #%d call=%d sz=%zu\n",
				seq, sig.call_id_, sig.data_.size());

			char uuid[SWITCH_UUID_FORMATTED_LENGTH + 1] = "";
			switch_mutex_lock(profile->calls_mutex);
			tg_call_entry *e = (tg_call_entry*)switch_core_hash_find(profile->calls, id_str);
			if (e) strncpy(uuid, e->session_uuid, SWITCH_UUID_FORMATTED_LENGTH);
			switch_mutex_unlock(profile->calls_mutex);

			if (!uuid[0]) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"Telegram: sigdata #%d call=%d: no call entry — DROPPED\n", seq, sig.call_id_);
			} else {
				switch_core_session_t *sess = switch_core_session_locate(uuid);
				if (!sess) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"Telegram: sigdata #%d call=%d: session gone — DROPPED\n", seq, sig.call_id_);
				} else {
					private_t *tp = static_cast<private_t*>(
						switch_core_session_get_private(sess));
					if (!tp || !tp->call_media || !tp->call_media->instance) {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							"Telegram: sigdata #%d call=%d: call_media not ready (tp=%p cm=%p) — DROPPED\n",
							seq, sig.call_id_,
							(void*)tp,
							tp ? (void*)tp->call_media : nullptr);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
							"Telegram: sigdata #%d call=%d: delivering to tgcalls\n", seq, sig.call_id_);
						std::vector<uint8_t> data(sig.data_.begin(), sig.data_.end());
						tp->call_media->instance->receiveSignalingData(data);
					}
					switch_core_session_rwunlock(sess);
				}
			}
			continue;
		}

		if (response.object->get_id() == td::td_api::updateNewMessage::ID) {
			auto &upd = static_cast<td::td_api::updateNewMessage &>(*response.object);
			auto &msg = *upd.message_;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"Telegram [%s]: updateNewMessage chat=%" PRId64 " msg_id=%" PRId64 " outgoing=%d content_id=%d\n",
				profile->tg_profile_name, msg.chat_id_, msg.id_,
				(int)msg.is_outgoing_,
				msg.content_ ? msg.content_->get_id() : 0);

			if (msg.is_outgoing_) continue;
			if (!msg.content_) continue;

			/* Extract sender for all inbound message types. */
			int64_t from_id = 0;
			if (msg.sender_id_ && msg.sender_id_->get_id() == td::td_api::messageSenderUser::ID)
				from_id = static_cast<td::td_api::messageSenderUser &>(*msg.sender_id_).user_id_;

			int content_id = msg.content_->get_id();

			/* ── Text messages ─────────────────────────────────────────────── */
			if (content_id == td::td_api::messageText::ID) {
				auto &mt = static_cast<td::td_api::messageText &>(*msg.content_);
				if (!mt.text_) continue;
				const std::string &body = mt.text_->text_;

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					"Telegram [%s]: inbound text msg_id=%" PRId64 " from user %" PRId64 ": %s\n",
					profile->tg_profile_name, msg.id_, from_id, body.c_str());

				char from_str[32];
				snprintf(from_str, sizeof(from_str), "%" PRId64, from_id);

				const char *dest_proto = !zstr(profile->tg_inbound_dest_proto)
					? profile->tg_inbound_dest_proto : "telegram";

				switch_event_t *ev = nullptr;
				if (switch_event_create(&ev, SWITCH_EVENT_MESSAGE) == SWITCH_STATUS_SUCCESS) {
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "proto",      "telegram");
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "from",       from_str);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "from_user",  from_str);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "from_host",  profile->tg_profile_name);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "to",         profile->tg_profile_name);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "to_user",    profile->tg_profile_name);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "to_host",    profile->tg_profile_name);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "type",       "text/plain");
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "context",    "default");
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "body",       body.c_str());
					switch_event_add_body(ev, "%s", body.c_str());
					switch_core_chat_deliver(dest_proto, &ev);
				}
				continue;
			}

			/* ── File messages — queue download ────────────────────────────── */
			{
				tg_pending_download pd = {};
				strncpy(pd.profile_name, profile->tg_profile_name, sizeof(pd.profile_name) - 1);
				pd.chat_id      = msg.chat_id_;
				pd.from_user_id = from_id;
				pd.msg_id       = msg.id_;

				const td::td_api::file *dl_file = nullptr;

#define TG_COPY(dst, src) if (!(src).empty()) strncpy(dst, (src).c_str(), sizeof(dst) - 1)
#define TG_CAP(obj) \
	if ((obj).caption_ && !(obj).caption_->text_.empty()) \
		strncpy(pd.caption, (obj).caption_->text_.c_str(), sizeof(pd.caption) - 1)

				if (content_id == td::td_api::messageDocument::ID) {
					auto &c = static_cast<td::td_api::messageDocument &>(*msg.content_);
					strncpy(pd.file_type, "document", sizeof(pd.file_type) - 1);
					if (c.document_) {
						TG_COPY(pd.file_name, c.document_->file_name_);
						TG_COPY(pd.mime_type, c.document_->mime_type_);
						dl_file = c.document_->document_.get();
					}
					TG_CAP(c);

				} else if (content_id == td::td_api::messagePhoto::ID) {
					auto &c = static_cast<td::td_api::messagePhoto &>(*msg.content_);
					strncpy(pd.file_type, "photo", sizeof(pd.file_type) - 1);
					if (c.photo_ && !c.photo_->sizes_.empty()) {
						auto &sz = *c.photo_->sizes_.back();
						pd.width  = sz.width_;
						pd.height = sz.height_;
						dl_file   = sz.photo_.get();
					}
					TG_CAP(c);

				} else if (content_id == td::td_api::messageAudio::ID) {
					auto &c = static_cast<td::td_api::messageAudio &>(*msg.content_);
					strncpy(pd.file_type, "audio", sizeof(pd.file_type) - 1);
					if (c.audio_) {
						pd.duration = c.audio_->duration_;
						TG_COPY(pd.audio_title,    c.audio_->title_);
						TG_COPY(pd.audio_performer,c.audio_->performer_);
						TG_COPY(pd.mime_type,      c.audio_->mime_type_);
						dl_file = c.audio_->audio_.get();
					}
					TG_CAP(c);

				} else if (content_id == td::td_api::messageVideo::ID) {
					auto &c = static_cast<td::td_api::messageVideo &>(*msg.content_);
					strncpy(pd.file_type, "video", sizeof(pd.file_type) - 1);
					if (c.video_) {
						pd.duration = c.video_->duration_;
						pd.width    = c.video_->width_;
						pd.height   = c.video_->height_;
						TG_COPY(pd.mime_type, c.video_->mime_type_);
						dl_file = c.video_->video_.get();
					}
					TG_CAP(c);

				} else if (content_id == td::td_api::messageVoiceNote::ID) {
					auto &c = static_cast<td::td_api::messageVoiceNote &>(*msg.content_);
					strncpy(pd.file_type, "voice", sizeof(pd.file_type) - 1);
					if (c.voice_note_) {
						pd.duration = c.voice_note_->duration_;
						TG_COPY(pd.mime_type, c.voice_note_->mime_type_);
						dl_file = c.voice_note_->voice_.get();
					}
					TG_CAP(c);

				} else if (content_id == td::td_api::messageVideoNote::ID) {
					auto &c = static_cast<td::td_api::messageVideoNote &>(*msg.content_);
					strncpy(pd.file_type, "video_note", sizeof(pd.file_type) - 1);
					if (c.video_note_) {
						pd.duration = c.video_note_->duration_;
						pd.width    = c.video_note_->length_;
						pd.height   = c.video_note_->length_;
						dl_file = c.video_note_->video_.get();
					}

				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
						"Telegram [%s]: updateNewMessage skipped — unsupported content_id=%d\n",
						profile->tg_profile_name, content_id);
					continue;
				}
#undef TG_COPY
#undef TG_CAP

				if (!dl_file) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						"Telegram [%s]: inbound %s msg_id=%" PRId64 " — no file object, skipping\n",
						profile->tg_profile_name, pd.file_type, msg.id_);
					continue;
				}

				pd.file_id = dl_file->id_;
				char map_key[128];
				snprintf(map_key, sizeof(map_key), "%s:%d", profile->tg_profile_name, pd.file_id);

				switch_mutex_lock(tg_pending_downloads_mutex);
				tg_pending_downloads[map_key] = pd;
				switch_mutex_unlock(tg_pending_downloads_mutex);
				tg_db_insert(map_key, pd);

				auto dl_req = td::td_api::make_object<td::td_api::downloadFile>();
				dl_req->file_id_     = pd.file_id;
				dl_req->priority_    = 1;
				dl_req->offset_      = 0;
				dl_req->limit_       = 0;
				dl_req->synchronous_ = false;
				telegram_client_manager->send(profile->tg_client_id, tg_new_request_id(), std::move(dl_req));

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					"Telegram [%s]: queued download file_id=%d type=%s msg_id=%" PRId64 " from user %" PRId64 "\n",
					profile->tg_profile_name, pd.file_id, pd.file_type, msg.id_, from_id);
			}
			continue;
		}

		/* updateMessageContent — fires when message content changes, e.g. speech recognition result */
		if (response.object->get_id() == td::td_api::updateMessageContent::ID) {
			auto &upd = static_cast<td::td_api::updateMessageContent &>(*response.object);
			if (!upd.new_content_) { continue; }

			const std::string *text = nullptr;
			bool is_error = false;

			if (upd.new_content_->get_id() == td::td_api::messageVoiceNote::ID) {
				auto &vn = static_cast<td::td_api::messageVoiceNote &>(*upd.new_content_);
				if (vn.voice_note_ && vn.voice_note_->speech_recognition_result_) {
					auto &r = *vn.voice_note_->speech_recognition_result_;
					if (r.get_id() == td::td_api::speechRecognitionResultText::ID) {
						text = &static_cast<td::td_api::speechRecognitionResultText &>(r).text_;
					} else if (r.get_id() == td::td_api::speechRecognitionResultError::ID) {
						is_error = true;
					}
				}
			} else if (upd.new_content_->get_id() == td::td_api::messageVideoNote::ID) {
				auto &vn = static_cast<td::td_api::messageVideoNote &>(*upd.new_content_);
				if (vn.video_note_ && vn.video_note_->speech_recognition_result_) {
					auto &r = *vn.video_note_->speech_recognition_result_;
					if (r.get_id() == td::td_api::speechRecognitionResultText::ID) {
						text = &static_cast<td::td_api::speechRecognitionResultText &>(r).text_;
					} else if (r.get_id() == td::td_api::speechRecognitionResultError::ID) {
						is_error = true;
					}
				}
			}

			if (!text && !is_error) continue;

			char chat_str[32], msg_str[32];
			snprintf(chat_str, sizeof(chat_str), "%" PRId64, upd.chat_id_);
			snprintf(msg_str,  sizeof(msg_str),  "%" PRId64, upd.message_id_);

			if (is_error) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
					"Telegram [%s]: speech recognition failed for message %" PRId64 "\n",
					profile->tg_profile_name, upd.message_id_);
			} else {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					"Telegram [%s]: speech recognition result for message %" PRId64 ": %s\n",
					profile->tg_profile_name, upd.message_id_, text->c_str());
			}

			switch_event_t *ev = nullptr;
			if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, "TELEGRAM::SPEECH_RECOGNITION") != SWITCH_STATUS_SUCCESS) continue;
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Profile",    profile->tg_profile_name);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Chat-Id",    chat_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Message-Id", msg_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Status",     is_error ? "error" : "ok");
			if (text) {
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Text", text->c_str());
				switch_event_add_body(ev, "%s", text->c_str());
			}
			switch_event_fire(&ev);
			continue;
		}

		/* updateFile — fires during and after downloadFile; we act only on completion */
		if (response.object->get_id() == td::td_api::updateFile::ID) {
			auto &upd = static_cast<td::td_api::updateFile &>(*response.object);
			if (!upd.file_ || !upd.file_->local_) continue;
			if (!upd.file_->local_->is_downloading_completed_) continue;

			auto &f = *upd.file_;
			char map_key[128];
			snprintf(map_key, sizeof(map_key), "%s:%d", profile->tg_profile_name, f.id_);

			switch_mutex_lock(tg_pending_downloads_mutex);
			auto it = tg_pending_downloads.find(map_key);
			if (it == tg_pending_downloads.end()) {
				switch_mutex_unlock(tg_pending_downloads_mutex);
				continue;
			}
			tg_pending_download pd = it->second;
			tg_pending_downloads.erase(it);
			switch_mutex_unlock(tg_pending_downloads_mutex);

			tg_db_delete(map_key);

			const std::string &local_path = f.local_->path_;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: download complete file_id=%d type=%s path=%s\n",
				pd.profile_name, f.id_, pd.file_type, local_path.c_str());

			switch_event_t *ev = nullptr;
			if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, "TELEGRAM::INBOUND_FILE") != SWITCH_STATUS_SUCCESS) continue;

			char chat_str[32], from_str[32], msg_str[32], size_str[32], dur_str[16], w_str[16], h_str[16];
			snprintf(chat_str, sizeof(chat_str), "%" PRId64, pd.chat_id);
			snprintf(from_str, sizeof(from_str), "%" PRId64, pd.from_user_id);
			snprintf(msg_str,  sizeof(msg_str),  "%" PRId64, pd.msg_id);
			snprintf(size_str, sizeof(size_str), "%" PRId64, (int64_t)f.size_);
			snprintf(dur_str,  sizeof(dur_str),  "%d",       pd.duration);
			snprintf(w_str,    sizeof(w_str),    "%d",       pd.width);
			snprintf(h_str,    sizeof(h_str),    "%d",       pd.height);

			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Profile",      pd.profile_name);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Chat-Id",      chat_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-From-User-Id", from_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Message-Id",   msg_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-File-Type",    pd.file_type);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-File-Path",    local_path.c_str());
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-File-Size",    size_str);
			if (pd.file_name[0])
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-File-Name",    pd.file_name);
			if (pd.mime_type[0])
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Mime-Type",    pd.mime_type);
			if (pd.caption[0])
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Caption",      pd.caption);
			if (pd.duration) {
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Duration",     dur_str);
			}
			if (pd.width) {
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Width",        w_str);
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Height",       h_str);
			}
			if (pd.audio_title[0])
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Audio-Title",     pd.audio_title);
			if (pd.audio_performer[0])
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Audio-Performer", pd.audio_performer);

			switch_event_fire(&ev);
			continue;
		}

		/* updateMessageInteractionInfo — reactions on channels/groups (has total counts per type) */
		if (response.object->get_id() == td::td_api::updateMessageInteractionInfo::ID) {
			auto &upd = static_cast<td::td_api::updateMessageInteractionInfo &>(*response.object);

			if (!upd.interaction_info_ || !upd.interaction_info_->reactions_) continue;
			auto &rxns = *upd.interaction_info_->reactions_;
			if (rxns.reactions_.empty()) continue;

			switch_event_t *ev = nullptr;
			if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, "TELEGRAM::REACTION") != SWITCH_STATUS_SUCCESS) continue;

			char chat_str[32], msg_str[32], count_str[16];
			snprintf(chat_str,  sizeof(chat_str),  "%" PRId64, upd.chat_id_);
			snprintf(msg_str,   sizeof(msg_str),   "%" PRId64, upd.message_id_);
			snprintf(count_str, sizeof(count_str), "%zu", rxns.reactions_.size());

			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Profile",        profile->tg_profile_name);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Chat-Id",        chat_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Message-Id",     msg_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Reaction-Count", count_str);

			int ridx = 0;
			for (auto &r : rxns.reactions_) {
				char key[64], val[16];

				snprintf(key, sizeof(key), "Telegram-Reaction-%d-Total", ridx);
				snprintf(val, sizeof(val), "%d", r->total_count_);
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, val);

				snprintf(key, sizeof(key), "Telegram-Reaction-%d-Chosen", ridx);
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, r->is_chosen_ ? "true" : "false");

				if (r->type_ && r->type_->get_id() == td::td_api::reactionTypeEmoji::ID) {
					auto &rt = static_cast<td::td_api::reactionTypeEmoji &>(*r->type_);
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "emoji");
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Emoji", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, rt.emoji_.c_str());
				} else if (r->type_ && r->type_->get_id() == td::td_api::reactionTypeCustomEmoji::ID) {
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "custom-emoji");
				} else {
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "paid");
				}
				ridx++;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: reaction on message %" PRId64 " in chat %" PRId64 " (%zu type(s))\n",
				profile->tg_profile_name, upd.message_id_, upd.chat_id_, rxns.reactions_.size());

			switch_event_fire(&ev);
			continue;
		}

		/* updateMessageUnreadReactions — per-message unread reactions in private chats
		 * (includes sender_id and emoji per reaction; fires when reactions are first unread) */
		if (response.object->get_id() == td::td_api::updateMessageUnreadReactions::ID) {
			auto &upd = static_cast<td::td_api::updateMessageUnreadReactions &>(*response.object);

			if (upd.unread_reactions_.empty()) continue;

			switch_event_t *ev = nullptr;
			if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, "TELEGRAM::REACTION") != SWITCH_STATUS_SUCCESS) continue;

			char chat_str[32], msg_str[32], count_str[16];
			snprintf(chat_str,  sizeof(chat_str),  "%" PRId64, upd.chat_id_);
			snprintf(msg_str,   sizeof(msg_str),   "%" PRId64, upd.message_id_);
			snprintf(count_str, sizeof(count_str), "%d",       upd.unread_reaction_count_);

			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Profile",        profile->tg_profile_name);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Chat-Id",        chat_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Message-Id",     msg_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Reaction-Count", count_str);

			std::string emoji_list;
			int ridx = 0;
			for (auto &r : upd.unread_reactions_) {
				char key[64], sender_str[32];

				if (r->sender_id_ && r->sender_id_->get_id() == td::td_api::messageSenderUser::ID) {
					snprintf(sender_str, sizeof(sender_str), "%" PRId64,
						static_cast<td::td_api::messageSenderUser &>(*r->sender_id_).user_id_);
				} else if (r->sender_id_ && r->sender_id_->get_id() == td::td_api::messageSenderChat::ID) {
					snprintf(sender_str, sizeof(sender_str), "%" PRId64,
						static_cast<td::td_api::messageSenderChat &>(*r->sender_id_).chat_id_);
				} else {
					snprintf(sender_str, sizeof(sender_str), "unknown");
				}

				snprintf(key, sizeof(key), "Telegram-Reaction-%d-Sender", ridx);
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, sender_str);

				snprintf(key, sizeof(key), "Telegram-Reaction-%d-Big", ridx);
				switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, r->is_big_ ? "true" : "false");

				if (r->type_ && r->type_->get_id() == td::td_api::reactionTypeEmoji::ID) {
					const std::string &emoji = static_cast<td::td_api::reactionTypeEmoji &>(*r->type_).emoji_;
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "emoji");
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Emoji", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, emoji.c_str());
					if (!emoji_list.empty()) emoji_list += " ";
					emoji_list += emoji;
				} else if (r->type_ && r->type_->get_id() == td::td_api::reactionTypeCustomEmoji::ID) {
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "custom-emoji");
					if (!emoji_list.empty()) emoji_list += " ";
					emoji_list += "[custom]";
				} else {
					snprintf(key, sizeof(key), "Telegram-Reaction-%d-Type", ridx);
					switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, key, "paid");
					if (!emoji_list.empty()) emoji_list += " ";
					emoji_list += "[paid]";
				}
				ridx++;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: unread reaction(s) on message %" PRId64 " in chat %" PRId64 ": %s\n",
				profile->tg_profile_name, upd.message_id_, upd.chat_id_, emoji_list.c_str());

			switch_event_fire(&ev);
			continue;
		}

		/* updateChatUnreadReactionCount — chat-level unread reaction count (no per-message detail) */
		if (response.object->get_id() == td::td_api::updateChatUnreadReactionCount::ID) {
			auto &upd = static_cast<td::td_api::updateChatUnreadReactionCount &>(*response.object);

			if (upd.unread_reaction_count_ == 0) continue;

			switch_event_t *ev = nullptr;
			if (switch_event_create_subclass(&ev, SWITCH_EVENT_CUSTOM, "TELEGRAM::REACTION_COUNT") != SWITCH_STATUS_SUCCESS) continue;

			char chat_str[32], count_str[16];
			snprintf(chat_str,  sizeof(chat_str),  "%" PRId64, upd.chat_id_);
			snprintf(count_str, sizeof(count_str), "%d",       upd.unread_reaction_count_);

			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Profile",        profile->tg_profile_name);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Chat-Id",        chat_str);
			switch_event_add_header_string(ev, SWITCH_STACK_BOTTOM, "Telegram-Reaction-Count", count_str);

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				"Telegram [%s]: %d unread reaction(s) in chat %" PRId64 "\n",
				profile->tg_profile_name, upd.unread_reaction_count_, upd.chat_id_);

			switch_event_fire(&ev);
			continue;
		}

		/* ── Inline query flow (response to searchPublicChat / getInlineQueryResults) ── */
		if (response.request_id != 0) {
			switch_mutex_lock(tg_inline_flows_mutex);
			auto fit = tg_inline_flows.find(response.request_id);
			if (fit != tg_inline_flows.end()) {
				tg_inline_flow flow = fit->second;
				tg_inline_flows.erase(fit);
				switch_mutex_unlock(tg_inline_flows_mutex);

				if (flow.phase == tg_inline_flow::SEARCHING &&
				    response.object->get_id() == td::td_api::chat::ID) {
					auto &ch = static_cast<td::td_api::chat &>(*response.object);
					int64_t bot_user_id = 0;
					if (ch.type_ && ch.type_->get_id() == td::td_api::chatTypePrivate::ID)
						bot_user_id = static_cast<td::td_api::chatTypePrivate &>(*ch.type_).user_id_;
					if (bot_user_id) {
						auto req = td::td_api::make_object<td::td_api::getInlineQueryResults>();
						req->bot_user_id_ = bot_user_id;
						req->chat_id_     = flow.chat_id;
						req->query_       = flow.query;
						req->offset_      = "";
						uint64_t new_rid = tg_new_request_id();
						flow.phase = tg_inline_flow::QUERYING;
						switch_mutex_lock(tg_inline_flows_mutex);
						tg_inline_flows[new_rid] = flow;
						switch_mutex_unlock(tg_inline_flows_mutex);
						telegram_client_manager->send(flow.tg_client_id, new_rid, std::move(req));
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
							"Telegram: inline query for bot_user_id=%" PRId64 " query='%s'\n",
							bot_user_id, flow.query.c_str());
					}
				} else if (flow.phase == tg_inline_flow::QUERYING &&
				           response.object->get_id() == td::td_api::inlineQueryResults::ID) {
					auto &res = static_cast<td::td_api::inlineQueryResults &>(*response.object);
					if (!res.results_.empty()) {
						/* Extract id_ from the concrete result type */
						static auto get_id = [](const td::td_api::InlineQueryResult &r) -> std::string {
							#define TRY(T) if (r.get_id() == td::td_api::T::ID) return static_cast<const td::td_api::T &>(r).id_;
							TRY(inlineQueryResultAnimation) TRY(inlineQueryResultArticle)
							TRY(inlineQueryResultAudio)     TRY(inlineQueryResultContact)
							TRY(inlineQueryResultDocument)  TRY(inlineQueryResultGame)
							TRY(inlineQueryResultLocation)  TRY(inlineQueryResultPhoto)
							TRY(inlineQueryResultSticker)   TRY(inlineQueryResultVenue)
							TRY(inlineQueryResultVideo)     TRY(inlineQueryResultVoiceNote)
							#undef TRY
							return "";
						};
						std::string result_id = get_id(*res.results_[0]);
						auto req2 = td::td_api::make_object<td::td_api::sendInlineQueryResultMessage>();
						req2->chat_id_      = flow.chat_id;
						req2->query_id_     = res.inline_query_id_;
						req2->result_id_    = result_id;
						req2->hide_via_bot_ = false;
						telegram_client_manager->send(flow.tg_client_id, tg_new_request_id(), std::move(req2));
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
							"Telegram: sent inline result id='%s' from query %" PRId64 "\n",
							result_id.c_str(), res.inline_query_id_);
					} else {
						switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							"Telegram: inline query returned no results\n");
					}
				}
			} else {
				switch_mutex_unlock(tg_inline_flows_mutex);
			}
		}
	}

	running = -1;
	return SWITCH_STATUS_TERM;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_telegram_shutdown)
{
	int                  x = 0;
	switch_hash_index_t *hi;
	const void          *key;
	void                *val;

	running = 0;

	while (running != -1) {
		if (x++ > 100) break;
		switch_yield(20000);
	}

	td::ClientManager::set_log_message_callback(0, nullptr);
	rtc::LogMessage::RemoveLogToStream(&g_rtc_log_sink);

	delete telegram_client_manager;
	telegram_client_manager = nullptr;

	tg_db_close();

	if (globals.profiles) {
		switch_mutex_lock(globals.profiles_mutex);
		for (hi = switch_core_hash_first(globals.profiles); hi; hi = switch_core_hash_next(&hi)) {
			switch_core_hash_this(hi, &key, NULL, &val);
			tg_profile_free((tg_profile_t *)val);
		}
		switch_core_hash_destroy(&globals.profiles);
		switch_mutex_unlock(globals.profiles_mutex);
	}

	switch_safe_free(globals.server);
	switch_safe_free(globals.dialplan);
	switch_safe_free(globals.context);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Telegram endpoint unloaded\n");
	return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
