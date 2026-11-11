#define _GNU_SOURCE
#include "runtime_tffscores.h"
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include "runtime_judgements.h"
#include "runtime_leaderboard.h"

#define theatrhythm_gpu_show_notice theatrhythm_gpu_notice

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

static RuntimeTffscoresCustomIdCheckFn s_custom_id_checker = NULL;

void runtime_tffscores_set_custom_id_checker(RuntimeTffscoresCustomIdCheckFn fn) {
    s_custom_id_checker = fn;
}

static inline bool is_custom_chart_id(uint32_t id) {
    if (s_custom_id_checker) return s_custom_id_checker(id) != 0;
    return id >= 169000000 && id < 170000000;
}

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
#else
#include <unistd.h>
static void Sleep(unsigned ms) {
    struct timespec ts = { (time_t)(ms / 1000u), (long)((ms % 1000u) * 1000000L) };
    nanosleep(&ts, NULL);
}
static uint64_t GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}
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
static volatile int s_stage_is_custom = 0;
static volatile uint64_t s_custom_suppress_until_ms = 0;
static char s_stage_save_host[700] = "";


static pthread_mutex_t s_tff_lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_tffscores_notify_stage_start(uint32_t music_id, int difficulty) {
    if (music_id == 0) return;
    int is_custom = is_custom_chart_id(music_id) ? 1 : 0;
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_music_id == music_id && s_stage_difficulty >= 4 && difficulty < 4) {
        /* Preserve extra difficulty slot 4/5 if PlaySetting+0x50 was clamped */
    } else {
        s_stage_difficulty = difficulty;
    }
    s_stage_music_id = music_id;
    s_stage_is_score_pb = 0;
    s_stage_is_damage_pb = 0;
    s_stage_active = 1;
    s_stage_is_custom = is_custom;
    if (!is_custom) s_custom_suppress_until_ms = 0;
    s_stage_save_host[0] = '\0';
    s_awaiting_result_screenshot = 0;
    theatrhythm_gpu_clear_last_screenshot();
    pthread_mutex_unlock(&s_tff_lock);
    if (is_custom) {
        printf("TFFScores: Custom chart stage started (placeholder ID %u, diff=%d) -> TFFScores upload disabled\n",
               music_id, s_stage_difficulty);
    } else {
        printf("TFFScores: Stage started: music_id=%u, difficulty=%d\n", music_id, s_stage_difficulty);
    }
}

void runtime_tffscores_notify_score_pb(int is_pb) {
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_is_custom) {
        s_stage_is_score_pb = 0;
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (is_pb) {
        s_stage_is_score_pb = 1;
    }
    int cur_pb = s_stage_is_score_pb;
    pthread_mutex_unlock(&s_tff_lock);
    printf("TFFScores: Score PB status: %d (effective=%d)\n", is_pb, cur_pb);
}

void runtime_tffscores_notify_damage_pb(int is_pb) {
    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_is_custom) {
        s_stage_is_damage_pb = 0;
        pthread_mutex_unlock(&s_tff_lock);
        return;
    }
    if (is_pb) {
        s_stage_is_damage_pb = 1;
    }
    int cur_pb = s_stage_is_damage_pb;
    pthread_mutex_unlock(&s_tff_lock);
    printf("TFFScores: Damage PB status: %d (effective=%d)\n", is_pb, cur_pb);
}
static pthread_t s_worker_thread;
static int s_worker_running = 0;

/* Save commit sync queue */
static char s_pending_save_host[700] = "";
static int s_pending_sync_req = 0;
static pthread_cond_t s_sync_cond = PTHREAD_COND_INITIALIZER;

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

#ifdef _WIN32
static void save_session(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/tffscores.json", s_user_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\n  \"server_url\": \"%s\",\n  \"token\": \"%s\",\n  \"username\": \"%s\",\n  \"user_id\": %d\n}\n",
            s_server_url, s_auth_token, s_username, s_user_id);
    fclose(f);
}
#endif

