/* Clean SlideHold note implementation for Theatrhythm Final Bar Line (PS4 v1.0.4). */

#include "runtime_slidehold.h"
#include "runtime_note_skins.h"
#include "gpu/theatrhythm_gpu.h"
#include "runtime_slidehold_assets.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================= */
/* Constants & Verified PS4 v1.0.4 Engine Offsets                            */
/* ========================================================================= */

#define MAX_CHART_SLIDEHOLDS  1024
#define MAX_SLIDEHOLD_TICKS   256
#define MAX_STICK_TRACKERS    16

/* TriggerType enum values verified from Il2CppUserAssemblies.prx */
enum {
    TRIG_TOUCH = 0,
    TRIG_SLIDE = 1,
    TRIG_TWINSLIDE = 2,
    TRIG_LINHOLD_START = 3,
    TRIG_LINHOLD_PASS = 4,
    TRIG_LINHOLD_END_TOUCH = 5,
    TRIG_LINHOLD_END_SLIDE = 6,
    TRIG_CRVHOLD_START = 7,
    TRIG_CRVHOLD_PASS = 8,
    TRIG_CRVHOLD_END_TOUCH = 9,
    TRIG_CRVHOLD_END_SLIDE = 10,
    TRIG_CUSTOM_SLIDEHOLD = 12
};

/* Judge enum values verified from Il2CppUserAssemblies.prx (JudgeTable..cctor @ 0x32d7ec0) */
enum {
    JUDGE_NONE      = 0,
    JUDGE_RAINBOW   = 1,
    JUDGE_CRITICAL  = 2,
    JUDGE_GREAT     = 3,
    JUDGE_GOOD      = 4,
    JUDGE_BAD       = 5,
    JUDGE_MISS      = 6,
    JUDGE_MISS_PASS = 6
};

/* Verified v1.0.0 / v1.0.4 RVAs */
#define SH_RVA(r100, r104) (is_game_104 ? (r104) : (r100))
#define RVA_RENDERER_GET_SORT_LAYER SH_RVA(0x0dff550ULL, 0x0e09780ULL)
#define RVA_RENDERER_SET_SORT_LAYER SH_RVA(0x0dff5a0ULL, 0x0e097d0ULL)
#define RVA_RENDERER_GET_SORT_ORDER SH_RVA(0x0dff600ULL, 0x0e09830ULL)
#define RVA_RENDERER_SET_SORT_ORDER SH_RVA(0x0dff650ULL, 0x0e09880ULL)
#define RVA_RENDERER_SET_ENABLED    SH_RVA(0x0dff430ULL, 0x0e09660ULL)
#define RVA_RENDERER_GET_ENABLED    SH_RVA(0x0dff3e0ULL, 0x0e09610ULL)
#define RVA_COMP_GET_TRANSFORM      SH_RVA(0x1e27890ULL, 0x1dc88c0ULL)
#define RVA_XFORM_SET_LOCAL_POS_INJ SH_RVA(0x1e36740ULL, 0x1dd7770ULL)
#define RVA_XFORM_GET_LOCAL_SCL_INJ SH_RVA(0x1e37cf0ULL, 0x1dd8d20ULL)
#define RVA_XFORM_SET_LOCAL_SCL_INJ SH_RVA(0x1e37de0ULL, 0x1dd8e10ULL)
#define RVA_XFORM_SET_LOCAL_EULER   SH_RVA(0x1e37030ULL, 0x1dd8060ULL)
#define RVA_SPRITEREND_GET_SPRITE   SH_RVA(0x1e3bb30ULL, 0x1ddcb60ULL)
#define RVA_SPRITEREND_SET_SPRITE   SH_RVA(0x1e3bb80ULL, 0x1ddcbb0ULL)
#define RVA_SPRITEREND_SET_COL_INJ  SH_RVA(0x1e3c140ULL, 0x1ddd170ULL)
#define RVA_SPRITE_GET_BOUNDS_INJ   SH_RVA(0x1e3cfd0ULL, 0x1dde000ULL)
#define RVA_SE_SCHED_TRIGGER        SH_RVA(0x23b0580ULL, 0x243e820ULL)
#define RVA_SE_SCHED_HOLD           SH_RVA(0x23b0460ULL, 0x243e700ULL)
#define RVA_MST_GET_TRIGGER_SPRITE  SH_RVA(0x281d020ULL, 0x2726020ULL)
#define RVA_CONTEXT_GET_JUDGE_TIME  SH_RVA(0x29aa0c0ULL, 0x32cf820ULL)
#define RVA_CONTEXT_IS_SLIDE_DEG    SH_RVA(0x29aa500ULL, 0x32cfc60ULL)
#define RVA_FIXED_TRIGGER_JUDGE4    SH_RVA(0x107c850ULL, 0x0aea3e0ULL)
#define RVA_ADD_HOLD_PARAM          SH_RVA(0x107cf50ULL, 0x0aeb120ULL)

/* ========================================================================= */
/* Types & Module State                                                      */
/* ========================================================================= */

typedef struct { float x, y, z; } SHVec3;
typedef struct { float r, g, b, a; } SHColor;
typedef struct { SHVec3 center, extents; } SHBounds;

typedef struct {
    int32_t start_idx;
    int32_t end_idx;
    int32_t lane;
    int32_t start_time_ms;
    int32_t end_time_ms;
    int32_t angle1;
    int32_t angle2;
    bool    is_twin;
    int32_t tick_idxs[MAX_SLIDEHOLD_TICKS];
    size_t  tick_count;

    /* Per-attempt runtime state (cleared on reset & stage retry) */
    void   *start_tp;           /* Validated TriggerPlayData* (resolved once per stage) */
    void   *end_tp;             /* Validated TriggerPlayData* (resolved once per stage) */
    void   *start_obj;          /* TriggerObject* */
    void   *end_obj;            /* TriggerObject* */
    void   *tick_objs[MAX_SLIDEHOLD_TICKS];
    void   *arrow_sr;           /* Child SpriteRenderer* for primary arrow */
    void   *arrow_sr2;          /* Child SpriteRenderer* for twin secondary arrow */
    void   *hold_line;          /* LinearHoldLine* */
    bool    visuals_applied;
    int8_t  arrow_vis;          /* Cached visibility (-1=unknown, 0=hidden, 1=visible) */
    bool    arrow_clear;        /* True after Trigger::Fade clears arrow color */

    /* Per-attempt gameplay state */
    bool    started;
    bool    holding;
    bool    dropped;
    bool    completed;
    int32_t device_id;
    int32_t input_type;
} SlideHoldEntry;

typedef struct {
    bool    in_use;
    int32_t device_id;
    int32_t input_type;
    bool    prev_active;
    bool    prev_flick;
    bool    flick_consumed;
    int32_t last_degree;
    int32_t consumed_degree;
    int32_t consumed_time_ms;
} StickTracker;

static int s_color_type = 0;

#define MAX_NATIVE_JUDGED_PER_UPDATE 16
#define SH_DIRECT_IDX_CAP            8192
#define SH_OBJ_HASH_CAP              8192
#define SH_LANE_CACHE_CAP            8

typedef struct {
    const void *obj;
    int32_t     hold_idx;
    uint8_t     is_start;
} ShObjHashSlot;

typedef struct {
    void    *hold_sr;
    void    *last_trig_pd;
    uint32_t last_skin_ver;
    int      last_color_type;
    bool     last_is_sh;
} LaneHoldSpriteCache;

static SlideHoldEntry s_holds[MAX_CHART_SLIDEHOLDS];
static size_t s_hold_count = 0;
static size_t s_holding_count = 0;
static bool   s_links_resolved = false;
static void  *s_pending_tp_list = NULL;
static void  *s_resolved_tp_list = NULL;
static void  *s_active_create_sm = NULL;

/* O(1) trigger index & TriggerObject* caches (rebuilt on load/sort/visual apply). */
static int16_t       s_start_idx_map[SH_DIRECT_IDX_CAP];
static int16_t       s_end_idx_map[SH_DIRECT_IDX_CAP];
static uint8_t       s_any_idx_bits[SH_DIRECT_IDX_CAP / 8];
static bool          s_idx_maps_inited = false;
static ShObjHashSlot s_obj_hash[SH_OBJ_HASH_CAP];
static size_t        s_obj_hash_count = 0;
static bool          s_obj_hash_overflow = false;
static LaneHoldSpriteCache s_lane_hold_cache[SH_LANE_CACHE_CAP];
static void         *s_win_setting = NULL;
static int32_t       s_win_times[7];

static bool slidehold_debug_enabled(void) {
    static int s_enabled = -1;
    if (s_enabled < 0) {
        const char *env = getenv("THEATRHYTHM_SLIDEHOLD_DEBUG");
        s_enabled = (env && env[0] && env[0] != '0') ? 1 : 0;
    }
    return s_enabled != 0;
}

static StickTracker s_sticks[MAX_STICK_TRACKERS];
static bool    s_in_internal_judge = false;
static int32_t s_native_judged_idxs[MAX_NATIVE_JUDGED_PER_UPDATE];
static size_t  s_native_judged_count = 0;
static bool    s_auto_mode = false;

/* Persistent calibrated sprites across retries (re-validated via unity_object_alive) */
static void *s_spr_circle = NULL;
static void *s_spr_arrow = NULL;
static void *s_spr_end = NULL;
static void *s_spr_tick = NULL;
static void *s_spr_ribbon = NULL;
static void *s_nat_hold_start = NULL;
static float s_ribbon_y_ratio = 1.0f;
static bool  s_ribbon_calibrated_from_native = false;
static bool  s_sprites_calibrated = false;

/* Original trampolines populated by runtime_custom_charts.c */
bool (ABI *original_create_triggers_movenext)(void *state_machine) = NULL;
void (ABI *original_runtime_context_update)(void *context, void *arg1, int32_t cur_time, void *input_data) = NULL;
void (ABI *original_play_judge_effect)(void *controller, void *trigger_pd, void *method) = NULL;

/* Resolved IL2CPP / Unity helpers */
static int32_t (ABI *fn_get_sort_layer)(void *r, void *m) = NULL;
static void    (ABI *fn_set_sort_layer)(void *r, int32_t id, void *m) = NULL;
static int32_t (ABI *fn_get_sort_order)(void *r, void *m) = NULL;
static void    (ABI *fn_set_sort_order)(void *r, int32_t ord, void *m) = NULL;
static void    (ABI *fn_set_enabled)(void *r, bool en, void *m) = NULL;
static bool    (ABI *fn_get_enabled)(void *r, void *m) = NULL;
static void *  (ABI *fn_comp_get_transform)(void *c, void *m) = NULL;
static void    (ABI *fn_set_local_pos_inj)(void *t, const SHVec3 *v, void *m) = NULL;
static void    (ABI *fn_get_local_scl_inj)(void *t, SHVec3 *out, void *m) = NULL;
static void    (ABI *fn_set_local_scl_inj)(void *t, const SHVec3 *v, void *m) = NULL;
static void    (ABI *fn_set_local_euler)(void *t, SHVec3 euler, void *m) = NULL;
static void *  (ABI *fn_sr_get_sprite)(void *sr, void *m) = NULL;
static void    (ABI *fn_sr_set_sprite)(void *sr, void *spr, void *m) = NULL;
static void    (ABI *fn_sr_set_color_inj)(void *sr, const SHColor *c, void *m) = NULL;
static void    (ABI *fn_spr_get_bounds_inj)(void *spr, SHBounds *out, void *m) = NULL;
static void    (ABI *fn_se_sched_trigger)(void *player, int32_t se_type) = NULL;
static void    (ABI *fn_se_sched_hold)(void *player, int32_t se_type, int32_t end_index) = NULL;
static void *  (ABI *fn_mst_get_sprite)(void *mst, int32_t trig_type) = NULL;
static int32_t (ABI *fn_get_judge_time)(void *setting, int32_t grade) = NULL;
static bool    (ABI *fn_is_slide_degree)(void *setting, int32_t diff, bool sharp) = NULL;
static void    (ABI *fn_fixed_judge4)(void *playdata, int32_t idx, uint8_t judge, int32_t hit_time) = NULL;
static void    (ABI *fn_add_hold_param)(void *playdata, void *trig_pd, void *input_data) = NULL;

