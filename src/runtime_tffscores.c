#define _GNU_SOURCE
#include "runtime_tffscores.h"
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#define theatrhythm_gpu_show_notice theatrhythm_gpu_notice
#include "runtime_judgements.h"
#include "runtime_custom_charts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#ifndef NULL
#define NULL NULL
#endif
#include <shellapi.h>
#endif

#define DEFAULT_SERVER_URL "https://tffscores.com"
#define LOOPBACK_PORT 48231
#define BOUNDARY_STRING "----WebKitFormBoundaryTFFScoresRecomp7MA4YWxkTrZu0gW"

static char s_user_dir[512] = "user";
static char s_server_url[256] = DEFAULT_SERVER_URL;
static char s_auth_token[256] = "";
static char s_username[128] = "";
static int s_user_id = 0;
static int s_logged_in = 0;

static volatile int s_song_result_pending = 0;
static volatile uint32_t s_stage_music_id = 0;
static volatile int s_stage_difficulty = -1;
static volatile int s_stage_is_score_pb = 0;
static volatile int s_stage_is_damage_pb = 0;
static volatile int s_awaiting_result_screenshot = 0;
static volatile int s_stage_active = 0;
static volatile int s_stage_is_local_custom = 0;
static volatile uint64_t s_custom_suppress_until_ms = 0;
static char s_stage_save_host[700] = "";
static uint64_t s_last_song_finish_ms = 0;
static volatile int s_cloud_download_checked = 0;


static pthread_mutex_t s_tff_lock = PTHREAD_MUTEX_INITIALIZER;

static int is_local_custom_id(uint32_t music_id) {
    return runtime_custom_charts_is_custom_id(music_id) &&
           !runtime_custom_charts_is_verified_id(music_id);
}

void runtime_tffscores_notify_stage_start(uint32_t music_id, int difficulty) {
    if (music_id == 0) return;
    int is_verified_custom = runtime_custom_charts_is_verified_id(music_id) ? 1 : 0;
    int is_local_custom = is_local_custom_id(music_id) ? 1 : 0;
    pthread_mutex_lock(&s_tff_lock);
    s_last_song_finish_ms = 0;
    if (s_stage_music_id == music_id && s_stage_difficulty >= 4 && difficulty < 4) {
        /* Preserve extra difficulty slot 4/5 if PlaySetting+0x50 was clamped */
    } else {
        s_stage_difficulty = difficulty;
    }
    s_stage_music_id = music_id;
    s_stage_is_score_pb = 0;
    s_stage_is_damage_pb = 0;
    s_stage_active = 1;
    s_stage_is_local_custom = is_local_custom;
    if (!is_local_custom) s_custom_suppress_until_ms = 0;
    s_stage_save_host[0] = '\0';
    s_awaiting_result_screenshot = 0;
    theatrhythm_gpu_clear_last_screenshot();
    pthread_mutex_unlock(&s_tff_lock);
    if (is_local_custom) {
        printf("TFFScores: Local custom chart stage started (placeholder ID %u, diff=%d) -> TFFScores upload disabled\n",
               music_id, s_stage_difficulty);
    } else if (is_verified_custom) {
        printf("TFFScores: Verified custom chart stage started: music_id=%u, diff=%d\n",
               music_id, s_stage_difficulty);
    } else {
        printf("TFFScores: Stage started: music_id=%u, difficulty=%d\n", music_id, s_stage_difficulty);
    }
}

/* Save commit sync queue */
static char s_pending_save_host[700] = "";
static int s_pending_sync_req = 0;
static pthread_cond_t s_sync_cond = PTHREAD_COND_INITIALIZER;

static void *delayed_screenshot_thread(void *arg);

static void trigger_late_pb_screenshot_locked(void) {
    if (!s_stage_active && s_stage_music_id != 0 && !s_stage_is_local_custom &&
        !is_local_custom_id(s_stage_music_id) && !s_awaiting_result_screenshot) {
        s_awaiting_result_screenshot = 1;
        printf("TFFScores: late PB notification after result finish -> starting screenshot capture\n");
        pthread_t th;
        if (pthread_create(&th, NULL, delayed_screenshot_thread, NULL) == 0) {
            pthread_detach(th);
        }
        if (!s_pending_save_host[0]) {
            if (s_stage_save_host[0]) {
                snprintf(s_pending_save_host, sizeof(s_pending_save_host), "%s", s_stage_save_host);
            } else if (runtime_savedata_get_active_save_dir(s_pending_save_host, sizeof(s_pending_save_host)) != 0 ||
                       !s_pending_save_host[0]) {
                snprintf(s_pending_save_host, sizeof(s_pending_save_host), "%s", s_user_dir);
            }
        }
        s_pending_sync_req = 1;
        pthread_cond_signal(&s_sync_cond);
    }
}

void runtime_tffscores_notify_score_pb(int is_pb) {
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_is_local_custom) {
        s_stage_is_score_pb = 0;
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (is_pb) {
        s_stage_is_score_pb = 1;
        trigger_late_pb_screenshot_locked();
    }
    int cur_pb = s_stage_is_score_pb;
    pthread_mutex_unlock(&s_tff_lock);
    printf("TFFScores: Score PB status: %d (effective=%d)\n", is_pb, cur_pb);
}

void runtime_tffscores_notify_damage_pb(int is_pb) {
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_is_local_custom) {
        s_stage_is_damage_pb = 0;
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (is_pb) {
        s_stage_is_damage_pb = 1;
        trigger_late_pb_screenshot_locked();
    }
    int cur_pb = s_stage_is_damage_pb;
    pthread_mutex_unlock(&s_tff_lock);
    printf("TFFScores: Damage PB status: %d (effective=%d)\n", is_pb, cur_pb);
}
static pthread_t s_worker_thread;
static int s_worker_running = 0;

