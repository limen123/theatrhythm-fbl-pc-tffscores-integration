#include "runtime_judgements.h"
#include "runtime_custom_charts.h"
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define JUDGEMENTS_MAGIC 0x4744554a /* 'JUDG' */
#define JUDGEMENTS_VERSION 1
#define MAX_STORED_ENTRIES 512

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t entry_count;
    uint32_t reserved;
} JudgementsHeader;

typedef struct {
    uint32_t music_id;
    uint32_t difficulty;
    uint32_t score;
    uint32_t note_count;
} JudgementsFileEntry;
#pragma pack(pop)

static pthread_mutex_t s_judg_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_save_dir[512] = "Save";
static char s_primary_save_path[560] = "";

/* Current live stage notes buffer */
static JudgementNote s_current_run_notes[MAX_STAGE_NOTES];
static size_t s_current_run_count = 0;
static uint32_t s_current_music_id = 0;
static int s_current_difficulty = 0;
static bool s_stage_unflushed = false;

/* Display notes buffer (either from current run or loaded best) */
static JudgementNote s_display_notes[MAX_STAGE_NOTES];
static size_t s_display_count = 0;
static uint32_t s_display_music_id = 0;
static int s_display_difficulty = 0;
static uint32_t s_display_score = 0;

void runtime_judgements_configure(const char *save_dir) {
    pthread_mutex_lock(&s_judg_lock);
    if (save_dir && *save_dir) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", save_dir);
    }
    snprintf(s_primary_save_path, sizeof(s_primary_save_path), "%s/judgementsBackup.inf", s_save_dir);
    pthread_mutex_unlock(&s_judg_lock);
    printf("Judgements: configured save dir: %s (path: %s)\n", s_save_dir, s_primary_save_path);
}

const char *runtime_judgements_get_save_path(void) {
    return s_primary_save_path[0] ? s_primary_save_path : "Save/judgementsBackup.inf";
}

static bool s_stage_saved_best = false;

void runtime_judgements_judge_color(uint8_t judge, uint16_t flags,
                                    uint8_t *out_r, uint8_t *out_g, uint8_t *out_b) {
    uint8_t r = 0xff, g = 0xff, b = 0xff;
    if (judge == JUDGE_CRITICAL) {
        if (flags & 1) {
            /* Rainbow Critical: Pure White */
            r = 0xff; g = 0xff; b = 0xff;
        } else {
            /* Critical: Gold / Yellow */
            r = 0xff; g = 0xd7; b = 0x00;
        }
    } else if (judge == JUDGE_GREAT) {
        /* Great: Orange */
        r = 0xff; g = 0x8c; b = 0x00;
    } else if (judge == JUDGE_GOOD) {
        /* Good: Green */
        r = 0x32; g = 0xcd; b = 0x32;
    } else if (judge == JUDGE_BAD) {
        /* Bad: Purple */
        r = 0x93; g = 0x70; b = 0xdb;
    }
    if (out_r) *out_r = r;
    if (out_g) *out_g = g;
    if (out_b) *out_b = b;
}

void runtime_judgements_begin_stage(uint32_t music_id, int difficulty) {
    if (music_id == 0) return;
    pthread_mutex_lock(&s_judg_lock);
    if (s_current_music_id == music_id && s_current_difficulty >= 4 && difficulty < 4) {
        /* Keep extra difficulty slot 4/5 if PlaySetting+0x50 was clamped to 0..3 */
    } else {
        s_current_difficulty = difficulty;
    }
    s_current_music_id = music_id;
    s_current_run_count = 0;
    s_stage_unflushed = true;
    s_stage_saved_best = false;
    pthread_mutex_unlock(&s_judg_lock);
}

void runtime_judgements_record(int target_ms, int offset_ms, int judge, int lane, int is_rainbow) {
    pthread_mutex_lock(&s_judg_lock);
    if (s_current_run_count < MAX_STAGE_NOTES) {
        JudgementNote *n = &s_current_run_notes[s_current_run_count++];
        n->target_ms = target_ms;
        n->offset_ms = offset_ms;
        n->judge = (uint8_t)judge;
        n->lane = (uint8_t)lane;
        n->flags = is_rainbow ? 1 : 0;
        s_stage_unflushed = true;
    }
    pthread_mutex_unlock(&s_judg_lock);
}