static const SHColor kWhite = {1.0f, 1.0f, 1.0f, 1.0f};
static const SHColor kClear = {1.0f, 1.0f, 1.0f, 0.0f};

/* ========================================================================= */
/* Small Utilities                                                           */
/* ========================================================================= */

static inline bool is_valid_ptr(const void *p) {
    return (((uintptr_t)p & 3ULL) == 0) && runtime_host_is_readable(p, 0x18) != 0;
}

static inline bool is_readable_ptr(const void *p, size_t size) {
    return (((uintptr_t)p & 3ULL) == 0) && runtime_host_is_readable(p, size) != 0;
}

static inline int32_t iabs32(int32_t v) { return v < 0 ? -v : v; }

static inline int32_t normalize_degree(int32_t deg) {
    if (deg == -360) deg = 0;
    deg %= 360;
    if (deg < 0) deg += 360;
    return deg;
}

static inline int32_t angle_distance(int32_t a, int32_t b) {
    int32_t d = iabs32(normalize_degree(a) - normalize_degree(b));
    return d > 180 ? (360 - d) : d;
}

/* Safely inspect either a 1D IL2CPP Array T[] (+0x10 == NULL) or List<T> (+0x10 == _items). */
static void **managed_ptr_seq(void *seq, int32_t *out_count) {
    if ((((uintptr_t)seq & 3ULL) != 0) || !runtime_host_is_readable(seq, 0x20)) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    void *p10 = *(void **)((char *)seq + 0x10);
    int32_t n18 = *(int32_t *)((char *)seq + 0x18);
    if (p10 == NULL) {
        if (n18 <= 0 || n18 > 65536 ||
            !runtime_host_is_readable((char *)seq + 0x20, (size_t)n18 * sizeof(void *))) {
            if (out_count) *out_count = 0;
            return NULL;
        }
        if (out_count) *out_count = n18;
        return (void **)((char *)seq + 0x20);
    }
    if ((((uintptr_t)p10 & 3ULL) == 0) && runtime_host_is_readable(p10, 0x20) &&
        *(void **)((char *)p10 + 0x10) == NULL) {
        int32_t cap = *(int32_t *)((char *)p10 + 0x18);
        if (n18 <= 0 || cap <= 0 || n18 > cap || cap > 65536 ||
            !runtime_host_is_readable((char *)p10 + 0x20, (size_t)n18 * sizeof(void *))) {
            if (out_count) *out_count = 0;
            return NULL;
        }
        if (out_count) *out_count = n18;
        return (void **)((char *)p10 + 0x20);
    }
    if (out_count) *out_count = 0;
    return NULL;
}

static inline void set_hold_holding(SlideHoldEntry *sh, bool val) {
    if (!sh || sh->holding == val) return;
    sh->holding = val;
    if (val) {
        ++s_holding_count;
    } else if (s_holding_count > 0) {
        --s_holding_count;
    }
}

static inline uint64_t sh_ptr_hash(const void *p) {
    uint64_t x = ((uint64_t)(uintptr_t)p) >> 4;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static void rebuild_index_maps(void) {
    memset(s_start_idx_map, 0xff, sizeof(s_start_idx_map));
    memset(s_end_idx_map, 0xff, sizeof(s_end_idx_map));
    memset(s_any_idx_bits, 0, sizeof(s_any_idx_bits));
    s_idx_maps_inited = true;

    for (size_t i = 0; i < s_hold_count; ++i) {
        const SlideHoldEntry *sh = &s_holds[i];
        if ((uint32_t)sh->start_idx < SH_DIRECT_IDX_CAP) {
            s_start_idx_map[sh->start_idx] = (int16_t)i;
            s_any_idx_bits[(uint32_t)sh->start_idx >> 3] |= (uint8_t)(1u << (sh->start_idx & 7));
        }
        if ((uint32_t)sh->end_idx < SH_DIRECT_IDX_CAP) {
            s_end_idx_map[sh->end_idx] = (int16_t)i;
            s_any_idx_bits[(uint32_t)sh->end_idx >> 3] |= (uint8_t)(1u << (sh->end_idx & 7));
        }
        for (size_t k = 0; k < sh->tick_count; ++k) {
            int32_t tidx = sh->tick_idxs[k];
            if ((uint32_t)tidx < SH_DIRECT_IDX_CAP) {
                s_any_idx_bits[(uint32_t)tidx >> 3] |= (uint8_t)(1u << (tidx & 7));
            }
        }
    }
}

static void clear_obj_hash(void) {
    if (s_obj_hash_count > 0 || s_obj_hash_overflow) {
        memset(s_obj_hash, 0, sizeof(s_obj_hash));
    }
    s_obj_hash_count = 0;
    s_obj_hash_overflow = false;
}

static void obj_hash_insert(const void *obj, size_t hold_idx, bool is_start) {
    if (!obj) return;
    if (s_obj_hash_count >= (SH_OBJ_HASH_CAP * 3u) / 4u) {
        s_obj_hash_overflow = true;
        return;
    }
    const size_t mask = SH_OBJ_HASH_CAP - 1u;
    size_t pos = (size_t)sh_ptr_hash(obj) & mask;
    for (size_t probe = 0; probe < SH_OBJ_HASH_CAP; ++probe) {
        ShObjHashSlot *slot = &s_obj_hash[pos];
        if (!slot->obj) {
            slot->obj = obj;
            slot->hold_idx = (int32_t)hold_idx;
            slot->is_start = is_start ? 1 : 0;
            ++s_obj_hash_count;
            return;
        }
        if (slot->obj == obj) {
            if (is_start && !slot->is_start) {
                slot->hold_idx = (int32_t)hold_idx;
                slot->is_start = 1;
            }
            return;
        }
        pos = (pos + 1u) & mask;
    }
    s_obj_hash_overflow = true;
}

static void rebuild_obj_hash(void) {
    clear_obj_hash();
    for (size_t i = 0; i < s_hold_count; ++i) {
        const SlideHoldEntry *sh = &s_holds[i];
        obj_hash_insert(sh->start_obj, i, true);
        obj_hash_insert(sh->end_obj, i, false);
        for (size_t k = 0; k < sh->tick_count; ++k) {
            obj_hash_insert(sh->tick_objs[k], i, false);
        }
    }
}

static const ShObjHashSlot *obj_hash_find(const void *obj) {
    if (!obj || s_obj_hash_count == 0) return NULL;
    const size_t mask = SH_OBJ_HASH_CAP - 1u;
    size_t pos = (size_t)sh_ptr_hash(obj) & mask;
    for (size_t probe = 0; probe < SH_OBJ_HASH_CAP; ++probe) {
        const ShObjHashSlot *slot = &s_obj_hash[pos];
        if (!slot->obj) return NULL;
        if (slot->obj == obj) return slot;
        pos = (pos + 1u) & mask;
    }
    return NULL;
}

static SlideHoldEntry *find_by_start_index(int32_t trigger_index) {
    if (s_hold_count == 0) return NULL;
    if ((uint32_t)trigger_index < SH_DIRECT_IDX_CAP && s_idx_maps_inited) {
        int16_t h = s_start_idx_map[trigger_index];
        return (h >= 0 && (size_t)h < s_hold_count) ? &s_holds[h] : NULL;
    }
    for (size_t i = 0; i < s_hold_count; ++i) {
        if (s_holds[i].start_idx == trigger_index) return &s_holds[i];
    }
    return NULL;
}

static SlideHoldEntry *find_by_end_index(int32_t trigger_index) {
    if (s_hold_count == 0) return NULL;
    if ((uint32_t)trigger_index < SH_DIRECT_IDX_CAP && s_idx_maps_inited) {
        int16_t h = s_end_idx_map[trigger_index];
        return (h >= 0 && (size_t)h < s_hold_count) ? &s_holds[h] : NULL;
    }
    for (size_t i = 0; i < s_hold_count; ++i) {
        if (s_holds[i].end_idx == trigger_index) return &s_holds[i];
    }
    return NULL;
}

static bool is_any_slidehold_index(int32_t trigger_index) {
    if (s_hold_count == 0) return false;
    if ((uint32_t)trigger_index < SH_DIRECT_IDX_CAP && s_idx_maps_inited) {
        return ((s_any_idx_bits[(uint32_t)trigger_index >> 3] >> (trigger_index & 7)) & 1u) != 0;
    }
    for (size_t i = 0; i < s_hold_count; ++i) {
        const SlideHoldEntry *sh = &s_holds[i];
        if (sh->start_idx == trigger_index || sh->end_idx == trigger_index) return true;
        for (size_t k = 0; k < sh->tick_count; ++k) {
            if (sh->tick_idxs[k] == trigger_index) return true;
        }
    }
    return false;
}

static SlideHoldEntry *find_by_start_obj(const void *obj) {
    if (!obj || s_hold_count == 0) return NULL;
    const ShObjHashSlot *slot = obj_hash_find(obj);
    if (slot) {
        return (slot->is_start && slot->hold_idx >= 0 && (size_t)slot->hold_idx < s_hold_count)
             ? &s_holds[slot->hold_idx] : NULL;
    }
    if (!s_obj_hash_overflow) return NULL;
    for (size_t i = 0; i < s_hold_count; ++i) {
        if (s_holds[i].start_obj == obj) return &s_holds[i];
    }
    return NULL;
}

static bool is_registered_slidehold_obj(const void *obj) {
    if (!obj || s_hold_count == 0) return false;
    if (obj_hash_find(obj) != NULL) return true;
    if (!s_obj_hash_overflow) return false;
    for (size_t i = 0; i < s_hold_count; ++i) {
        const SlideHoldEntry *sh = &s_holds[i];
        if (sh->start_obj == obj || sh->end_obj == obj) return true;
        for (size_t k = 0; k < sh->tick_count; ++k) {
            if (sh->tick_objs[k] == obj) return true;
        }
    }
    return false;
}

static StickTracker *get_stick_tracker(int32_t dev_id, int32_t in_type, bool create) {
    for (size_t i = 0; i < MAX_STICK_TRACKERS; ++i) {
        if (s_sticks[i].in_use && s_sticks[i].device_id == dev_id && s_sticks[i].input_type == in_type) {
            return &s_sticks[i];
        }
    }
    if (!create) return NULL;
    for (size_t i = 0; i < MAX_STICK_TRACKERS; ++i) {
        if (!s_sticks[i].in_use) {
            memset(&s_sticks[i], 0, sizeof(s_sticks[i]));
            s_sticks[i].in_use = true;
            s_sticks[i].device_id = dev_id;
            s_sticks[i].input_type = in_type;
            s_sticks[i].last_degree = -1;
            s_sticks[i].consumed_degree = -1;
            return &s_sticks[i];
        }
    }
    return NULL;
}

static void *find_native_input_state(void *ctx_input, int32_t dev_id, int32_t in_type) {
    if (!is_valid_ptr(ctx_input)) return NULL;
    void *arr = *(void **)((char *)ctx_input + 0x10);
    int32_t count = 0;
    void **items = managed_ptr_seq(arr, &count);
    if (!items) return NULL;
    for (int32_t i = 0; i < count; ++i) {
        void *st = items[i];
        if (!is_readable_ptr(st, 0x28)) continue;
        if (*(int32_t *)((char *)st + 0x1c) == dev_id &&
            *(int32_t *)((char *)st + 0x24) == in_type) {
            return st;
        }
    }
    return NULL;
}

/* ========================================================================= */
/* Init, Reset, & Stateless Chart Normalization / Parsing                    */
/* ========================================================================= */

void runtime_slidehold_init(unsigned char *module, int is_game_104) {
    if (!module) return;

    fn_get_sort_layer      = (void *)(module + RVA_RENDERER_GET_SORT_LAYER);
    fn_set_sort_layer      = (void *)(module + RVA_RENDERER_SET_SORT_LAYER);
    fn_get_sort_order      = (void *)(module + RVA_RENDERER_GET_SORT_ORDER);
    fn_set_sort_order      = (void *)(module + RVA_RENDERER_SET_SORT_ORDER);
    fn_set_enabled         = (void *)(module + RVA_RENDERER_SET_ENABLED);
    fn_get_enabled         = (void *)(module + RVA_RENDERER_GET_ENABLED);
    fn_comp_get_transform  = (void *)(module + RVA_COMP_GET_TRANSFORM);
    fn_set_local_pos_inj   = (void *)(module + RVA_XFORM_SET_LOCAL_POS_INJ);
    fn_get_local_scl_inj   = (void *)(module + RVA_XFORM_GET_LOCAL_SCL_INJ);
    fn_set_local_scl_inj   = (void *)(module + RVA_XFORM_SET_LOCAL_SCL_INJ);
    fn_set_local_euler     = (void *)(module + RVA_XFORM_SET_LOCAL_EULER);
    fn_sr_get_sprite       = (void *)(module + RVA_SPRITEREND_GET_SPRITE);
    fn_sr_set_sprite       = (void *)(module + RVA_SPRITEREND_SET_SPRITE);
    fn_sr_set_color_inj    = (void *)(module + RVA_SPRITEREND_SET_COL_INJ);
    fn_spr_get_bounds_inj  = (void *)(module + RVA_SPRITE_GET_BOUNDS_INJ);
    fn_se_sched_trigger    = (void *)(module + RVA_SE_SCHED_TRIGGER);
    fn_se_sched_hold       = (void *)(module + RVA_SE_SCHED_HOLD);
    fn_mst_get_sprite      = (void *)(module + RVA_MST_GET_TRIGGER_SPRITE);
    fn_get_judge_time      = (void *)(module + RVA_CONTEXT_GET_JUDGE_TIME);
    fn_is_slide_degree     = (void *)(module + RVA_CONTEXT_IS_SLIDE_DEG);
    fn_fixed_judge4        = (void *)(module + RVA_FIXED_TRIGGER_JUDGE4);
    fn_add_hold_param      = (void *)(module + RVA_ADD_HOLD_PARAM);
}

void runtime_slidehold_set_color_type(int color_type) {
    s_color_type = color_type;
}

void runtime_slidehold_reset_playback(void) {
    for (size_t i = 0; i < s_hold_count; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        sh->start_tp = NULL;
        sh->end_tp = NULL;
        sh->start_obj = NULL;
        sh->end_obj = NULL;
        sh->arrow_sr = NULL;
        sh->arrow_sr2 = NULL;
        sh->hold_line = NULL;
        sh->visuals_applied = false;
        sh->arrow_vis = -1;
        sh->arrow_clear = false;
        memset(sh->tick_objs, 0, sizeof(sh->tick_objs));
        sh->started = false;
        sh->holding = false;
        sh->dropped = false;
        sh->completed = false;
        sh->device_id = -1;
        sh->input_type = -1;
    }
    memset(s_sticks, 0, sizeof(s_sticks));
    memset(s_lane_hold_cache, 0, sizeof(s_lane_hold_cache));
    clear_obj_hash();
    s_holding_count = 0;
    s_win_setting = NULL;
    s_links_resolved = false;
    s_pending_tp_list = NULL;
    s_resolved_tp_list = NULL;
    s_active_create_sm = NULL;
    s_in_internal_judge = false;
    s_native_judged_count = 0;
    s_auto_mode = false;
}

void runtime_slidehold_reset(void) {
    runtime_slidehold_reset_playback();
    memset(s_holds, 0, sizeof(s_holds));
    s_hold_count = 0;
    rebuild_index_maps();
}

void runtime_slidehold_normalize_chart(uint8_t *cdata, size_t size) {
    if (!cdata || size < 44) return;
    uint32_t count = 0;
    memcpy(&count, cdata + 28, 4);
    if (size != 44 + (size_t)count * 56) return;

    bool lane_sh[4] = {false, false, false, false};
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t *rec = cdata + 44 + (size_t)i * 56;
        int32_t type = 0, lane = 0, param1 = 0;
        memcpy(&type, rec + 8, 4);
        memcpy(&lane, rec + 12, 4);
        memcpy(&param1, rec + 40, 4);
        if (lane < 0 || lane > 3) continue;

        if (type == TRIG_CUSTOM_SLIDEHOLD) {
            int32_t fixed_type = TRIG_LINHOLD_START;
            int32_t fixed_flag = SLIDEHOLD_PARAM_FLAG;
            memcpy(rec + 8, &fixed_type, 4);
            memcpy(rec + 40, &fixed_flag, 4);
            lane_sh[lane] = true;
        } else if (type == TRIG_LINHOLD_START && param1 == SLIDEHOLD_PARAM_FLAG) {
            lane_sh[lane] = true;
        } else if (type == TRIG_LINHOLD_START || type == TRIG_CRVHOLD_START) {
            lane_sh[lane] = false;
        } else if (type == TRIG_LINHOLD_END_TOUCH || type == TRIG_LINHOLD_END_SLIDE ||
                   type == TRIG_CRVHOLD_END_TOUCH || type == TRIG_CRVHOLD_END_SLIDE) {
            if (lane_sh[lane]) {
                int32_t fixed_end = TRIG_LINHOLD_END_TOUCH;
                memcpy(rec + 8, &fixed_end, 4);
                lane_sh[lane] = false;
            }
        }
    }
}