static void load_session(void) {
    char path[600];
    snprintf(path, sizeof(path), "%s/tffscores.json", s_user_dir);
    FILE *f = fopen(path, "r");
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
#ifndef _WIN32
    (void)url_path;
    (void)post_data;
    (void)post_len;
    return -1;
#else
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

    /* Add headers */
    char hdr_buf[512];
    snprintf(hdr_buf, sizeof(hdr_buf),
             "Content-Type: multipart/form-data; boundary=%s\r\nAuthorization: Bearer %s\r\n",
             BOUNDARY_STRING, s_auth_token);
    wchar_t whdrs[512];
    MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 512);

    BOOL res = WinHttpSendRequest(hRequest, whdrs, -1L, (LPVOID)post_data, (DWORD)post_len, (DWORD)post_len, 0);
    if (res) res = WinHttpReceiveResponse(hRequest, NULL);

    int status_code = 0;
    if (res) {
        DWORD dwStatusCode = 0, dwSize = sizeof(dwStatusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &dwStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);
        status_code = (int)dwStatusCode;

        if (resp_out && resp_cap > 1) {
            DWORD bytes_read = 0;
            WinHttpReadData(hRequest, resp_out, (DWORD)(resp_cap - 1), &bytes_read);
            resp_out[bytes_read] = 0;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (status_code >= 200 && status_code < 300) ? 0 : (status_code ? status_code : -1);
#endif
}

static int http_get_json(const char *url_path, char *resp_out, size_t resp_cap) {
    if (resp_out && resp_cap) resp_out[0] = 0;
#ifndef _WIN32
    (void)url_path;
    return -1;
#else
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

    char hdr_buf[512];
    snprintf(hdr_buf, sizeof(hdr_buf), "Authorization: Bearer %s\r\n", s_auth_token);
    wchar_t whdrs[512];
    MultiByteToWideChar(CP_UTF8, 0, hdr_buf, -1, whdrs, 512);

    BOOL res = WinHttpSendRequest(hRequest, whdrs, -1L, NULL, 0, 0, 0);
    if (res) res = WinHttpReceiveResponse(hRequest, NULL);

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
#endif
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

static void extract_json_field_alloc(const char *json, const char *key, char **out_val, size_t *out_len) {
    *out_val = NULL;
    *out_len = 0;
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

    char url[512];
    snprintf(url, sizeof(url), "/api/game/cloud-save/download?title_id=%s", title_id && *title_id ? title_id : "CUSA32853");
    size_t resp_cap = 1024 * 1024 * 16; /* 16 MB max JSON */
    char *resp = malloc(resp_cap);
    if (!resp) return -1;
    int err = http_get_json(url, resp, resp_cap);
    if (err != 0) {
        free(resp);
        return -1;
    }

    char *b_b64 = NULL, *m_b64 = NULL, *p_b64 = NULL, *j_b64 = NULL;
    size_t b_len = 0, m_len = 0, p_len = 0, j_len = 0;
    extract_json_field_alloc(resp, "backup_b64", &b_b64, &b_len);
    extract_json_field_alloc(resp, "music_backup_b64", &m_b64, &m_len);
    extract_json_field_alloc(resp, "profica_backup_b64", &p_b64, &p_len);
    extract_json_field_alloc(resp, "judgements_backup_b64", &j_b64, &j_len);

    int restored = 0;
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

    if (b_b64) free(b_b64);
    if (m_b64) free(m_b64);
    if (p_b64) free(p_b64);
    if (j_b64) free(j_b64);
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
#ifndef _WIN32
    s_auth_server_running = 0;
    return NULL;
#else
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
#endif
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
        int stage_is_custom = 0;

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
            stage_is_custom = s_stage_is_custom;
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
                    stage_is_custom = s_stage_is_custom;
                }
            }
            pthread_mutex_unlock(&s_tff_lock);
        }

        const char *title_id = runtime_savedata_title_id();
        if (!title_id || !*title_id) title_id = "CUSA32853";

        /* If this stage was a Personal Best, wait for screenshot before uploading */
        if (is_pb && stage_music_id != 0 && !stage_is_custom && !is_custom_chart_id(stage_music_id) && host[0]) {
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

            /* Submit score + screenshot proof */
            char music_path[750];
            snprintf(music_path, sizeof(music_path), "%s/musicBackup.inf", host);
            size_t m_sz = 0;
            unsigned char *m_bytes = read_entire_file(music_path, &m_sz);
            if (!m_bytes) {
                char alt_m[750];
                snprintf(alt_m, sizeof(alt_m), "%s/musicBackup.inf", s_user_dir);
                m_bytes = read_entire_file(alt_m, &m_sz);
            }

            if (m_bytes && m_sz > 0) {
                MultipartBuf mp;
                mp_init(&mp);
                mp_add_field(&mp, "title_id", title_id);

                char str_song[32], str_diff[16];
                if (stage_music_id > 0) {
                    snprintf(str_song, sizeof(str_song), "%u", stage_music_id);
                    mp_add_field(&mp, "song_id", str_song);
                }
                if (stage_difficulty >= 0) {
                    snprintf(str_diff, sizeof(str_diff), "%d", stage_difficulty);
                    mp_add_field(&mp, "difficulty_index", str_diff);
                }

                const char *ptype = "score";
                if (score_pb && damage_pb) ptype = "both";
                else if (score_pb) ptype = "score";
                else if (damage_pb) ptype = "damage";
                mp_add_field(&mp, "proof_type", ptype);
                printf("TFFScores: Uploading PB proof with proof_type=%s (song_id=%u, diff=%d)\n",
                       ptype, stage_music_id, stage_difficulty);

                mp_add_file(&mp, "music_backup", "musicBackup.inf", m_bytes, m_sz);

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
                    printf("TFFScores: /api/game/submit-score returned status %d\n", err);
                }
                mp_free(&mp);
                free(m_bytes);
            }
            theatrhythm_gpu_clear_last_screenshot();
        } else {
            /* No new PB: do not wait for any screenshot */
            s_awaiting_result_screenshot = 0;
        }

        /* 4-file cloud save backup (backup.inf, musicBackup.inf, proficaBackup.inf, judgementsBackup.inf) */
        if (has_work && host[0]) {
            char b_path[750], m_path[750], p_path[750], j_path[750];
            snprintf(b_path, sizeof(b_path), "%s/backup.inf", host);
            snprintf(m_path, sizeof(m_path), "%s/musicBackup.inf", host);
            snprintf(p_path, sizeof(p_path), "%s/proficaBackup.inf", host);
            snprintf(j_path, sizeof(j_path), "%s/judgementsBackup.inf", host);

            size_t b_sz = 0, m_sz = 0, p_sz = 0, j_sz = 0;
            unsigned char *b_data = read_entire_file(b_path, &b_sz);
            unsigned char *m_data = read_entire_file(m_path, &m_sz);
            unsigned char *p_data = read_entire_file(p_path, &p_sz);
            unsigned char *j_data = read_entire_file(j_path, &j_sz);

            if (!j_data) {
                const char *alt_j = runtime_judgements_get_save_path();
                if (alt_j && *alt_j) j_data = read_entire_file(alt_j, &j_sz);
            }
            if (!j_data) {
                char alt_j2[750];
                snprintf(alt_j2, sizeof(alt_j2), "%s/judgementsBackup.inf", s_user_dir);
                j_data = read_entire_file(alt_j2, &j_sz);
            }

            if (b_data || m_data || p_data || j_data) {
                MultipartBuf mp;
                mp_init(&mp);
                mp_add_field(&mp, "title_id", title_id);
                if (b_data) mp_add_file(&mp, "file_backup", "backup.inf", b_data, b_sz);
                if (m_data) mp_add_file(&mp, "file_music_backup", "musicBackup.inf", m_data, m_sz);
                if (p_data) mp_add_file(&mp, "file_profica_backup", "proficaBackup.inf", p_data, p_sz);
                if (j_data) mp_add_file(&mp, "file_judgements_backup", "judgementsBackup.inf", j_data, j_sz);
                mp_finish(&mp);

                char resp[1024];
                int err = http_post_multipart("/api/game/cloud-save/upload", mp.data, mp.size, resp, sizeof(resp));
                if (err == 0) {
                    printf("TFFScores: 4-file cloud save backed up successfully for %s (j_sz=%zu)\n", title_id, j_sz);
                    if (!is_pb) {
                        theatrhythm_gpu_show_notice("TFFScores: Cloud save synced", 3000);
                    }
                } else {
                    printf("TFFScores: cloud save upload returned %d\n", err);
                }
                mp_free(&mp);
            }

            if (b_data) free(b_data);
            if (m_data) free(m_data);
            if (p_data) free(p_data);
            if (j_data) free(j_data);
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
    runtime_leaderboard_init();
    runtime_savedata_set_cloud_hooks(runtime_tffscores_maybe_download_save, runtime_tffscores_on_save_commit);
    runtime_pad_set_confirm_hook(runtime_tffscores_trigger_immediate_screenshot);
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

static uint64_t s_last_song_finish_ms = 0;

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
    if (s_last_song_finish_ms != 0 && (now_ms - s_last_song_finish_ms) < 12000) {
        /* Debounce: count-up behaviours fire for every counter during results sequence */
        return;
    }
    s_last_song_finish_ms = now_ms;

    pthread_mutex_lock(&s_tff_lock);
    if (s_stage_music_id == 0 || s_stage_is_custom || is_custom_chart_id(s_stage_music_id)) {
        uint32_t cid = s_stage_music_id;
        s_stage_active = 0;
        s_stage_is_score_pb = 0;
        s_stage_is_damage_pb = 0;
        s_stage_save_host[0] = '\0';
        s_awaiting_result_screenshot = 0;
        s_custom_suppress_until_ms = now_ms + 15000;
        pthread_mutex_unlock(&s_tff_lock);
        printf("TFFScores: Skipping upload & screenshot for custom/unknown chart (ID %u)\n", cid);
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
    if (s_stage_is_custom && (s_stage_active || GetTickCount64() < s_custom_suppress_until_ms)) {
        printf("TFFScores: Ignoring save commit from custom chart session (%s)\n", save_host);
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

/* -------------------------------------------------------------------------- */
/* Game code hooks (Leap Day guard, Results Page 3 UI, & Play Style/Damage)   */
/* -------------------------------------------------------------------------- */

typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out; } TffHookRegs;
typedef int (ABI *TffHookFn)(TffHookRegs *regs);

static uint64_t load64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t load32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put64(void *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void *object_class(const void *obj) { return obj ? (void *)load64(obj) : NULL; }

#define MAX_TFF_CALLBACKS 4
static RuntimeTffscoresPlaySettingCallback s_play_setting_cbs[MAX_TFF_CALLBACKS];
static size_t s_play_setting_cb_count = 0;
static RuntimeTffscoresInputDamageCallback s_input_damage_cbs[MAX_TFF_CALLBACKS];
static size_t s_input_damage_cb_count = 0;

void runtime_tffscores_add_play_setting_config_callback(RuntimeTffscoresPlaySettingCallback cb) {
    if (cb && s_play_setting_cb_count < MAX_TFF_CALLBACKS) {
        s_play_setting_cbs[s_play_setting_cb_count++] = cb;
    }
}

void runtime_tffscores_add_input_damage_callback(RuntimeTffscoresInputDamageCallback cb) {
    if (cb && s_input_damage_cb_count < MAX_TFF_CALLBACKS) {
        s_input_damage_cbs[s_input_damage_cb_count++] = cb;
    }
}

enum {
    /* 12 Hook Sites */
    TFF_DATE_TO_TICKS,
    TFF_GREGORIAN_DATE_TO_TICKS,
    TFF_COUNT_UP_ON_END,
    TFF_RETRY_WINDOW_OPEN,
    TFF_PLAY_SETTING_CONFIG,
    TFF_INPUT_DAMAGE,
    TFF_BEST_PLAY_SETUP_6,
    TFF_BEST_PLAY_SETUP_2,
    TFF_RESULT_DETAIL_INIT,
    TFF_RESULT_DETAIL_ACTION,
    TFF_MOVE_NEXT_SCENE,
    TFF_GUMMI_ANIM_UPDATE,
    TFF_HOOK_SITES,

    /* 46 Callee Sites */
    C_MUSIC_RESULT_PLAY_DATA_INSTANCE = TFF_HOOK_SITES,
    C_BEFORE_PLAY_DATA_MAX_DAMAGE,
    C_OBJECT_OP_IMPLICIT,
    C_OBJECT_SET_HIDE_FLAGS,
    C_OBJECT_GET_NAME,
    C_OBJECT_SET_NAME,
    C_OBJECT_CLONE_WITH_PARENT,
    C_COMPONENT_GET_GO,
    C_COMPONENT_GET_TR,
    C_GO_SET_ACTIVE,
    C_BEHAVIOUR_SET_ENABLED,
    C_TR_GET_PARENT,
    C_TR_GET_CHILD_COUNT,
    C_TR_GET_CHILD,
    C_TR_SET_AS_LAST_SIBLING,
    C_TR_GET_LOCAL_POS,
    C_TR_SET_LOCAL_POS,
    C_TR_SET_LOCAL_SCALE,
    C_RT_GET_RECT,
    C_RT_SET_ANCHOR_MIN,
    C_RT_SET_ANCHOR_MAX,
    C_RT_SET_PIVOT,
    C_RT_SET_ANCHORED_POS,
    C_RT_SET_SIZE_DELTA,
    C_GRAPHIC_SET_COLOR,
    C_GRAPHIC_SET_RAYCAST,
    C_IMAGE_GET_SPRITE,
    C_IMAGE_SET_SPRITE,
    C_IMAGE_SET_TYPE,
    C_IMAGE_SET_PRESERVE_ASPECT,
    C_TEXT_SET_FONT_SIZE,
    C_TEXT_SET_ALIGNMENT,
    C_TEXT_SET_H_OVERFLOW,
    C_TEXT_SET_V_OVERFLOW,
    C_TEXT_SET_BEST_FIT,
    C_SPRITE_GET_TEXTURE,
    C_SPRITE_CREATE,
    C_TEX2D_CTOR,
    C_TEX_SET_FILTER,
    C_TEX_SET_WRAP,
    C_TEX2D_LOAD_RAW,
    C_TEX2D_APPLY,
    C_GUMMI_ANIM_PLAY,
    C_CLEAR_MARK_DISPLAY,
    C_CLEAR_MARK_SKIP_ANIM,
    C_UIOBJECT_END_ACTION,
    TFF_SITES
};

static const GameSite tff_sites[TFF_SITES] = {
    /* 12 Hook Sites */
    {"System.DateTime::DateToTicks", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    {"System.Globalization.GregorianCalendarHelper::DateToTicks", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 41 89 d6"}},
    {"Game.Scene.MusicResult.CountUpBehaviour::OnEnd", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 41 89 d6"}},
    {"Game.Scene.MusicResult.MusicResultRetryWindowPartsMain::Open", 0, 0, 0,
     {"48 8b bf 90 00 00 00 31 d2 | e9 ?? ?? ?? ?? 66 90 48 8b 87 90 00 00 00"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::setConfigInfo", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 41 89 f6"}},
    {"Game.Scene.MusicStage.MusicStageMainLoop::getTriggerInputDamage", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 80 3d ?? ?? ?? ?? 00"}},
    {"Game.Scene.MusicResult.MusicResultTitlePartsMain/BestPlayResultDisplay::Setup(6)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 e4 e0 48 81 ec a0 00 00 00"}},
    {"Game.Scene.MusicResult.MusicResultTitlePartsMain/BestPlayResultDisplay::Setup(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 48 83 e4 e0 48 83 ec 60 4c 8b 25 ?? ?? ?? ??"}},
    {"Game.Scene.MusicResult.MusicResultDetailPartsMain::InitializeSetting", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 e4 e0 48 83 ec 60 4c 8b 3d ?? ?? ?? ??"}},
    {"Game.Scene.MusicResult.MusicResultDetailPartsMain::OnExecutedScenePartsMain", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 48 83 e4 e0 48 83 ec 60 4c 8b 25 ?? ?? ?? ??"}},
    {"Game.Scene.MusicResult.MusicResultMain::moveNextScene", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 49 89 fe 75 2b 48 8d 3d ?? ?? ?? ??"}},
    {"Gummi.Graphics.GummiAnimation::Update", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 ec 38 8b 47 70 48 89 fb 83 f8 03"}},

    /* 46 Callee Sites */
    {"Game.Scene.MusicResult.MusicResultPlayData::get_Instance", 0, 0, 0, {""}},
    {"Game.PlayData.BeforePlayData::get_MaxDamage", 0, 0, 0, {""}},
    {"UnityEngine.Object::op_Implicit", 0, 0, 0, {""}},
    {"UnityEngine.Object::set_hideFlags", 0, 0, 0, {""}},
    {"UnityEngine.Object::get_name", 0, 0, 0, {""}},
    {"UnityEngine.Object::set_name", 0, 0, 0, {""}},
    {"UnityEngine.Object::Internal_CloneSingleWithParent", 0, 0, 0, {""}},
    {"UnityEngine.Component::get_gameObject", 0, 0, 0, {""}},
    {"UnityEngine.Component::get_transform", 0, 0, 0, {""}},
    {"UnityEngine.GameObject::SetActive", 0, 0, 0, {""}},
    {"UnityEngine.Behaviour::set_enabled", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_parent", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_childCount", 0, 0, 0, {""}},
    {"UnityEngine.Transform::GetChild", 0, 0, 0, {""}},
    {"UnityEngine.Transform::SetAsLastSibling", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_localPosition", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_localPosition", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_localScale", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::get_rect", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_anchorMin", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_anchorMax", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_pivot", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_anchoredPosition", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_sizeDelta", 0, 0, 0, {""}},
    {"UnityEngine.UI.Graphic::set_color", 0, 0, 0, {""}},
    {"UnityEngine.UI.Graphic::set_raycastTarget", 0, 0, 0, {""}},
    {"UnityEngine.UI.Image::get_sprite", 0, 0, 0, {""}},
    {"UnityEngine.UI.Image::set_sprite", 0, 0, 0, {""}},
    {"UnityEngine.UI.Image::set_type", 0, 0, 0, {""}},
    {"UnityEngine.UI.Image::set_preserveAspect", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_fontSize", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_alignment", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_horizontalOverflow", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_verticalOverflow", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_resizeTextForBestFit", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_texture", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::CreateSprite", 0, 0, 0, {""}},
    {NULL, 0, 1, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00 45 89 c6 41 89 cf 41 89 d4 41 89 f5 48 89 fb 75 1f"}},
    {"UnityEngine.Texture::set_filterMode", 0, 0, 0, {""}},
    {"UnityEngine.Texture::set_wrapMode", 0, 0, 0, {""}},
    {"UnityEngine.Texture2D::LoadRawTextureData(2)", 0, 0, 0, {""}},
    {"UnityEngine.Texture2D::Apply(2)", 0, 0, 0, {""}},
    {"Gummi.Graphics.GummiAnimation::Play", 0, 0, 0, {""}},
    {"Game.Prefab.ClearMarkBehaviour::DisplayClearMark", 0, 0, 0, {""}},
    {"Game.Prefab.ClearMarkBehaviour::SkipClearMarkInAnim", 0, 0, 0, {""}},
    {"Gummi.Scene.UI.UIObject::EndAction", 0, 0, 0, {""}},
};

static unsigned char *s_tff_module = NULL;
static GameCode s_tff_code[TFF_SITES];
#define CODE(id) ((void *)(s_tff_module + s_tff_code[id].offset))

/* Lazily resolved IL2CPP API exports from Il2CppUserAssemblies.prx */
static void *(ABI *il2cpp_domain_get_fn)(void) = NULL;
static void **(ABI *il2cpp_domain_get_assemblies_fn)(void *domain, size_t *count) = NULL;
static void *(ABI *il2cpp_assembly_get_image_fn)(void *assembly) = NULL;
static void *(ABI *il2cpp_class_from_name_fn)(void *image, const char *ns, const char *name) = NULL;
static void *(ABI *il2cpp_object_new_fn)(void *klass) = NULL;
static void *(ABI *il2cpp_string_new_fn)(const char *str) = NULL;
static void (ABI *il2cpp_runtime_class_init_fn)(void *klass) = NULL;
static uint32_t (ABI *il2cpp_gchandle_new_fn)(void *obj, bool pinned) = NULL;

static bool s_il2cpp_resolved = false;
static void *s_texture2d_class = NULL;

static void *find_il2cpp_class(const char *ns, const char *name) {
    if (!il2cpp_domain_get_fn || !il2cpp_domain_get_assemblies_fn ||
        !il2cpp_assembly_get_image_fn || !il2cpp_class_from_name_fn) {
        return NULL;
    }
    void *domain = il2cpp_domain_get_fn();
    if (!domain) return NULL;
    size_t count = 0;
    void **assemblies = il2cpp_domain_get_assemblies_fn(domain, &count);
    if (!assemblies || count == 0 || count > 512) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (!assemblies[i]) continue;
        void *img = il2cpp_assembly_get_image_fn(assemblies[i]);
        if (!img) continue;
        void *klass = il2cpp_class_from_name_fn(img, ns, name);
        if (klass) {
            if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
            return klass;
        }
    }
    return NULL;
}

static void ensure_il2cpp_api(void) {
    if (!s_il2cpp_resolved) {
        const char *prx = "Il2CppUserAssemblies.prx";
        void *dom = runtime_module_export(prx, "il2cpp_domain_get");
        if (dom) {
            il2cpp_domain_get_fn = (void *(ABI *)(void))dom;
            il2cpp_domain_get_assemblies_fn = (void **(ABI *)(void *, size_t *))runtime_module_export(prx, "il2cpp_domain_get_assemblies");
            il2cpp_assembly_get_image_fn = (void *(ABI *)(void *))runtime_module_export(prx, "il2cpp_assembly_get_image");
            il2cpp_class_from_name_fn = (void *(ABI *)(void *, const char *, const char *))runtime_module_export(prx, "il2cpp_class_from_name");
            il2cpp_object_new_fn = (void *(ABI *)(void *))runtime_module_export(prx, "il2cpp_object_new");
            il2cpp_string_new_fn = (void *(ABI *)(const char *))runtime_module_export(prx, "il2cpp_string_new");
            void *rci = runtime_module_export(prx, "il2cpp_runtime_class_init");
            if (rci) il2cpp_runtime_class_init_fn = (void (ABI *)(void *))rci;
            il2cpp_gchandle_new_fn = (uint32_t (ABI *)(void *, bool))runtime_module_export(prx, "il2cpp_gchandle_new");
            s_il2cpp_resolved = true;
        }
    }
    if (s_il2cpp_resolved && !s_texture2d_class) {
        void *dom = il2cpp_domain_get_fn ? il2cpp_domain_get_fn() : NULL;
        if (dom) {
            s_texture2d_class = find_il2cpp_class("UnityEngine", "Texture2D");
        }
    }
}

static uint32_t gc_pin(void *obj, bool pinned) {
    if (!obj) return 0;
    ensure_il2cpp_api();
    if (il2cpp_gchandle_new_fn) return il2cpp_gchandle_new_fn(obj, pinned);
    return 0;
}

static void *alloc_managed_object(void *klass) {
    if (!klass || ((uintptr_t)klass & 1u) != 0 || (uintptr_t)klass < 0x10000ULL) return NULL;
    ensure_il2cpp_api();
    if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
    if (il2cpp_object_new_fn) return il2cpp_object_new_fn(klass);
    return NULL;
}

static void *make_string(const void *like, const char *text) {
    if (!text) return NULL;
    ensure_il2cpp_api();
    if (!like && il2cpp_string_new_fn) {
        return il2cpp_string_new_fn(text);
    }
    const size_t n = strlen(text);
    unsigned char *s = (unsigned char *)runtime_low_alloc(0x16 + 2 * n);
    if (!s) return NULL;
    if (like) memcpy(s, like, 8);
    put32(s + 0x10, (int32_t)n);
    for (size_t i = 0; i < n; ++i) {
        uint16_t c = (uint8_t)text[i];
        memcpy(s + 0x14 + 2 * i, &c, 2);
    }
    return s;
}

static void read_guest_string(const void *s, char *out, size_t size) {
    size_t n = 0;
    if (s && (uintptr_t)s > 0x10000 && (uintptr_t)s < 0x00007fffffffffffULL) {
        const int32_t length = load32((const char *)s + 0x10);
        if (length > 0 && length < 2048) {
            const uint16_t *chars = (const uint16_t *)((const char *)s + 0x14);
            for (int32_t i = 0; i < length && n + 1 < size; ++i)
                out[n++] = chars[i] < 128 ? (char)chars[i] : '?';
        }
    }
    out[n] = 0;
}

static bool uobj_alive(const void *obj) {
    if (!obj) return false;
    uintptr_t p = (uintptr_t)obj;
    if (p < 0x10000ULL || p > 0x00007fffffffffffULL) return false;
    return load64((const char *)obj + 0x10) != 0;
}

static bool unity_native_alive(const void *obj) {
    if (!uobj_alive(obj)) return false;
    typedef bool (ABI *OpImplicitFn)(const void *obj, void *method);
    OpImplicitFn op_imp = (OpImplicitFn)CODE(C_OBJECT_OP_IMPLICIT);
    return op_imp(obj, NULL);
}

static void protect_cached_unity_object(void *obj) {
    if (!unity_native_alive(obj)) return;
    typedef void (ABI *SetHideFlagsFn)(void *obj, int32_t flags, void *method);
    SetHideFlagsFn set_hf = (SetHideFlagsFn)CODE(C_OBJECT_SET_HIDE_FLAGS);
    set_hf(obj, 32 /* HideFlags.DontUnloadUnusedAsset */, NULL);
}

/* Clamp Feb 29 on non-leap years to Feb 28 before DateToTicks validates it (official 1.0.5 fix). */
static ABI int leap_day_guard(TffHookRegs *r) {
    int year = (int)r->rdi;
    int month = (int)r->rsi;
    int day = (int)r->rdx;
    if (month == 2 && day == 29 && !(year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) {
        r->rdx = 28;
    }
    return 0;
}

static ABI int on_song_result_countup_end(TffHookRegs *r) {
    (void)r;
    runtime_tffscores_on_song_result_finish();
    return 0;
}

static ABI int on_retry_window_open(TffHookRegs *r) {
    (void)r;
    runtime_tffscores_trigger_immediate_screenshot();
    return 0;
}

static char s_current_play_style[32] = "standard";
static int s_results_page = 1;
static void *s_title_parts_main = NULL;
static void *s_best_play_display = NULL;

int runtime_tffscores_get_results_page(void) {
    return s_results_page;
}

static ABI int hook_play_setting_config(TffHookRegs *r) {
    void *setting = (void *)r->rdi;
    void *config_info = (void *)r->rsi;
    if (setting) {
        int play_style = (int)load32((const char *)setting + 0x54);
        if (play_style == 1) snprintf(s_current_play_style, sizeof(s_current_play_style), "simple");
        else if (play_style == 2) snprintf(s_current_play_style, sizeof(s_current_play_style), "pair");
        else snprintf(s_current_play_style, sizeof(s_current_play_style), "standard");
        printf("TFFScores: Play style configured: %s (%d)\n", s_current_play_style, play_style);
        for (size_t i = 0; i < s_play_setting_cb_count; ++i) {
            s_play_setting_cbs[i](setting, config_info);
        }
    }
    return 0;
}

static ABI int hook_input_damage(TffHookRegs *r) {
    void *main_loop = (void *)r->rdi;
    void *trigger_data = (void *)r->rsi;
    void *attacked_out = (void *)r->rdx;
    for (size_t i = 0; i < s_input_damage_cb_count; ++i) {
        float dmg = 0.0f;
        if (s_input_damage_cbs[i](main_loop, trigger_data, attacked_out, &dmg)) {
            uint32_t bits = 0;
            memcpy(&bits, &dmg, 4);
            r->rax = bits;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_best_play_setup(TffHookRegs *r) {
    if (r->rdi) s_best_play_display = (void *)r->rdi;
    return 0;
}

static void check_and_notify_damage_pb(void) {
    typedef void *(ABI *GetInstanceFn)(void *method);
    typedef int32_t (ABI *GetMaxDamageFn)(void *method);
    GetInstanceFn get_play_data = (GetInstanceFn)CODE(C_MUSIC_RESULT_PLAY_DATA_INSTANCE);
    GetMaxDamageFn get_prev_max_damage = (GetMaxDamageFn)CODE(C_BEFORE_PLAY_DATA_MAX_DAMAGE);
    void *play_data = get_play_data(NULL);
    if (!play_data) return;

    uint8_t need_save_damage = *(const uint8_t *)((const char *)play_data + 0x84);
    uint8_t is_not_save = *(const uint8_t *)((const char *)play_data + 0xbc);
    uint8_t is_demo_or_auto = *(const uint8_t *)((const char *)play_data + 0xbd);
    int32_t cur_damage = (int32_t)load32((const char *)play_data + 0x80);
    int32_t prev_max_damage = get_prev_max_damage(NULL);

    if (need_save_damage != 0 && is_not_save == 0 && is_demo_or_auto == 0 &&
        cur_damage > 0 && cur_damage > prev_max_damage) {
        runtime_tffscores_notify_damage_pb(1);
    }
}

static void on_judgements_stage_begin(void *enumerator, int state_before, bool move_next_pending) {
    (void)enumerator;
    if (move_next_pending && state_before == 0) {
        s_results_page = 1;
        uint32_t music_id = 0;
        int difficulty = 0;
        runtime_judgements_get_current_stage(&music_id, &difficulty);
        runtime_tffscores_notify_stage_start(music_id, difficulty);
    }
}

static void on_judgements_score_pb(void *title_parts_main, bool is_new_best) {
    if (title_parts_main) s_title_parts_main = title_parts_main;
    runtime_tffscores_notify_score_pb(is_new_best ? 1 : 0);
    check_and_notify_damage_pb();
}

static void on_judgements_title_init(void *title_parts_main) {
    if (title_parts_main) s_title_parts_main = title_parts_main;
    s_results_page = 1;
    check_and_notify_damage_pb();
}

/* -------------------------------------------------------------------------- */
/* NATIVE UNITY UI PAGE 3 (LEADERBOARD + JUDGEMENT GRAPH + ANIMATION)         */
/* -------------------------------------------------------------------------- */

typedef struct { float x, y; } UGuiVec2;
typedef struct { float x, y, z; } UGuiVec3;
typedef struct { float x, y, z, w; } UGuiVec4;
typedef struct { float x, y, w, h; } UGuiRect;

typedef void *(ABI *GetGOPtrFn)(void *comp, void *method);
typedef void *(ABI *GetTransformFn)(void *obj, void *method);
typedef void (ABI *SetActiveFn)(void *go, bool act, void *method);
typedef void (ABI *SetEnabledFn)(void *beh, bool en, void *method);
typedef void *(ABI *GetParentFn)(void *t, void *method);
typedef int32_t (ABI *GetChildCountFn)(void *t, void *method);
typedef void *(ABI *GetChildFn)(void *t, int32_t idx, void *method);
typedef void (ABI *SetAsLastSiblingFn)(void *t, void *method);
typedef UGuiVec3 (ABI *GetLocalPosFn)(void *t, void *method);
typedef void (ABI *SetLocalPosFn)(void *t, UGuiVec3 v, void *method);
typedef void (ABI *SetLocalScaleFn)(void *t, UGuiVec3 v, void *method);
typedef void *(ABI *GetObjNameFn)(void *obj, void *method);
typedef void (ABI *SetObjNameFn)(void *obj, void *str, void *method);
typedef void *(ABI *CloneWithParentFn)(void *obj, void *parent_t, int world_stays);
typedef UGuiRect (ABI *GetRectFn)(void *rt, void *method);
typedef void (ABI *SetVec2Fn)(void *rt, UGuiVec2 v, void *method);
typedef void (ABI *SetColorFn)(void *graphic, UGuiVec4 c, void *method);
typedef void (ABI *SetRaycastFn)(void *graphic, bool b, void *method);
typedef void *(ABI *GetSpriteFn)(void *img, void *method);
typedef void (ABI *SetSpriteFn)(void *img, void *spr, void *method);
typedef void (ABI *SetImgTypeFn)(void *img, int32_t type, void *method);
typedef void (ABI *SetPreserveAspectFn)(void *img, bool b, void *method);
typedef void (ABI *SetFontSizeFn)(void *txt, int32_t sz, void *method);
typedef void (ABI *SetTextIntFn)(void *txt, int32_t val, void *method);
typedef void (ABI *SetBestFitFn)(void *txt, bool b, void *method);
typedef void *(ABI *SpriteGetTexFn)(void *spr, void *method);
typedef void *(ABI *SpriteCreateFullFn)(void *tex, UGuiRect rect, UGuiVec2 pivot, float ppu,
                                        uint32_t extrude, int32_t meshType, UGuiVec4 border,
                                        bool genFallback, void *method);
typedef void (ABI *TexCtorFn)(void *self, int32_t w, int32_t h, int32_t fmt, bool mip, void *method);
typedef void (ABI *TexSetIntFn)(void *self, int32_t mode, void *method);
typedef void (ABI *TexLoadRawFn)(void *self, const void *data, int32_t size, void *method);
typedef void (ABI *TexApplyFn)(void *self, bool updateMip, bool makeUnreadable, void *method);
typedef void (ABI *GummiPlayFn)(void *anim, void *param_arr, void *method);

/* Reusable alternating Il2Cpp System.String pair so UI.Text::set_text never short-circuits */
typedef struct {
    unsigned char *buf[2];
    int toggle;
} DualGuestString;

static void *s_uobj_string_proto = NULL;

static void ensure_string_proto(void *any_uobj) {
    if (s_uobj_string_proto || !uobj_alive(any_uobj)) return;
    GetObjNameFn get_name = (GetObjNameFn)CODE(C_OBJECT_GET_NAME);
    void *nm = get_name(any_uobj, NULL);
    if (nm && (uintptr_t)nm > 0x10000ULL) s_uobj_string_proto = nm;
}

static void *dual_string_set(DualGuestString *ds, const char *text) {
    if (!ds || !text) return NULL;
    if (!ds->buf[0]) {
        ds->buf[0] = (unsigned char *)runtime_low_alloc(320);
        ds->buf[1] = (unsigned char *)runtime_low_alloc(320);
        if (ds->buf[0] && s_uobj_string_proto) memcpy(ds->buf[0], s_uobj_string_proto, 8);
        if (ds->buf[1] && s_uobj_string_proto) memcpy(ds->buf[1], s_uobj_string_proto, 8);
    }
    ds->toggle ^= 1;
    unsigned char *s = ds->buf[ds->toggle];
    if (!s) return NULL;
    if (s_uobj_string_proto && load64(s) == 0) memcpy(s, s_uobj_string_proto, 8);
    size_t n = strlen(text);
    if (n > 140) n = 140;
    put32(s + 0x10, (int32_t)n);
    for (size_t i = 0; i < n; ++i) {
        uint16_t c = (uint8_t)text[i];
        memcpy(s + 0x14 + 2 * i, &c, 2);
    }
    uint16_t zero = 0;
    memcpy(s + 0x14 + 2 * n, &zero, 2);
    return s;
}

static void ugui_set_text_str(void *txt_comp, DualGuestString *ds, const char *str) {
    if (!uobj_alive(txt_comp) || !str) return;
    void *gstr = dual_string_set(ds, str);
    if (!gstr) return;
    void *klass = object_class(txt_comp);
    if (!klass) return;
    typedef void (ABI *SetTextVirtFn)(void *self, void *val, void *method);
    SetTextVirtFn fn = (SetTextVirtFn)load64((const char *)klass + 0x5e8);
    void *method = (void *)load64((const char *)klass + 0x5f0);
    if (fn) fn(txt_comp, gstr, method);
}

static void ugui_place_topleft(void *rt, float x, float y_down, float w, float h) {
    if (!uobj_alive(rt)) return;
    SetVec2Fn set_amin = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MIN);
    SetVec2Fn set_amax = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MAX);
    SetVec2Fn set_piv  = (SetVec2Fn)CODE(C_RT_SET_PIVOT);
    SetVec2Fn set_apos = (SetVec2Fn)CODE(C_RT_SET_ANCHORED_POS);
    SetVec2Fn set_size = (SetVec2Fn)CODE(C_RT_SET_SIZE_DELTA);
    SetLocalPosFn set_lpos = (SetLocalPosFn)CODE(C_TR_SET_LOCAL_POS);
    SetLocalScaleFn set_scale = (SetLocalScaleFn)CODE(C_TR_SET_LOCAL_SCALE);

    set_lpos(rt, (UGuiVec3){0.0f, 0.0f, 0.0f}, NULL);
    set_scale(rt, (UGuiVec3){1.0f, 1.0f, 1.0f}, NULL);
    set_amin(rt, (UGuiVec2){0.0f, 1.0f}, NULL);
    set_amax(rt, (UGuiVec2){0.0f, 1.0f}, NULL);
    set_piv(rt, (UGuiVec2){0.0f, 1.0f}, NULL);
    set_size(rt, (UGuiVec2){w, h}, NULL);
    set_apos(rt, (UGuiVec2){x, -y_down}, NULL);
}

static void *ugui_clone_label(void *tpl_txt, void *parent_rt, const char *obj_name,
                              float x, float y_down, float w, float h,
                              int32_t font_size, int32_t align, bool clip_horiz,
                              UGuiVec4 color, DualGuestString *ds, const char *initial_text) {
    if (!uobj_alive(tpl_txt) || !uobj_alive(parent_rt)) return NULL;
    CloneWithParentFn clone_fn = (CloneWithParentFn)CODE(C_OBJECT_CLONE_WITH_PARENT);
    GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
    GetTransformFn get_tr = (GetTransformFn)CODE(C_COMPONENT_GET_TR);
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);
    SetEnabledFn set_enabled = (SetEnabledFn)CODE(C_BEHAVIOUR_SET_ENABLED);
    SetObjNameFn set_name = (SetObjNameFn)CODE(C_OBJECT_SET_NAME);
    SetFontSizeFn set_fs = (SetFontSizeFn)CODE(C_TEXT_SET_FONT_SIZE);
    SetTextIntFn set_align = (SetTextIntFn)CODE(C_TEXT_SET_ALIGNMENT);
    SetTextIntFn set_hover = (SetTextIntFn)CODE(C_TEXT_SET_H_OVERFLOW);
    SetTextIntFn set_vover = (SetTextIntFn)CODE(C_TEXT_SET_V_OVERFLOW);
    SetBestFitFn set_bestfit = (SetBestFitFn)CODE(C_TEXT_SET_BEST_FIT);
    SetColorFn set_color = (SetColorFn)CODE(C_GRAPHIC_SET_COLOR);
    SetRaycastFn set_ray = (SetRaycastFn)CODE(C_GRAPHIC_SET_RAYCAST);

    void *txt = clone_fn(tpl_txt, parent_rt, 0);
    if (!uobj_alive(txt)) return NULL;
    void *go = get_go(txt, NULL);
    void *rt = get_tr(txt, NULL);
    if (obj_name && s_uobj_string_proto && uobj_alive(go)) {
        set_name(go, make_string(s_uobj_string_proto, obj_name), NULL);
    }
    ugui_place_topleft(rt, x, y_down, w, h);
    set_bestfit(txt, false, NULL);
    set_fs(txt, font_size, NULL);
    set_align(txt, align, NULL);
    set_hover(txt, clip_horiz ? 0 : 1, NULL); /* 0=Wrap/Clip, 1=Overflow */
    set_vover(txt, 1, NULL);                  /* 1=Overflow */
    set_color(txt, color, NULL);
    set_ray(txt, false, NULL);
    set_enabled(txt, true, NULL);
    if (uobj_alive(go)) set_active(go, true, NULL);
    if (initial_text && ds) ugui_set_text_str(txt, ds, initial_text);
    return txt;
}

static bool extract_ui_templates(void *detail_parts_main, void **out_img_tpl, void **out_txt_tpl, void **out_tex_class_src) {
    *out_img_tpl = NULL;
    *out_txt_tpl = NULL;
    *out_tex_class_src = NULL;
    if (!detail_parts_main) return false;

    void *status_array = *(void **)((char *)detail_parts_main + 0xa0);
    if (!status_array || *(uint32_t *)((char *)status_array + 0x18) == 0) return false;
    void *card0 = *(void **)((char *)status_array + 0x20);
    if (!uobj_alive(card0)) return false;

    ensure_string_proto(card0);

    /* Template LocalizedText from card0 numeric stat array (+0x20) */
    void *stat_texts = *(void **)((char *)card0 + 0x20);
    if (stat_texts && *(uint32_t *)((char *)stat_texts + 0x18) > 0) {
        void *t0 = *(void **)((char *)stat_texts + 0x20);
        if (uobj_alive(t0)) *out_txt_tpl = t0;
    }

    /* Template UI.Image from card0 rank_displays[0].images[0] (+0x30 -> [0] -> +0x18 -> [0]) */
    void *rank_displays = *(void **)((char *)card0 + 0x30);
    if (rank_displays && *(uint32_t *)((char *)rank_displays + 0x18) > 0) {
        void *rank0 = *(void **)((char *)rank_displays + 0x20);
        if (rank0) {
            void *imgs = *(void **)((char *)rank0 + 0x18);
            if (imgs && *(uint32_t *)((char *)imgs + 0x18) > 0) {
                GetSpriteFn get_spr = (GetSpriteFn)CODE(C_IMAGE_GET_SPRITE);
                SpriteGetTexFn get_tex = (SpriteGetTexFn)CODE(C_SPRITE_GET_TEXTURE);
                uint32_t n_imgs = *(uint32_t *)((char *)imgs + 0x18);
                for (uint32_t i = 0; i < n_imgs; ++i) {
                    void *im = *(void **)((char *)imgs + 0x20 + i * sizeof(void *));
                    if (!uobj_alive(im)) continue;
                    if (!*out_img_tpl) *out_img_tpl = im;
                    void *sp = get_spr(im, NULL);
                    if (uobj_alive(sp)) {
                        void *tx = get_tex(sp, NULL);
                        if (uobj_alive(tx)) {
                            *out_tex_class_src = tx;
                            break;
                        }
                    }
                }
            }
        }
    }
    return (*out_img_tpl != NULL && *out_txt_tpl != NULL && *out_tex_class_src != NULL);
}

/* Sum localPositions from child_rt up to (but not including) ancestor_rt */
static UGuiVec2 accum_local_offset(void *child_rt, void *ancestor_rt) {
    UGuiVec2 sum = {0.0f, 0.0f};
    if (!uobj_alive(child_rt) || !uobj_alive(ancestor_rt) || child_rt == ancestor_rt) return sum;
    GetParentFn get_parent = (GetParentFn)CODE(C_TR_GET_PARENT);
    GetLocalPosFn get_lpos = (GetLocalPosFn)CODE(C_TR_GET_LOCAL_POS);
    void *cur = child_rt;
    for (int depth = 0; depth < 6 && uobj_alive(cur) && cur != ancestor_rt; ++depth) {
        UGuiVec3 lp = get_lpos(cur, NULL);
        sum.x += lp.x;
        sum.y += lp.y;
        cur = get_parent(cur, NULL);
    }
    return sum;
}

/* Search root_rt and its children (depth <= 2) for the frame RectTransform matching target dimensions */
static void find_best_frame_in_window(void *root_rt, void *skip_rt,
                                      float min_w, float max_w, float min_h, float max_h,
                                      float ideal_w, float ideal_h,
                                      float *out_cx, float *out_cy, float *out_w, float *out_h) {
    GetRectFn get_rect = (GetRectFn)CODE(C_RT_GET_RECT);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(C_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(C_TR_GET_CHILD);
    GetObjNameFn get_name = (GetObjNameFn)CODE(C_OBJECT_GET_NAME);

    void *best_rt = NULL;
    UGuiRect best_r = {0, 0, ideal_w, ideal_h};
    float best_err = 1e9f;

    UGuiRect rr = get_rect(root_rt, NULL);
    if (rr.w >= min_w && rr.w <= max_w && rr.h >= min_h && rr.h <= max_h) {
        best_rt = root_rt;
        best_r = rr;
        best_err = fabsf(rr.w - ideal_w) + fabsf(rr.h - ideal_h);
    }

    int32_t n1 = get_cc(root_rt, NULL);
    for (int32_t i = 0; i < n1 && i < 32; ++i) {
        void *c1 = get_ch(root_rt, i, NULL);
        if (!uobj_alive(c1) || c1 == skip_rt) continue;
        char nm[64] = {0};
        read_guest_string(get_name(c1, NULL), nm, sizeof(nm));
        if (!strncmp(nm, "TFFPage3_", 9)) continue;

        UGuiRect r1 = get_rect(c1, NULL);
        if (r1.w >= min_w && r1.w <= max_w && r1.h >= min_h && r1.h <= max_h) {
            float err = fabsf(r1.w - ideal_w) + fabsf(r1.h - ideal_h);
            if (err < best_err) {
                best_err = err;
                best_rt = c1;
                best_r = r1;
            }
        }
        int32_t n2 = get_cc(c1, NULL);
        for (int32_t j = 0; j < n2 && j < 24; ++j) {
            void *c2 = get_ch(c1, j, NULL);
            if (!uobj_alive(c2) || c2 == skip_rt) continue;
            char nm2[64] = {0};
            read_guest_string(get_name(c2, NULL), nm2, sizeof(nm2));
            if (!strncmp(nm2, "TFFPage3_", 9)) continue;
            UGuiRect r2 = get_rect(c2, NULL);
            if (r2.w >= min_w && r2.w <= max_w && r2.h >= min_h && r2.h <= max_h) {
                float err = fabsf(r2.w - ideal_w) + fabsf(r2.h - ideal_h);
                if (err < best_err) {
                    best_err = err;
                    best_rt = c2;
                    best_r = r2;
                }
            }
        }
    }

    if (best_rt) {
        UGuiVec2 off = accum_local_offset(best_rt, root_rt);
        *out_cx = off.x + best_r.x + best_r.w * 0.5f;
        *out_cy = off.y + best_r.y + best_r.h * 0.5f;
        *out_w = best_r.w;
        *out_h = best_r.h;
    } else {
        *out_cx = rr.x + rr.w * 0.5f;
        *out_cy = rr.y + rr.h * 0.5f;
        *out_w = (rr.w > 200.0f) ? rr.w : ideal_w;
        *out_h = (rr.h > 150.0f) ? rr.h : ideal_h;
    }
}

static void place_centered_panel(void *panel_rt, float cx, float cy, float w, float h) {
    SetVec2Fn set_amin = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MIN);
    SetVec2Fn set_amax = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MAX);
    SetVec2Fn set_piv  = (SetVec2Fn)CODE(C_RT_SET_PIVOT);
    SetVec2Fn set_size = (SetVec2Fn)CODE(C_RT_SET_SIZE_DELTA);
    SetLocalPosFn set_lpos = (SetLocalPosFn)CODE(C_TR_SET_LOCAL_POS);
    SetLocalScaleFn set_scale = (SetLocalScaleFn)CODE(C_TR_SET_LOCAL_SCALE);
    SetAsLastSiblingFn set_last = (SetAsLastSiblingFn)CODE(C_TR_SET_AS_LAST_SIBLING);

    set_scale(panel_rt, (UGuiVec3){1.0f, 1.0f, 1.0f}, NULL);
    set_amin(panel_rt, (UGuiVec2){0.5f, 0.5f}, NULL);
    set_amax(panel_rt, (UGuiVec2){0.5f, 0.5f}, NULL);
    set_piv(panel_rt, (UGuiVec2){0.5f, 0.5f}, NULL);
    set_size(panel_rt, (UGuiVec2){w, h}, NULL);
    set_lpos(panel_rt, (UGuiVec3){cx, cy, 0.0f}, NULL);
    set_last(panel_rt, NULL);
}

static void *create_reusable_tex_and_sprite(void *tex_class_src, int32_t w, int32_t h,
                                            const uint8_t *rgba, void **out_tex) {
    if (!uobj_alive(tex_class_src) || !rgba) return NULL;
    ensure_il2cpp_api();
    TexCtorFn tex_ctor = (TexCtorFn)CODE(C_TEX2D_CTOR);
    TexSetIntFn set_filter = (TexSetIntFn)CODE(C_TEX_SET_FILTER);
    TexSetIntFn set_wrap = (TexSetIntFn)CODE(C_TEX_SET_WRAP);
    TexLoadRawFn load_raw = (TexLoadRawFn)CODE(C_TEX2D_LOAD_RAW);
    TexApplyFn apply = (TexApplyFn)CODE(C_TEX2D_APPLY);
    SpriteCreateFullFn spr_create = (SpriteCreateFullFn)CODE(C_SPRITE_CREATE);

    void *tex_klass = s_texture2d_class ? s_texture2d_class : object_class(tex_class_src);
    void *tex = alloc_managed_object(tex_klass);
    if (!tex) {
        tex = runtime_low_alloc(0x50);
        if (!tex) return NULL;
        memcpy(tex, tex_class_src, 8);
    }
    (void)gc_pin(tex, false);
    tex_ctor(tex, w, h, 4 /* RGBA32 */, false, NULL);
    set_filter(tex, 1 /* Bilinear */, NULL);
    set_wrap(tex, 1 /* Clamp */, NULL);
    load_raw(tex, rgba, w * h * 4, NULL);
    apply(tex, false, false /* keep readable for zero-leak reuse */, NULL);
    protect_cached_unity_object(tex);

    void *spr = spr_create(tex, (UGuiRect){0.0f, 0.0f, (float)w, (float)h},
                           (UGuiVec2){0.5f, 0.5f}, 100.0f, 0, 0,
                           (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f}, false, NULL);
    if (spr) {
        (void)gc_pin(spr, false);
        protect_cached_unity_object(spr);
    }
    if (out_tex) *out_tex = tex;
    return spr;
}

/* ---- Judgement Graph Rasterizer (1024 x 336 RGBA32, Unity bottom-up UV) ---- */
#define GRAPH_TEX_W 1024
#define GRAPH_TEX_H 336

static uint8_t *s_graph_rgba = NULL;
static void *s_graph_tex = NULL;
static void *s_graph_sprite = NULL;
static void *s_white_tex = NULL;
static void *s_white_sprite = NULL;

/* Write top-down (x, y) into Unity bottom-up RGBA32 buffer with alpha blending */
static inline void graph_blend_px(uint8_t *buf, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((unsigned)x >= GRAPH_TEX_W || (unsigned)y >= GRAPH_TEX_H || a == 0) return;
    int uy = (GRAPH_TEX_H - 1) - y;
    uint8_t *dst = buf + ((size_t)uy * GRAPH_TEX_W + (size_t)x) * 4;
    if (a == 255) {
        dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = 255;
    } else {
        uint32_t ia = 255u - a;
        dst[0] = (uint8_t)((r * a + dst[0] * ia) / 255u);
        dst[1] = (uint8_t)((g * a + dst[1] * ia) / 255u);
        dst[2] = (uint8_t)((b * a + dst[2] * ia) / 255u);
        dst[3] = 255;
    }
}

static void graph_fill_rect(uint8_t *buf, int x0, int y0, int x1, int y1,
                            uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GRAPH_TEX_W) x1 = GRAPH_TEX_W;
    if (y1 > GRAPH_TEX_H) y1 = GRAPH_TEX_H;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            graph_blend_px(buf, x, y, r, g, b, a);
        }
    }
}

static void graph_draw_aa_disc(uint8_t *buf, float cx, float cy, float radius,
                               uint8_t r, uint8_t g, uint8_t b) {
    int x0 = (int)floorf(cx - radius - 1.0f);
    int x1 = (int)ceilf(cx + radius + 1.0f);
    int y0 = (int)floorf(cy - radius - 1.0f);
    int y1 = (int)ceilf(cy + radius + 1.0f);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            float dx = ((float)x + 0.5f) - cx;
            float dy = ((float)y + 0.5f) - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist <= radius - 0.5f) {
                graph_blend_px(buf, x, y, r, g, b, 255);
            } else if (dist < radius + 0.6f) {
                float t = (radius + 0.6f - dist) / 1.1f;
                if (t > 0.0f) {
                    if (t > 1.0f) t = 1.0f;
                    graph_blend_px(buf, x, y, r, g, b, (uint8_t)(t * 255.0f));
                }
            }
        }
    }
}

/* Signed distance to a rounded rectangle [0..w] x [0..h] with corner radius r */
static inline float graph_rounded_rect_sdf(float px, float py, float w, float h, float r) {
    float qx = fabsf(px - w * 0.5f) - (w * 0.5f - r);
    float qy = fabsf(py - h * 0.5f) - (h * 0.5f - r);
    float ox = (qx > 0.0f) ? qx : 0.0f;
    float oy = (qy > 0.0f) ? qy : 0.0f;
    float outside = sqrtf(ox * ox + oy * oy);
    float inside = (qx > qy) ? qx : qy;
    if (inside > 0.0f) inside = 0.0f;
    return outside + inside - r;
}

static void apply_graph_rounded_mask_and_border(uint8_t *buf, float corner_r) {
    const float fw = (float)GRAPH_TEX_W;
    const float fh = (float)GRAPH_TEX_H;
    for (int y = 0; y < GRAPH_TEX_H; ++y) {
        int uy = (GRAPH_TEX_H - 1) - y;
        uint8_t *row = buf + (size_t)uy * GRAPH_TEX_W * 4;
        float py = (float)y + 0.5f;
        for (int x = 0; x < GRAPH_TEX_W; ++x) {
            if (x >= 3 && x < GRAPH_TEX_W - 3 && y >= 3 && y < GRAPH_TEX_H - 3 &&
                ((x >= 18 && x < GRAPH_TEX_W - 18) || (y >= 18 && y < GRAPH_TEX_H - 18))) {
                continue;
            }
            float d = graph_rounded_rect_sdf((float)x + 0.5f, py, fw, fh, corner_r);
            uint8_t *dst = row + (size_t)x * 4;
            if (d >= 0.5f) {
                dst[0] = 0; dst[1] = 0; dst[2] = 0; dst[3] = 0;
            } else {
                if (d >= -1.5f) {
                    float bt = 1.0f - fabsf(d + 0.65f) / 0.95f;
                    if (bt > 0.0f) {
                        if (bt > 1.0f) bt = 1.0f;
                        uint32_t a = (uint32_t)(bt * 220.0f);
                        uint32_t ia = 255u - a;
                        dst[0] = (uint8_t)((45u * a + dst[0] * ia) / 255u);
                        dst[1] = (uint8_t)((58u * a + dst[1] * ia) / 255u);
                        dst[2] = (uint8_t)((82u * a + dst[2] * ia) / 255u);
                    }
                }
                if (d > -0.5f) {
                    float cov = 0.5f - d;
                    if (cov < 0.0f) cov = 0.0f;
                    if (cov > 1.0f) cov = 1.0f;
                    dst[3] = (uint8_t)(cov * 255.0f);
                } else {
                    dst[3] = 255;
                }
            }
        }
    }
}

static void rasterize_judgement_graph_rgba(uint8_t *buf) {
    const float corner_r = 14.0f;

    /* 1. Solid opaque #10141F fill + header divider */
    graph_fill_rect(buf, 0, 0, GRAPH_TEX_W, GRAPH_TEX_H, 16, 20, 31, 255);
    graph_fill_rect(buf, 14, 37, GRAPH_TEX_W - 14, 38, 45, 58, 82, 190);

    /* 2. Scatter plot recessed area */
    const int px0 = 78;
    const int px1 = GRAPH_TEX_W - 20;
    const int py0 = 46;
    const int py1 = GRAPH_TEX_H - 12;
    const float plot_w = (float)(px1 - px0);
    const float plot_h = (float)(py1 - py0);
    const float cy = (float)py0 + plot_h * 0.5f;
    const float max_y_dev = (plot_h * 0.5f) - 10.0f;

    graph_fill_rect(buf, px0, py0, px1, py1, 11, 14, 22, 255);
    graph_fill_rect(buf, px0, py0, px1, py0 + 1, 34, 44, 62, 200);
    graph_fill_rect(buf, px0, py1 - 1, px1, py1, 34, 44, 62, 200);
    graph_fill_rect(buf, px0, py0, px0 + 1, py1, 34, 44, 62, 200);
    graph_fill_rect(buf, px1 - 1, py0, px1, py1, 34, 44, 62, 200);

    /* 0 ms centerline */
    int icy = (int)(cy + 0.5f);
    graph_fill_rect(buf, px0 + 1, icy, px1 - 1, icy + 2, 82, 100, 128, 255);

    /* +/- 33ms & +/- 66ms dashed reference lines */
    int y_e33 = (int)(cy - (33.0f / 120.0f) * max_y_dev + 0.5f);
    int y_l33 = (int)(cy + (33.0f / 120.0f) * max_y_dev + 0.5f);
    int y_e66 = (int)(cy - (66.0f / 120.0f) * max_y_dev + 0.5f);
    int y_l66 = (int)(cy + (66.0f / 120.0f) * max_y_dev + 0.5f);
    for (int x = px0 + 4; x < px1 - 4; x += 14) {
        int xe = (x + 7 < px1 - 2) ? (x + 7) : (px1 - 2);
        graph_fill_rect(buf, x, y_e33, xe, y_e33 + 1, 50, 64, 86, 200);
        graph_fill_rect(buf, x, y_l33, xe, y_l33 + 1, 50, 64, 86, 200);
        graph_fill_rect(buf, x, y_e66, xe, y_e66 + 1, 38, 49, 68, 170);
        graph_fill_rect(buf, x, y_l66, xe, y_l66 + 1, 38, 49, 68, 170);
    }

    /* 3. Notes: Miss vertical bars first, then anti-aliased hit dots on top */
    size_t note_count = runtime_judgements_get_count();
    uint32_t duration = runtime_judgements_get_duration();
    const JudgementNote *notes = runtime_judgements_get_notes();
    if (notes && note_count > 0) {
        if (duration == 0) duration = 1;

        int miss_y0 = (int)(cy - max_y_dev * 0.80f);
        int miss_y1 = (int)(cy + max_y_dev * 0.80f);

        for (size_t i = 0; i < note_count; ++i) {
            if (notes[i].judge < 6) continue;
            float fx = (float)(px0 + 6) + ((float)notes[i].target_ms / (float)duration) * (plot_w - 12.0f);
            if (fx < (float)(px0 + 3)) fx = (float)(px0 + 3);
            if (fx > (float)(px1 - 4)) fx = (float)(px1 - 4);
            int ix = (int)(fx + 0.5f);
            graph_fill_rect(buf, ix, miss_y0, ix + 2, miss_y1, 238, 38, 54, 215);
        }

        for (size_t i = 0; i < note_count; ++i) {
            const JudgementNote *n = &notes[i];
            if (n->judge >= 6 || n->judge == 0) continue;
            float fx = (float)(px0 + 6) + ((float)n->target_ms / (float)duration) * (plot_w - 12.0f);
            if (fx < (float)(px0 + 4)) fx = (float)(px0 + 4);
            if (fx > (float)(px1 - 5)) fx = (float)(px1 - 5);

            float off = (float)n->offset_ms;
            if (off > 120.0f) off = 120.0f;
            if (off < -120.0f) off = -120.0f;
            float fy = cy + (off / 120.0f) * max_y_dev;

            uint8_t cr = 255, cg = 255, cb = 255;
            if ((n->flags & 1) || n->judge == 1) {
                cr = 255; cg = 255; cb = 255; /* Rainbow Critical: White */
            } else if (n->judge == 2) {
                cr = 255; cg = 215; cb = 0;   /* Gold Critical: Gold */
            } else if (n->judge == 3) {
                cr = 255; cg = 140; cb = 0;   /* Great: Orange */
            } else if (n->judge == 4) {
                cr = 50;  cg = 205; cb = 50;  /* Good: Green */
            } else if (n->judge == 5) {
                cr = 155; cg = 115; cb = 225; /* Bad: Purple */
            }
            graph_draw_aa_disc(buf, fx, fy, 3.1f, cr, cg, cb);
        }
    }

    /* 4. Apply anti-aliased rounded corners (alpha=0 outside arc) & smooth rounded border */
    apply_graph_rounded_mask_and_border(buf, corner_r);
}

/* ---- Cached Per-Scene Native Unity Page 3 Objects ---- */
static void *s_page3_owner = NULL;
static void *s_detail_anim = NULL;
static void *s_anim_in_arr = NULL;

/* Graph UI handles */
static void *s_graph_root_go = NULL;
static void *s_graph_img = NULL;
static void *s_graph_txt_early = NULL;
static void *s_graph_txt_late = NULL;
static void *s_graph_txt_offset = NULL;
static void *s_graph_txt_empty = NULL;
static DualGuestString s_ds_graph_title;
static DualGuestString s_ds_graph_early;
static DualGuestString s_ds_graph_late;
static DualGuestString s_ds_graph_offset;
static DualGuestString s_ds_graph_empty;
static DualGuestString s_ds_graph_axis[7];

/* Leaderboard UI handles */
static void *s_lb_root_go = NULL;
static float s_lb_panel_h = 292.0f;
static void *s_lb_scroll_track_go = NULL;
static void *s_lb_scroll_thumb_rt = NULL;
static void *s_lb_txt_empty = NULL;
static DualGuestString s_ds_lb_hdr[4];
static DualGuestString s_ds_lb_empty;

typedef struct {
    void *txt_rank;
    void *txt_player;
    void *txt_score;
    void *txt_damage;
    void *badge_cm;     /* Cloned ClearMarkBehaviour* (actual in-game badge icons) */
    void *badge_go;
    DualGuestString ds_rank;
    DualGuestString ds_player;
    DualGuestString ds_score;
    DualGuestString ds_damage;
} NativeLbRow;

static NativeLbRow s_lb_rows[LEADERBOARD_VISIBLE_ROWS];
static uint32_t s_lb_rendered_rev = 0;

static void *find_clear_mark_template(void) {
    if (uobj_alive(s_best_play_display)) {
        void *cm = *(void **)((char *)s_best_play_display + 0x38);
        if (uobj_alive(cm)) return cm;
    }
    if (uobj_alive(s_title_parts_main)) {
        void *best_disp = *(void **)((char *)s_title_parts_main + 0xa8);
        if (uobj_alive(best_disp)) {
            void *cm = *(void **)((char *)best_disp + 0x38);
            if (uobj_alive(cm)) return cm;
        }
        void *cur_disp = *(void **)((char *)s_title_parts_main + 0xa0);
        if (uobj_alive(cur_disp)) {
            void *cm = *(void **)((char *)cur_disp + 0x30);
            if (uobj_alive(cm)) return cm;
        }
    }
    return NULL;
}

static bool str_contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) ++i;
        if (i == nlen) return true;
    }
    return false;
}

static void set_native_badge_icon(void *cm_behaviour, const char *badge_str, bool visible) {
    if (!uobj_alive(cm_behaviour)) return;
    GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);
    typedef void (ABI *ClearMarkIntFn)(void *cm, int32_t mark, void *method);
    ClearMarkIntFn display_cm = (ClearMarkIntFn)CODE(C_CLEAR_MARK_DISPLAY);
    ClearMarkIntFn skip_cm_anim = (ClearMarkIntFn)CODE(C_CLEAR_MARK_SKIP_ANIM);

    void *cm_go = get_go(cm_behaviour, NULL);
    if (!uobj_alive(cm_go)) return;
    if (!visible || !badge_str || !*badge_str || !strcmp(badge_str, "-")) {
        set_active(cm_go, false, NULL);
        return;
    }

    int32_t badge_idx = 2; /* Default: Clear */
    if (str_contains_ci(badge_str, "Rainbow") || str_contains_ci(badge_str, "Crit") ||
        !strcmp(badge_str, "0")) {
        badge_idx = 0; /* All Critical */
    } else if (str_contains_ci(badge_str, "Chain") || str_contains_ci(badge_str, "Perfect") ||
               !strcmp(badge_str, "1")) {
        badge_idx = 1; /* Perfect Chain */
    } else if (str_contains_ci(badge_str, "Fail") || !strcmp(badge_str, "3")) {
        badge_idx = 3; /* Failed */
    } else {
        badge_idx = 2; /* Clear */
    }

    void *data_arr = *(void **)((char *)cm_behaviour + 0x20);
    if (!data_arr) {
        set_active(cm_go, false, NULL);
        return;
    }
    int32_t len = *(int32_t *)((char *)data_arr + 0x18);
    if (len <= 0 || len > 8) {
        set_active(cm_go, false, NULL);
        return;
    }
    if (badge_idx >= len) badge_idx = len - 1;

    set_active(cm_go, true, NULL);
    display_cm(cm_behaviour, badge_idx, NULL);
    skip_cm_anim(cm_behaviour, badge_idx, NULL);
}

static void refresh_page3_leaderboard_rows(void) {
    if (!uobj_alive(s_lb_root_go)) return;
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);
    SetColorFn set_color = (SetColorFn)CODE(C_GRAPHIC_SET_COLOR);

    size_t lb_count = runtime_leaderboard_get_count();
    int scroll_val = runtime_leaderboard_get_scroll();
    size_t scroll = (scroll_val > 0) ? (size_t)scroll_val : 0;
    s_lb_rendered_rev = runtime_leaderboard_get_revision();

    if (uobj_alive(s_lb_txt_empty)) {
        if (lb_count == 0) {
            uint32_t cur_mid = 0;
            runtime_judgements_get_song_info(&cur_mid, NULL, NULL);
            if (is_custom_chart_id(cur_mid)) {
                ugui_set_text_str(s_lb_txt_empty, &s_ds_lb_empty,
                                  "Custom Chart (Local Only - TFFScores Disabled)");
            } else {
                ugui_set_text_str(s_lb_txt_empty, &s_ds_lb_empty,
                                  "Fetching leaderboard scores...");
            }
        }
        GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
        void *ego = get_go(s_lb_txt_empty, NULL);
        if (uobj_alive(ego)) set_active(ego, lb_count == 0, NULL);
    }

    for (int i = 0; i < LEADERBOARD_VISIBLE_ROWS; ++i) {
        NativeLbRow *row = &s_lb_rows[i];
        size_t idx = scroll + (size_t)i;
        LeaderboardEntry e;
        if (idx < lb_count && runtime_leaderboard_copy_entry(idx, &e)) {
            UGuiVec4 rank_col = {0.72f, 0.76f, 0.84f, 1.0f};
            if (e.rank == 1) rank_col = (UGuiVec4){1.0f, 0.85f, 0.15f, 1.0f};      /* Gold */
            else if (e.rank == 2) rank_col = (UGuiVec4){0.86f, 0.88f, 0.92f, 1.0f}; /* Silver */
            else if (e.rank == 3) rank_col = (UGuiVec4){0.85f, 0.55f, 0.25f, 1.0f}; /* Bronze */

            char rank_buf[16], player_buf[32], score_buf[24], dmg_buf[24];
            snprintf(rank_buf, sizeof(rank_buf), "#%d", e.rank);

            const char *src_name = e.username[0] ? e.username : "Player";
            if (strlen(src_name) > 16) {
                memcpy(player_buf, src_name, 14);
                player_buf[14] = '.';
                player_buf[15] = '.';
                player_buf[16] = '\0';
            } else {
                snprintf(player_buf, sizeof(player_buf), "%s", src_name);
            }

            snprintf(score_buf, sizeof(score_buf), "%u", e.score);
            snprintf(dmg_buf, sizeof(dmg_buf), "%u", e.damage);

            if (uobj_alive(row->txt_rank)) {
                set_color(row->txt_rank, rank_col, NULL);
                ugui_set_text_str(row->txt_rank, &row->ds_rank, rank_buf);
            }
            if (uobj_alive(row->txt_player)) {
                set_color(row->txt_player, (UGuiVec4){0.96f, 0.97f, 1.0f, 1.0f}, NULL);
                ugui_set_text_str(row->txt_player, &row->ds_player, player_buf);
            }
            if (uobj_alive(row->txt_score)) {
                set_color(row->txt_score, (UGuiVec4){0.74f, 0.91f, 1.0f, 1.0f}, NULL);
                ugui_set_text_str(row->txt_score, &row->ds_score, score_buf);
            }
            if (uobj_alive(row->txt_damage)) {
                set_color(row->txt_damage, (UGuiVec4){1.0f, 0.58f, 0.38f, 1.0f}, NULL);
                ugui_set_text_str(row->txt_damage, &row->ds_damage, dmg_buf);
            }
            set_native_badge_icon(row->badge_cm, e.badge, true);
        } else {
            UGuiVec4 dim = {0.42f, 0.46f, 0.54f, 0.65f};
            const char *dash = (lb_count == 0) ? "" : "-";
            if (uobj_alive(row->txt_rank)) {
                set_color(row->txt_rank, dim, NULL);
                ugui_set_text_str(row->txt_rank, &row->ds_rank, dash);
            }
            if (uobj_alive(row->txt_player)) {
                set_color(row->txt_player, dim, NULL);
                ugui_set_text_str(row->txt_player, &row->ds_player, dash);
            }
            if (uobj_alive(row->txt_score)) {
                set_color(row->txt_score, dim, NULL);
                ugui_set_text_str(row->txt_score, &row->ds_score, dash);
            }
            if (uobj_alive(row->txt_damage)) {
                set_color(row->txt_damage, dim, NULL);
                ugui_set_text_str(row->txt_damage, &row->ds_damage, dash);
            }
            set_native_badge_icon(row->badge_cm, NULL, false);
        }
    }

    /* Update native scrollbar indicator when entries exceed visible window */
    if (uobj_alive(s_lb_scroll_track_go)) {
        bool has_scroll = (lb_count > LEADERBOARD_VISIBLE_ROWS);
        set_active(s_lb_scroll_track_go, has_scroll, NULL);
        if (has_scroll && uobj_alive(s_lb_scroll_thumb_rt)) {
            float track_h = s_lb_panel_h - 123.0f;
            if (track_h < 40.0f) track_h = 40.0f;
            float thumb_h = track_h * ((float)LEADERBOARD_VISIBLE_ROWS / (float)lb_count);
            if (thumb_h < 22.0f) thumb_h = 22.0f;
            float max_sc = (float)(lb_count - LEADERBOARD_VISIBLE_ROWS);
            float t = (max_sc > 0.0f) ? ((float)scroll / max_sc) : 0.0f;
            if (t > 1.0f) t = 1.0f;
            float thumb_y = t * (track_h - thumb_h);
            ugui_place_topleft(s_lb_scroll_thumb_rt, 0.0f, thumb_y, 4.0f, thumb_h);
        }
    }
}