/* Local loopback auth server */
static pthread_t s_auth_thread;
static volatile int s_auth_server_running = 0;

static void json_get_string(const char *json, const char *key, char *out, size_t out_cap) {
    out[0] = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\"') {
        p++;
        const char *end = strchr(p, '\"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (len >= out_cap) len = out_cap - 1;
            memcpy(out, p, len);
            out[len] = 0;
        }
    }
}

static int json_get_int(const char *json, const char *key, int default_val) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return default_val;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static bool get_localappdata_fbl_dir(char *out, size_t cap, bool create_dir) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    const char *local_app = getenv("LOCALAPPDATA");
    if (!local_app || !*local_app) local_app = getenv("APPDATA");
    if (!local_app || !*local_app) return false;
    snprintf(out, cap, "%s\\TheatrhythmFBL", local_app);
    if (create_dir) {
        CreateDirectoryA(out, NULL);
    }
    return true;
}

static void save_session(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/tffscores.json", s_user_dir);
    FILE *f = fopen(path, "w");
    if (!f) {
        char app_dir[512];
        if (get_localappdata_fbl_dir(app_dir, sizeof(app_dir), true)) {
            snprintf(path, sizeof(path), "%s\\tffscores.json", app_dir);
            f = fopen(path, "w");
        }
    }
    if (!f) return;
    fprintf(f, "{\n  \"server_url\": \"%s\",\n  \"token\": \"%s\",\n  \"username\": \"%s\",\n  \"user_id\": %d\n}\n",
            s_server_url, s_auth_token, s_username, s_user_id);
    fclose(f);
}

static void load_session(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/tffscores.json", s_user_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        char app_dir[512];
        if (get_localappdata_fbl_dir(app_dir, sizeof(app_dir), false)) {
            snprintf(path, sizeof(path), "%s\\tffscores.json", app_dir);
            f = fopen(path, "r");
        }
    }
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    size_t rd = fread(buf, 1, sz, f);
    buf[rd] = 0;
    fclose(f);

    json_get_string(buf, "token", s_auth_token, sizeof(s_auth_token));
    json_get_string(buf, "username", s_username, sizeof(s_username));
    char url_buf[256];
    json_get_string(buf, "server_url", url_buf, sizeof(url_buf));
    if (url_buf[0]) snprintf(s_server_url, sizeof(s_server_url), "%s", url_buf);
    s_user_id = json_get_int(buf, "user_id", 0);

    const char *env_url = getenv("THEATRHYTHM_TFFSCORES_URL");
    if (env_url && *env_url) snprintf(s_server_url, sizeof(s_server_url), "%s", env_url);

    if (s_auth_token[0]) {
        s_logged_in = 1;
        printf("TFFScores: loaded session for user '%s' (server: %s)\n", s_username, s_server_url);
    }
    free(buf);
}