bool runtime_slidehold_load_chart(const uint8_t *cdata, size_t size) {
    runtime_slidehold_reset();
    if (!cdata || size < 44) return false;

    uint32_t count = 0;
    memcpy(&count, cdata + 28, 4);
    if (size != 44 + (size_t)count * 56) return false;

    int32_t open_idx[4] = {-1, -1, -1, -1};

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *rec = cdata + 44 + (size_t)i * 56;
        int32_t time_ms = 0, type = 0, lane = 0, a1 = 0, a2 = -1, param1 = 0;
        memcpy(&time_ms, rec + 4, 4);
        memcpy(&type, rec + 8, 4);
        memcpy(&lane, rec + 12, 4);
        memcpy(&a1, rec + 16, 4);
        memcpy(&a2, rec + 20, 4);
        memcpy(&param1, rec + 40, 4);
        if (lane < 0 || lane > 3) continue;

        if ((type == TRIG_CUSTOM_SLIDEHOLD || (type == TRIG_LINHOLD_START && param1 == SLIDEHOLD_PARAM_FLAG)) &&
            s_hold_count < MAX_CHART_SLIDEHOLDS) {
            SlideHoldEntry *sh = &s_holds[s_hold_count++];
            memset(sh, 0, sizeof(*sh));
            sh->start_idx = (int32_t)i;
            sh->end_idx = -1;
            sh->lane = lane;
            sh->start_time_ms = time_ms;
            sh->angle1 = normalize_degree(a1);
            sh->is_twin = (a2 >= 0);
            sh->angle2 = sh->is_twin ? normalize_degree(a2) : -1;
            sh->device_id = -1;
            sh->input_type = -1;
            open_idx[lane] = (int32_t)(s_hold_count - 1);
        } else if (type == TRIG_LINHOLD_PASS || type == TRIG_CRVHOLD_PASS) {
            int32_t hidx = open_idx[lane];
            if (hidx >= 0) {
                SlideHoldEntry *sh = &s_holds[hidx];
                if (sh->tick_count < MAX_SLIDEHOLD_TICKS) {
                    sh->tick_idxs[sh->tick_count++] = (int32_t)i;
                }
            }
        } else if (type == TRIG_LINHOLD_END_TOUCH || type == TRIG_LINHOLD_END_SLIDE ||
                   type == TRIG_CRVHOLD_END_TOUCH || type == TRIG_CRVHOLD_END_SLIDE) {
            int32_t hidx = open_idx[lane];
            if (hidx >= 0) {
                SlideHoldEntry *sh = &s_holds[hidx];
                sh->end_idx = (int32_t)i;
                sh->end_time_ms = time_ms;
                open_idx[lane] = -1;
            }
        } else if (type == TRIG_LINHOLD_START || type == TRIG_CRVHOLD_START) {
            /* Unrelated regular hold on same lane closes any dangling state */
            open_idx[lane] = -1;
        }
    }

    /* Compact out malformed holds missing a tail */
    size_t valid = 0;
    for (size_t i = 0; i < s_hold_count; ++i) {
        if (s_holds[i].end_idx >= 0) {
            if (valid != i) s_holds[valid] = s_holds[i];
            ++valid;
        }
    }
    s_hold_count = valid;
    rebuild_index_maps();
    printf("SlideHold: loaded %zu SlideHold groups for gameplay (color_type=%d)\n", s_hold_count, s_color_type);
    return s_hold_count > 0;
}

bool runtime_slidehold_is_start_trigger(int32_t trigger_index) {
    return find_by_start_index(trigger_index) != NULL;
}

bool runtime_slidehold_is_end_trigger(int32_t trigger_index) {
    return find_by_end_index(trigger_index) != NULL;
}

bool runtime_slidehold_is_internal_judge(void) {
    return s_in_internal_judge;
}

void runtime_slidehold_set_auto_mode(bool is_auto) {
    s_auto_mode = is_auto;
}

bool runtime_slidehold_is_auto_mode(void) {
    return s_auto_mode;
}

void runtime_slidehold_on_native_judge(int32_t trigger_index, uint8_t judge, int32_t hit_time) {
    (void)hit_time;
    if (s_in_internal_judge) return;

    /* Record non-miss trigger indices judged during Update so Update() can check whether a flick was consumed. */
    if (judge > JUDGE_NONE && judge < JUDGE_MISS && s_native_judged_count < MAX_NATIVE_JUDGED_PER_UPDATE) {
        s_native_judged_idxs[s_native_judged_count++] = trigger_index;
    }

    SlideHoldEntry *sh = find_by_start_index(trigger_index);
    if (sh) {
        if (judge >= JUDGE_MISS) {
            sh->dropped = true;
            set_hold_holding(sh, false);
        } else if (judge > JUDGE_NONE) {
            /* Auto mode or native completion */
            sh->started = true;
        }
        return;
    }

    SlideHoldEntry *end_sh = find_by_end_index(trigger_index);
    if (end_sh && judge > JUDGE_NONE) {
        set_hold_holding(end_sh, false);
        end_sh->completed = true;
    }
}

/* ========================================================================= */
/* Post-Sort TriggerPlaydata Link Resolution & Calibrated Visuals            */
/* ========================================================================= */

/* Native ContextPlaydata::Setup runs TriggerSort (0xadc910) before UpdateHoldLink (0xadcc60).
 * Simultaneous notes (e.g., a Slide on lane 0 at the exact timestamp of a SlideHold End on lane 2)
 * can change index order during TriggerSort. Resolve each SlideHoldEntry's start_idx, tick_idxs[],
 * and end_idx from the post-sort TriggerPlaydata list and its native holdNextIndex/holdEndIndex links. */