static void save_file_to_disk(const char *path, const uint8_t *data, size_t size) {
    if (!path || !path[0] || !data || !size) return;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

void runtime_judgements_ensure_stage_finished(uint32_t fallback_score) {
    bool need_flush = false;
    pthread_mutex_lock(&s_judg_lock);
    if (s_stage_unflushed && s_current_run_count > 0) {
        need_flush = true;
    }
    pthread_mutex_unlock(&s_judg_lock);
    if (need_flush) {
        runtime_judgements_finish_stage(fallback_score, false);
    }
}

void runtime_judgements_finish_stage(uint32_t score, bool is_new_best) {
    pthread_mutex_lock(&s_judg_lock);
    bool was_unflushed = s_stage_unflushed;
    s_stage_unflushed = false;

    /* Copy live run to display buffer once, or upgrade display score if caller supplies real score */
    if (was_unflushed || s_display_music_id != s_current_music_id ||
        s_display_difficulty != s_current_difficulty || (score > 0 && s_display_score == 0)) {
        s_display_count = s_current_run_count;
        s_display_music_id = s_current_music_id;
        s_display_difficulty = s_current_difficulty;
        if (score > 0 || was_unflushed) s_display_score = score;
        if (s_current_run_count > 0) {
            memcpy(s_display_notes, s_current_run_notes, s_current_run_count * sizeof(JudgementNote));
        }
    }

    if (!is_new_best || score == 0 || s_stage_saved_best ||
        s_current_run_count == 0 || s_current_music_id == 0) {
        pthread_mutex_unlock(&s_judg_lock);
        return;
    }

    /* Read existing file if present to update or append.
     * Custom charts use placeholder IDs (169xxxxxx) and are isolated in customJudgements.dat
     * so stock judgementsBackup.inf stays 100% pure for TFFScores cloud saves! */
    bool is_custom = runtime_custom_charts_is_custom_id(s_current_music_id);
    char path_backup[560], path_dat[560];
    if (is_custom) {
        snprintf(path_backup, sizeof(path_backup), "%s/customJudgements.dat", s_save_dir);
        path_dat[0] = '\0';
    } else {
        snprintf(path_backup, sizeof(path_backup), "%s/judgementsBackup.inf", s_save_dir);
        snprintf(path_dat, sizeof(path_dat), "%s/judgements.dat", s_save_dir);
    }

    FILE *f = fopen(path_backup, "rb");
    if (!f && path_dat[0]) f = fopen(path_dat, "rb");

    JudgementsHeader hdr = {0};
    uint8_t *file_data = NULL;
    size_t file_size = 0;

    if (f) {
        fseek(f, 0, SEEK_END);
        file_size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        if (file_size >= sizeof(JudgementsHeader)) {
            file_data = (uint8_t *)malloc(file_size);
            if (file_data) {
                if (fread(file_data, 1, file_size, f) == file_size) {
                    memcpy(&hdr, file_data, sizeof(JudgementsHeader));
                } else {
                    free(file_data);
                    file_data = NULL;
                }
            }
        }
        fclose(f);
    }

    /* Build new combined binary payload */
    size_t est_cap = file_size + sizeof(JudgementsFileEntry) + s_current_run_count * sizeof(JudgementNote) + 4096;
    uint8_t *out_buf = (uint8_t *)malloc(est_cap);
    if (!out_buf) {
        if (file_data) free(file_data);
        pthread_mutex_unlock(&s_judg_lock);
        return;
    }

    size_t out_pos = sizeof(JudgementsHeader);
    uint32_t count = 0;
    bool updated = false;

    if (hdr.magic == JUDGEMENTS_MAGIC && hdr.version == JUDGEMENTS_VERSION && file_data) {
        size_t off = sizeof(JudgementsHeader);
        for (uint32_t i = 0; i < hdr.entry_count && off + sizeof(JudgementsFileEntry) <= file_size; ++i) {
            JudgementsFileEntry e;
            memcpy(&e, file_data + off, sizeof(e));
            off += sizeof(e);
            size_t note_bytes = e.note_count * sizeof(JudgementNote);
            if (off + note_bytes > file_size) break;

            if (e.music_id == s_current_music_id && (int)e.difficulty == s_current_difficulty) {
                if (is_new_best && (e.score == 0 || score >= e.score)) {
                    JudgementsFileEntry new_e;
                    new_e.music_id = s_current_music_id;
                    new_e.difficulty = (uint32_t)s_current_difficulty;
                    new_e.score = score;
                    new_e.note_count = (uint32_t)s_current_run_count;
                    memcpy(out_buf + out_pos, &new_e, sizeof(new_e));
                    out_pos += sizeof(new_e);
                    memcpy(out_buf + out_pos, s_current_run_notes, s_current_run_count * sizeof(JudgementNote));
                    out_pos += s_current_run_count * sizeof(JudgementNote);
                } else {
                    memcpy(out_buf + out_pos, &e, sizeof(e));
                    out_pos += sizeof(e);
                    memcpy(out_buf + out_pos, file_data + off, note_bytes);
                    out_pos += note_bytes;
                }
                updated = true;
            } else {
                memcpy(out_buf + out_pos, &e, sizeof(e));
                out_pos += sizeof(e);
                memcpy(out_buf + out_pos, file_data + off, note_bytes);
                out_pos += note_bytes;
            }
            off += note_bytes;
            ++count;
        }
    }

    if (!updated) {
        JudgementsFileEntry new_e;
        new_e.music_id = s_current_music_id;
        new_e.difficulty = (uint32_t)s_current_difficulty;
        new_e.score = score;
        new_e.note_count = (uint32_t)s_current_run_count;
        memcpy(out_buf + out_pos, &new_e, sizeof(new_e));
        out_pos += sizeof(new_e);
        memcpy(out_buf + out_pos, s_current_run_notes, s_current_run_count * sizeof(JudgementNote));
        out_pos += s_current_run_count * sizeof(JudgementNote);
        ++count;
    }

    JudgementsHeader new_hdr;
    new_hdr.magic = JUDGEMENTS_MAGIC;
    new_hdr.version = JUDGEMENTS_VERSION;
    new_hdr.entry_count = count;
    new_hdr.reserved = 0;
    memcpy(out_buf, &new_hdr, sizeof(new_hdr));

    /* Write to primary save location (judgementsBackup.inf for stock, customJudgements.dat for custom) */
    save_file_to_disk(path_backup, out_buf, out_pos);
    if (path_dat[0]) save_file_to_disk(path_dat, out_buf, out_pos);

    /* Also write into active mounted savedata slot if known */
    char active_slot[700] = {0};
    if (runtime_savedata_get_active_save_dir(active_slot, sizeof(active_slot)) == 0 && active_slot[0]) {
        char slot_path[800];
        snprintf(slot_path, sizeof(slot_path), "%s/%s", active_slot,
                 is_custom ? "customJudgements.dat" : "judgementsBackup.inf");
        save_file_to_disk(slot_path, out_buf, out_pos);
    }

    s_stage_saved_best = true;
    printf("Judgements: Successfully saved %s to %s (%u entries, %zu notes)\n",
           is_custom ? "customJudgements.dat" : "4th save file (judgementsBackup.inf)",
           path_backup, count, s_current_run_count);

    free(out_buf);
    if (file_data) free(file_data);
    pthread_mutex_unlock(&s_judg_lock);
}

bool runtime_judgements_load(uint32_t music_id, int difficulty) {
    pthread_mutex_lock(&s_judg_lock);
    bool is_custom = runtime_custom_charts_is_custom_id(music_id);
    char path[560];
    FILE *f = NULL;
    if (is_custom) {
        snprintf(path, sizeof(path), "%s/customJudgements.dat", s_save_dir);
        f = fopen(path, "rb");
        if (!f) {
            char active_slot[700] = {0};
            if (runtime_savedata_get_active_save_dir(active_slot, sizeof(active_slot)) == 0 && active_slot[0]) {
                char slot_path[800];
                snprintf(slot_path, sizeof(slot_path), "%s/customJudgements.dat", active_slot);
                f = fopen(slot_path, "rb");
            }
        }
    } else {
        snprintf(path, sizeof(path), "%s/judgementsBackup.inf", s_save_dir);
        f = fopen(path, "rb");
        if (!f) {
            snprintf(path, sizeof(path), "%s/judgements.dat", s_save_dir);
            f = fopen(path, "rb");
        }
    }
    if (!f) {
        pthread_mutex_unlock(&s_judg_lock);
        return false;
    }

    JudgementsHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != JUDGEMENTS_MAGIC || hdr.version != JUDGEMENTS_VERSION) {
        fclose(f);
        pthread_mutex_unlock(&s_judg_lock);
        return false;
    }

    for (uint32_t i = 0; i < hdr.entry_count; ++i) {
        JudgementsFileEntry e;
        if (fread(&e, sizeof(e), 1, f) != 1) break;

        if (e.music_id == music_id && (int)e.difficulty == difficulty) {
            s_display_count = e.note_count < MAX_STAGE_NOTES ? e.note_count : MAX_STAGE_NOTES;
            s_display_music_id = e.music_id;
            s_display_difficulty = (int)e.difficulty;
            s_display_score = e.score;
            if (fread(s_display_notes, sizeof(JudgementNote), s_display_count, f) == s_display_count) {
                fclose(f);
                pthread_mutex_unlock(&s_judg_lock);
                return true;
            }
            break;
        } else {
            fseek(f, e.note_count * sizeof(JudgementNote), SEEK_CUR);
        }
    }

    fclose(f);
    pthread_mutex_unlock(&s_judg_lock);
    return false;
}

