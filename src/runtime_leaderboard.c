#define _GNU_SOURCE
#include "runtime_leaderboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif

extern int runtime_tffscores_is_logged_in(void);
extern const char *runtime_tffscores_username(void);

#include <ctype.h>

static pthread_mutex_t s_lb_lock = PTHREAD_MUTEX_INITIALIZER;
static LeaderboardEntry s_entries[MAX_LEADERBOARD_ENTRIES];
static size_t s_entry_count = 0;
static int s_scroll_index = 0;
static LeaderboardStatus s_status = LB_STATUS_IDLE;
static uint32_t s_lb_revision = 1;
static uint32_t s_last_music_id = 0;
static int s_last_difficulty = -1;
static char s_last_style[32] = "";

void runtime_leaderboard_init(void) {
    pthread_mutex_lock(&s_lb_lock);
    s_entry_count = 0;
    s_scroll_index = 0;
    s_status = LB_STATUS_IDLE;
    s_last_music_id = 0;
    s_last_difficulty = -1;
    s_last_style[0] = '\0';
    __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s_lb_lock);
}

static uint32_t leaderboard_api_music_id(uint32_t music_id) {
    /* Vanilla songs (160000001+) and verified custom charts (160090000..160099999)
     * drop the Game.MusicID PrefixNumber offset (16 * 10,000,000) for TFFScores. */
    if (music_id >= 160000000u && music_id < 161000000u) {
        return music_id - 160000000u;
    }
    return music_id;
}

static bool leaderboard_is_local_only_id(uint32_t music_id) {
    return music_id >= 169000000u && music_id < 170000000u;
}

/* Locate `"key":` outside string values within a single flat JSON object buffer. */
static const char *find_json_value_start(const char *obj, const char *key) {
    size_t klen = strlen(key);
    const char *p = obj;
    bool in_str = false;
    while (*p) {
        if (*p == '\\' && in_str && p[1]) {
            p += 2;
            continue;
        }
        if (*p == '"') {
            if (!in_str) {
                if (strncmp(p, key, klen) == 0) {
                    const char *q = p + klen;
                    while (*q && isspace((unsigned char)*q)) ++q;
                    if (*q == ':') {
                        ++q;
                        while (*q && isspace((unsigned char)*q)) ++q;
                        return q;
                    }
                }
                in_str = true;
            } else {
                in_str = false;
            }
        }
        ++p;
    }
    return NULL;
}

static void extract_json_string(const char *obj, const char *key, char *out, size_t out_sz) {
    out[0] = '\0';
    const char *v = find_json_value_start(obj, key);
    if (!v || *v != '"') return;
    ++v;
    size_t i = 0;
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) {
            ++v;
        }
        if (i + 1 < out_sz) {
            out[i++] = *v;
        }
        ++v;
    }
    out[i] = '\0';
}

static uint32_t extract_json_uint(const char *obj, const char *key) {
    const char *v = find_json_value_start(obj, key);
    if (!v || !isdigit((unsigned char)*v)) return 0;
    return (uint32_t)strtoul(v, NULL, 10);
}