static void resolve_post_sort_slidehold_links(void *tp_list) {
    if (s_links_resolved || s_hold_count == 0 || !is_readable_ptr(tp_list, 0x20)) return;

    int32_t tp_count = 0;
    void **tp_items = managed_ptr_seq(tp_list, &tp_count);
    if (!tp_items || tp_count <= 0 || tp_count > 32768) return;

    int32_t *lin_idxs = (int32_t *)malloc((size_t)tp_count * sizeof(int32_t));
    uint8_t *lin_claimed = (uint8_t *)calloc((size_t)tp_count, 1);
    if (!lin_idxs || !lin_claimed) {
        free(lin_idxs);
        free(lin_claimed);
        return;
    }

    int32_t lin_count = 0;
    for (int32_t idx = 0; idx < tp_count; ++idx) {
        void *tp = tp_items[idx];
        if (!is_readable_ptr(tp, 0x40)) continue;
        if (*(uint8_t *)((char *)tp + 0x18) == TRIG_LINHOLD_START) {
            lin_idxs[lin_count++] = idx;
        }
    }

    size_t resolved_count = 0;
    for (size_t i = 0; i < s_hold_count; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        int32_t matched_start = -1;
        int32_t matched_c = -1;
        int32_t fallback_start = -1;
        int32_t fallback_c = -1;

        for (int32_t c = 0; c < lin_count; ++c) {
            if (lin_claimed[c]) continue;
            int32_t idx = lin_idxs[c];
            void *tp = tp_items[idx];
            int32_t ttime = *(int32_t *)((char *)tp + 0x14);
            int32_t tlane = *(int32_t *)((char *)tp + 0x1c);
            if (ttime != sh->start_time_ms || tlane != sh->lane) {
                continue;
            }

            if (fallback_start < 0) {
                fallback_start = idx;
                fallback_c = c;
            }
            int32_t cand_end = *(int32_t *)((char *)tp + 0x38);
            if (sh->end_time_ms > 0 && cand_end >= 0 && cand_end < tp_count &&
                is_readable_ptr(tp_items[cand_end], 0x20)) {
                int32_t cand_end_time = *(int32_t *)((char *)tp_items[cand_end] + 0x14);
                if (cand_end_time == sh->end_time_ms) {
                    matched_start = idx;
                    matched_c = c;
                    break;
                }
            } else if (sh->end_time_ms <= 0) {
                matched_start = idx;
                matched_c = c;
                break;
            }
        }
        if (matched_start < 0) {
            matched_start = fallback_start;
            matched_c = fallback_c;
        }
        if (matched_start < 0) continue;
        if (matched_c >= 0) lin_claimed[matched_c] = 1;

        void *start_tp = tp_items[matched_start];
        int32_t end_idx = *(int32_t *)((char *)start_tp + 0x38);
        sh->start_idx = matched_start;
        sh->start_tp = is_readable_ptr(start_tp, 0x50) ? start_tp : NULL;
        sh->end_tp = NULL;
        if (end_idx >= 0 && end_idx < tp_count && is_readable_ptr(tp_items[end_idx], 0x50)) {
            sh->end_idx = end_idx;
            sh->end_tp = tp_items[end_idx];
            sh->end_time_ms = *(int32_t *)((char *)sh->end_tp + 0x14);
        }

        sh->tick_count = 0;
        int32_t cur = *(int32_t *)((char *)start_tp + 0x34);
        while (cur >= 0 && cur < tp_count && cur != sh->end_idx && sh->tick_count < MAX_SLIDEHOLD_TICKS) {
            void *cur_tp = tp_items[cur];
            if (!is_readable_ptr(cur_tp, 0x38)) break;
            sh->tick_idxs[sh->tick_count++] = cur;
            int32_t next = *(int32_t *)((char *)cur_tp + 0x34);
            if (next == cur) break;
            cur = next;
        }
        ++resolved_count;
    }

    free(lin_idxs);
    free(lin_claimed);
    s_links_resolved = true;
    s_resolved_tp_list = tp_list;
    rebuild_index_maps();
    printf("SlideHold: resolved post-sort links %zu/%zu (tp_count=%d)\n", resolved_count, s_hold_count, tp_count);
}

static float sprite_bounds_width(void *spr, float fallback) {
    if (!runtime_unity_object_alive(spr) || !fn_spr_get_bounds_inj) return fallback;
    SHBounds b;
    memset(&b, 0, sizeof(b));
    fn_spr_get_bounds_inj(spr, &b, NULL);
    float w = b.extents.x * 2.0f;
    return (w > 0.05f && w < 20.0f) ? w : fallback;
}

static uint32_t s_slidehold_skin_ver = 0;

static void *build_slidehold_sprite(const uint8_t *png, size_t png_sz,
                                    int crop_x, int crop_y, int crop_w, int crop_h,
                                    float ppu, float pivot_x, float pivot_y, const char *tag) {
    return runtime_note_skins_create_recolored_sprite(
        png, png_sz, crop_x, crop_y, crop_w, crop_h, ppu, pivot_x, pivot_y, THEATRHYTHM_NOTE_SLIDEHOLD, tag);
}

static void refresh_existing_slidehold_visuals(void) {
    for (size_t i = 0; i < s_hold_count; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        if (!sh->visuals_applied) continue;
        if (is_valid_ptr(sh->start_obj) && runtime_unity_object_alive(s_spr_circle) && fn_sr_set_sprite) {
            void *sr = *(void **)((char *)sh->start_obj + 0x38);
            if (runtime_unity_object_alive(sr)) fn_sr_set_sprite(sr, s_spr_circle, NULL);
        }
        if (runtime_unity_object_alive(sh->arrow_sr) && runtime_unity_object_alive(s_spr_arrow) && fn_sr_set_sprite) {
            fn_sr_set_sprite(sh->arrow_sr, s_spr_arrow, NULL);
        }
        if (runtime_unity_object_alive(sh->arrow_sr2) && runtime_unity_object_alive(s_spr_arrow) && fn_sr_set_sprite) {
            fn_sr_set_sprite(sh->arrow_sr2, s_spr_arrow, NULL);
        }
        for (size_t k = 0; k < sh->tick_count; ++k) {
            void *tobj = sh->tick_objs[k];
            if (!is_valid_ptr(tobj) || !runtime_unity_object_alive(s_spr_tick) || !fn_sr_set_sprite) continue;
            void *sr = *(void **)((char *)tobj + 0x38);
            if (runtime_unity_object_alive(sr)) fn_sr_set_sprite(sr, s_spr_tick, NULL);
        }
        if (is_valid_ptr(sh->end_obj) && runtime_unity_object_alive(s_spr_end) && fn_sr_set_sprite) {
            void *sr = *(void **)((char *)sh->end_obj + 0x38);
            if (runtime_unity_object_alive(sr)) fn_sr_set_sprite(sr, s_spr_end, NULL);
        }
        if (is_valid_ptr(sh->hold_line) && runtime_unity_object_alive(s_spr_ribbon) && fn_sr_set_sprite) {
            void *sr = *(void **)((char *)sh->hold_line + 0x20);
            if (runtime_unity_object_alive(sr)) fn_sr_set_sprite(sr, s_spr_ribbon, NULL);
        }
    }
}

static void ensure_calibrated_sprites(void *mst, void *sample_native_line_spr) {
    static int s_slidehold_color_type = -1;
    if (fn_mst_get_sprite && mst) {
        void *hs = fn_mst_get_sprite(mst, TRIG_LINHOLD_START);
        if (runtime_unity_object_alive(hs)) {
            s_nat_hold_start = hs;
        }
    }
    uint32_t skin_ver = theatrhythm_note_skin_version();
    int color_type = runtime_note_skins_get_color_type();
    bool skin_dirty = (skin_ver != s_slidehold_skin_ver) || (color_type != s_slidehold_color_type);
    bool need_ribbon_recal = runtime_unity_object_alive(sample_native_line_spr) && !s_ribbon_calibrated_from_native;
    if (s_sprites_calibrated &&
        runtime_unity_object_alive(s_spr_circle) &&
        runtime_unity_object_alive(s_spr_arrow) &&
        runtime_unity_object_alive(s_spr_end) &&
        runtime_unity_object_alive(s_spr_tick) &&
        runtime_unity_object_alive(s_spr_ribbon) &&
        !need_ribbon_recal && !skin_dirty) {
        return;
    }

    void *nat_hold  = s_nat_hold_start;
    void *nat_touch = fn_mst_get_sprite ? fn_mst_get_sprite(mst, TRIG_TOUCH) : NULL;
    void *nat_end   = fn_mst_get_sprite ? fn_mst_get_sprite(mst, TRIG_LINHOLD_END_TOUCH) : NULL;
    void *nat_tick  = fn_mst_get_sprite ? fn_mst_get_sprite(mst, TRIG_LINHOLD_PASS) : NULL;

    /* Native TriggerSprite_00 (Skin A) & TriggerSprite_01 (Skin B) share identical PPU=100.0 sizes:
     *  - Trigger_HoldStart / TwinSlideBase: 220x220 (2.20 units), pivot=(0.5, 0.5)
     *  - Trigger_TwinSlideArrow:            128x256 (1.28x2.56),  pivot=(0.5, 0.1800)
     *  - Trigger_HoldEnd:                   128x128 (1.28 units), pivot=(0.5, 0.5)
     *  - Trigger_HoldMidPoint:               64x64  (0.64 units), pivot=(0.5, 0.5)
     *  - Trigger_HoldLine:                   24x104 (0.24x1.04),  pivot=(0.5, 0.5)
     */
    float hold_start_w = sprite_bounds_width(nat_hold, 2.20f);
    if (!runtime_unity_object_alive(nat_hold) && runtime_unity_object_alive(nat_touch)) {
        hold_start_w = sprite_bounds_width(nat_touch, 2.24f) * (220.0f / 224.0f);
    }
    float end_w        = sprite_bounds_width(nat_end, 1.28f * (hold_start_w / 2.20f));
    float tick_w       = sprite_bounds_width(nat_tick, 0.64f * (hold_start_w / 2.20f));
    float ppu_circle   = 220.0f / hold_start_w;
    float ppu_end      = 128.0f / end_w;
    float ppu_tick     = 64.0f  / tick_w;

    /* Extract exact bounds & pivot from native linearHoldLineSprite (Trigger_HoldLine: 24x104, PPU=100). */
    float line_w = 0.24f;
    float line_h = 1.04f;
    float piv_x  = 0.5f;
    float piv_y  = 0.5f;
    bool has_native_line = runtime_unity_object_alive(sample_native_line_spr) && fn_spr_get_bounds_inj;
    if (has_native_line) {
        SHBounds b_line;
        memset(&b_line, 0, sizeof(b_line));
        fn_spr_get_bounds_inj(sample_native_line_spr, &b_line, NULL);
        float bw = b_line.extents.x * 2.0f;
        float bh = b_line.extents.y * 2.0f;
        if (bw > 0.05f && bw < 20.0f) {
            line_w = bw;
            piv_x = 0.5f - (b_line.center.x / line_w);
            if (piv_x < -0.5f || piv_x > 1.5f) piv_x = 0.5f;
        }
        if (bh > 0.02f && bh < 20.0f) {
            line_h = bh;
            piv_y = 0.5f - (b_line.center.y / line_h);
            if (piv_y < -0.5f || piv_y > 1.5f) piv_y = 0.5f;
        }
    }
    float ppu_ribbon = 24.0f / line_w;

    if (!runtime_unity_object_alive(s_spr_circle) || skin_dirty) {
        s_spr_circle = build_slidehold_sprite(
            s_asset_slidehold_circle, sizeof(s_asset_slidehold_circle),
            0, 0, 0, 0, ppu_circle, 0.5f, 0.5f, "SlideHold/Circle");
    }
    if (!runtime_unity_object_alive(s_spr_arrow) || skin_dirty) {
        s_spr_arrow = build_slidehold_sprite(
            s_asset_slidehold_arrow_alt, sizeof(s_asset_slidehold_arrow_alt),
            0, 0, 128, 256, ppu_circle, 0.5f, 0.1800f, "SlideHold/ArrowAlt");
    }
    if (!runtime_unity_object_alive(s_spr_end) || skin_dirty) {
        s_spr_end = build_slidehold_sprite(
            s_asset_slidehold_end_ring, sizeof(s_asset_slidehold_end_ring),
            0, 0, 0, 0, ppu_end, 0.5f, 0.5f, "SlideHold/EndRing");
    }
    if (!runtime_unity_object_alive(s_spr_tick) || skin_dirty) {
        s_spr_tick = build_slidehold_sprite(
            s_asset_slidehold_tick, sizeof(s_asset_slidehold_tick),
            0, 0, 0, 0, ppu_tick, 0.5f, 0.5f, "SlideHold/Tick");
    }
    if (!runtime_unity_object_alive(s_spr_ribbon) || need_ribbon_recal || skin_dirty) {
        s_spr_ribbon = build_slidehold_sprite(
            s_asset_slidehold_ribbon_h, sizeof(s_asset_slidehold_ribbon_h),
            0, 0, 24, 104, ppu_ribbon, piv_x, piv_y, "SlideHold/Ribbon");
        if (has_native_line) s_ribbon_calibrated_from_native = true;
    }

    float custom_h = 104.0f / ppu_ribbon;
    s_ribbon_y_ratio = (custom_h > 0.001f) ? (line_h / custom_h) : 1.0f;
    s_sprites_calibrated = true;
    s_slidehold_skin_ver = skin_ver;
    s_slidehold_color_type = color_type;
    if (skin_dirty) {
        refresh_existing_slidehold_visuals();
    }

    printf("SlideHold: calibrated PPU (nat=(%.3f,%.3f,%.3f,line=%.3fx%.3f) circle=%.2f end=%.2f tick=%.2f ribbon=%.2f piv=(%.2f,%.2f) y_ratio=%.3f ver=%u)\n",
           hold_start_w, end_w, tick_w, line_w, line_h, ppu_circle, ppu_end, ppu_tick, ppu_ribbon, piv_x, piv_y, s_ribbon_y_ratio, skin_ver);
}