size_t runtime_judgements_get_count(void) {
    pthread_mutex_lock(&s_judg_lock);
    size_t count = s_display_count;
    pthread_mutex_unlock(&s_judg_lock);
    return count;
}

const JudgementNote *runtime_judgements_get_notes(void) {
    return s_display_notes;
}

uint32_t runtime_judgements_get_duration(void) {
    pthread_mutex_lock(&s_judg_lock);
    uint32_t dur = 0;
    for (size_t i = 0; i < s_display_count; ++i) {
        if ((uint32_t)s_display_notes[i].target_ms > dur)
            dur = (uint32_t)s_display_notes[i].target_ms;
    }
    dur += 2000;
    pthread_mutex_unlock(&s_judg_lock);
    return dur < 1000 ? 1000 : dur;
}

void runtime_judgements_get_song_info(uint32_t *out_music_id, int *out_difficulty, uint32_t *out_score) {
    pthread_mutex_lock(&s_judg_lock);
    if (out_music_id) *out_music_id = s_display_music_id;
    if (out_difficulty) *out_difficulty = s_display_difficulty;
    if (out_score) *out_score = s_display_score;
    pthread_mutex_unlock(&s_judg_lock);
}

void runtime_judgements_get_summary(float *mean_offset, float *std_dev, int *early_count, int *late_count) {
    pthread_mutex_lock(&s_judg_lock);
    double sum = 0.0;
    int early = 0, late = 0;
    size_t n = 0;

    for (size_t i = 0; i < s_display_count; ++i) {
        if (s_display_notes[i].judge < 5) {
            int off = s_display_notes[i].offset_ms;
            sum += off;
            if (off < 0) ++early;
            else if (off > 0) ++late;
            ++n;
        }
    }

    double mean = n ? sum / (double)n : 0.0;
    double var_sum = 0.0;
    for (size_t i = 0; i < s_display_count; ++i) {
        if (s_display_notes[i].judge < 5) {
            double diff = s_display_notes[i].offset_ms - mean;
            var_sum += diff * diff;
        }
    }
    double sd = n > 1 ? sqrt(var_sum / (double)(n - 1)) : 0.0;

    if (mean_offset) *mean_offset = (float)mean;
    if (std_dev) *std_dev = (float)sd;
    if (early_count) *early_count = early;
    if (late_count) *late_count = late;
    pthread_mutex_unlock(&s_judg_lock);
}

