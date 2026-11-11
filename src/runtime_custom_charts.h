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
bool runtime_custom_charts_is_custom_id(uint32_t music_id);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_CUSTOM_CHARTS_H */