static void *attach_arrow_child(void *base_sr, void *parent_tr, int32_t angle_deg, const char *name) {
    if (!runtime_unity_object_alive(base_sr) || !runtime_unity_object_alive(parent_tr) || !runtime_unity_object_alive(s_spr_arrow)) {
        return NULL;
    }
    void *arrow_sr = runtime_create_child_sprite_renderer(base_sr, parent_tr, name);
    if (!runtime_unity_object_alive(arrow_sr)) return NULL;

    fn_sr_set_sprite(arrow_sr, s_spr_arrow, NULL);
    fn_sr_set_color_inj(arrow_sr, &kWhite, NULL);
    if (fn_get_sort_layer && fn_set_sort_layer) {
        fn_set_sort_layer(arrow_sr, fn_get_sort_layer(base_sr, NULL), NULL);
    }
    if (fn_get_sort_order && fn_set_sort_order) {
        fn_set_sort_order(arrow_sr, fn_get_sort_order(base_sr, NULL) + 1, NULL);
    }
    if (fn_comp_get_transform && fn_set_local_euler) {
        void *arrow_tr = fn_comp_get_transform(arrow_sr, NULL);
        if (runtime_unity_object_alive(arrow_tr)) {
            if (fn_set_local_pos_inj) {
                SHVec3 zero = {0.0f, 0.0f, 0.0f};
                fn_set_local_pos_inj(arrow_tr, &zero, NULL);
            }
            if (fn_set_local_scl_inj) {
                SHVec3 one = {1.0f, 1.0f, 1.0f};
                fn_set_local_scl_inj(arrow_tr, &one, NULL);
            }
            float rot_z = (float)(360 - normalize_degree(angle_deg));
            if (rot_z >= 360.0f) rot_z -= 360.0f;
            fn_set_local_euler(arrow_tr, (SHVec3){0.0f, 0.0f, rot_z}, NULL);
        }
    }
    if (fn_get_enabled && fn_set_enabled) {
        fn_set_enabled(arrow_sr, fn_get_enabled(base_sr, NULL), NULL);
    }
    return arrow_sr;
}

static void apply_slidehold_visuals(void *mst) {
    if (!is_readable_ptr(mst, 0x90) || s_hold_count == 0) return;

    void *triggers_seq = *(void **)((char *)mst + 0x70);
    void *lines_seq    = *(void **)((char *)mst + 0x88);
    int32_t trig_count = 0, line_count = 0;
    void **trig_items  = managed_ptr_seq(triggers_seq, &trig_count);
    void **line_items  = managed_ptr_seq(lines_seq, &line_count);
    if (!trig_items || trig_count <= 0) return;

    /* Read native linearHoldLineSprite directly from mst->setting (+0x40)->linearHoldLineSprite (+0x50)
     * (verified in createLinearHoldLine @ 0x271e9fb), with fallback to line_items. */
    void *sample_native_line_spr = NULL;
    void *mst_setting = *(void **)((char *)mst + 0x40);
    if (is_readable_ptr(mst_setting, 0x58)) {
        void *cfg_spr = *(void **)((char *)mst_setting + 0x50);
        if (runtime_unity_object_alive(cfg_spr) && cfg_spr != s_spr_ribbon) {
            sample_native_line_spr = cfg_spr;
        }
    }
    if (!sample_native_line_spr && line_items && line_count > 0 && fn_sr_get_sprite) {
        for (int32_t li = 0; li < line_count; ++li) {
            void *ln = line_items[li];
            if (!is_readable_ptr(ln, 0x28)) continue;
            void *sr = *(void **)((char *)ln + 0x20);
            void *spr = runtime_unity_object_alive(sr) ? fn_sr_get_sprite(sr, NULL) : NULL;
            if (runtime_unity_object_alive(spr) && spr != s_spr_ribbon) {
                sample_native_line_spr = spr;
                break;
            }
        }
    }

    ensure_calibrated_sprites(mst, sample_native_line_spr);

    size_t visuals_done = 0;
    size_t lines_matched = 0;
    for (size_t i = 0; i < s_hold_count; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        if (sh->visuals_applied) {
            ++visuals_done;
            if (sh->hold_line) ++lines_matched;
            continue;
        }
        if (sh->start_idx < 0 || sh->start_idx >= trig_count ||
            sh->end_idx < 0 || sh->end_idx >= trig_count) {
            continue;
        }

        void *start_obj = trig_items[sh->start_idx];
        void *end_obj   = trig_items[sh->end_idx];
        sh->start_obj = is_readable_ptr(start_obj, 0x40) ? start_obj : NULL;
        sh->end_obj   = is_readable_ptr(end_obj, 0x40) ? end_obj : NULL;

        if (sh->start_obj) {
            void *start_tr = *(void **)((char *)sh->start_obj + 0x28);
            void *start_sr = *(void **)((char *)sh->start_obj + 0x38);
            if (runtime_unity_object_alive(start_tr) && fn_set_local_euler) {
                /* Native CreateTriggers sets start_tr localRotation to (360 - angle1) on every
                 * non-TWINSLIDE trigger; reset it before parenting SHArrow/SHArrow2 so child
                 * arrows are not double-rotated. */
                fn_set_local_euler(start_tr, (SHVec3){0.0f, 0.0f, 0.0f}, NULL);
            }
            if (runtime_unity_object_alive(start_sr) && runtime_unity_object_alive(s_spr_circle)) {
                fn_sr_set_sprite(start_sr, s_spr_circle, NULL);
                fn_sr_set_color_inj(start_sr, &kWhite, NULL);
                sh->arrow_sr = attach_arrow_child(start_sr, start_tr, sh->angle1, "SHArrow");
                if (sh->is_twin && sh->angle2 >= 0) {
                    sh->arrow_sr2 = attach_arrow_child(start_sr, start_tr, sh->angle2, "SHArrow2");
                }
            }
        }

        for (size_t k = 0; k < sh->tick_count; ++k) {
            int32_t tidx = sh->tick_idxs[k];
            if (tidx >= 0 && tidx < trig_count && is_readable_ptr(trig_items[tidx], 0x40)) {
                sh->tick_objs[k] = trig_items[tidx];
                void *tsr = *(void **)((char *)trig_items[tidx] + 0x38);
                if (runtime_unity_object_alive(tsr) && runtime_unity_object_alive(s_spr_tick)) {
                    fn_sr_set_sprite(tsr, s_spr_tick, NULL);
                    fn_sr_set_color_inj(tsr, &kWhite, NULL);
                }
            }
        }

        if (sh->end_obj) {
            void *end_sr = *(void **)((char *)sh->end_obj + 0x38);
            if (runtime_unity_object_alive(end_sr) && runtime_unity_object_alive(s_spr_end)) {
                fn_sr_set_sprite(end_sr, s_spr_end, NULL);
                fn_sr_set_color_inj(end_sr, &kWhite, NULL);
            }
        }

        if (line_items && line_count > 0 && runtime_unity_object_alive(s_spr_ribbon)) {
            for (int32_t li = 0; li < line_count; ++li) {
                void *ln = line_items[li];
                if (!is_readable_ptr(ln, 0x68)) continue;
                void *ln_start_obj = *(void **)((char *)ln + 0x28);
                int32_t ln_end_idx = *(int32_t *)((char *)ln + 0x60);
                if (sh->start_obj ? (ln_start_obj != sh->start_obj) : (ln_end_idx != sh->end_idx)) {
                    continue;
                }

                sh->hold_line = ln;
                ++lines_matched;
                void *line_sr = *(void **)((char *)ln + 0x20);
                if (runtime_unity_object_alive(line_sr)) {
                    fn_sr_set_sprite(line_sr, s_spr_ribbon, NULL);
                    fn_sr_set_color_inj(line_sr, &kWhite, NULL);
                }

                /* Calibrate vertical thickness on BOTH _originalScale.y (+0x3c) & CachedTransform (+0x58). */
                float *orig_scale_y = (float *)((char *)ln + 0x3c);
                *orig_scale_y *= s_ribbon_y_ratio;
                void *line_tr = *(void **)((char *)ln + 0x58);
                if (runtime_unity_object_alive(line_tr) && fn_get_local_scl_inj && fn_set_local_scl_inj) {
                    SHVec3 scl = {1.0f, 1.0f, 1.0f};
                    fn_get_local_scl_inj(line_tr, &scl, NULL);
                    scl.y *= s_ribbon_y_ratio;
                    fn_set_local_scl_inj(line_tr, &scl, NULL);
                }
                break;
            }
        }

        sh->visuals_applied = true;
        sh->arrow_vis = -1;
        sh->arrow_clear = false;
        ++visuals_done;
    }

    rebuild_obj_hash();
    printf("SlideHold: visuals applied %zu/%zu (lines=%zu resolved=%d trigs=%d line_objs=%d)\n",
           visuals_done, s_hold_count, lines_matched, s_links_resolved ? 1 : 0, trig_count, line_count);
}