static void build_or_update_page3_ui(void *detail_parts_main) {
    if (!detail_parts_main) return;

    void *tpl_img = NULL, *tpl_txt = NULL, *tex_class_src = NULL;
    if (!extract_ui_templates(detail_parts_main, &tpl_img, &tpl_txt, &tex_class_src)) {
        printf("TFFScores: Could not extract Unity UI templates from detail_parts_main\n");
        return;
    }

    CloneWithParentFn clone_fn = (CloneWithParentFn)CODE(C_OBJECT_CLONE_WITH_PARENT);
    GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
    GetTransformFn get_tr = (GetTransformFn)CODE(C_COMPONENT_GET_TR);
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);
    SetEnabledFn set_enabled = (SetEnabledFn)CODE(C_BEHAVIOUR_SET_ENABLED);
    GetParentFn get_parent = (GetParentFn)CODE(C_TR_GET_PARENT);
    SetObjNameFn set_name = (SetObjNameFn)CODE(C_OBJECT_SET_NAME);
    SetSpriteFn set_sprite = (SetSpriteFn)CODE(C_IMAGE_SET_SPRITE);
    SetImgTypeFn set_img_type = (SetImgTypeFn)CODE(C_IMAGE_SET_TYPE);
    SetPreserveAspectFn set_aspect = (SetPreserveAspectFn)CODE(C_IMAGE_SET_PRESERVE_ASPECT);
    SetColorFn set_color = (SetColorFn)CODE(C_GRAPHIC_SET_COLOR);
    SetRaycastFn set_ray = (SetRaycastFn)CODE(C_GRAPHIC_SET_RAYCAST);
    TexLoadRawFn load_raw = (TexLoadRawFn)CODE(C_TEX2D_LOAD_RAW);
    TexApplyFn apply = (TexApplyFn)CODE(C_TEX2D_APPLY);
    SetVec2Fn set_amin = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MIN);
    SetVec2Fn set_amax = (SetVec2Fn)CODE(C_RT_SET_ANCHOR_MAX);
    SetVec2Fn set_piv  = (SetVec2Fn)CODE(C_RT_SET_PIVOT);
    SetVec2Fn set_apos = (SetVec2Fn)CODE(C_RT_SET_ANCHORED_POS);
    SetLocalPosFn set_lpos = (SetLocalPosFn)CODE(C_TR_SET_LOCAL_POS);
    SetLocalScaleFn set_scale = (SetLocalScaleFn)CODE(C_TR_SET_LOCAL_SCALE);

    /* Ensure reusable RGBA32 buffer and textures exist */
    if (!s_graph_rgba) {
        s_graph_rgba = (uint8_t *)malloc((size_t)GRAPH_TEX_W * GRAPH_TEX_H * 4);
    }
    if (!s_graph_rgba) return;

    rasterize_judgement_graph_rgba(s_graph_rgba);

    if (!unity_native_alive(s_graph_tex) || !unity_native_alive(s_graph_sprite)) {
        s_graph_sprite = create_reusable_tex_and_sprite(tex_class_src, GRAPH_TEX_W, GRAPH_TEX_H,
                                                        s_graph_rgba, &s_graph_tex);
    } else {
        load_raw(s_graph_tex, s_graph_rgba, GRAPH_TEX_W * GRAPH_TEX_H * 4, NULL);
        apply(s_graph_tex, false, false, NULL);
    }

    if (!unity_native_alive(s_white_tex) || !unity_native_alive(s_white_sprite)) {
        uint8_t white4x4[4 * 4 * 4];
        memset(white4x4, 255, sizeof(white4x4));
        s_white_sprite = create_reusable_tex_and_sprite(tex_class_src, 4, 4, white4x4, &s_white_tex);
    }

    bool need_build = (s_page3_owner != detail_parts_main ||
                       !unity_native_alive(s_graph_root_go) || !unity_native_alive(s_lb_root_go));

    if (need_build) {
        s_page3_owner = detail_parts_main;
        s_detail_anim = *(void **)((char *)detail_parts_main + 0x98);
        void *anim_rt = uobj_alive(s_detail_anim) ? get_tr(s_detail_anim, NULL) : NULL;

        /* ---- 1. Build Native Judgement Graph inside Character Stats Window ---- */
        void *status_array = *(void **)((char *)detail_parts_main + 0xa0);
        void *card0 = (status_array && *(uint32_t *)((char *)status_array + 0x18) > 0)
                      ? *(void **)((char *)status_array + 0x20) : NULL;
        void *card0_rt = uobj_alive(card0) ? get_tr(card0, NULL) : NULL;

        /* Walk up from card0_rt to the Character Stats window root right under _animation */
        void *stats_root_rt = card0_rt;
        for (int d = 0; d < 6 && uobj_alive(stats_root_rt); ++d) {
            void *par = get_parent(stats_root_rt, NULL);
            if (!uobj_alive(par) || par == anim_rt) break;
            stats_root_rt = par;
        }

        if (uobj_alive(stats_root_rt)) {
            float g_cx = 0.0f, g_cy = 0.0f, g_fw = 1030.0f, g_fh = 342.0f;
            find_best_frame_in_window(stats_root_rt, NULL,
                                      900.0f, 1150.0f, 280.0f, 420.0f, 1030.0f, 342.0f,
                                      &g_cx, &g_cy, &g_fw, &g_fh);
            float gw, gh, panel_cx, panel_cy;
            if (g_fh > 356.0f) {
                gw = g_fw - 28.0f;
                gh = 332.0f;
                panel_cx = g_cx - 1.0f;
                float frame_top = g_cy + g_fh * 0.5f;
                float panel_top = frame_top - 12.5f;
                panel_cy = panel_top - gh * 0.5f;
            } else {
                gw = g_fw - 12.0f;
                gh = g_fh - 12.0f;
                panel_cx = g_cx;
                panel_cy = g_cy;
            }
            printf("TFFScores: Stats window native frame=(%.1f x %.1f) center=(%.1f, %.1f) -> Graph=(%.1f x %.1f) center=(%.1f, %.1f)\n",
                   g_fw, g_fh, g_cx, g_cy, gw, gh, panel_cx, panel_cy);

            s_graph_img = clone_fn(tpl_img, stats_root_rt, 0);
            if (uobj_alive(s_graph_img)) {
                s_graph_root_go = get_go(s_graph_img, NULL);
                void *grt = get_tr(s_graph_img, NULL);
                if (s_uobj_string_proto && uobj_alive(s_graph_root_go)) {
                    set_name(s_graph_root_go, make_string(s_uobj_string_proto, "TFFPage3_JudgementGraph"), NULL);
                }
                place_centered_panel(grt, panel_cx, panel_cy, gw, gh);
                set_img_type(s_graph_img, 0 /* Simple */, NULL);
                set_aspect(s_graph_img, false, NULL);
                set_color(s_graph_img, (UGuiVec4){1.0f, 1.0f, 1.0f, 1.0f}, NULL);
                set_ray(s_graph_img, false, NULL);
                set_sprite(s_graph_img, s_graph_sprite, NULL);
                set_enabled(s_graph_img, true, NULL);

                /* Header labels: ONLY Title, Early, Late, Offset, Deviation */
                ugui_clone_label(tpl_txt, grt, "TFFPage3_GraphTitle",
                                 16.0f, 6.0f, 340.0f, 26.0f, 18, 3 /* MiddleLeft */, false,
                                 (UGuiVec4){0.60f, 0.82f, 0.98f, 1.0f}, &s_ds_graph_title, "TIMING DEVIATION GRAPH");

                s_graph_txt_early = ugui_clone_label(tpl_txt, grt, "TFFPage3_GraphEarly",
                                                     370.0f, 6.0f, 145.0f, 26.0f, 17, 3, false,
                                                     (UGuiVec4){1.0f, 0.36f, 0.16f, 1.0f}, &s_ds_graph_early, "EARLY: 0");

                s_graph_txt_late = ugui_clone_label(tpl_txt, grt, "TFFPage3_GraphLate",
                                                    515.0f, 6.0f, 145.0f, 26.0f, 17, 3, false,
                                                    (UGuiVec4){0.26f, 0.66f, 1.0f, 1.0f}, &s_ds_graph_late, "LATE: 0");

                s_graph_txt_offset = ugui_clone_label(tpl_txt, grt, "TFFPage3_GraphOffset",
                                                      660.0f, 6.0f, gw - 676.0f, 26.0f, 17, 5 /* MiddleRight */, false,
                                                      (UGuiVec4){0.92f, 0.94f, 0.98f, 1.0f}, &s_ds_graph_offset,
                                                      "OFFSET: +0.0 ms   DEV: 0.0 ms");

                /* Y-axis labels aligned with plot area (py0=46, py1=GRAPH_TEX_H-12) */
                float sy = gh / (float)GRAPH_TEX_H;
                float cy_ui = (46.0f + ((float)GRAPH_TEX_H - 12.0f - 46.0f) * 0.5f) * sy;
                float max_dev_ui = (((float)GRAPH_TEX_H - 12.0f - 46.0f) * 0.5f - 10.0f) * sy;

                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisEarly",
                                 12.0f, 45.0f * sy, 62.0f, 20.0f, 15, 3, false,
                                 (UGuiVec4){1.0f, 0.36f, 0.16f, 1.0f}, &s_ds_graph_axis[0], "EARLY");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisE66",
                                 14.0f, cy_ui - (66.0f / 120.0f) * max_dev_ui - 9.0f, 60.0f, 18.0f, 12, 3, false,
                                 (UGuiVec4){0.56f, 0.64f, 0.75f, 0.90f}, &s_ds_graph_axis[1], "-66ms");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisE33",
                                 14.0f, cy_ui - (33.0f / 120.0f) * max_dev_ui - 9.0f, 60.0f, 18.0f, 12, 3, false,
                                 (UGuiVec4){0.56f, 0.64f, 0.75f, 0.90f}, &s_ds_graph_axis[2], "-33ms");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_Axis0",
                                 16.0f, cy_ui - 10.0f, 58.0f, 20.0f, 13, 3, false,
                                 (UGuiVec4){0.76f, 0.84f, 0.94f, 1.0f}, &s_ds_graph_axis[3], "0 ms");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisL33",
                                 14.0f, cy_ui + (33.0f / 120.0f) * max_dev_ui - 9.0f, 60.0f, 18.0f, 12, 3, false,
                                 (UGuiVec4){0.56f, 0.64f, 0.75f, 0.90f}, &s_ds_graph_axis[4], "+33ms");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisL66",
                                 14.0f, cy_ui + (66.0f / 120.0f) * max_dev_ui - 9.0f, 60.0f, 18.0f, 12, 3, false,
                                 (UGuiVec4){0.56f, 0.64f, 0.75f, 0.90f}, &s_ds_graph_axis[5], "+66ms");
                ugui_clone_label(tpl_txt, grt, "TFFPage3_AxisLate",
                                 12.0f, ((float)GRAPH_TEX_H - 34.0f) * sy, 62.0f, 20.0f, 15, 3, false,
                                 (UGuiVec4){0.26f, 0.66f, 1.0f, 1.0f}, &s_ds_graph_axis[6], "LATE");

                s_graph_txt_empty = ugui_clone_label(tpl_txt, grt, "TFFPage3_GraphEmpty",
                                                     80.0f, cy_ui - 14.0f, gw - 100.0f, 28.0f, 18, 4 /* MiddleCenter */, false,
                                                     (UGuiVec4){0.62f, 0.68f, 0.78f, 0.90f}, &s_ds_graph_empty,
                                                     "No judgement data recorded");
            }
        }

        /* ---- 2. Build Native Leaderboard inside Rhythmia Window (_detailRhythpo) ---- */
        void *rhythpo_display = *(void **)((char *)detail_parts_main + 0xa8);
        void *bonus_list = rhythpo_display ? *(void **)((char *)rhythpo_display + 0x10) : NULL;
        void *bonus_rt = uobj_alive(bonus_list) ? get_tr(bonus_list, NULL) : NULL;
        void *rhythpo_rt = rhythpo_display ? *(void **)((char *)rhythpo_display + 0x18) : NULL;

        if (uobj_alive(rhythpo_rt)) {
            float l_cx = 0.0f, l_cy = 0.0f, l_fw = 632.0f, l_fh = 304.0f;
            find_best_frame_in_window(rhythpo_rt, bonus_rt,
                                      540.0f, 740.0f, 240.0f, 360.0f, 632.0f, 304.0f,
                                      &l_cx, &l_cy, &l_fw, &l_fh);
            float lw = l_fw - 16.0f;
            float lh = l_fh - 12.0f;
            s_lb_panel_h = lh;
            printf("TFFScores: Rhythmia window native frame=(%.1f x %.1f) center=(%.1f, %.1f) -> LB=(%.1f x %.1f)\n",
                   l_fw, l_fh, l_cx, l_cy, lw, lh);

            /* Transparent container parented directly inside _detailRhythpo */
            void *lb_container_img = clone_fn(tpl_img, rhythpo_rt, 0);
            if (uobj_alive(lb_container_img)) {
                s_lb_root_go = get_go(lb_container_img, NULL);
                void *lrt = get_tr(lb_container_img, NULL);
                if (s_uobj_string_proto && uobj_alive(s_lb_root_go)) {
                    set_name(s_lb_root_go, make_string(s_uobj_string_proto, "TFFPage3_Leaderboard"), NULL);
                }
                place_centered_panel(lrt, l_cx, l_cy, lw, lh);
                /* Disable Image rendering on root container so native Rhythmia window texture shows cleanly */
                set_enabled(lb_container_img, false, NULL);

                float x_rank   = 12.0f;
                float x_player = 64.0f;
                float w_player = lw * 0.31f;
                float x_score  = x_player + w_player + 6.0f;
                float x_badge  = x_score + 104.0f;
                float w_badge  = 76.0f;
                float x_damage = x_badge + w_badge + 8.0f;
                float w_damage = lw - x_damage - 14.0f;
                if (w_damage < 80.0f) w_damage = 80.0f;

                /* Start header at y=88px so BADGE/DAMAGE sit cleanly below the green "+Rhythmia" counter.
                 * Omit the "RANK" column header so the left column isn't squished, keeping #1..#5 row numbers. */
                const float hdr_y = 88.0f;
                UGuiVec4 hdr_gold = {0.93f, 0.78f, 0.30f, 1.0f};
                ugui_clone_label(tpl_txt, lrt, "TFFPage3_HdrPlayer", x_player, hdr_y, w_player, 22.0f, 15, 3, false, hdr_gold, &s_ds_lb_hdr[0], "PLAYER");
                ugui_clone_label(tpl_txt, lrt, "TFFPage3_HdrScore",  x_score,  hdr_y, 100.0f,   22.0f, 15, 3, false, hdr_gold, &s_ds_lb_hdr[1], "SCORE");
                ugui_clone_label(tpl_txt, lrt, "TFFPage3_HdrBadge",  x_badge,  hdr_y, w_badge,  22.0f, 15, 4, false, hdr_gold, &s_ds_lb_hdr[2], "BADGE");
                ugui_clone_label(tpl_txt, lrt, "TFFPage3_HdrDamage", x_damage, hdr_y, w_damage, 22.0f, 15, 3, false, hdr_gold, &s_ds_lb_hdr[3], "DAMAGE");

                /* Thin horizontal header rule */
                if (uobj_alive(s_white_sprite)) {
                    void *div_img = clone_fn(tpl_img, lrt, 0);
                    if (uobj_alive(div_img)) {
                        void *dgo = get_go(div_img, NULL);
                        void *drt = get_tr(div_img, NULL);
                        ugui_place_topleft(drt, 10.0f, 112.0f, lw - 20.0f, 1.5f);
                        set_img_type(div_img, 0, NULL);
                        set_aspect(div_img, false, NULL);
                        set_sprite(div_img, s_white_sprite, NULL);
                        set_color(div_img, (UGuiVec4){0.32f, 0.42f, 0.56f, 0.75f}, NULL);
                        set_ray(div_img, false, NULL);
                        set_enabled(div_img, true, NULL);
                        if (uobj_alive(dgo)) set_active(dgo, true, NULL);
                    }
                }

                void *cm_template = find_clear_mark_template();
                const float rows_y0 = 115.0f;
                const float row_h = (lh - rows_y0 - 6.0f) / (float)LEADERBOARD_VISIBLE_ROWS;

                for (int i = 0; i < LEADERBOARD_VISIBLE_ROWS; ++i) {
                    NativeLbRow *row = &s_lb_rows[i];
                    float ry = rows_y0 + (float)i * row_h;

                    /* Subtle alternating row band on odd rows */
                    if ((i % 2) == 1 && uobj_alive(s_white_sprite)) {
                        void *band = clone_fn(tpl_img, lrt, 0);
                        if (uobj_alive(band)) {
                            void *bgo = get_go(band, NULL);
                            void *brt = get_tr(band, NULL);
                            ugui_place_topleft(brt, 8.0f, ry + 1.0f, lw - 20.0f, row_h - 2.0f);
                            set_img_type(band, 0, NULL);
                            set_aspect(band, false, NULL);
                            set_sprite(band, s_white_sprite, NULL);
                            set_color(band, (UGuiVec4){0.14f, 0.19f, 0.28f, 0.38f}, NULL);
                            set_ray(band, false, NULL);
                            set_enabled(band, true, NULL);
                            if (uobj_alive(bgo)) set_active(bgo, true, NULL);
                        }
                    }

                    row->txt_rank = ugui_clone_label(tpl_txt, lrt, "TFFPage3_RowRank",
                                                     x_rank, ry, 50.0f, row_h, 17, 3, false,
                                                     (UGuiVec4){0.9f, 0.9f, 0.95f, 1.0f}, &row->ds_rank, "");
                    row->txt_player = ugui_clone_label(tpl_txt, lrt, "TFFPage3_RowPlayer",
                                                       x_player, ry, w_player, row_h, 17, 3, true,
                                                       (UGuiVec4){0.96f, 0.97f, 1.0f, 1.0f}, &row->ds_player, "");
                    row->txt_score = ugui_clone_label(tpl_txt, lrt, "TFFPage3_RowScore",
                                                      x_score, ry, 102.0f, row_h, 17, 3, false,
                                                      (UGuiVec4){0.74f, 0.91f, 1.0f, 1.0f}, &row->ds_score, "");
                    row->txt_damage = ugui_clone_label(tpl_txt, lrt, "TFFPage3_RowDamage",
                                                       x_damage, ry, w_damage, row_h, 17, 3, false,
                                                       (UGuiVec4){1.0f, 0.58f, 0.38f, 1.0f}, &row->ds_damage, "");

                    /* Clone actual in-game ClearMarkBehaviour badge icon (scaled to 0.135 so it fits cleanly inside row/column) */
                    row->badge_cm = NULL;
                    row->badge_go = NULL;
                    if (uobj_alive(cm_template)) {
                        void *cm_clone = clone_fn(cm_template, lrt, 0);
                        if (uobj_alive(cm_clone)) {
                            row->badge_cm = cm_clone;
                            row->badge_go = get_go(cm_clone, NULL);
                            void *cm_rt = get_tr(cm_clone, NULL);
                            if (uobj_alive(cm_rt)) {
                                set_lpos(cm_rt, (UGuiVec3){0.0f, 0.0f, 0.0f}, NULL);
                                set_scale(cm_rt, (UGuiVec3){0.135f, 0.135f, 1.0f}, NULL);
                                set_amin(cm_rt, (UGuiVec2){0.0f, 1.0f}, NULL);
                                set_amax(cm_rt, (UGuiVec2){0.0f, 1.0f}, NULL);
                                set_piv(cm_rt, (UGuiVec2){0.5f, 0.5f}, NULL);
                                set_apos(cm_rt, (UGuiVec2){x_badge + w_badge * 0.5f, -(ry + row_h * 0.5f)}, NULL);
                            }
                            set_native_badge_icon(cm_clone, NULL, false);
                        }
                    }
                }

                /* Empty / loading text */
                s_lb_txt_empty = ugui_clone_label(tpl_txt, lrt, "TFFPage3_LbEmpty",
                                                  20.0f, lh * 0.58f, lw - 40.0f, 28.0f, 17, 4, false,
                                                  (UGuiVec4){0.65f, 0.72f, 0.82f, 0.90f}, &s_ds_lb_empty,
                                                  "Fetching leaderboard scores...");

                /* Native scrollbar track & thumb at right inner edge */
                s_lb_scroll_track_go = NULL;
                s_lb_scroll_thumb_rt = NULL;
                if (uobj_alive(s_white_sprite)) {
                    void *trk = clone_fn(tpl_img, lrt, 0);
                    if (uobj_alive(trk)) {
                        s_lb_scroll_track_go = get_go(trk, NULL);
                        void *trt = get_tr(trk, NULL);
                        ugui_place_topleft(trt, lw - 8.0f, 117.0f, 4.0f, lh - 123.0f);
                        set_img_type(trk, 0, NULL);
                        set_aspect(trk, false, NULL);
                        set_sprite(trk, s_white_sprite, NULL);
                        set_color(trk, (UGuiVec4){0.18f, 0.23f, 0.32f, 0.55f}, NULL);
                        set_ray(trk, false, NULL);
                        set_enabled(trk, true, NULL);

                        void *thb = clone_fn(tpl_img, trt, 0);
                        if (uobj_alive(thb)) {
                            void *thgo = get_go(thb, NULL);
                            s_lb_scroll_thumb_rt = get_tr(thb, NULL);
                            ugui_place_topleft(s_lb_scroll_thumb_rt, 0.0f, 0.0f, 4.0f, 36.0f);
                            set_img_type(thb, 0, NULL);
                            set_aspect(thb, false, NULL);
                            set_sprite(thb, s_white_sprite, NULL);
                            set_color(thb, (UGuiVec4){0.60f, 0.76f, 0.95f, 0.90f}, NULL);
                            set_ray(thb, false, NULL);
                            set_enabled(thb, true, NULL);
                            if (uobj_alive(thgo)) set_active(thgo, true, NULL);
                        }
                    }
                }
            }
        }
    }

    /* Always force UI.Image to refresh vertices/material after updating s_graph_tex in place */
    if (unity_native_alive(s_graph_img) && unity_native_alive(s_graph_sprite)) {
        set_sprite(s_graph_img, NULL, NULL);
        set_sprite(s_graph_img, s_graph_sprite, NULL);
    }

    /* Update live Judgement Graph header metrics */
    float mean_offset = 0.0f, std_dev = 0.0f;
    int early_count = 0, late_count = 0;
    runtime_judgements_get_summary(&mean_offset, &std_dev, &early_count, &late_count);
    size_t note_count = runtime_judgements_get_count();

    char early_buf[32], late_buf[32], off_buf[64];
    snprintf(early_buf, sizeof(early_buf), "EARLY: %d", early_count);
    snprintf(late_buf, sizeof(late_buf), "LATE: %d", late_count);
    snprintf(off_buf, sizeof(off_buf), "OFFSET: %+.1f ms   DEV: %.1f ms", mean_offset, std_dev);

    ugui_set_text_str(s_graph_txt_early, &s_ds_graph_early, early_buf);
    ugui_set_text_str(s_graph_txt_late, &s_ds_graph_late, late_buf);
    ugui_set_text_str(s_graph_txt_offset, &s_ds_graph_offset, off_buf);

    if (uobj_alive(s_graph_txt_empty)) {
        void *ego = get_go(s_graph_txt_empty, NULL);
        if (uobj_alive(ego)) set_active(ego, note_count == 0, NULL);
    }

    /* Populate Leaderboard rows & activate both native Page 3 panels */
    if (uobj_alive(s_graph_root_go)) set_active(s_graph_root_go, true, NULL);
    if (uobj_alive(s_lb_root_go)) set_active(s_lb_root_go, true, NULL);
    refresh_page3_leaderboard_rows();
}