static void draw_char_5x7(uint8_t *rgba, int width, int height, int x0, int y0, char c, uint8_t r, uint8_t g, uint8_t b, int scale) {
    static const uint8_t glyph_E[7] = {0x1f, 0x10, 0x1e, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_A[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
    static const uint8_t glyph_R[7] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11};
    static const uint8_t glyph_L[7] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
    static const uint8_t glyph_Y[7] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t glyph_T[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};

    const uint8_t *glyph = NULL;
    if (c == 'E') glyph = glyph_E;
    else if (c == 'A') glyph = glyph_A;
    else if (c == 'R') glyph = glyph_R;
    else if (c == 'L') glyph = glyph_L;
    else if (c == 'Y') glyph = glyph_Y;
    else if (c == 'T') glyph = glyph_T;
    if (!glyph) return;

    for (int row = 0; row < 7; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 5; ++col) {
            if (bits & (1 << (4 - col))) {
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        int px = x0 + col * scale + dx;
                        int py = y0 + row * scale + dy;
                        if (px >= 0 && px < width && py >= 0 && py < height) {
                            int idx = (py * width + px) * 4;
                            rgba[idx + 0] = r;
                            rgba[idx + 1] = g;
                            rgba[idx + 2] = b;
                            rgba[idx + 3] = 0xff;
                        }
                    }
                }
            }
        }
    }
}

static void draw_text(uint8_t *rgba, int width, int height, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b, int scale) {
    while (*text) {
        draw_char_5x7(rgba, width, height, x, y, *text, r, g, b, scale);
        x += (5 + 1) * scale;
        ++text;
    }
}

