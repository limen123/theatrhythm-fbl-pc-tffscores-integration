#ifndef RUNTIME_TFFSCORES_H
#define RUNTIME_TFFSCORES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes the TFFScores integration subsystem. */
void runtime_tffscores_init(const char *user_dir);

/* Called when the user presses F9 to open the login flow / check account status. */
void runtime_tffscores_trigger_auth_hotkey(void);

/* Called by runtime_savedata.c when a save directory has been unmounted / committed. */
void runtime_tffscores_on_save_commit(const char *save_host);

/* Called when the song results count-up finishes (Game.Scene.MusicResult.CountUpBehaviour::OnEnd). */
void runtime_tffscores_on_song_result_finish(void);

/* Called when user presses Confirm or when Retry Window opens before screenshot was snapped. */
void runtime_tffscores_trigger_immediate_screenshot(void);

/* Attempts to restore a cloud save from TFFScores if the local save is missing. */
int runtime_tffscores_maybe_download_save(const char *save_host, const char *title_id);

/* Returns 1 if currently logged in to TFFScores, 0 otherwise. */
int runtime_tffscores_is_logged_in(void);

/* Gets the logged-in username, or empty string if not logged in. */
/* Notifies the stage start (music_id and difficulty). Called from theater begin init. */
void runtime_tffscores_notify_stage_start(uint32_t music_id, int difficulty);

/* Notifies that the stage was a score PB (or not). */
void runtime_tffscores_notify_score_pb(int is_pb);

/* Notifies that the stage was a damage PB (or not). */
void runtime_tffscores_notify_damage_pb(int is_pb);

const char *runtime_tffscores_username(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_TFFSCORES_H */