/* WinHTTP request helpers */
static int http_post_multipart(const char *url_path, const char *post_data, size_t post_len, char *resp_out, size_t resp_cap) {
    if (resp_out && resp_cap) resp_out[0] = 0;

    int is_https = (strncmp(s_server_url, "https://", 8) == 0);
    const char *host_start = s_server_url + (is_https ? 8 : (strncmp(s_server_url, "http://", 7) == 0 ? 7 : 0));
    char host[256];
    snprintf(host, sizeof(host), "%s", host_start);
    char *slash = strchr(host, '/');
    if (slash) *slash = 0;
    char *colon = strchr(host, ':');
    int port = is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }

    wchar_t whost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);

    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, url_path, -1, wpath, 512);

    HINTERNET hSession = WinHttpOpen(L"TheatrhythmRecomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, whost, (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    WinHttpSetTimeouts(hRequest, 10000, 15000, 60000, 60000);

    /* Add headers */
    char hdr_buf[1024];
    snprintf(hdr_buf, sizeof(hdr_buf),
             "Content-Type: multipart/form-data; boundary=%s\r\nAuthorization: Bearer %s\r\n",
             BOUNDARY_STRING, s_auth_token);
    wchar_t whdrs[1024];
    MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 1024);

    BOOL res = WinHttpSendRequest(hRequest, whdrs, -1L, (LPVOID)post_data, (DWORD)post_len, (DWORD)post_len, 0);
    if (res) res = WinHttpReceiveResponse(hRequest, NULL);
    if (!res) {
        DWORD err = GetLastError();
        printf("TFFScores: WinHTTP POST %s failed (err=%lu)\n", url_path, (unsigned long)err);
    }

    int status_code = 0;
    if (res) {
        DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        status_code = (int)dwStatusCode;

        if (resp_out && resp_cap > 1) {
            DWORD total_read = 0;
            DWORD bytes_read = 0;
            while (total_read + 1 < resp_cap &&
                   WinHttpReadData(hRequest, resp_out + total_read, (DWORD)(resp_cap - 1 - total_read), &bytes_read) &&
                   bytes_read > 0) {
                total_read += bytes_read;
            }
            resp_out[total_read] = 0;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (status_code >= 200 && status_code < 300) ? 0 : (status_code ? status_code : -1);
}

static int http_get_json(const char *url_path, char *resp_out, size_t resp_cap) {
    if (resp_out && resp_cap) resp_out[0] = 0;

    int is_https = (strncmp(s_server_url, "https://", 8) == 0);
    const char *host_start = s_server_url + (is_https ? 8 : (strncmp(s_server_url, "http://", 7) == 0 ? 7 : 0));
    char host[256];
    snprintf(host, sizeof(host), "%s", host_start);
    char *slash = strchr(host, '/');
    if (slash) *slash = 0;
    char *colon = strchr(host, ':');
    int port = is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }

    wchar_t whost[256];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);

    wchar_t wpath[512];
    MultiByteToWideChar(CP_UTF8, 0, url_path, -1, wpath, 512);

    HINTERNET hSession = WinHttpOpen(L"TheatrhythmRecomp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, whost, (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    WinHttpSetTimeouts(hRequest, 10000, 15000, 60000, 60000);

    char hdr_buf[512];
    snprintf(hdr_buf, sizeof(hdr_buf), "Authorization: Bearer %s\r\n", s_auth_token);
    wchar_t whdrs[512];
    MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 512);

    BOOL res = WinHttpSendRequest(hRequest, whdrs, -1L, NULL, 0, 0, 0);
    if (res) res = WinHttpReceiveResponse(hRequest, NULL);
    if (!res) {
        DWORD err = GetLastError();
        printf("TFFScores: WinHTTP GET %s failed (err=%lu)\n", url_path, (unsigned long)err);
    }

    int status_code = 0;
    if (res) {
        DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        status_code = (int)dwStatusCode;

        if (resp_out && resp_cap > 1) {
            DWORD total_read = 0;
            DWORD bytes_read = 0;
            while (total_read + 1 < resp_cap && WinHttpReadData(hRequest, resp_out + total_read, (DWORD)(resp_cap - 1 - total_read), &bytes_read) && bytes_read > 0) {
                total_read += bytes_read;
            }
            resp_out[total_read] = 0;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (status_code >= 200 && status_code < 300) ? 0 : (status_code ? status_code : -1);
}

/* Base64 decode helper */
static unsigned char *b64_decode(const char *in, size_t in_len, size_t *out_len) {
    if (!in || !in_len) { *out_len = 0; return NULL; }
    static const unsigned char b64_table[256] = {
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
        64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
        64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
        64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
        64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64
    };
    unsigned char *out = malloc(in_len * 3 / 4 + 4);
    if (!out) { *out_len = 0; return NULL; }
    size_t out_idx = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '=' || c <= ' ') continue;
        unsigned char val = b64_table[c];
        if (val >= 64) continue;
        buf = (buf << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[out_idx++] = (unsigned char)((buf >> bits) & 0xFF);
        }
    }
    *out_len = out_idx;
    return out;
}

static void write_file_bytes(const char *path, const unsigned char *bytes, size_t sz) {
    if (!bytes || !sz) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(bytes, 1, sz, f);
    fclose(f);
}

static void ensure_dir_tree(const char *path) {
    if (!path || !*path) return;
    char buf[760];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf; *p; ++p) { if (*p == '\\') *p = '/'; }
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void extract_json_field_alloc(const char *json, const char *key, char **out_val, size_t *out_len) {
    *out_val = NULL;
    *out_len = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '\"') {
        p++;
        const char *end = strchr(p, '\"');
        if (end) {
            size_t len = (size_t)(end - p);
            char *buf = malloc(len + 1);
            if (buf) {
                memcpy(buf, p, len);
                buf[len] = 0;
                *out_val = buf;
                *out_len = len;
            }
        }
    }
}