/* Parse a JSON array of leaderboard objects into s_entries; returns false if not a JSON array. */
static bool parse_leaderboard_json(const char *json, uint32_t log_song_id, uint32_t api_id, int diff, const char *style_str) {
    if (!json) return false;

    const char *p = json;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '[') return false;
    ++p;

    LeaderboardEntry temp[MAX_LEADERBOARD_ENTRIES];
    size_t count = 0;

    while (*p && *p != ']' && count < MAX_LEADERBOARD_ENTRIES) {
        while (*p && *p != '{' && *p != ']') ++p;
        if (*p != '{') break;

        const char *obj_start = p;
        int depth = 0;
        bool in_str = false;
        while (*p) {
            if (*p == '\\' && in_str && p[1]) {
                p += 2;
                continue;
            }
            if (*p == '"') {
                in_str = !in_str;
            } else if (!in_str) {
                if (*p == '{') ++depth;
                else if (*p == '}' && --depth == 0) break;
            }
            ++p;
        }
        if (*p != '}') break;

        char obj[1024];
        size_t len = (size_t)(p - obj_start + 1);
        if (len >= sizeof(obj)) len = sizeof(obj) - 1;
        memcpy(obj, obj_start, len);
        obj[len] = 0;

        LeaderboardEntry *e = &temp[count];
        memset(e, 0, sizeof(*e));

        e->rank = (int)extract_json_uint(obj, "\"rank\"");
        if (e->rank <= 0) e->rank = (int)(count + 1);

        extract_json_string(obj, "\"username\"", e->username, sizeof(e->username));
        if (!e->username[0]) extract_json_string(obj, "\"player_name\"", e->username, sizeof(e->username));
        if (!e->username[0]) extract_json_string(obj, "\"name\"", e->username, sizeof(e->username));
        if (!e->username[0]) extract_json_string(obj, "\"user\"", e->username, sizeof(e->username));

        e->score = extract_json_uint(obj, "\"score\"");
        extract_json_string(obj, "\"badge\"", e->badge, sizeof(e->badge));
        if (!e->badge[0]) extract_json_string(obj, "\"clear_mark\"", e->badge, sizeof(e->badge));
        if (!e->badge[0]) snprintf(e->badge, sizeof(e->badge), "Clear");

        e->damage = extract_json_uint(obj, "\"damage\"");
        if (e->damage == 0) e->damage = extract_json_uint(obj, "\"total_damage\"");

        if (e->username[0] && e->score > 0) {
            ++count;
        }
        ++p;
    }

    if (count > 0) {
        memcpy(s_entries, temp, count * sizeof(LeaderboardEntry));
        s_entry_count = count;
        s_scroll_index = 0;
        s_status = LB_STATUS_READY;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        printf("Leaderboard: parsed %zu live entries from TFFScores for song=%u (api=%u, diff=%d, style=%s)\n",
               count, log_song_id, api_id, diff, style_str);
    } else {
        s_entry_count = 0;
        s_scroll_index = 0;
        s_status = LB_STATUS_EMPTY;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        printf("Leaderboard: 0 live entries on TFFScores for song=%u (api=%u, diff=%d, style=%s)\n",
               log_song_id, api_id, diff, style_str);
    }
    return true;
}

typedef struct {
    uint32_t music_id;
    int difficulty;
    char style[32];
} LbFetchArg;

#ifdef _WIN32
static void parse_tffscores_host_port(bool *out_https, wchar_t *whost, size_t whost_len, int *out_port) {
    const char *env_url = getenv("THEATRHYTHM_TFFSCORES_URL");
    const char *url = (env_url && *env_url) ? env_url : "https://tffscores.com";
    bool is_https = (strncmp(url, "http://", 7) != 0);
    const char *host_start = url;
    if (strncmp(url, "https://", 8) == 0) {
        host_start = url + 8;
        is_https = true;
    } else if (strncmp(url, "http://", 7) == 0) {
        host_start = url + 7;
        is_https = false;
    }
    char host[256];
    snprintf(host, sizeof(host), "%s", host_start);
    char *slash = strchr(host, '/');
    if (slash) *slash = '\0';
    int port = is_https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        int parsed = atoi(colon + 1);
        if (parsed > 0 && parsed <= 65535) port = parsed;
    }
    if (!host[0]) snprintf(host, sizeof(host), "tffscores.com");
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, (int)whost_len);
    *out_https = is_https;
    *out_port = port;
}
#endif

