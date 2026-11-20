/* Custom note skin recoloring & per-note particle color matching (Theatrhythm Final Bar Line PS4 v1.0.4). */
#ifndef RUNTIME_NOTE_SKINS_H
#define RUNTIME_NOTE_SKINS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef HOOK_REGS_DEFINED
#define HOOK_REGS_DEFINED
typedef struct {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out;
    double xmm0, xmm1, xmm2, xmm3;
} HookRegs;
#endif

/* Install note skin & particle color-matching hooks into Il2CppUserAssemblies.prx. */
int runtime_note_skins_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size);

/* Track active TriggerColorIndex (0 = Noteskin A, 1 = Noteskin B).
 * Custom note skin & particle recoloring only applies to Noteskin A (0). */
void runtime_note_skins_set_color_type(int color_type);
int runtime_note_skins_get_color_type(void);

/* Recolor an RGBA8 pixel buffer in place using the category's Hue/Saturation while preserving
 * neutral white inner markings and dark outlines. */
void runtime_note_skins_recolor_rgba(uint8_t *rgba, int w, int h, int category);

/* Decode PNG bytes, apply the category's Hue/Saturation, and create a Unity Sprite. */
void *runtime_note_skins_create_recolored_sprite(const uint8_t *png, size_t png_sz,
                                                 int crop_x, int crop_y, int crop_w, int crop_h,
                                                 float ppu, float pivot_x, float pivot_y,
                                                 int category, const char *tag);

/* Apply custom RGB tinting to ParticleSystem lists and Lane/Effect hierarchies. */
void runtime_note_skins_apply_ps_list_rgb(void *ps_list, float r, float g, float b);

/* Callbacks invoked from runtime_slidehold.c's shared hooks (avoids double-hooking the same entry). */
void runtime_note_skins_on_play_judge_effect(void *controller, void *trig_pd);
void runtime_note_skins_on_bms_holding(void *controller, void *trig_pd);
void *runtime_note_skins_active_hold_start_sprite(void);
void runtime_note_skins_on_triggers_created(void *mst);
void runtime_note_skins_sync_simple_colors(void);
void runtime_note_skins_on_set_simple_type(void *tobj);

/* Cross-module helpers exposed by runtime_custom_charts.c & runtime_slidehold.c. */
uint8_t *runtime_decode_png_rgba(const uint8_t *png, size_t png_sz, int *out_w, int *out_h);
void runtime_free_decoded_rgba(uint8_t *pixels);
void *runtime_create_texture2d_rgba(const uint8_t *rgba, int width, int height, const char *tag);
void *runtime_create_sprite_from_rgba_metrics(const uint8_t *rgba, int width, int height,
                                            float rect_x, float rect_y, float rect_w, float rect_h,
                                            float pivot_x, float pivot_y, float ppu,
                                            float border_x, float border_y, float border_z, float border_w,
                                            const char *tag);
void *runtime_il2cpp_string(const char *utf8);
uint8_t *runtime_extract_texture_rgba(void *tex, int *out_w, int *out_h);
void runtime_free_extracted_rgba(uint8_t *pixels);
void *runtime_get_component_by_name(void *go, const char *full_name, const char *short_name);
void *runtime_clone_material_raw(void *src_mat);
void runtime_destroy_unity_object_imm(void *obj);

int runtime_slidehold_classify_trigger(int32_t trig_idx);
bool runtime_slidehold_is_registered_trigger_obj(const void *obj);
const uint8_t *runtime_slidehold_onpu_png(size_t *out_sz);
void runtime_slidehold_on_skin_changed(void *mst);

#endif /* RUNTIME_NOTE_SKINS_H */