static void hide_page3_custom_ui(void) {
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);
    if (uobj_alive(s_graph_root_go)) set_active(s_graph_root_go, false, NULL);
    if (uobj_alive(s_lb_root_go)) set_active(s_lb_root_go, false, NULL);
}

/* Replay native ContentDetail_In slide-in animation on detail_parts_main->_animation */
static void play_page3_slide_in_anim(void *detail_parts_main) {
    if (!detail_parts_main) return;
    void *anim = *(void **)((char *)detail_parts_main + 0x98);
    if (!uobj_alive(anim)) return;
    s_detail_anim = anim;

    ensure_il2cpp_api();
    void *klass = object_class(detail_parts_main);
    if (!klass) return;
    if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
    void *static_fields = (void *)load64((const char *)klass + 0xb8);
    if (!static_fields) return;

    if (!s_anim_in_arr) {
        s_anim_in_arr = runtime_low_alloc(0x50);
    }
    if (!s_anim_in_arr) return;

    put32((char *)s_anim_in_arr + 0x18, 1); /* PlayParameter[1] Length = 1 */
    memcpy((char *)s_anim_in_arr + 0x20, static_fields, 0x28); /* 40-byte ContentDetail_In PlayParameter */

    GummiPlayFn play_fn = (GummiPlayFn)CODE(C_GUMMI_ANIM_PLAY);
    play_fn(anim, s_anim_in_arr, NULL);
}

