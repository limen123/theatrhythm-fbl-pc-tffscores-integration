/* SlideHold note support for Theatrhythm Final Bar Line (PS4 v1.0.4).
 *
 * SlideHold notes are directional analog-stick holds:
 *  - Start: layered circle + direction arrow matched to native Trigger_Slide size
 *  - Body: cleanly cropped HoldLine cyan ribbon matched to native HoldLine thickness
 *  - End: HoldEnd ring matched to native Trigger_HoldEnd size (0.60x start)
 *  - Input: fresh analog flick with closest-trigger arbitration, +/-45 deg hold maintenance,
 *           per-stick (deviceID, inputType) isolation, tail release judgement, overhold miss.
 */
#ifndef RUNTIME_SLIDEHOLD_H
#define RUNTIME_SLIDEHOLD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "runtime.h"

#ifndef HOOK_REGS_DEFINED
#define HOOK_REGS_DEFINED
typedef struct {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out;
    double xmm0, xmm1, xmm2, xmm3;
} HookRegs;
#endif

#define SLIDEHOLD_PARAM_FLAG 1

/* Module init (stores Il2CppUserAssemblies.prx base address and game version flag). */
void runtime_slidehold_init(unsigned char *module, int is_game_104);

/* Trigger color preset (0 = Skin A / BMS Standard, 1 = Skin B; SlideHolds use Cyan on both). */
void runtime_slidehold_set_color_type(int color_type);

/* Clear all active gameplay SlideHold state (stage load/finish). */
void runtime_slidehold_reset(void);

/* Reset only per-attempt playback flags & sticks (preserves parsed chart SlideHolds on Retry). */
void runtime_slidehold_reset_playback(void);

/* Statelessly normalize custom chart trigger bytes in place:
 *  - Converts raw type=12 -> (type=3, param1=1) and enforces release-to-neutral tail type=5.
 * Never mutates runtime SlideHold gameplay state (safe during song scan & menu preview). */
void runtime_slidehold_normalize_chart(uint8_t *cdata, size_t size);

/* Parse SlideHold note groups from the chart selected for gameplay (called only on gameplay start). */
bool runtime_slidehold_load_chart(const uint8_t *cdata, size_t size);

/* Judgement filtering & native notification hooks (for ContextPlaydata::FixedTriggerJudge). */
bool runtime_slidehold_is_start_trigger(int32_t trigger_index);
bool runtime_slidehold_is_end_trigger(int32_t trigger_index);
bool runtime_slidehold_is_internal_judge(void);
void runtime_slidehold_set_auto_mode(bool is_auto);
bool runtime_slidehold_is_auto_mode(void);
void runtime_slidehold_on_native_judge(int32_t trigger_index, uint8_t judge, int32_t hit_time);

/* Hook original trampolines wired by runtime_custom_charts.c. */
extern bool (ABI *original_create_triggers_movenext)(void *state_machine);
extern void (ABI *original_runtime_context_update)(void *context, void *arg1, int32_t cur_time, void *input_data);
extern void (ABI *original_play_judge_effect)(void *controller, void *trigger_pd, void *method);

/* Stage & visual presentation hooks (SysV ABI, PS4 v1.0.4). */
ABI int hook_slidehold_create_triggers_movenext(HookRegs *r);
ABI int hook_slidehold_trigger_set_visible(HookRegs *r);
ABI int hook_slidehold_trigger_fade(HookRegs *r);
ABI int hook_slidehold_trigger_set_simple_type(HookRegs *r);
ABI int hook_slidehold_trigger_set_simple_color(HookRegs *r);

/* Audio SFX hook. */
ABI int hook_slidehold_se(HookRegs *r);

/* Input / gameplay update & frame miss hooks. */
ABI int hook_slidehold_runtime_update(HookRegs *r);
ABI int hook_slidehold_miss_judge(HookRegs *r);
ABI int hook_slidehold_bms_update_holding(HookRegs *r);
ABI int hook_slidehold_play_judge_effect(HookRegs *r);

/* Helpers provided by runtime_custom_charts.c for native Unity sprite & particle lifecycle. */
void *runtime_create_sprite_from_png(const uint8_t *png, size_t png_sz, float ppu, const char *tag);
void *runtime_create_sprite_from_png_rect(const uint8_t *png, size_t png_sz,
                                           int crop_x, int crop_y, int crop_w, int crop_h,
                                           float ppu, float pivot_x, float pivot_y,
                                           const char *tag);
void *runtime_create_texture2d_from_png(const uint8_t *png, size_t png_sz, const char *tag);
void *runtime_clone_material_with_main_tex(void *src_mat, void *tex);
void *runtime_clone_gradient_solid_color(void *src_grad, float r, float g, float b);
void *runtime_get_particle_system_renderer(void *ps);
void *runtime_resolve_icall(const char *name);
bool runtime_object_name_equals(void *obj, const char *ascii_name);
uint32_t runtime_gc_pin_obj(void *obj);
void runtime_gc_unpin_obj(uint32_t handle);
bool runtime_unity_object_alive(const void *obj);
bool runtime_list_remove_at(void *list, int32_t index);
void *runtime_create_child_sprite_renderer(void *base_sr, void *parent_tr, const char *name);

#endif /* RUNTIME_SLIDEHOLD_H */