int runtime_tffscores_maybe_download_save(const char *save_host, const char *title_id) {
    if (!s_logged_in || !s_auth_token[0] || !save_host || !save_host[0]) return -1;
    char check_path[750];
    snprintf(check_path, sizeof(check_path), "%s/backup.inf", save_host);
    struct stat st;
    if (stat(check_path, &st) == 0 && st.st_size > 0) {
        /* Local save already exists, keep local save */
        return 0;
    }
    if (s_cloud_download_checked) {
        return 0;
    }
    s_cloud_download_checked = 1;

    char url[512];
    snprintf(url, sizeof(url), "/api/game/cloud-save/download?title_id=%s", title_id && *title_id ? title_id : "CUSA32853");
    size_t resp_cap = 1024 * 1024 * 16; /* 16 MB max JSON */
    char *resp = malloc(resp_cap);
    if (!resp) return -1;
    int err = http_get_json(url, resp, resp_cap);
    if (err != 0) {
        if (err != 404) {
            printf("TFFScores: cloud save download check returned %d (%s)\n", err, resp);
        }
        free(resp);
        return -1;
    }

    char *b_b64 = NULL, *m_b64 = NULL, *p_b64 = NULL, *j_b64 = NULL;
    char *cr_b64 = NULL, *cj_b64 = NULL;
    size_t b_len = 0, m_len = 0, p_len = 0, j_len = 0, cr_len = 0, cj_len = 0;
    extract_json_field_alloc(resp, "backup_b64", &b_b64, &b_len);
    extract_json_field_alloc(resp, "music_backup_b64", &m_b64, &m_len);
    extract_json_field_alloc(resp, "profica_backup_b64", &p_b64, &p_len);
    extract_json_field_alloc(resp, "judgements_backup_b64", &j_b64, &j_len);
    extract_json_field_alloc(resp, "custom_records_b64", &cr_b64, &cr_len);
    extract_json_field_alloc(resp, "custom_judgements_b64", &cj_b64, &cj_len);

    int restored = 0;
    if ((b_b64 && b_len > 0) || (m_b64 && m_len > 0) || (p_b64 && p_len > 0) ||
        (j_b64 && j_len > 0) || (cr_b64 && cr_len > 0) || (cj_b64 && cj_len > 0)) {
        ensure_dir_tree(save_host);
    }
    if (b_b64 && b_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(b_b64, b_len, &dec_len);
        if (dec) {
            char p[750]; snprintf(p, sizeof(p), "%s/backup.inf", save_host);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }
    if (m_b64 && m_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(m_b64, m_len, &dec_len);
        if (dec) {
            char p[750]; snprintf(p, sizeof(p), "%s/musicBackup.inf", save_host);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }
    if (p_b64 && p_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(p_b64, p_len, &dec_len);
        if (dec) {
            char p[750]; snprintf(p, sizeof(p), "%s/proficaBackup.inf", save_host);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }
    if (j_b64 && j_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(j_b64, j_len, &dec_len);
        if (dec) {
            char p[750];
            snprintf(p, sizeof(p), "%s/judgementsBackup.inf", save_host);
            write_file_bytes(p, dec, dec_len);
            const char *primary_j = runtime_judgements_get_save_path();
            if (primary_j && *primary_j) write_file_bytes(primary_j, dec, dec_len);
            snprintf(p, sizeof(p), "%s/judgementsBackup.inf", s_user_dir);
            write_file_bytes(p, dec, dec_len);
            snprintf(p, sizeof(p), "%s/judgements.dat", save_host);
            write_file_bytes(p, dec, dec_len);
            snprintf(p, sizeof(p), "%s/judgements.dat", s_user_dir);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }
    if (cr_b64 && cr_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(cr_b64, cr_len, &dec_len);
        if (dec) {
            char p[750];
            snprintf(p, sizeof(p), "%s/customRecords.dat", save_host);
            write_file_bytes(p, dec, dec_len);
            snprintf(p, sizeof(p), "%s/customRecords.dat", s_user_dir);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }
    if (cj_b64 && cj_len > 0) {
        size_t dec_len = 0;
        unsigned char *dec = b64_decode(cj_b64, cj_len, &dec_len);
        if (dec) {
            char p[750];
            snprintf(p, sizeof(p), "%s/customJudgements.dat", save_host);
            write_file_bytes(p, dec, dec_len);
            snprintf(p, sizeof(p), "%s/customJudgements.dat", s_user_dir);
            write_file_bytes(p, dec, dec_len);
            free(dec);
            restored = 1;
        }
    }

    if (b_b64) free(b_b64);
    if (m_b64) free(m_b64);
    if (p_b64) free(p_b64);
    if (j_b64) free(j_b64);
    if (cr_b64) free(cr_b64);
    if (cj_b64) free(cj_b64);
    free(resp);

    if (restored) {
        printf("TFFScores: Cloud save restored into %s for title %s\n", save_host, title_id);
        theatrhythm_gpu_show_notice("TFFScores: Cloud save downloaded!", 4000);
        return 1;
    }
    return 0;
}

/* Multipart body builder */
typedef struct {
    char *data;
    size_t size;
    size_t cap;
} MultipartBuf;

static void mp_init(MultipartBuf *b) {
    b->cap = 1024 * 1024 * 4; /* 4 MB initial */
    b->size = 0;
    b->data = malloc(b->cap);
}

static void mp_free(MultipartBuf *b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->size = b->cap = 0;
}

static void mp_append(MultipartBuf *b, const void *bytes, size_t len) {
    if (b->size + len > b->cap) {
        b->cap = (b->cap + len) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->size, bytes, len);
    b->size += len;
}

static void mp_add_field(MultipartBuf *b, const char *name, const char *value) {
    char header[512];
    snprintf(header, sizeof(header), "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
             BOUNDARY_STRING, name, value);
    mp_append(b, header, strlen(header));
}

static void mp_add_file(MultipartBuf *b, const char *field_name, const char *filename, const void *bytes, size_t len) {
    char header[512];
    snprintf(header, sizeof(header),
             "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n",
             BOUNDARY_STRING, field_name, filename);
    mp_append(b, header, strlen(header));
    if (bytes && len > 0) {
        mp_append(b, bytes, len);
    }
    mp_append(b, "\r\n", 2);
}

static void mp_finish(MultipartBuf *b) {
    char footer[128];
    snprintf(footer, sizeof(footer), "--%s--\r\n", BOUNDARY_STRING);
    mp_append(b, footer, strlen(footer));
}

static unsigned char *read_entire_file(const char *path, size_t *out_size) {
    *out_size = 0;
    if (!path || !*path) return NULL;
    char norm[760];
    snprintf(norm, sizeof(norm), "%s", path);
    for (char *p = norm; *p; ++p) { if (*p == '\\') *p = '/'; }
    FILE *f = fopen(norm, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, sz, f);
    fclose(f);
    *out_size = rd;
    return buf;
}

/* Background loopback server for OAuth-style local browser login */
static void *auth_loopback_thread(void *arg) {
    (void)arg;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        s_auth_server_running = 0;
        return NULL;
    }

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        WSACleanup();
        s_auth_server_running = 0;
        return NULL;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    sin.sin_port = htons(LOOPBACK_PORT);

    if (bind(listen_sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        closesocket(listen_sock);
        WSACleanup();
        s_auth_server_running = 0;
        return NULL;
    }

    if (listen(listen_sock, 1) != 0) {
        closesocket(listen_sock);
        WSACleanup();
        s_auth_server_running = 0;
        return NULL;
    }

    s_auth_server_running = 1;
    printf("TFFScores: loopback server listening on 127.0.0.1:%d\n", LOOPBACK_PORT);

    /* Open default browser to the authorization endpoint */
    char auth_url[512];
    snprintf(auth_url, sizeof(auth_url), "%s/auth/game-authorize?port=%d", s_server_url, LOOPBACK_PORT);
    ShellExecuteA(NULL, "open", auth_url, NULL, NULL, SW_SHOWNORMAL);

    /* Timeout after 120 seconds */
    DWORD timeout_ms = 120000;
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    SOCKET client_sock = accept(listen_sock, NULL, NULL);
    if (client_sock != INVALID_SOCKET) {
        char req_buf[2048];
        int n = recv(client_sock, req_buf, sizeof(req_buf) - 1, 0);
        if (n > 0) {
            req_buf[n] = 0;
            char *token_param = strstr(req_buf, "token=");
            char *username_param = strstr(req_buf, "username=");
            char *user_id_param = strstr(req_buf, "user_id=");

            if (token_param) {
                token_param += 6;
                char *end = strpbrk(token_param, " &;\r\n");
                if (end) *end = 0;
                pthread_mutex_lock(&s_tff_lock);
                snprintf(s_auth_token, sizeof(s_auth_token), "%s", token_param);
                pthread_mutex_unlock(&s_tff_lock);
            }

            if (username_param) {
                username_param += 9;
                char *end = strpbrk(username_param, " &;\r\n");
                if (end) *end = 0;
                pthread_mutex_lock(&s_tff_lock);
                snprintf(s_username, sizeof(s_username), "%s", username_param);
                pthread_mutex_unlock(&s_tff_lock);
            }

            if (user_id_param) {
                user_id_param += 8;
                s_user_id = atoi(user_id_param);
            }

            if (s_auth_token[0]) {
                s_logged_in = 1;
                s_cloud_download_checked = 0;
                save_session();
                printf("TFFScores: successfully authenticated as user '%s' (ID %d)\n", s_username, s_user_id);
                char notice[128];
                snprintf(notice, sizeof(notice), "TFFScores: Logged in as %s", s_username);
                theatrhythm_gpu_show_notice(notice, 5000);
            }

            const char *http_resp =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Connection: close\r\n\r\n"
                "<!DOCTYPE html><html><body style=\"background:#0b1528;color:#4dc4ff;font-family:sans-serif;text-align:center;padding:50px;\">"
                "<h2>Authorization Complete!</h2>"
                "<p style=\"color:#fff;\">You are connected to Theatrhythm Final Bar Line. You can close this tab and return to the game.</p>"
                "</body></html>";
            send(client_sock, http_resp, (int)strlen(http_resp), 0);
        }
        closesocket(client_sock);
    }

    closesocket(listen_sock);
    WSACleanup();
    s_auth_server_running = 0;
    return NULL;
}

void runtime_tffscores_trigger_auth_hotkey(void) {
    if (s_logged_in) {
        char msg[128];
        snprintf(msg, sizeof(msg), "TFFScores: Logged in as %s", s_username);
        theatrhythm_gpu_show_notice(msg, 3000);
        return;
    }

    if (s_auth_server_running) {
        theatrhythm_gpu_show_notice("TFFScores: Authorization in progress in browser...", 3000);
        return;
    }

    theatrhythm_gpu_show_notice("TFFScores: Opening browser to log in...", 3000);
    pthread_create(&s_auth_thread, NULL, auth_loopback_thread, NULL);
    pthread_detach(s_auth_thread);
}

/* Sync worker thread */
static unsigned char *read_save_file_with_fallbacks(const char *dir1, const char *filename, const char *alt_path, const char *dir2, size_t *out_sz) {
    char path[750];
    unsigned char *data = NULL;
    if (dir1 && dir1[0]) {
        snprintf(path, sizeof(path), "%s/%s", dir1, filename);
        data = read_entire_file(path, out_sz);
        if (data) return data;
    }
    if (alt_path && alt_path[0]) {
        data = read_entire_file(alt_path, out_sz);
        if (data) return data;
    }
    if (dir2 && dir2[0]) {
        snprintf(path, sizeof(path), "%s/%s", dir2, filename);
        data = read_entire_file(path, out_sz);
        if (data) return data;
    }
    char active_dir[512];
    if (runtime_savedata_get_active_save_dir(active_dir, sizeof(active_dir)) == 0 && active_dir[0]) {
        snprintf(path, sizeof(path), "%s/%s", active_dir, filename);
        data = read_entire_file(path, out_sz);
        if (data) return data;
    }
    char app_dir[512];
    if (get_localappdata_fbl_dir(app_dir, sizeof(app_dir), false)) {
        snprintf(path, sizeof(path), "%s\\%s", app_dir, filename);
        data = read_entire_file(path, out_sz);
        if (data) return data;
    }
    return NULL;
}

static void *sync_worker_loop(void *arg) {
    (void)arg;
    while (s_worker_running) {
        char host[700] = "";
        int has_work = 0;
        int is_pb = 0;
        int score_pb = 0;
        int damage_pb = 0;
        uint32_t stage_music_id = 0;
        int stage_difficulty = -1;
        int stage_is_local_custom = 0;

        pthread_mutex_lock(&s_tff_lock);
        while (s_worker_running && !s_pending_sync_req) {
            pthread_cond_wait(&s_sync_cond, &s_tff_lock);
        }
        if (!s_worker_running) {
            pthread_mutex_unlock(&s_tff_lock);
            break;
        }

        if (s_pending_sync_req) {
            snprintf(host, sizeof(host), "%s", s_pending_save_host);
            s_pending_sync_req = 0;
            has_work = 1;
            score_pb = s_stage_is_score_pb;
            damage_pb = s_stage_is_damage_pb;
            is_pb = (score_pb || damage_pb);
            stage_music_id = s_stage_music_id;
            stage_difficulty = s_stage_difficulty;
            stage_is_local_custom = s_stage_is_local_custom;
            s_song_result_pending = 0;
        }
        pthread_mutex_unlock(&s_tff_lock);

        if (!s_logged_in || !s_auth_token[0]) continue;

        /* Coalesce back-to-back save unmounts (e.g. backup.inf + musicBackup.inf + proficaBackup.inf) */
        if (!is_pb) {
            Sleep(350);
            pthread_mutex_lock(&s_tff_lock);
            if (s_pending_sync_req) {
                snprintf(host, sizeof(host), "%s", s_pending_save_host);
                s_pending_sync_req = 0;
                if (s_stage_is_score_pb) score_pb = 1;
                if (s_stage_is_damage_pb) damage_pb = 1;
                is_pb = (score_pb || damage_pb);
                if (s_stage_music_id != 0) {
                    stage_music_id = s_stage_music_id;
                    stage_difficulty = s_stage_difficulty;
                    stage_is_local_custom = s_stage_is_local_custom;
                }
            }
            pthread_mutex_unlock(&s_tff_lock);
        }

        const char *title_id = runtime_savedata_title_id();
        if (!title_id || !*title_id) title_id = "CUSA32853";

        /* If this stage was a Personal Best, wait for screenshot before uploading */
        if (is_pb && stage_music_id != 0 && !stage_is_local_custom && !is_local_custom_id(stage_music_id) && host[0]) {
            char screenshot_path[512] = "";
            uint64_t ss_time = 0;
            int has_ss = 0;

            printf("TFFScores: Personal Best detected (score_pb=%d, damage_pb=%d) -> gating save & score upload on screenshot proof...\n",
                   score_pb, damage_pb);

            /* Wait for screenshot trigger (normal settle ~6.25s or early skip) */
            uint64_t wait_start = GetTickCount64();
            while (s_awaiting_result_screenshot && (GetTickCount64() - wait_start < 12000)) {
                Sleep(50);
            }

            /* Wait up to 5 seconds for Vulkan presenter writer to finish writing PNG file */
            uint64_t png_start = GetTickCount64();
            while (GetTickCount64() - png_start < 5000) {
                has_ss = theatrhythm_gpu_get_last_screenshot(screenshot_path, sizeof(screenshot_path), &ss_time);
                if (has_ss && screenshot_path[0]) {
                    FILE *chk = fopen(screenshot_path, "rb");
                    if (chk) {
                        fseek(chk, 0, SEEK_END);
                        long sz = ftell(chk);
                        fclose(chk);
                        if (sz > 40000) break; /* Valid rendered frame (>40KB) */
                    }
                }
                Sleep(100);
            }

            /* Re-read PB flags in case damage/score PB was notified slightly after sync wake */
            pthread_mutex_lock(&s_tff_lock);
            if (s_stage_music_id == stage_music_id) {
                if (s_stage_is_score_pb) score_pb = 1;
                if (s_stage_is_damage_pb) damage_pb = 1;
            }
            pthread_mutex_unlock(&s_tff_lock);

            /* Wait briefly if savedata unmount or customRecords write is still flushing to disk */
            for (int w = 0; w < 10 && (runtime_savedata_busy() || runtime_custom_charts_records_busy()); ++w) {
                Sleep(50);
            }

            /* Submit score + screenshot proof */
            size_t m_sz = 0, c_sz = 0;
            unsigned char *m_bytes = read_save_file_with_fallbacks(host, "musicBackup.inf", NULL, s_user_dir, &m_sz);
            unsigned char *c_bytes = read_save_file_with_fallbacks(host, "customRecords.dat", NULL, s_user_dir, &c_sz);

            if ((m_bytes && m_sz > 0) || (c_bytes && c_sz > 0)) {
                MultipartBuf mp;
                mp_init(&mp);
                mp_add_field(&mp, "title_id", title_id);

                uint32_t submit_id = stage_music_id;
                if (runtime_custom_charts_is_verified_id(stage_music_id)) {
                    submit_id = (stage_music_id >= 160000000u) ? (stage_music_id - 160000000u) : stage_music_id;
                } else if (stage_music_id >= 160000000u && stage_music_id < 160090000u) {
                    /* Vanilla songs use 3-digit mcode (1..999) on TFFScores; sending full 160000xxx
                     * caused the server to treat vanilla songs as >=90000 custom songs and drop proofs! */
                    submit_id = stage_music_id % 1000u;
                }

                char str_song[32], str_diff[16];
                if (submit_id > 0) {
                    snprintf(str_song, sizeof(str_song), "%u", submit_id);
                    mp_add_field(&mp, "song_id", str_song);
                }
                if (stage_difficulty >= 0) {
                    snprintf(str_diff, sizeof(str_diff), "%d", stage_difficulty);
                    mp_add_field(&mp, "difficulty_index", str_diff);
                    mp_add_field(&mp, "slot", str_diff);
                }
                mp_add_field(&mp, "style_key", runtime_custom_charts_get_play_style());

                const char *ptype = "score";
                if (score_pb && damage_pb) ptype = "both";
                else if (score_pb) ptype = "score";
                else if (damage_pb) ptype = "damage";
                mp_add_field(&mp, "proof_type", ptype);
                printf("TFFScores: Uploading PB proof with proof_type=%s (song_id=%u, diff=%d, style=%s)\n",
                       ptype, submit_id, stage_difficulty, runtime_custom_charts_get_play_style());

                if (m_bytes && m_sz > 0) {
                    mp_add_file(&mp, "music_backup", "musicBackup.inf", m_bytes, m_sz);
                }
                if (c_bytes && c_sz > 0) {
                    mp_add_file(&mp, "custom_records", "customRecords.dat", c_bytes, c_sz);
                }

                if (has_ss && screenshot_path[0]) {
                    size_t ss_sz = 0;
                    unsigned char *ss_bytes = read_entire_file(screenshot_path, &ss_sz);
                    if (ss_bytes && ss_sz > 0) {
                        printf("TFFScores: Attaching screenshot %s (%zu bytes) to score submission\n", screenshot_path, ss_sz);
                        mp_add_file(&mp, "screenshot", "result.png", ss_bytes, ss_sz);
                        free(ss_bytes);
                    }
                } else {
                    printf("TFFScores: Warning: no screenshot ready for score submission (has_ss=%d, path=%s)\n", has_ss, screenshot_path);
                }
                mp_finish(&mp);

                char resp[1024];
                int err = http_post_multipart("/api/game/submit-score", mp.data, mp.size, resp, sizeof(resp));
                if (err == 0) {
                    printf("TFFScores: Score & screenshot submitted successfully! Response: %s\n", resp);
                    if (strstr(resp, "\"proof_attached\":true")) {
                        theatrhythm_gpu_show_notice("TFFScores: Personal Best & Proof Uploaded!", 4000);
                    } else {
                        theatrhythm_gpu_show_notice("TFFScores: Scores Synced", 3000);
                    }
                } else {
                    printf("TFFScores: /api/game/submit-score returned status %d (%s)\n", err, resp);
                }
                mp_free(&mp);
            }
            if (m_bytes) free(m_bytes);
            if (c_bytes) free(c_bytes);
            theatrhythm_gpu_clear_last_screenshot();
        } else {
            /* No new PB: do not wait for any screenshot */
            s_awaiting_result_screenshot = 0;
        }

        /* Cloud save backup (backup.inf, musicBackup.inf, proficaBackup.inf, judgementsBackup.inf, customRecords.dat, customJudgements.dat) */
        if (has_work && host[0]) {
            for (int w = 0; w < 10 && (runtime_savedata_busy() || runtime_custom_charts_records_busy()); ++w) {
                Sleep(50);
            }
            /* Coalesce redundant save commit queued for the same host while awaiting results screenshot */
            if (is_pb) {
                pthread_mutex_lock(&s_tff_lock);
                if (s_pending_sync_req && strcmp(s_pending_save_host, host) == 0) {
                    s_pending_sync_req = 0;
                }
                pthread_mutex_unlock(&s_tff_lock);
            }

            size_t b_sz = 0, m_sz = 0, p_sz = 0, j_sz = 0, c_sz = 0, cj_sz = 0;
            unsigned char *b_data = read_save_file_with_fallbacks(host, "backup.inf", NULL, NULL, &b_sz);
            unsigned char *m_data = read_save_file_with_fallbacks(host, "musicBackup.inf", NULL, NULL, &m_sz);
            unsigned char *p_data = read_save_file_with_fallbacks(host, "proficaBackup.inf", NULL, NULL, &p_sz);
            unsigned char *j_data = read_save_file_with_fallbacks(host, "judgementsBackup.inf", runtime_judgements_get_save_path(), s_user_dir, &j_sz);
            unsigned char *c_data = read_save_file_with_fallbacks(host, "customRecords.dat", NULL, s_user_dir, &c_sz);
            unsigned char *cj_data = read_save_file_with_fallbacks(s_user_dir, "customJudgements.dat", NULL, host, &cj_sz);

            if (b_data || m_data || p_data || j_data || c_data || cj_data) {
                MultipartBuf mp;
                mp_init(&mp);
                mp_add_field(&mp, "title_id", title_id);
                if (b_data) mp_add_file(&mp, "file_backup", "backup.inf", b_data, b_sz);
                if (m_data) mp_add_file(&mp, "file_music_backup", "musicBackup.inf", m_data, m_sz);
                if (p_data) mp_add_file(&mp, "file_profica_backup", "proficaBackup.inf", p_data, p_sz);
                if (j_data) mp_add_file(&mp, "file_judgements_backup", "judgementsBackup.inf", j_data, j_sz);
                if (c_data) mp_add_file(&mp, "file_custom_records", "customRecords.dat", c_data, c_sz);
                if (cj_data) mp_add_file(&mp, "file_custom_judgements", "customJudgements.dat", cj_data, cj_sz);
                mp_finish(&mp);

                char resp[1024];
                int err = http_post_multipart("/api/game/cloud-save/upload", mp.data, mp.size, resp, sizeof(resp));
                if (err == 0) {
                    printf("TFFScores: Cloud save backed up successfully for %s (j_sz=%zu, c_sz=%zu)\n", title_id, j_sz, c_sz);
                    if (!is_pb) {
                        theatrhythm_gpu_show_notice("TFFScores: Cloud save synced", 3000);
                    }
                } else {
                    printf("TFFScores: cloud save upload returned %d (%s)\n", err, resp);
                }
                mp_free(&mp);
            }

            if (b_data) free(b_data);
            if (m_data) free(m_data);
            if (p_data) free(p_data);
            if (j_data) free(j_data);
            if (c_data) free(c_data);
            if (cj_data) free(cj_data);
        }

        pthread_mutex_lock(&s_tff_lock);
        if (!s_stage_active && s_stage_music_id == stage_music_id) {
            s_stage_music_id = 0;
            s_stage_difficulty = -1;
            s_stage_is_score_pb = 0;
            s_stage_is_damage_pb = 0;
        }
        pthread_mutex_unlock(&s_tff_lock);
    }
    return NULL;
}

void runtime_tffscores_init(const char *user_dir) {
    if (user_dir && *user_dir) snprintf(s_user_dir, sizeof(s_user_dir), "%s", user_dir);
    load_session();

    s_worker_running = 1;
    pthread_create(&s_worker_thread, NULL, sync_worker_loop, NULL);

    if (s_logged_in) {
        char notice[128];
        snprintf(notice, sizeof(notice), "TFFScores: Logged in as %s", s_username);
        theatrhythm_gpu_show_notice(notice, 4000);
    } else {
        /* Startup prompt toast */
        theatrhythm_gpu_show_notice("TFFScores: Press F9 to Log In (Cloud Saves & Leaderboards)", 6000);
    }
}

static void *immediate_screenshot_thread(void *arg) {
    (void)arg;
    Sleep(1300); /* +500ms timing on skip */
    theatrhythm_gpu_request_screenshot();
    return NULL;
}

void runtime_tffscores_trigger_immediate_screenshot(void) {
    if (!s_awaiting_result_screenshot) return;
    s_awaiting_result_screenshot = 0;
    printf("TFFScores: early confirm/skip detected -> snapping screenshot with 1300ms buffer before popup!\n");
    pthread_t th;
    if (pthread_create(&th, NULL, immediate_screenshot_thread, NULL) == 0) {
        pthread_detach(th);
    } else {
        theatrhythm_gpu_request_screenshot();
    }
}

static void *delayed_screenshot_thread(void *arg) {
    (void)arg;
    Sleep(7250); /* +500ms timing on wait: 7.25s for results screen fade-in, count-ups, and damage numbers to settle completely */
    if (s_awaiting_result_screenshot) {
        s_awaiting_result_screenshot = 0;
        printf("TFFScores: capturing results screen proof now (normal settle)...\n");
        theatrhythm_gpu_request_screenshot();
    }
    return NULL;
}

void runtime_tffscores_on_song_result_finish(void) {
    uint64_t now_ms = GetTickCount64();

    pthread_mutex_lock(&s_tff_lock);
    if (!s_stage_active) {
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (s_last_song_finish_ms != 0 && (now_ms - s_last_song_finish_ms) < 5000) {
        /* Debounce: count-up behaviours fire for every counter during results sequence */
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    s_last_song_finish_ms = now_ms;

    if (s_stage_music_id == 0 || s_stage_is_local_custom || is_local_custom_id(s_stage_music_id)) {
        uint32_t cid = s_stage_music_id;
        s_stage_active = 0;
        s_stage_is_score_pb = 0;
        s_stage_is_damage_pb = 0;
        s_stage_save_host[0] = '\0';
        s_awaiting_result_screenshot = 0;
        s_custom_suppress_until_ms = now_ms + 15000;
        pthread_mutex_unlock(&s_tff_lock);
        printf("TFFScores: Skipping upload & screenshot for local/unknown custom chart (ID %u)\n", cid);
        return;
    }
    int is_pb = (s_stage_is_score_pb || s_stage_is_damage_pb);
    s_stage_active = 0; /* Stage complete */

    if (s_stage_save_host[0]) {
        snprintf(s_pending_save_host, sizeof(s_pending_save_host), "%s", s_stage_save_host);
    } else {
        runtime_savedata_get_active_save_dir(s_pending_save_host, sizeof(s_pending_save_host));
    }
    s_stage_save_host[0] = '\0';
    if (!s_pending_save_host[0]) {
        snprintf(s_pending_save_host, sizeof(s_pending_save_host), "%s", s_user_dir);
    }

    if (is_pb) {
        s_awaiting_result_screenshot = 1;
        printf("TFFScores: song result finish detected for new PB -> awaiting results screenshot\n");
        pthread_t th;
        if (pthread_create(&th, NULL, delayed_screenshot_thread, NULL) == 0) {
            pthread_detach(th);
        }
    } else {
        s_awaiting_result_screenshot = 0;
        printf("TFFScores: song result finish detected, but NOT a new PB -> syncing cloud save immediately\n");
    }

    s_pending_sync_req = 1;
    pthread_cond_signal(&s_sync_cond);
    pthread_mutex_unlock(&s_tff_lock);
}

void runtime_tffscores_on_save_commit(const char *save_host) {
    if (!save_host || !*save_host) return;
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_is_local_custom && (s_stage_active || GetTickCount64() < s_custom_suppress_until_ms)) {
        printf("TFFScores: Ignoring save commit from local custom chart session (%s)\n", save_host);
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (s_stage_active) {
        snprintf(s_stage_save_host, sizeof(s_stage_save_host), "%s", save_host);
        printf("TFFScores: Save commit during active stage cached (%s). Deferring upload to results.\n", save_host);
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    snprintf(s_pending_save_host, sizeof(s_pending_save_host), "%s", save_host);
    s_pending_sync_req = 1;
    pthread_cond_signal(&s_sync_cond);
    pthread_mutex_unlock(&s_tff_lock);
}

int runtime_tffscores_is_logged_in(void) {
    return s_logged_in;
}

const char *runtime_tffscores_username(void) {
    return s_username;
}