static void *s_swapped_hide_btns[8];
static void *s_orig_hide_se_labels[8];
static int s_swapped_hide_count = 0;

static void set_page2_triangle_open_se(void *detail_parts_main, bool use_open_se) {
    if (!use_open_se) {
        for (int i = 0; i < s_swapped_hide_count; ++i) {
            void *btn = s_swapped_hide_btns[i];
            if (btn && (uintptr_t)btn > 0x10000ULL && (uintptr_t)btn < 0x00007fffffffffffULL) {
                put64((char *)btn + 0x28, (uint64_t)s_orig_hide_se_labels[i]);
            }
        }
        s_swapped_hide_count = 0;
        return;
    }
    if (!detail_parts_main) return;
    set_page2_triangle_open_se(NULL, false);

    void *ui_btns = *(void **)((char *)detail_parts_main + 0x90);
    if (!ui_btns) return;
    uint32_t len = load32((char *)ui_btns + 0x18);
    if (len == 0 || len > 32) return;

    void *open_se_label = NULL;
    for (uint32_t i = 0; i < len; ++i) {
        void *btn = *(void **)((char *)ui_btns + 0x20 + i * sizeof(void *));
        if (!btn) continue;
        int32_t val = (int32_t)load32((char *)btn + 0x20);
        if (val == 1) { /* DisplayDetail */
            void *lbl = (void *)load64((char *)btn + 0x28);
            if (lbl) {
                open_se_label = lbl;
                break;
            }
        }
    }
    if (!open_se_label) return;

    for (uint32_t i = 0; i < len && s_swapped_hide_count < 8; ++i) {
        void *btn = *(void **)((char *)ui_btns + 0x20 + i * sizeof(void *));
        if (!btn) continue;
        int32_t val = (int32_t)load32((char *)btn + 0x20);
        if (val == 2) { /* HideDetail */
            s_swapped_hide_btns[s_swapped_hide_count] = btn;
            s_orig_hide_se_labels[s_swapped_hide_count] = (void *)load64((char *)btn + 0x28);
            put64((char *)btn + 0x28, (uint64_t)open_se_label);
            ++s_swapped_hide_count;
        }
    }
}

