#ifndef RUNTIME_CUSTOM_CHARTS_H
#define RUNTIME_CUSTOM_CHARTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void runtime_custom_charts_configure(const char *game_dir, const char *user_dir);
int runtime_custom_charts_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size);
int runtime_custom_charts_get_results_page(void);
uint32_t runtime_custom_charts_normalize_id(uint32_t music_id);
bool runtime_custom_charts_is_verified_id(uint32_t music_id);
bool runtime_custom_charts_is_custom_id(uint32_t music_id);
const char *runtime_custom_charts_get_play_style(void);
bool runtime_custom_charts_records_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_CUSTOM_CHARTS_H */