ABI int hook_slidehold_create_triggers_movenext(HookRegs *r) {
    void *sm = (void *)r->rdi;
    if (!sm || !original_create_triggers_movenext) return 0;

    int32_t entry_state = *(int32_t *)sm;
    if (entry_state == -1 && s_hold_count > 0) {
        /* MusicStageTrigger::CreateTriggers initializes state = -1 (0x271991d); reset cleanly
         * before a fresh attempt / Retry. */
        runtime_slidehold_reset_playback();
    }

    if (s_hold_count > 0) {
        void *pre_tp = *(void **)((char *)sm + 0x48);
        if (is_valid_ptr(pre_tp)) {
            s_pending_tp_list = pre_tp;
        }
    }

    void *prev_sm = s_active_create_sm;
    s_active_create_sm = sm;
    bool cont = original_create_triggers_movenext(sm);
    s_active_create_sm = prev_sm;

    if (s_hold_count > 0) {
        /* CreateTriggers$$MoveNext zeroes sm + 0x48 right before returning with *sm == -2 (0x271ba16),
         * often synchronously (-1 -> -2 in one call). Capture it mid-call in trigger_set_visible
         * or from pre/post sm + 0x48 so post-sort links always resolve before visuals apply. */
        void *post_tp = *(void **)((char *)sm + 0x48);
        if (is_valid_ptr(post_tp)) {
            s_pending_tp_list = post_tp;
        }

        if (*(int32_t *)sm == -2) {
            void *tp_list = is_valid_ptr(post_tp) ? post_tp : s_pending_tp_list;
            resolve_post_sort_slidehold_links(tp_list);
            s_pending_tp_list = NULL;
            void *mst = *(void **)((char *)sm + 0x18);
            runtime_note_skins_on_triggers_created(mst);
            apply_slidehold_visuals(mst);
        }
    } else if (*(int32_t *)sm == -2) {
        void *mst = *(void **)((char *)sm + 0x18);
        runtime_note_skins_on_triggers_created(mst);
    }

    r->rax = cont ? 1ULL : 0ULL;
    return 1;
}

ABI int hook_slidehold_trigger_set_visible(HookRegs *r) {
    if (s_hold_count == 0) return 0;
    if (s_active_create_sm && !s_pending_tp_list && is_valid_ptr(s_active_create_sm)) {
        void *tp_in_sm = *(void **)((char *)s_active_create_sm + 0x48);
        if (is_valid_ptr(tp_in_sm)) {
            s_pending_tp_list = tp_in_sm;
        }
    }
    if (s_obj_hash_count == 0 && !s_obj_hash_overflow) return 0;
    SlideHoldEntry *sh = find_by_start_obj((const void *)r->rdi);
    if (!sh) return 0;

    bool vis = ((r->rsi & 0xffULL) != 0);
    int8_t want_vis = vis ? 1 : 0;
    if (sh->arrow_vis == want_vis && (!vis || !sh->arrow_clear)) return 0;

    void *arrows[2] = {sh->arrow_sr, sh->arrow_sr2};
    for (int i = 0; i < 2; ++i) {
        if (!arrows[i] || !runtime_unity_object_alive(arrows[i])) continue;
        if (fn_set_enabled) fn_set_enabled(arrows[i], vis, NULL);
        if (vis && fn_sr_set_color_inj) fn_sr_set_color_inj(arrows[i], &kWhite, NULL);
    }
    sh->arrow_vis = want_vis;
    if (vis) sh->arrow_clear = false;
    return 0;
}

ABI int hook_slidehold_trigger_fade(HookRegs *r) {
    if (s_hold_count == 0 || (s_obj_hash_count == 0 && !s_obj_hash_overflow)) return 0;
    SlideHoldEntry *sh = find_by_start_obj((const void *)r->rdi);
    if (!sh) return 0;
    if (sh->arrow_vis == 0 && sh->arrow_clear) return 0;

    void *arrows[2] = {sh->arrow_sr, sh->arrow_sr2};
    for (int i = 0; i < 2; ++i) {
        if (!arrows[i] || !runtime_unity_object_alive(arrows[i])) continue;
        if (fn_sr_set_color_inj) fn_sr_set_color_inj(arrows[i], &kClear, NULL);
        if (fn_set_enabled) fn_set_enabled(arrows[i], false, NULL);
    }
    sh->arrow_vis = 0;
    sh->arrow_clear = true;
    return 0;
}

ABI int hook_slidehold_trigger_set_simple_type(HookRegs *r) {
    if (s_hold_count > 0 && is_registered_slidehold_obj((const void *)r->rdi)) {
        r->rax = 0;
        return 1;
    }
    runtime_note_skins_on_set_simple_type((void *)r->rdi);
    return 0;
}

ABI int hook_slidehold_trigger_set_simple_color(HookRegs *r) {
    if (s_hold_count > 0 && is_registered_slidehold_obj((const void *)r->rdi)) {
        r->rax = 0;
        return 1;
    }
    runtime_note_skins_sync_simple_colors();
    return 0;
}

/* ========================================================================= */
/* SlideHold Audio SFX Hook                                                  */
/* ========================================================================= */

ABI int hook_slidehold_se(HookRegs *r) {
    if (s_hold_count == 0) return 0;
    void *player = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    int32_t explicit_se = (int32_t)r->rdx;

    if (!is_valid_ptr(player) || !is_readable_ptr(trig_pd, 0x50) || explicit_se != 0) return 0;

    int32_t trigger_index = *(int32_t *)((char *)trig_pd + 0x10);
    SlideHoldEntry *sh = find_by_start_index(trigger_index);
    if (!sh) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge >= JUDGE_MISS) {
        r->rax = 0;
        return 1;
    }

    int se_type = 1; /* 1=Slide, 2=SlideUp, 3=SlideDown */
    int32_t a1 = sh->angle1;
    if (sh->is_twin && sh->angle2 >= 0) {
        bool up1 = (a1 == 0 || a1 == 45 || a1 == 315);
        bool up2 = (sh->angle2 == 0 || sh->angle2 == 45 || sh->angle2 == 315);
        bool dn1 = (a1 == 135 || a1 == 180 || a1 == 225);
        bool dn2 = (sh->angle2 == 135 || sh->angle2 == 180 || sh->angle2 == 225);
        if ((up1 || up2) && !(dn1 || dn2)) se_type = 2;
        else if ((dn1 || dn2) && !(up1 || up2)) se_type = 3;
    } else {
        if (a1 == 90 || a1 == 270) se_type = 1;
        else if (a1 < 90 || a1 > 270) se_type = 2;
        else se_type = 3;
    }

    if (fn_se_sched_hold && sh->end_idx >= 0) {
        fn_se_sched_hold(player, 5, sh->end_idx); /* Hold loop SE paired cleanly by end_idx */
    }
    if (fn_se_sched_trigger) {
        fn_se_sched_trigger(player, se_type);
    }
    r->rax = 0;
    return 1; /* Suppress native Touch SE click */
}

/* ========================================================================= */
/* HoldParam Maintenance & Judgement Helpers                                 */
/* ========================================================================= */

static void remove_hold_param(void *playdata, int32_t start_idx, int32_t end_idx) {
    if (!is_readable_ptr(playdata, 0x28) || (start_idx < 0 && end_idx < 0)) return;
    void *hold_list = *(void **)((char *)playdata + 0x20);
    int32_t count = 0;
    void **items = managed_ptr_seq(hold_list, &count);
    if (!items || count <= 0) return;

    for (int32_t i = count - 1; i >= 0; --i) {
        void *hp = items[i];
        if (!is_readable_ptr(hp, 0x20)) continue;
        int32_t hp_start = *(int32_t *)((char *)hp + 0x18);
        int32_t hp_end   = *(int32_t *)((char *)hp + 0x1c);
        if ((start_idx >= 0 && hp_start == start_idx) || (end_idx >= 0 && hp_end == end_idx)) {
            runtime_list_remove_at(hold_list, i);
        }
    }
}

static void insert_hold_param(void *playdata, void *start_tp, void *input_data,
                              int32_t dev_id, int32_t in_type, int32_t start_idx, int32_t end_idx) {
    if (!fn_add_hold_param || !is_readable_ptr(playdata, 0x28) || !is_readable_ptr(start_tp, 0x20) ||
        !is_readable_ptr(input_data, 0x20)) {
        return;
    }
    fn_add_hold_param(playdata, start_tp, input_data);
    void *hold_list = *(void **)((char *)playdata + 0x20);
    int32_t count = 0;
    void **items = managed_ptr_seq(hold_list, &count);
    if (!items) return;
    for (int32_t i = count - 1; i >= 0; --i) {
        void *hp = items[i];
        if (!is_valid_ptr(hp)) continue;
        int32_t hp_start = *(int32_t *)((char *)hp + 0x18);
        int32_t hp_end   = *(int32_t *)((char *)hp + 0x1c);
        if ((start_idx >= 0 && hp_start == start_idx) || (end_idx >= 0 && hp_end == end_idx)) {
            *(int32_t *)((char *)hp + 0x14) = in_type;
            *(int32_t *)((char *)hp + 0x2c) = dev_id;
            break;
        }
    }
}

static int32_t judge_window(void *setting, int32_t grade, int32_t fallback) {
    if (grade >= 0 && grade < 7 && s_win_setting && setting == s_win_setting) {
        return s_win_times[grade] > 0 ? s_win_times[grade] : fallback;
    }
    if (fn_get_judge_time && is_valid_ptr(setting)) {
        if (setting != s_win_setting) {
            static const int32_t kDefaultWin[7] = {0, 27, 46, 81, 142, 191, 191};
            for (int32_t g = 0; g < 7; ++g) {
                int32_t w = (g == 0) ? 0 : fn_get_judge_time(setting, g);
                s_win_times[g] = (w > 0 && w < 2000) ? w : kDefaultWin[g];
            }
            s_win_setting = setting;
            if (grade >= 0 && grade < 7) {
                return s_win_times[grade] > 0 ? s_win_times[grade] : fallback;
            }
        }
        int32_t w = fn_get_judge_time(setting, grade);
        if (w > 0 && w < 2000) return w;
    }
    return fallback;
}

static uint8_t timing_grade(void *setting, int32_t abs_dt) {
    if (abs_dt <= judge_window(setting, JUDGE_RAINBOW,  27)) return JUDGE_RAINBOW;
    if (abs_dt <= judge_window(setting, JUDGE_CRITICAL, 46)) return JUDGE_CRITICAL;
    if (abs_dt <= judge_window(setting, JUDGE_GREAT,    81)) return JUDGE_GREAT;
    if (abs_dt <= judge_window(setting, JUDGE_GOOD,    142)) return JUDGE_GOOD;
    if (abs_dt <= judge_window(setting, JUDGE_BAD,     191)) return JUDGE_BAD;
    return JUDGE_NONE;
}

static bool angle_matches(void *setting, int32_t stick_deg, int32_t target_deg) {
    int32_t diff = angle_distance(stick_deg, target_deg);
    if (fn_is_slide_degree && is_valid_ptr(setting)) {
        return fn_is_slide_degree(setting, diff, false);
    }
    return diff <= 45;
}

static bool hold_angle_matches(void *setting, const SlideHoldEntry *sh, int32_t stick_deg) {
    if (angle_matches(setting, stick_deg, sh->angle1)) return true;
    if (sh->is_twin && sh->angle2 >= 0 && angle_matches(setting, stick_deg, sh->angle2)) return true;
    return false;
}

/* Allow slightly wider cone (<= 60 deg) while sustaining an active hold so minor thumb wobble
 * near a 45-degree boundary doesn't falsely drop ticks, while orthogonal 90-deg turns still drop. */
static bool hold_sustain_angle_matches(void *setting, const SlideHoldEntry *sh, int32_t stick_deg) {
    if (hold_angle_matches(setting, sh, stick_deg)) return true;
    if (angle_distance(stick_deg, sh->angle1) <= 60) return true;
    if (sh->is_twin && sh->angle2 >= 0 && angle_distance(stick_deg, sh->angle2) <= 60) return true;
    return false;
}