static ABI int hook_result_detail_init(TffHookRegs *r) {
    void *detail_parts_main = (void *)r->rdi;
    s_results_page = 1;
    runtime_judgements_ensure_stage_finished(0);
    set_page2_triangle_open_se(NULL, false);
    if (detail_parts_main != s_page3_owner) {
        s_page3_owner = NULL;
        s_graph_root_go = NULL;
        s_lb_root_go = NULL;
        s_detail_anim = NULL;
    } else {
        hide_page3_custom_ui();
    }
    return 0;
}

static ABI int hook_move_next_scene(TffHookRegs *r) {
    (void)r;
    s_results_page = 1;
    set_page2_triangle_open_se(NULL, false);
    return 0;
}

static ABI int hook_gummi_anim_update(TffHookRegs *r) {
    if (s_results_page == 3 && s_detail_anim && (void *)r->rdi == s_detail_anim) {
        if (runtime_leaderboard_get_revision() != s_lb_rendered_rev) {
            refresh_page3_leaderboard_rows();
        }
    }
    return 0;
}

static void hide_vanilla_results_ui(void *detail_parts_main) {
    if (!detail_parts_main) return;
    GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);

    void *status_array = *(void **)((char *)detail_parts_main + 0xa0);
    if (status_array) {
        uint32_t card_count = *(uint32_t *)((char *)status_array + 0x18);
        for (uint32_t i = 0; i < card_count && i < 4; ++i) {
            void *card = *(void **)((char *)status_array + 0x20 + i * sizeof(void *));
            if (uobj_alive(card)) {
                void *card_go = get_go(card, NULL);
                if (uobj_alive(card_go)) set_active(card_go, false, NULL);
            }
        }
    }

    void *rhythpo_display = *(void **)((char *)detail_parts_main + 0xa8);
    if (rhythpo_display) {
        void *bonus_list = *(void **)((char *)rhythpo_display + 0x10);
        if (uobj_alive(bonus_list)) {
            void *b_go = get_go(bonus_list, NULL);
            if (uobj_alive(b_go)) set_active(b_go, false, NULL);
        }
    }
}