/* Render timeline graph to RGBA32 bitmap matching StepMania / Simply Love style */
int runtime_judgements_render_graph(uint8_t *rgba, int width, int height, uint32_t duration_ms) {
    if (!rgba || width <= 10 || height <= 10) return -1;
    pthread_mutex_lock(&s_judg_lock);

    /* 1. Background: dark charcoal / navy (0x12, 0x16, 0x22, 0xff) */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = (y * width + x) * 4;
            rgba[idx + 0] = 0x12;
            rgba[idx + 1] = 0x16;
            rgba[idx + 2] = 0x22;
            rgba[idx + 3] = 0xff;
        }
    }

    int cy = height / 2;

    /* 2. Grid lines */
    /* Center line (0 ms): subtle grey (0x4a, 0x55, 0x68) */
    for (int x = 0; x < width; ++x) {
        int idx = (cy * width + x) * 4;
        rgba[idx + 0] = 0x4a; rgba[idx + 1] = 0x55; rgba[idx + 2] = 0x68; rgba[idx + 3] = 0xff;
    }

    /* Reference lines (+/- 33ms and +/- 66ms) */
    int y_p33 = cy - (int)(33.0f / 120.0f * (cy - 10));
    int y_m33 = cy + (int)(33.0f / 120.0f * (cy - 10));
    int y_p66 = cy - (int)(66.0f / 120.0f * (cy - 10));
    int y_m66 = cy + (int)(66.0f / 120.0f * (cy - 10));

    for (int x = 0; x < width; x += 4) {
        if (y_p33 >= 0 && y_p33 < height) { int idx = (y_p33 * width + x) * 4; rgba[idx+0]=0x2a; rgba[idx+1]=0x33; rgba[idx+2]=0x42; }
        if (y_m33 >= 0 && y_m33 < height) { int idx = (y_m33 * width + x) * 4; rgba[idx+0]=0x2a; rgba[idx+1]=0x33; rgba[idx+2]=0x42; }
        if (y_p66 >= 0 && y_p66 < height) { int idx = (y_p66 * width + x) * 4; rgba[idx+0]=0x22; rgba[idx+1]=0x28; rgba[idx+2]=0x36; }
        if (y_m66 >= 0 && y_m66 < height) { int idx = (y_m66 * width + x) * 4; rgba[idx+0]=0x22; rgba[idx+1]=0x28; rgba[idx+2]=0x36; }
    }

    /* 3. EARLY (Orange/Red) and LATE (Blue) labels matching in-game timing indicators */
    draw_text(rgba, width, height, 14, 10, "EARLY", 255, 69, 0, 2);
    draw_text(rgba, width, height, 14, height - 26, "LATE", 30, 144, 255, 2);

    if (s_display_count == 0) {
        pthread_mutex_unlock(&s_judg_lock);
        return 0;
    }

    /* Compute song duration if not provided */
    uint32_t song_dur = duration_ms;
    if (song_dur == 0) {
        for (size_t i = 0; i < s_display_count; ++i) {
            if ((uint32_t)s_display_notes[i].target_ms > song_dur)
                song_dur = (uint32_t)s_display_notes[i].target_ms;
        }
        song_dur += 2000;
    }
    if (song_dur < 1000) song_dur = 1000;

    /* Plot note timeline dots */
    for (size_t i = 0; i < s_display_count; ++i) {
        const JudgementNote *n = &s_display_notes[i];
        int x = 12 + (int)((double)n->target_ms / (double)song_dur * (width - 24));
        if (x < 2) x = 2;
        if (x >= width - 2) x = width - 3;

        if (n->judge == 0) continue;
        if (n->judge >= 5) {
            /* Miss: draw a vertical red line */
            for (int y = cy - (cy * 2 / 3); y <= cy + (cy * 2 / 3); ++y) {
                if (y >= 0 && y < height) {
                    int idx = (y * width + x) * 4;
                    rgba[idx + 0] = 0xee; /* Red */
                    rgba[idx + 1] = 0x22;
                    rgba[idx + 2] = 0x33;
                    rgba[idx + 3] = 0xff;
                }
            }
        } else {
            /* Note hit: early is above center (negative offset), late is below center (positive offset) */
            int off = n->offset_ms;
            if (off > 120) off = 120;
            if (off < -120) off = -120;
            int y = cy + (int)((float)off / 120.0f * (cy - 12));
            if (y < 2) y = 2;
            if (y >= height - 2) y = height - 3;

            /* Select color */
            uint8_t r = 0xff, g = 0xff, b = 0xff;
            runtime_judgements_judge_color(n->judge, n->flags, &r, &g, &b);

            /* Draw 3x3 dot */
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    int px = x + dx;
                    int py = y + dy;
                    if (px >= 0 && px < width && py >= 0 && py < height) {
                        int idx = (py * width + px) * 4;
                        rgba[idx + 0] = r;
                        rgba[idx + 1] = g;
                        rgba[idx + 2] = b;
                        rgba[idx + 3] = 0xff;
                    }
                }
            }
        }
    }

    pthread_mutex_unlock(&s_judg_lock);
    return (int)s_display_count;
}