static bool is_flick_trigger_type(uint8_t ttype) {
    return ttype == TRIG_SLIDE || ttype == TRIG_TWINSLIDE ||
           ttype == TRIG_LINHOLD_END_SLIDE || ttype == TRIG_CRVHOLD_END_SLIDE;
}

static void call_judge4_safe(void *playdata, int32_t idx, uint8_t grade, int32_t cur_time) {
    if (!fn_fixed_judge4 || !is_valid_ptr(playdata) || idx < 0 || grade == JUDGE_NONE) return;
    s_in_internal_judge = true;
    fn_fixed_judge4(playdata, idx, grade, cur_time);
    s_in_internal_judge = false;
}

static void finish_hold_release(void *setting, void *playdata, SlideHoldEntry *sh, int32_t cur_time) {
    if (!sh || !sh->holding) return;
    set_hold_holding(sh, false);
    remove_hold_param(playdata, sh->start_idx, sh->end_idx);

    int32_t bad_win = judge_window(setting, JUDGE_GOOD, 142);
    int32_t dt = iabs32(cur_time - sh->end_time_ms);
    if (dt <= bad_win) {
        uint8_t grade = timing_grade(setting, dt);
        if (grade != JUDGE_NONE) {
            call_judge4_safe(playdata, sh->end_idx, grade, cur_time);
        }
        sh->completed = true;
        if (slidehold_debug_enabled()) {
            printf("SlideHold: END HIT idx=%d end_t=%d cur=%d dt=%d grade=%u\n",
                   sh->end_idx, sh->end_time_ms, cur_time, cur_time - sh->end_time_ms, (unsigned)grade);
        }
    } else {
        /* Early release drops the hold; missing HoldParam fails remaining ticks/tail cleanly. */
        sh->dropped = true;
        if (slidehold_debug_enabled()) {
            printf("SlideHold: END EARLY_DROP idx=%d end_t=%d cur=%d dt=%d bad=%d\n",
                   sh->end_idx, sh->end_time_ms, cur_time, cur_time - sh->end_time_ms, bad_win);
        }
    }
}

static void drop_hold_by_drift(void *playdata, SlideHoldEntry *sh, int32_t cur_time, int32_t degree) {
    if (!sh || !sh->holding) return;
    set_hold_holding(sh, false);
    sh->dropped = true;
    remove_hold_param(playdata, sh->start_idx, sh->end_idx);
    if (slidehold_debug_enabled()) {
        printf("SlideHold: DRIFT_DROP idx=%d t=[%d..%d] cur=%d deg=%d exp=%d/%d\n",
               sh->end_idx, sh->start_time_ms, sh->end_time_ms, cur_time, degree, sh->angle1, sh->angle2);
    }
}

static SlideHoldEntry *find_active_hold_by_stick(int32_t dev_id, int32_t in_type) {
    if (s_holding_count == 0) return NULL;
    for (size_t i = 0; i < s_hold_count; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        if (sh->holding && !sh->completed && !sh->dropped &&
            sh->device_id == dev_id && sh->input_type == in_type) {
            return sh;
        }
    }
    return NULL;
}

/* ========================================================================= */
/* Sequential Pattern Arbitration & Frame Update Hooks                       */
/* ========================================================================= */

static bool native_slide_closer_in_window(void *setting, void **tp_items, int32_t tp_count,
                                          int32_t cursor, int32_t cur_time, int32_t stick_deg,
                                          int32_t win_bad, int32_t sh_best_dt) {
    if (!tp_items || tp_count <= 0) return false;
    int32_t start = cursor > 0 ? cursor : 0;
    if (start >= tp_count) start = tp_count - 1;
    while (start > 0) {
        void *prev_tp = tp_items[start - 1];
        if (is_readable_ptr(prev_tp, 0x20) && *(int32_t *)((char *)prev_tp + 0x14) < cur_time - win_bad) {
            break;
        }
        --start;
    }

    for (int32_t i = start; i < tp_count; ++i) {
        void *tp = tp_items[i];
        if (!is_readable_ptr(tp, 0x50)) continue;
        int32_t t = *(int32_t *)((char *)tp + 0x14);
        if (t > cur_time + win_bad) break;

        uint8_t judged = *(uint8_t *)((char *)tp + 0x4c);
        if (judged != JUDGE_NONE) continue;

        uint8_t type = *(uint8_t *)((char *)tp + 0x18);
        if (type != TRIG_SLIDE && type != TRIG_TWINSLIDE &&
            type != TRIG_LINHOLD_END_SLIDE && type != TRIG_CRVHOLD_END_SLIDE) {
            continue;
        }

        int32_t dt = iabs32(t - cur_time);
        if (dt > win_bad || dt > sh_best_dt) continue;

        int32_t nat_a1 = *(int32_t *)((char *)tp + 0x20);
        int32_t nat_a2 = *(int32_t *)((char *)tp + 0x24);
        if (nat_a1 < -360 || angle_matches(setting, stick_deg, nat_a1) ||
            (type == TRIG_TWINSLIDE && nat_a2 >= 0 && angle_matches(setting, stick_deg, nat_a2))) {
            return true;
        }
    }
    return false;
}

/* Temporarily mask SlideHold start/end triggers' input category byte (+0x19 = 2) during
 * manual-mode original_runtime_context_update so native candidate scanning (0x32d1b5b)
 * and Twin Hold checks (0x32d4511 / 0x32d4709) skip SlideHold triggers for both Button (0)
 * and Stick (1) inputs, then restore +0x19 = 0 immediately after so FMS array indexing stays safe. */
static void call_orig_runtime_update_masked(void *ctx, void *arg1, int32_t cur_time, void *input_data,
                                            void *playdata, bool is_auto) {
    void *masked_tps[128];
    size_t masked_count = 0;
    bool overflow_masked = false;

    if (!is_auto && s_hold_count > 0 && is_readable_ptr(playdata, 0x30)) {
        void *tp_list = *(void **)((char *)playdata + 0x10);
        if (!s_links_resolved || tp_list != s_resolved_tp_list) {
            resolve_post_sort_slidehold_links(tp_list);
        }
        if (s_links_resolved && tp_list == s_resolved_tp_list) {
            int64_t t_now = (int64_t)cur_time;
            for (size_t i = 0; i < s_hold_count; ++i) {
                SlideHoldEntry *sh = &s_holds[i];
                if (!sh->holding &&
                    (t_now < (int64_t)sh->start_time_ms - 2000 ||
                     t_now > (int64_t)sh->end_time_ms + 2000)) {
                    continue;
                }
                if (sh->start_tp) {
                    *(uint8_t *)((char *)sh->start_tp + 0x19) = 2;
                    if (masked_count < 128) masked_tps[masked_count++] = sh->start_tp;
                    else overflow_masked = true;
                }
                if (sh->end_tp) {
                    *(uint8_t *)((char *)sh->end_tp + 0x19) = 2;
                    if (masked_count < 128) masked_tps[masked_count++] = sh->end_tp;
                    else overflow_masked = true;
                }
            }
        }
    }

    original_runtime_context_update(ctx, arg1, cur_time, input_data);

    if (overflow_masked) {
        for (size_t i = 0; i < s_hold_count; ++i) {
            SlideHoldEntry *sh = &s_holds[i];
            if (sh->start_tp) *(uint8_t *)((char *)sh->start_tp + 0x19) = 0;
            if (sh->end_tp)   *(uint8_t *)((char *)sh->end_tp + 0x19)   = 0;
        }
    } else {
        for (size_t i = 0; i < masked_count; ++i) {
            *(uint8_t *)((char *)masked_tps[i] + 0x19) = 0;
        }
    }
}