static void restore_vanilla_results_ui(void *detail_parts_main) {
    if (!detail_parts_main) return;
    GetGOPtrFn get_go = (GetGOPtrFn)CODE(C_COMPONENT_GET_GO);
    SetActiveFn set_active = (SetActiveFn)CODE(C_GO_SET_ACTIVE);

    void *status_array = *(void **)((char *)detail_parts_main + 0xa0);
    if (status_array) {
        uint32_t card_count = *(uint32_t *)((char *)status_array + 0x18);
        for (uint32_t i = 0; i < card_count && i < 4; ++i) {
            void *card = *(void **)((char *)status_array + 0x20 + i * sizeof(void *));
            if (uobj_alive(card)) {
                void *card_go = get_go(card, NULL);
                if (uobj_alive(card_go)) set_active(card_go, true, NULL);
            }
        }
    }

    void *rhythpo_display = *(void **)((char *)detail_parts_main + 0xa8);
    if (rhythpo_display) {
        void *bonus_list = *(void **)((char *)rhythpo_display + 0x10);
        if (uobj_alive(bonus_list)) {
            void *b_go = get_go(bonus_list, NULL);
            if (uobj_alive(b_go)) set_active(b_go, true, NULL);
        }
    }
}

static ABI int hook_result_detail_action(TffHookRegs *r) {
    void *detail_parts_main = (void *)r->rdi;
    void *action = (void *)r->rsi;
    if (!action) return 0;

    int action_type = *(int32_t *)((char *)action + 0x20);
    printf("TFFScores: hook_result_detail_action: action_type=%d, current_page=%d\n", action_type, s_results_page);

    typedef void (ABI *EndActionFn)(void *action, void *method);
    EndActionFn end_action = (EndActionFn)CODE(C_UIOBJECT_END_ACTION);

    if (action_type == 1) { /* DisplayDetail (Page 1 -> Page 2) */
        s_results_page = 2;
        set_page2_triangle_open_se(detail_parts_main, true);
        hide_page3_custom_ui();
        restore_vanilla_results_ui(detail_parts_main);
        printf("TFFScores: Results detail opened -> Page 2\n");
        return 0; /* Let vanilla play ContentDetail_In for Page 2 */
    } else if (action_type == 2) { /* HideDetail (Triangle button) */
        if (s_results_page == 2) {
            s_results_page = 3;
            set_page2_triangle_open_se(detail_parts_main, false);
            printf("TFFScores: Results detail Triangle clicked -> Page 3 (Native UI, style: %s)\n", s_current_play_style);

            uint32_t music_id = 0;
            int difficulty = 0;
            runtime_judgements_get_song_info(&music_id, &difficulty, NULL);
            runtime_leaderboard_reset_scroll();
            runtime_leaderboard_fetch(music_id, difficulty, s_current_play_style);

            hide_vanilla_results_ui(detail_parts_main);
            build_or_update_page3_ui(detail_parts_main);
            play_page3_slide_in_anim(detail_parts_main);

            end_action(action, NULL);
            r->rax = 0;
            return 1; /* Consumed: Page 3 slides in natively */
        } else if (s_results_page == 3) {
            s_results_page = 1;
            set_page2_triangle_open_se(detail_parts_main, false);
            printf("TFFScores: Results detail Triangle clicked on Page 3 -> Sliding out to Page 1\n");
            return 0; /* Vanilla HideDetail plays ContentDetail_Out */
        }
    } else if (action_type == 4 || action_type == 5) { /* CrossUp / CrossDown (D-Pad) */
        if (s_results_page == 3) {
            if (action_type == 4) runtime_leaderboard_scroll_up();
            else runtime_leaderboard_scroll_down();
            refresh_page3_leaderboard_rows();
            end_action(action, NULL);
            r->rax = 0;
            return 1; /* Consumed */
        }
    } else if (action_type == 0) { /* Next button pressed to exit results */
        s_results_page = 1;
        set_page2_triangle_open_se(detail_parts_main, false);
    }
    return 0;
}