static void *lb_async_fetch_thread(void *arg) {
    LbFetchArg *a = (LbFetchArg *)arg;
    uint32_t music_id = a->music_id;
    uint32_t api_music_id = leaderboard_api_music_id(music_id);
    int difficulty = a->difficulty;
    char style_str[32];
    snprintf(style_str, sizeof(style_str), "%s", a->style[0] ? a->style : "standard");
    free(a);

    bool fetched_ok = false;

#ifdef _WIN32
    bool is_https = true;
    wchar_t whost[256] = L"tffscores.com";
    int port = INTERNET_DEFAULT_HTTPS_PORT;
    parse_tffscores_host_port(&is_https, whost, sizeof(whost) / sizeof(whost[0]), &port);

    HINTERNET hSession = WinHttpOpen(L"TheatrhythmRecomp/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (hSession) {
        HINTERNET hConnect = WinHttpConnect(hSession, whost, (INTERNET_PORT)port, 0);
        if (hConnect) {
            wchar_t path[256];
            swprintf(path, 256, L"/api/game/leaderboard?music_id=%u&difficulty=%d&slot=%d&style=%hs",
                     api_music_id, difficulty, difficulty, style_str);
            DWORD req_flags = is_https ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
                                                   WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   req_flags);
            if (hRequest) {
                if (WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0) &&
                    WinHttpReceiveResponse(hRequest, NULL)) {
                    DWORD status_code = 0, sc_size = sizeof(status_code);
                    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &sc_size, WINHTTP_NO_HEADER_INDEX);

                    if (status_code >= 200 && status_code < 300) {
                        size_t cap = 16384;
                        size_t total = 0;
                        char *resp = (char *)malloc(cap);
                        if (resp) {
                            bool read_err = false;
                            for (;;) {
                                DWORD avail = 0;
                                if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
                                    read_err = true;
                                    break;
                                }
                                if (avail == 0) break;
                                if (total + avail + 1 > cap) {
                                    size_t next_cap = cap * 2;
                                    while (next_cap < total + avail + 1) next_cap *= 2;
                                    if (next_cap > 512u * 1024u) {
                                        read_err = true;
                                        break;
                                    }
                                    char *grown = (char *)realloc(resp, next_cap);
                                    if (!grown) {
                                        read_err = true;
                                        break;
                                    }
                                    resp = grown;
                                    cap = next_cap;
                                }
                                DWORD chunk = 0;
                                if (!WinHttpReadData(hRequest, resp + total, avail, &chunk)) {
                                    read_err = true;
                                    break;
                                }
                                if (chunk == 0) break;
                                total += chunk;
                            }
                            if (!read_err) {
                                resp[total] = '\0';
                                pthread_mutex_lock(&s_lb_lock);
                                if (s_last_music_id == music_id && s_last_difficulty == difficulty && strcmp(s_last_style, style_str) == 0) {
                                    fetched_ok = parse_leaderboard_json(resp, music_id, api_music_id, difficulty, style_str);
                                } else {
                                    fetched_ok = true; /* Stale request superseded by newer song/diff/style */
                                }
                                pthread_mutex_unlock(&s_lb_lock);
                            }
                            free(resp);
                        }
                    } else {
                        printf("Leaderboard: TFFScores returned HTTP %lu for song=%u (api=%u, diff=%d, style=%s)\n",
                               (unsigned long)status_code, music_id, api_music_id, difficulty, style_str);
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
        WinHttpCloseHandle(hSession);
    }
#else
    (void)api_music_id;
#endif

    if (!fetched_ok) {
        pthread_mutex_lock(&s_lb_lock);
        if (s_last_music_id == music_id && s_last_difficulty == difficulty && strcmp(s_last_style, style_str) == 0) {
            s_entry_count = 0;
            s_scroll_index = 0;
            s_status = LB_STATUS_ERROR;
            __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&s_lb_lock);
    }
    return NULL;
}