ABI int hook_slidehold_runtime_update(HookRegs *r) {
    void *ctx = (void *)r->rdi;
    void *arg1 = (void *)r->rsi;
    int32_t cur_time = (int32_t)r->rdx;
    void *input_data = (void *)r->rcx;

    if (!original_runtime_context_update) return 0;
    if (s_hold_count == 0 || !is_readable_ptr(ctx, 0x38) || !is_readable_ptr(input_data, 0x30)) {
        original_runtime_context_update(ctx, arg1, cur_time, input_data);
        return 1;
    }

    void *setting   = *(void **)((char *)ctx + 0x10);
    void *playdata  = *(void **)((char *)ctx + 0x28);
    int32_t cursor  = *(int32_t *)((char *)ctx + 0x30);
    bool is_auto    = is_readable_ptr(setting, 0x20) ? (*(uint8_t *)((char *)setting + 0x12) != 0) : false;
    s_auto_mode     = is_auto;

    int32_t dev_id  = *(int32_t *)((char *)input_data + 0x14);
    int32_t in_type = *(int32_t *)((char *)input_data + 0x18);
    bool is_stick   = ((in_type & 0xfffffff0) == 0x10);

    if (!is_stick || is_auto || !is_readable_ptr(playdata, 0x30)) {
        call_orig_runtime_update_masked(ctx, arg1, cur_time, input_data, playdata, is_auto);
        return 1;
    }

    if (!s_links_resolved) {
        resolve_post_sort_slidehold_links(*(void **)((char *)playdata + 0x10));
    }

    uint8_t active      = *(uint8_t *)((char *)input_data + 0x1c);
    int32_t degree      = normalize_degree(*(int32_t *)((char *)input_data + 0x20));
    int32_t turn_slide  = *(int32_t *)((char *)input_data + 0x24);
    int32_t slide_state = *(int32_t *)((char *)input_data + 0x28);

    StickTracker *st = get_stick_tracker(dev_id, in_type, true);

    /* Case A: Stick returned to neutral */
    if (!active) {
        if (st) {
            st->prev_active = false;
            st->prev_flick = false;
            st->flick_consumed = false;
            st->last_degree = -1;
            st->consumed_degree = -1;
            st->consumed_time_ms = 0;
        }
        SlideHoldEntry *held = find_active_hold_by_stick(dev_id, in_type);
        if (held) {
            finish_hold_release(setting, playdata, held, cur_time);
        }
        call_orig_runtime_update_masked(ctx, arg1, cur_time, input_data, playdata, is_auto);
        return 1;
    }

    /* Case B: Stick currently holding a SlideHold */
    SlideHoldEntry *held = find_active_hold_by_stick(dev_id, in_type);
    if (held) {
        if (!hold_sustain_angle_matches(setting, held, degree)) {
            int32_t bad_win = judge_window(setting, JUDGE_GOOD, 142);
            if (iabs32(cur_time - held->end_time_ms) <= bad_win) {
                /* Inside the valid tail release window: analog springback wobble or direct
                 * transition into another note completes the SlideHold End instead of dropping
                 * it as mid-hold drift, and unlocks this stick for same-frame Case C starts. */
                finish_hold_release(setting, playdata, held, cur_time);
                if (st) {
                    st->flick_consumed = false;
                    st->consumed_degree = -1;
                    st->consumed_time_ms = 0;
                }
            } else {
                drop_hold_by_drift(playdata, held, cur_time, degree);
            }
        } else {
            if (st) {
                st->prev_active = true;
                st->prev_flick = (slide_state >= 2);
                st->last_degree = degree;
            }
            *(int32_t *)((char *)input_data + 0x24) = 0;
            *(int32_t *)((char *)input_data + 0x28) = 1;
            call_orig_runtime_update_masked(ctx, arg1, cur_time, input_data, playdata, is_auto);
            *(int32_t *)((char *)input_data + 0x24) = turn_slide;
            *(int32_t *)((char *)input_data + 0x28) = slide_state;
            return 1;
        }
    }

    /* Case C: Fresh stick flick arbitration between SlideHold Start & native Slide */
    bool is_flick = (slide_state >= 2);
    if (st) {
        if (!st->prev_active) {
            st->flick_consumed = false;
            st->consumed_degree = -1;
        } else if (is_flick && !st->prev_flick) {
            st->flick_consumed = false;
            st->consumed_degree = -1;
        } else if (is_flick && st->consumed_degree >= 0) {
            /* Native turnSlide triggers at 30 deg delta (addSlide @ 0x2fef450); allow 45 deg sector jumps unconditionally. */
            int32_t cdiff = angle_distance(degree, st->consumed_degree);
            if ((turn_slide != 0 && cdiff >= 30) || cdiff >= 45) {
                st->flick_consumed = false;
                st->consumed_degree = -1;
            }
        }
    }

    void *tp_list = *(void **)((char *)playdata + 0x10);
    int32_t tp_count = 0;
    void **tp_items = managed_ptr_seq(tp_list, &tp_count);

    bool suppressed_for_native = false;
    bool can_try_start = (is_flick && st && !st->flick_consumed);

    if (can_try_start) {
        int32_t win_bad = judge_window(setting, JUDGE_GOOD, 142);

        SlideHoldEntry *best_sh = NULL;
        void *best_tp = NULL;
        int32_t best_dt = win_bad + 1;

        for (size_t i = 0; i < s_hold_count; ++i) {
            SlideHoldEntry *sh = &s_holds[i];
            if (sh->started || sh->dropped || sh->completed) continue;
            if (!tp_items || sh->start_idx < 0 || sh->start_idx >= tp_count) continue;

            int32_t dt = iabs32(cur_time - sh->start_time_ms);
            if (dt > win_bad || dt >= best_dt) continue;
            if (!hold_angle_matches(setting, sh, degree)) continue;

            void *tp = tp_items[sh->start_idx];
            if (!is_readable_ptr(tp, 0x50) || *(uint8_t *)((char *)tp + 0x4c) != JUDGE_NONE) continue;

            best_sh = sh;
            best_tp = tp;
            best_dt = dt;
        }

        if (best_sh &&
            !native_slide_closer_in_window(setting, tp_items, tp_count, cursor, cur_time, degree, win_bad, best_dt)) {
            uint8_t grade = timing_grade(setting, best_dt);
            if (grade != JUDGE_NONE) {
                best_sh->started = true;
                set_hold_holding(best_sh, true);
                best_sh->device_id = dev_id;
                best_sh->input_type = in_type;

                call_judge4_safe(playdata, best_sh->start_idx, grade, cur_time);
                insert_hold_param(playdata, best_tp, input_data, dev_id, in_type, best_sh->start_idx, best_sh->end_idx);

                st->flick_consumed = true;
                st->consumed_degree = degree;
                st->consumed_time_ms = cur_time;
                /* Downgrade slideState from flick(>=2) to held(1) and clear turnSlide for this
                 * Update call so native cannot consume the same stick flick for an adjacent Slide. */
                *(int32_t *)((char *)input_data + 0x24) = 0;
                *(int32_t *)((char *)input_data + 0x28) = 1;
                suppressed_for_native = true;
            }
        }
    }

    s_native_judged_count = 0;
    call_orig_runtime_update_masked(ctx, arg1, cur_time, input_data, playdata, is_auto);
    if (suppressed_for_native) {
        *(int32_t *)((char *)input_data + 0x24) = turn_slide;
        *(int32_t *)((char *)input_data + 0x28) = slide_state;
    }

    if (st) {
        bool native_flick_judged = false;
        if (!suppressed_for_native && tp_items) {
            for (size_t j = 0; j < s_native_judged_count; ++j) {
                int32_t tidx = s_native_judged_idxs[j];
                if (tidx >= 0 && tidx < tp_count && is_valid_ptr(tp_items[tidx])) {
                    uint8_t ttype = *(uint8_t *)((char *)tp_items[tidx] + 0x18);
                    if (is_flick_trigger_type(ttype)) {
                        native_flick_judged = true;
                        break;
                    }
                }
            }
        }
        if (native_flick_judged) {
            st->flick_consumed = true;
            st->consumed_degree = degree;
            st->consumed_time_ms = cur_time;
        }
        st->prev_active = true;
        st->prev_flick = is_flick;
        st->last_degree = degree;
    }

    return 1;
}

ABI int hook_slidehold_miss_judge(HookRegs *r) {
    if (s_hold_count == 0 || (s_links_resolved && s_holding_count == 0)) return 0;
    void *ctx = (void *)r->rdi;
    int32_t cur_time = (int32_t)r->rsi;
    if (!is_readable_ptr(ctx, 0x38)) return 0;

    void *setting   = *(void **)((char *)ctx + 0x10);
    void *ctx_input = *(void **)((char *)ctx + 0x20);
    void *playdata  = *(void **)((char *)ctx + 0x28);
    if (!is_readable_ptr(playdata, 0x30)) return 0;

    if (!s_links_resolved) {
        resolve_post_sort_slidehold_links(*(void **)((char *)playdata + 0x10));
    }
    if (s_holding_count == 0) return 0;

    bool is_auto = is_readable_ptr(setting, 0x20) ? (*(uint8_t *)((char *)setting + 0x12) != 0) : false;
    s_auto_mode = is_auto;
    if (is_auto) return 0;

    int32_t bad_win = judge_window(setting, JUDGE_GOOD, 142);
    bool have_input_list = false;
    if (is_readable_ptr(ctx_input, 0x20)) {
        int32_t in_cnt = 0;
        void **in_items = managed_ptr_seq(*(void **)((char *)ctx_input + 0x10), &in_cnt);
        have_input_list = (in_items != NULL && in_cnt > 0);
    }

    size_t remaining = s_holding_count;
    for (size_t i = 0; i < s_hold_count && remaining > 0; ++i) {
        SlideHoldEntry *sh = &s_holds[i];
        if (!sh->holding || sh->completed || sh->dropped) continue;
        --remaining;

        /* Check native InputState flags (enabled=+0x10, isHeld=+0x12, isRelease=+0x13);
         * PreUpdate (0x32d12e0) zeroes [st+0x10..+0x1a] on release and allows the slot to be
         * reused, so if the populated InputState list no longer holds this stick, treat as released. */
        void *st = find_native_input_state(ctx_input, sh->device_id, sh->input_type);
        if (is_readable_ptr(st, 0x20)) {
            uint8_t enabled    = *(uint8_t *)((char *)st + 0x10);
            uint8_t is_held    = *(uint8_t *)((char *)st + 0x12);
            uint8_t is_release = *(uint8_t *)((char *)st + 0x13);
            if (enabled == 0 || is_held == 0 || is_release != 0) {
                finish_hold_release(setting, playdata, sh, cur_time);
                continue;
            }
        } else if (have_input_list) {
            finish_hold_release(setting, playdata, sh, cur_time);
            continue;
        }

        /* Overholding past the tail BAD window fails the tail cleanly. */
        if (cur_time > sh->end_time_ms + bad_win) {
            set_hold_holding(sh, false);
            sh->dropped = true;
            sh->completed = true;
            remove_hold_param(playdata, sh->start_idx, sh->end_idx);
            call_judge4_safe(playdata, sh->end_idx, JUDGE_MISS, cur_time);
            if (slidehold_debug_enabled()) {
                printf("SlideHold: END OVERHOLD_MISS idx=%d end_t=%d cur=%d bad=%d\n",
                       sh->end_idx, sh->end_time_ms, cur_time, bad_win);
            }
        }
    }

    return 0;
}

/* ========================================================================= */
/* Cross-module note-skin queries & shared hook delegation                   */
/* ========================================================================= */

int runtime_slidehold_classify_trigger(int32_t trig_idx) {
    return is_any_slidehold_index(trig_idx) ? THEATRHYTHM_NOTE_SLIDEHOLD : -1;
}

bool runtime_slidehold_is_registered_trigger_obj(const void *obj) {
    return is_registered_slidehold_obj(obj);
}

const uint8_t *runtime_slidehold_onpu_png(size_t *out_sz) {
    if (out_sz) *out_sz = sizeof(s_asset_slidehold_onpu_cyan);
    return s_asset_slidehold_onpu_cyan;
}

void runtime_slidehold_on_skin_changed(void *mst) {
    if (s_hold_count == 0) {
        s_slidehold_skin_ver = 0;
        return;
    }
    ensure_calibrated_sprites(mst, NULL);
}

ABI int hook_slidehold_bms_update_holding(HookRegs *r) {
    void *controller = (void *)r->rdi;
    void *trig_pd    = (void *)r->rsi;
    if (!is_readable_ptr(controller, 0x20) || !is_readable_ptr(trig_pd, 0x50)) return 0;

    if (s_hold_count > 0 && fn_sr_set_sprite) {
        uint8_t type  = *(uint8_t *)((char *)trig_pd + 0x18);
        uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
        if (((type & 0xfb) == 3) && judge != JUDGE_MISS_PASS) {
            int32_t trig_idx = *(int32_t *)((char *)trig_pd + 0x10);
            int32_t lane_idx = *(int32_t *)((char *)trig_pd + 0x1c);
            int32_t lane_cnt = 0;
            void **lanes = managed_ptr_seq(*(void **)((char *)controller + 0x18), &lane_cnt);
            if (lanes && lane_idx >= 0 && lane_idx < lane_cnt && is_readable_ptr(lanes[lane_idx], 0x50)) {
                void *lane = lanes[lane_idx];
                void *hold_sr = *(void **)((char *)lane + 0x48);
                if (is_valid_ptr(hold_sr)) {
                    bool is_sh = runtime_slidehold_is_start_trigger(trig_idx);
                    uint32_t skin_ver = theatrhythm_note_skin_version();
                    LaneHoldSpriteCache *lc = (lane_idx < SH_LANE_CACHE_CAP) ? &s_lane_hold_cache[lane_idx] : NULL;
                    if (!lc || lc->hold_sr != hold_sr || lc->last_trig_pd != trig_pd ||
                        lc->last_is_sh != is_sh || lc->last_skin_ver != skin_ver ||
                        lc->last_color_type != s_color_type) {
                        void *nat_hold = runtime_note_skins_active_hold_start_sprite();
                        if (!runtime_unity_object_alive(nat_hold)) {
                            if (!runtime_unity_object_alive(s_nat_hold_start) && fn_sr_get_sprite) {
                                void *cur_spr = fn_sr_get_sprite(hold_sr, NULL);
                                if (cur_spr != s_spr_circle && runtime_unity_object_alive(cur_spr)) {
                                    s_nat_hold_start = cur_spr;
                                }
                            }
                            nat_hold = s_nat_hold_start;
                        }
                        void *target_spr = is_sh ? s_spr_circle : nat_hold;
                        if (runtime_unity_object_alive(target_spr)) {
                            void *cur = fn_sr_get_sprite ? fn_sr_get_sprite(hold_sr, NULL) : NULL;
                            if (cur != target_spr) {
                                fn_sr_set_sprite(hold_sr, target_spr, NULL);
                            }
                            if (lc) {
                                lc->hold_sr = hold_sr;
                                lc->last_trig_pd = trig_pd;
                                lc->last_skin_ver = skin_ver;
                                lc->last_color_type = s_color_type;
                                lc->last_is_sh = is_sh;
                            }
                        }
                    }
                }
            }
        }
    }

    runtime_note_skins_on_bms_holding(controller, trig_pd);
    return 0;
}

ABI int hook_slidehold_play_judge_effect(HookRegs *r) {
    runtime_note_skins_on_play_judge_effect((void *)r->rdi, (void *)r->rsi);
    return 0;
}