static unsigned char *tff_emit(unsigned char *p, const void *bytes, size_t size) {
    memcpy(p, bytes, size);
    return p + size;
}

static unsigned char *tff_rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[] = {
        (unsigned char)(0x48 | (reg >= 8 ? 4 : 0)),
        (unsigned char)(store ? 0x89 : 0x8b),
        (unsigned char)(0x44 | (reg & 7) << 3),
        0x24,
        disp
    };
    return tff_emit(p, bytes, sizeof bytes);
}

static size_t tff_stub_size(void) { return 148; }

static unsigned char *emit_tff_hook_stub(unsigned char *p, TffHookFn fn_ptr, unsigned char *function, unsigned moved) {
    static const int args[6] = {7, 6, 2, 1, 8, 9}; /* rdi rsi rdx rcx r8 r9 */
    const int32_t frame = 72;
    p = tff_emit(p, (const unsigned char[]){0x48, 0x81, 0xec}, 3);
    p = tff_emit(p, &frame, 4); /* sub rsp, frame */
    for (int i = 0; i < 6; ++i) p = tff_rsp_slot(p, 1, args[i], (uint8_t)(8 * i));
    p = tff_emit(p, (const unsigned char[]){0x48, 0x89, 0xe7}, 3); /* mov rdi, rsp */
    const uint64_t fn = (uint64_t)(uintptr_t)fn_ptr;
    p = tff_emit(p, (const unsigned char[]){0x48, 0xb8}, 2);
    p = tff_emit(p, &fn, 8); /* mov rax, fn */
    p = tff_emit(p, (const unsigned char[]){0xff, 0xd0, 0x85, 0xc0}, 4); /* call rax; test eax, eax */
    unsigned char *jnz = p;
    p = tff_emit(p, (const unsigned char[]){0x75, 0}, 2); /* jnz ret */
    for (int i = 0; i < 6; ++i) p = tff_rsp_slot(p, 0, args[i], (uint8_t)(8 * i));
    p = tff_emit(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3);
    p = tff_emit(p, &frame, 4); /* add rsp, frame */
    p = tff_emit(p, function, moved);
    const uint64_t back = (uint64_t)(uintptr_t)(function + moved);
    p = tff_emit(p, (const unsigned char[]){0xff, 0x25, 0, 0, 0, 0}, 6);
    p = tff_emit(p, &back, 8); /* jmp [rip]: back */
    jnz[1] = (unsigned char)(p - (jnz + 2));
    p = tff_rsp_slot(p, 0, 0, 48);
    p = tff_rsp_slot(p, 0, 2, 56);
    p = tff_emit(p, (const unsigned char[]){0x66, 0x0f, 0x6e, 0xc0}, 4); /* movd xmm0, eax */
    p = tff_emit(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3);
    p = tff_emit(p, &frame, 4); /* add rsp, frame */
    return tff_emit(p, (const unsigned char[]){0xc3}, 1); /* ret */
}

int runtime_tffscores_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    static const struct { int site; TffHookFn fn; } tff_hooks[TFF_HOOK_SITES] = {
        {TFF_DATE_TO_TICKS, leap_day_guard},
        {TFF_GREGORIAN_DATE_TO_TICKS, leap_day_guard},
        {TFF_COUNT_UP_ON_END, on_song_result_countup_end},
        {TFF_RETRY_WINDOW_OPEN, on_retry_window_open},
        {TFF_PLAY_SETTING_CONFIG, hook_play_setting_config},
        {TFF_INPUT_DAMAGE, hook_input_damage},
        {TFF_BEST_PLAY_SETUP_6, hook_best_play_setup},
        {TFF_BEST_PLAY_SETUP_2, hook_best_play_setup},
        {TFF_RESULT_DETAIL_INIT, hook_result_detail_init},
        {TFF_RESULT_DETAIL_ACTION, hook_result_detail_action},
        {TFF_MOVE_NEXT_SCENE, hook_move_next_scene},
        {TFF_GUMMI_ANIM_UPDATE, hook_gummi_anim_update},
    };
    unsigned char *module = image + module_base;
    if (runtime_game_code_find(tff_sites, TFF_SITES, s_tff_code, "TFFScores")) {
        printf("TFFScores: hooks not installed for game %s\n", runtime_game_version());
        return -1;
    }
    s_tff_module = module;
    size_t need = 0;
    for (size_t i = 0; i < TFF_HOOK_SITES; ++i) {
        const unsigned moved = s_tff_code[tff_hooks[i].site].moved;
        if (moved < 5 || moved > 16) return -1;
        need += tff_stub_size();
        const int64_t rel = (int64_t)(stubs + need - (module + s_tff_code[tff_hooks[i].site].offset + 5));
        if (rel != (int32_t)rel || need > size) {
            printf("TFFScores: no room for hook stubs\n");
            return -1;
        }
    }
    unsigned char *p = stubs;
    for (size_t i = 0; i < TFF_HOOK_SITES; ++i) {
        const GameCode *gc = &s_tff_code[tff_hooks[i].site];
        unsigned char *function = module + gc->offset;
        unsigned char *stub = p;
        p = emit_tff_hook_stub(p, tff_hooks[i].fn, function, gc->moved);
        const int32_t rel32 = (int32_t)(stub - (function + 5));
        function[0] = 0xe9;
        memcpy(function + 1, &rel32, 4);
        memset(function + 5, 0x90, gc->moved - 5u);
    }
    runtime_judgements_add_stage_begin_callback(on_judgements_stage_begin);
    runtime_judgements_add_score_pb_callback(on_judgements_score_pb);
    runtime_judgements_add_title_init_callback(on_judgements_title_init);
    printf("TFFScores: installed %d hooks and %d callees (Leap Day guard, result/retry capture & Page 3 UI, game %s)\n",
           TFF_HOOK_SITES, TFF_SITES - TFF_HOOK_SITES, runtime_game_version());
    return 0;
}