void runtime_leaderboard_fetch(uint32_t music_id, int difficulty, const char *style) {
    const char *st = (style && *style) ? style : "standard";
    bool local_only = leaderboard_is_local_only_id(music_id);

    pthread_mutex_lock(&s_lb_lock);
    bool same_query = (s_last_music_id == music_id && s_last_difficulty == difficulty && strcmp(s_last_style, st) == 0);
    if (local_only) {
        s_last_music_id = music_id;
        s_last_difficulty = difficulty;
        snprintf(s_last_style, sizeof(s_last_style), "%s", st);
        s_entry_count = 0;
        s_scroll_index = 0;
        s_status = LB_STATUS_LOCAL_ONLY;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&s_lb_lock);
        printf("Leaderboard: skipping TFFScores fetch for local-only custom ID %u\n", music_id);
        return;
    }

    if (same_query && s_status == LB_STATUS_LOADING) {
        pthread_mutex_unlock(&s_lb_lock);
        return;
    }

    s_last_music_id = music_id;
    s_last_difficulty = difficulty;
    snprintf(s_last_style, sizeof(s_last_style), "%s", st);
    if (!same_query || s_status != LB_STATUS_READY) {
        s_entry_count = 0;
        s_scroll_index = 0;
    }
    s_status = LB_STATUS_LOADING;
    __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s_lb_lock);

    printf("Leaderboard: fetching TFFScores for song=%u (api=%u, diff=%d, style=%s)\n",
           music_id, leaderboard_api_music_id(music_id), difficulty, st);

    LbFetchArg *a = (LbFetchArg *)malloc(sizeof(LbFetchArg));
    if (a) {
        a->music_id = music_id;
        a->difficulty = difficulty;
        snprintf(a->style, sizeof(a->style), "%s", st);
        pthread_t th;
        if (pthread_create(&th, NULL, lb_async_fetch_thread, a) == 0) {
            pthread_detach(th);
        } else {
            free(a);
            pthread_mutex_lock(&s_lb_lock);
            if (s_last_music_id == music_id && s_last_difficulty == difficulty && strcmp(s_last_style, st) == 0) {
                s_status = LB_STATUS_ERROR;
                __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&s_lb_lock);
        }
    }
}

size_t runtime_leaderboard_get_count(void) {
    pthread_mutex_lock(&s_lb_lock);
    size_t count = s_entry_count;
    pthread_mutex_unlock(&s_lb_lock);
    return count;
}

LeaderboardStatus runtime_leaderboard_get_status(void) {
    pthread_mutex_lock(&s_lb_lock);
    LeaderboardStatus st = s_status;
    pthread_mutex_unlock(&s_lb_lock);
    return st;
}

const LeaderboardEntry *runtime_leaderboard_get_entry(size_t index) {
    if (index >= s_entry_count) return NULL;
    return &s_entries[index];
}

bool runtime_leaderboard_copy_entry(size_t index, LeaderboardEntry *out) {
    if (!out) return false;
    bool ok = false;
    pthread_mutex_lock(&s_lb_lock);
    if (index < s_entry_count) {
        *out = s_entries[index];
        ok = true;
    }
    pthread_mutex_unlock(&s_lb_lock);
    return ok;
}

uint32_t runtime_leaderboard_get_revision(void) {
    return __atomic_load_n(&s_lb_revision, __ATOMIC_ACQUIRE);
}

int runtime_leaderboard_get_scroll(void) {
    pthread_mutex_lock(&s_lb_lock);
    int scroll = s_scroll_index;
    pthread_mutex_unlock(&s_lb_lock);
    return scroll;
}

void runtime_leaderboard_scroll_up(void) {
    pthread_mutex_lock(&s_lb_lock);
    if (s_scroll_index > 0) {
        --s_scroll_index;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        printf("Leaderboard: scrolled UP to index %d\n", s_scroll_index);
    }
    pthread_mutex_unlock(&s_lb_lock);
}

void runtime_leaderboard_scroll_down(void) {
    pthread_mutex_lock(&s_lb_lock);
    if (s_entry_count > LEADERBOARD_VISIBLE_ROWS &&
        s_scroll_index + LEADERBOARD_VISIBLE_ROWS < (int)s_entry_count) {
        ++s_scroll_index;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
        printf("Leaderboard: scrolled DOWN to index %d\n", s_scroll_index);
    }
    pthread_mutex_unlock(&s_lb_lock);
}

void runtime_leaderboard_reset_scroll(void) {
    pthread_mutex_lock(&s_lb_lock);
    if (s_scroll_index != 0) {
        s_scroll_index = 0;
        __atomic_add_fetch(&s_lb_revision, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s_lb_lock);
}
