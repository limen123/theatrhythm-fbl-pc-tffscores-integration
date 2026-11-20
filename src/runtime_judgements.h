#ifndef RUNTIME_JUDGEMENTS_H
#define RUNTIME_JUDGEMENTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Judgement rating matching game TriggerJudge enum */
enum {
    JUDGE_NONE = 0,
    JUDGE_CRITICAL = 1, /* Rainbow SP when JudgementNote.flags & 1, else Gold */
    JUDGE_GREAT = 2,
    JUDGE_GOOD = 3,
    JUDGE_BAD = 4,
    JUDGE_MISS = 5,
    JUDGE_MISS_PASS = 6
};

#pragma pack(push, 1)
typedef struct {
    int32_t target_ms;     /* Target song time in milliseconds */
    int32_t offset_ms;     /* Deviation: hit_time - target_ms (+ = late, - = early or vice-versa) */
    uint8_t judge;         /* TriggerJudge enum */
    uint8_t lane;          /* Note lane 0..3 */
    uint16_t flags;
} JudgementNote;
#pragma pack(pop)

#define MAX_STAGE_NOTES 4096

/* Configure save path root (e.g. "Save") */
void runtime_judgements_configure(const char *save_dir);

/* Called at stage start to prepare note recording buffer */
void runtime_judgements_begin_stage(uint32_t music_id, int difficulty);

/* Record a single note judgement during stage play */
void runtime_judgements_record(int target_ms, int offset_ms, int judge, int lane, int is_rainbow);

/* Map TriggerJudge enum + flags to RGB24 color for hit dots (1..4) */
void runtime_judgements_judge_color(uint8_t judge, uint16_t flags,
                                    uint8_t *out_r, uint8_t *out_g, uint8_t *out_b);

/* Render timeline judgement graph to RGBA32 bitmap */
int runtime_judgements_render_graph(uint8_t *rgba_out, int width, int height, uint32_t duration_ms);
const char *runtime_judgements_get_save_path(void);

/* Called at stage end to persist to Save/judgements.dat if new best score */
void runtime_judgements_finish_stage(uint32_t score, bool is_new_best);
void runtime_judgements_ensure_stage_finished(uint32_t fallback_score);

/* Query judgements for display on Page 3 (from live run or loaded best) */
size_t runtime_judgements_get_count(void);
const JudgementNote *runtime_judgements_get_notes(void);
uint32_t runtime_judgements_get_duration(void);
void runtime_judgements_get_song_info(uint32_t *out_music_id, int *out_difficulty, uint32_t *out_score);
void runtime_judgements_get_summary(float *mean_offset, float *std_dev, int *early_count, int *late_count);

/* Load saved judgements for a specific song and difficulty */
bool runtime_judgements_load(uint32_t music_id, int difficulty);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_JUDGEMENTS_H */
