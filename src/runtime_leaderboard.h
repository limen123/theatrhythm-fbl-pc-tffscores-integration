#ifndef RUNTIME_LEADERBOARD_H
#define RUNTIME_LEADERBOARD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int rank;
    char username[64];
    uint32_t score;
    char badge[32];   /* "All Critical", "Full Chain", "Clear" */
    uint32_t damage;
} LeaderboardEntry;

#define MAX_LEADERBOARD_ENTRIES 128
#define LEADERBOARD_VISIBLE_ROWS 5

void runtime_leaderboard_init(void);
void runtime_leaderboard_fetch(uint32_t music_id, int difficulty, const char *style);
size_t runtime_leaderboard_get_count(void);
const LeaderboardEntry *runtime_leaderboard_get_entry(size_t index);
bool runtime_leaderboard_copy_entry(size_t index, LeaderboardEntry *out);
uint32_t runtime_leaderboard_get_revision(void);

int runtime_leaderboard_get_scroll(void);
void runtime_leaderboard_scroll_up(void);
void runtime_leaderboard_scroll_down(void);
void runtime_leaderboard_reset_scroll(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_LEADERBOARD_H */
