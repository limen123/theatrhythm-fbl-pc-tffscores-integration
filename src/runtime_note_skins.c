/* Custom note skin recoloring & per-note particle color matching (Theatrhythm Final Bar Line PS4 v1.0.4). */

#include "runtime_note_skins.h"
#include "runtime.h"
#include "runtime_slidehold.h"
#include "gpu/theatrhythm_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef _MSC_VER
#define ABI
#else
#define ABI __attribute__((sysv_abi))
#endif

/* ========================================================================= */
/* Unity & Game Structures (PS4 v1.0.4 IL2CPP Layout)                        */
/* ========================================================================= */

typedef struct { float x, y; } NSVec2;
typedef struct { float x, y, width, height; } NSRect;
typedef struct { float x, y, z, w; } NSVec4;
typedef struct { float r, g, b, a; } NSColor;

typedef struct {
    int32_t m_Mode;
    int32_t pad;
    void   *m_GradientMin;
    void   *m_GradientMax;
    NSColor m_ColorMin;
    NSColor m_ColorMax;
} NSMinMaxGradient;

_Static_assert(sizeof(NSMinMaxGradient) == 56, "NSMinMaxGradient must match Unity 56-byte MinMaxGradient");

enum {
    NS_TRIG_TOUCH          = 0,
    NS_TRIG_SLIDE          = 1,
    NS_TRIG_TWINSLIDE      = 2,
    NS_TRIG_LINHOLD_START  = 3,
    NS_TRIG_LINHOLD_PASS   = 4,
    NS_TRIG_LINHOLD_END_T  = 5,
    NS_TRIG_LINHOLD_END_S  = 6,
    NS_TRIG_CRVHOLD_START  = 7,
    NS_TRIG_CRVHOLD_PASS   = 8,
    NS_TRIG_CRVHOLD_END_T  = 9,
    NS_TRIG_CRVHOLD_END_S  = 10
};

enum {
    NS_JUDGE_NONE      = 0,
    NS_JUDGE_RAINBOW   = 1,
    NS_JUDGE_CRITICAL  = 2,
    NS_JUDGE_GREAT     = 3,
    NS_JUDGE_GOOD      = 4,
    NS_JUDGE_BAD       = 5,
    NS_JUDGE_MISS      = 6,
    NS_JUDGE_MISS_PASS = 6
};

/* Slots inside Game.Scene.MusicStage.TriggerSprite (+0x10..+0x60) */
enum {
    SLOT_TOUCH = 0,
    SLOT_SLIDE,
    SLOT_TWIN_SLIDE_BASE,
    SLOT_TWIN_SLIDE_ARROW,
    SLOT_HOLD_START,
    SLOT_HOLD_PASS,
    SLOT_HOLD_END_TOUCH,
    SLOT_HOLD_END_SLIDE,
    SLOT_HOLD_LINE,
    SLOT_JOINT_LINE,
    SLOT_SPRITE_COUNT
};

static const size_t kSlotOffset[SLOT_SPRITE_COUNT] = {
    0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58
};

static const int kSlotCategory[SLOT_SPRITE_COUNT] = {
    THEATRHYTHM_NOTE_TOUCH,
    THEATRHYTHM_NOTE_SLIDE,
    THEATRHYTHM_NOTE_SLIDE,
    THEATRHYTHM_NOTE_SLIDE,
    THEATRHYTHM_NOTE_HOLD,
    THEATRHYTHM_NOTE_HOLD,
    THEATRHYTHM_NOTE_HOLD,
    THEATRHYTHM_NOTE_HOLD,
    THEATRHYTHM_NOTE_HOLD,
    THEATRHYTHM_NOTE_JOINT,
};

static const char *const kSlotTags[SLOT_SPRITE_COUNT] = {
    "NoteSkin/Touch",
    "NoteSkin/Slide",
    "NoteSkin/TwinSlideBase",
    "NoteSkin/TwinSlideArrow",
    "NoteSkin/HoldStart",
    "NoteSkin/HoldPass",
    "NoteSkin/HoldEndTouch",
    "NoteSkin/HoldEndSlide",
    "NoteSkin/HoldLine",
    "NoteSkin/JointLine",
};

#define HOLD_LINE_TEX_OFFSET 0x60

/* ========================================================================= */
/* Resolved Game Sites (Hooks & Engine Helpers)                              */
/* ========================================================================= */

enum {
    H_CREATE_TRIGGERS = 0,
    H_UPDATE_TRIGGER,
    H_TLC_SETUP,
    H_BMS_SET_JUDGE,
    H_FMS_PLAY_JUDGE,
    H_EMS_SET_JUDGE,
    H_FMS_UPDATE_HOLDING,
    H_EMS_SET_HOLDING,
    H_TOBJ_SET_CRIT,

    C_SPR_GET_TEX,
    C_SPR_GET_TEX_RECT,
    C_SPR_GET_TEX_OFFSET,
    C_SPR_GET_PIVOT,
    C_SPR_GET_PPU,
    C_SPR_GET_BORDER,
    C_TEX_GET_WRAP,
    C_TEX_SET_WRAP,
    C_SR_GET_SPRITE,
    C_SR_SET_SPRITE,
    C_SR_SET_COLOR,
    C_REND_GET_SHARED_MAT,
    C_REND_SET_SHARED_MAT,
    C_MAT_GET_MAIN_TEX,
    C_MAT_SET_MAIN_TEX,
    C_SHADER_PROP_TO_ID,
    C_GO_GET_TRANSFORM,
    C_COMP_GET_TRANSFORM,
    C_COMP_GET_GAMEOBJECT,
    C_XFORM_CHILD_COUNT,
    C_XFORM_GET_CHILD,
    C_JOINT_SET_SPRITE,
    C_LHOLD_SET_SPRITE,
    C_CHOLD_SET_TEX,
    NS_SITES
};

static const GameSite kSites[NS_SITES] = {
    [H_CREATE_TRIGGERS]    = {"Game.Scene.MusicStage.MusicStageTrigger::CreateTriggers", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 53 | 48 83 e4 e0"}},
    [H_UPDATE_TRIGGER]     = {"Game.Scene.MusicStage.MusicStageTrigger::UpdateTrigger", 0, 0, 0,
        {"55 48 89 e5 53 50 | 80 7f 64 00"}},
    [H_TLC_SETUP]          = {"Game.Scene.MusicStage.TriggerLaneController::Setup", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 e4"}},
    [H_BMS_SET_JUDGE]      = {"Game.Scene.MusicStage.TriggerLaneBMS::SetTriggerJudge", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 | 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    [H_FMS_PLAY_JUDGE]     = {"Game.Scene.MusicStage.TriggerLaneFMS::PlayJudgeAnimation", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 | 48 83 ec 38 80 3d ?? ?? ?? ?? 00"}},
    [H_EMS_SET_JUDGE]      = {"Game.Scene.MusicStage.TriggerLaneEMS::SetTriggerJudge", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 | 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    [H_FMS_UPDATE_HOLDING] = {"Game.Scene.MusicStage.TriggerLaneFMSController::updateHoldingInfo", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 | 50 80 3d ?? ?? ?? ?? 00"}},
    [H_EMS_SET_HOLDING]    = {"Game.Scene.MusicStage.TriggerLaneEMS::SetHolding", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 55 41 54 53 | 48 83 ec 38"}},
    [H_TOBJ_SET_CRIT]      = {"Game.Scene.MusicStage.TriggerObject::SetCriticalHitTriggerEffect", 0, 0, 0,
        {"55 48 89 e5 41 57 41 56 41 54 53 | 48 83 ec 20"}},

    [C_SPR_GET_TEX]        = {"UnityEngine.Sprite::get_texture", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_SPR_GET_TEX_RECT]   = {"UnityEngine.Sprite::get_textureRect", 0, 0, 0, {"55 48 89 e5 41 56 53 48 83 ec 20"}},
    [C_SPR_GET_TEX_OFFSET] = {"UnityEngine.Sprite::get_textureRectOffset", 0, 0, 0, {"55 48 89 e5 41 56 53 48 83 ec 10"}},
    [C_SPR_GET_PIVOT]      = {"UnityEngine.Sprite::get_pivot", 0, 0, 0, {"55 48 89 e5 41 56 53 48 83 ec 10"}},
    [C_SPR_GET_PPU]        = {"UnityEngine.Sprite::get_pixelsPerUnit", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_SPR_GET_BORDER]     = {"UnityEngine.Sprite::get_border", 0, 0, 0, {"55 48 89 e5 41 56 53 48 83 ec 20"}},
    [C_TEX_GET_WRAP]       = {"UnityEngine.Texture::get_wrapMode", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_TEX_SET_WRAP]       = {"UnityEngine.Texture::set_wrapMode", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_SR_GET_SPRITE]      = {"UnityEngine.SpriteRenderer::get_sprite", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_SR_SET_SPRITE]      = {"UnityEngine.SpriteRenderer::set_sprite", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_SR_SET_COLOR]       = {"UnityEngine.SpriteRenderer::set_color", 0, 0, 0, {"55 48 89 e5 41 56 53 48 83 ec 20"}},
    [C_REND_GET_SHARED_MAT]= {"UnityEngine.Renderer::get_sharedMaterial", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_REND_SET_SHARED_MAT]= {"UnityEngine.Renderer::set_sharedMaterial", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_MAT_GET_MAIN_TEX]   = {"UnityEngine.Material::get_mainTexture", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_MAT_SET_MAIN_TEX]   = {"UnityEngine.Material::set_mainTexture", 0, 0, 0, {"55 48 89 e5 41 57 41 56 53 50"}},
    [C_SHADER_PROP_TO_ID]  = {"UnityEngine.Shader::PropertyToID", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_GO_GET_TRANSFORM]   = {"UnityEngine.GameObject::get_transform", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_COMP_GET_TRANSFORM] = {"UnityEngine.Component::get_transform", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_COMP_GET_GAMEOBJECT]= {"UnityEngine.Component::get_gameObject", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_XFORM_CHILD_COUNT]  = {"UnityEngine.Transform::get_childCount", 0, 0, 0, {"55 48 89 e5 53 50"}},
    [C_XFORM_GET_CHILD]    = {"UnityEngine.Transform::GetChild", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_JOINT_SET_SPRITE]   = {"Game.Scene.MusicStage.JointLine::SetSprite", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_LHOLD_SET_SPRITE]   = {"Game.Scene.MusicStage.LinearHoldLine::SetLineSprite", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
    [C_CHOLD_SET_TEX]      = {"Game.Scene.MusicStage.CurveHoldLine::SetLineTexture", 0, 0, 0, {"55 48 89 e5 41 56 53"}},
};

static unsigned char *s_module = NULL;
static GameCode       s_code[NS_SITES];
static bool           s_installed = false;

#define CODE(s) (s_module + s_code[s].offset)

static int s_is_game_104 = 0;
static int s_active_color_type = 0;

void runtime_note_skins_set_color_type(int color_type) {
    int normalized = (color_type == 1) ? 1 : 0;
    if (normalized == s_active_color_type) return;
    s_active_color_type = normalized;
    runtime_note_skins_sync_simple_colors();
}

int runtime_note_skins_get_color_type(void) {
    return s_active_color_type;
}

static inline bool ns_should_apply(void) {
    return s_active_color_type == 0;
}

#define RVA_EFFECT_POOL_TYPEINFO    (s_is_game_104 ? 0x609a4e8ULL : 0x6099250ULL)
#define RVA_TRIGGER_OBJECT_TYPEINFO (s_is_game_104 ? 0x609f5e8ULL : 0ULL)
#define RVA_IL2CPP_CLASS_INIT       (s_is_game_104 ? 0x045b590ULL : 0x04577c0ULL)
#define RVA_TOBJ_SET_SIMPLE_COLOR   (s_is_game_104 ? 0x2724090ULL : 0ULL)
#define RVA_PS_MAIN_GET_COL_INJ     (s_is_game_104 ? 0x44c25b0ULL : 0x44af080ULL)
#define RVA_PS_MAIN_SET_COL_INJ     (s_is_game_104 ? 0x44c2610ULL : 0x44af0e0ULL)
#define TOBJ_TRIG_TYPE_OFF          (s_is_game_104 ? 0xd0u : 0xb8u)

static inline uint64_t ns_ptr_hash(const void *p) {
    uint64_t x = ((uint64_t)(uintptr_t)p) >> 4;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* ========================================================================= */
/* Small Safe Pointer & IL2CPP Helpers                                       */
/* ========================================================================= */

static inline bool ns_valid_ptr(const void *p) {
    return (((uintptr_t)p & 3ULL) == 0) && runtime_host_is_readable(p, 0x18) != 0;
}

static inline bool ns_mem_readable(const void *p, size_t size) {
    if (size == 0 || (((uintptr_t)p & 3ULL) != 0)) return false;
    return runtime_host_is_readable(p, size) != 0;
}

static void **ns_managed_ptr_seq(void *seq, int32_t *out_count) {
    if (!ns_mem_readable(seq, 0x20)) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    void *p10 = *(void **)((char *)seq + 0x10);
    int32_t n18 = *(int32_t *)((char *)seq + 0x18);
    if (p10 == NULL) {
        if (n18 <= 0 || n18 > 65536 || !ns_mem_readable((char *)seq + 0x20, (size_t)n18 * sizeof(void *))) {
            if (out_count) *out_count = 0;
            return NULL;
        }
        if (out_count) *out_count = n18;
        return (void **)((char *)seq + 0x20);
    }
    /* Multidimensional Il2CppArray (e.g. JudgeEffect[2,6]): bounds pointer at +0x10 points
       immediately after the flattened inline element array at +0x20 + n18 * sizeof(void *). */
    if (n18 > 0 && n18 <= 65536) {
        char *elem_end = (char *)seq + 0x20 + (size_t)n18 * sizeof(void *);
        if ((char *)p10 >= elem_end && (char *)p10 <= elem_end + 0x20 &&
            ns_mem_readable((char *)seq + 0x20, (size_t)n18 * sizeof(void *))) {
            if (out_count) *out_count = n18;
            return (void **)((char *)seq + 0x20);
        }
    }
    if (ns_mem_readable(p10, 0x20)) {
        int32_t cap = *(int32_t *)((char *)p10 + 0x18);
        if (n18 <= 0 || cap <= 0 || n18 > cap || cap > 65536 ||
            !ns_mem_readable((char *)p10 + 0x20, (size_t)n18 * sizeof(void *))) {
            if (out_count) *out_count = 0;
            return NULL;
        }
        if (out_count) *out_count = n18;
        return (void **)((char *)p10 + 0x20);
    }
    if (out_count) *out_count = 0;
    return NULL;
}

static int ns_trigger_type_to_category(uint8_t trig_type) {
    switch (trig_type) {
    case NS_TRIG_TOUCH:
        return THEATRHYTHM_NOTE_TOUCH;
    case NS_TRIG_SLIDE:
    case NS_TRIG_TWINSLIDE:
        return THEATRHYTHM_NOTE_SLIDE;
    case NS_TRIG_LINHOLD_START:
    case NS_TRIG_LINHOLD_PASS:
    case NS_TRIG_LINHOLD_END_T:
    case NS_TRIG_LINHOLD_END_S:
    case NS_TRIG_CRVHOLD_START:
    case NS_TRIG_CRVHOLD_PASS:
    case NS_TRIG_CRVHOLD_END_T:
    case NS_TRIG_CRVHOLD_END_S:
        return THEATRHYTHM_NOTE_HOLD;
    default:
        return -1;
    }
}

static int ns_classify_trig_pd(void *trig_pd) {
    if (!ns_mem_readable(trig_pd, 0x20)) return -1;
    int32_t trig_idx = *(int32_t *)((char *)trig_pd + 0x10);
    int sh = runtime_slidehold_classify_trigger(trig_idx);
    if (sh >= 0) return sh;
    return ns_trigger_type_to_category(*(uint8_t *)((char *)trig_pd + 0x18));
}

/* ========================================================================= */
/* HSV <-> RGB & Note RGBA Recoloring                                        */
/* ========================================================================= */

static float ns_clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static void ns_hsv_to_rgb(float h_deg, float s_norm, float v_norm, float *out_r, float *out_g, float *out_b) {
    float h = fmodf(h_deg, 360.0f);
    if (h < 0.0f) h += 360.0f;
    float s = ns_clamp01(s_norm);
    float v = ns_clamp01(v_norm);
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0.0f, g = 0.0f, b = 0.0f;
    if (h < 60.0f)       { r = c; g = x; b = 0.0f; }
    else if (h < 120.0f) { r = x; g = c; b = 0.0f; }
    else if (h < 180.0f) { r = 0.0f; g = c; b = x; }
    else if (h < 240.0f) { r = 0.0f; g = x; b = c; }
    else if (h < 300.0f) { r = x; g = 0.0f; b = c; }
    else                 { r = c; g = 0.0f; b = x; }
    *out_r = r + m;
    *out_g = g + m;
    *out_b = b + m;
}

void runtime_note_skins_recolor_rgba(uint8_t *rgba, int w, int h, int category) {
    if (!rgba || w <= 0 || h <= 0) return;
    int hue = 0, sat = 100;
    theatrhythm_note_skin_get(category, &hue, &sat);
    float h_deg = (float)hue;
    float s_target = ns_clamp01((float)sat / 100.0f);
    bool is_joint = (category == THEATRHYTHM_NOTE_JOINT);

    size_t count = (size_t)w * (size_t)h;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *px = rgba + i * 4u;
        uint8_t a = px[3];
        if (a == 0) continue;

        float r = (float)px[0] * (1.0f / 255.0f);
        float g = (float)px[1] * (1.0f / 255.0f);
        float b = (float)px[2] * (1.0f / 255.0f);
        float max_c = fmaxf(r, fmaxf(g, b));
        float min_c = fminf(r, fminf(g, b));
        if (max_c <= 0.001f) continue;

        float s_pix = (max_c - min_c) / max_c;
        float s_out;
        if (is_joint) {
            /* JointLine art can be white/silver or lightly tinted; preserve black edges, tint visible bar. */
            float lum_mask = ns_clamp01((max_c - 0.12f) / 0.25f);
            float sat_mask = ns_clamp01((s_pix - 0.06f) / 0.45f);
            s_out = fmaxf(sat_mask, lum_mask * 0.94f) * s_target;
        } else {
            /* Preserve crisp white centers, inner icons, and specular glints; recolor chromatic body pixels. */
            if (s_pix <= 0.08f) continue;
            float chrom = ns_clamp01((s_pix - 0.08f) / 0.52f);
            s_out = chrom * s_target;
        }

        float nr, ng, nb;
        ns_hsv_to_rgb(h_deg, s_out, max_c, &nr, &ng, &nb);
        int ir = (int)(nr * 255.0f + 0.5f);
        int ig = (int)(ng * 255.0f + 0.5f);
        int ib = (int)(nb * 255.0f + 0.5f);
        px[0] = (uint8_t)(ir < 0 ? 0 : (ir > 255 ? 255 : ir));
        px[1] = (uint8_t)(ig < 0 ? 0 : (ig > 255 ? 255 : ig));
        px[2] = (uint8_t)(ib < 0 ? 0 : (ib > 255 ? 255 : ib));
    }
}

void *runtime_note_skins_create_recolored_sprite(const uint8_t *png, size_t png_sz,
                                                 int crop_x, int crop_y, int crop_w, int crop_h,
                                                 float ppu, float pivot_x, float pivot_y,
                                                 int category, const char *tag) {
    if (!png || png_sz == 0) return NULL;
    int w = 0, h = 0;
    uint8_t *rgba = runtime_decode_png_rgba(png, png_sz, &w, &h);
    if (!rgba || w <= 0 || h <= 0) {
        runtime_free_decoded_rgba(rgba);
        return NULL;
    }

    uint8_t *src = rgba;
    uint8_t *sub = NULL;
    int out_w = w, out_h = h;
    if (crop_w > 0 && crop_h > 0 && crop_x >= 0 && crop_y >= 0 &&
        crop_x + crop_w <= w && crop_y + crop_h <= h) {
        size_t row_bytes = (size_t)crop_w * 4u;
        sub = (uint8_t *)malloc(row_bytes * (size_t)crop_h);
        if (sub) {
            for (int r = 0; r < crop_h; ++r) {
                memcpy(sub + (size_t)r * row_bytes,
                       rgba + (((size_t)(crop_y + r) * (size_t)w + (size_t)crop_x) * 4u),
                       row_bytes);
            }
            src = sub;
            out_w = crop_w;
            out_h = crop_h;
        }
    }

    if (ns_should_apply()) {
        runtime_note_skins_recolor_rgba(src, out_w, out_h, category);
    }
    void *spr = runtime_create_sprite_from_rgba_metrics(
        src, out_w, out_h,
        0.0f, 0.0f, (float)out_w, (float)out_h,
        pivot_x, pivot_y, ppu > 0.0f ? ppu : 100.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        tag ? tag : "NoteSkinSprite");

    free(sub);
    runtime_free_decoded_rgba(rgba);
    return spr;
}

/* ========================================================================= */
/* Deferred Destroy Ring (prevents VRAM leak on live slider drag)            */
/* ========================================================================= */

#define DEFERRED_DESTROY_CAP 64
typedef struct {
    void    *obj;
    uint32_t pin;
    int      ticks;
} DeferredDestroy;

static DeferredDestroy s_destroy_ring[DEFERRED_DESTROY_CAP];

static void enqueue_deferred_destroy(void *obj, uint32_t pin) {
    if (!obj && !pin) return;
    for (int i = 0; i < DEFERRED_DESTROY_CAP; ++i) {
        if (!s_destroy_ring[i].obj && !s_destroy_ring[i].pin) {
            s_destroy_ring[i].obj = obj;
            s_destroy_ring[i].pin = pin;
            s_destroy_ring[i].ticks = 4;
            return;
        }
    }
    if (runtime_unity_object_alive(s_destroy_ring[0].obj)) {
        runtime_destroy_unity_object_imm(s_destroy_ring[0].obj);
    }
    if (s_destroy_ring[0].pin) runtime_gc_unpin_obj(s_destroy_ring[0].pin);
    s_destroy_ring[0].obj = obj;
    s_destroy_ring[0].pin = pin;
    s_destroy_ring[0].ticks = 4;
}

static void tick_deferred_destroys(void) {
    for (int i = 0; i < DEFERRED_DESTROY_CAP; ++i) {
        DeferredDestroy *d = &s_destroy_ring[i];
        if (!d->obj && !d->pin) continue;
        if (--d->ticks > 0) continue;
        if (d->obj && runtime_unity_object_alive(d->obj)) {
            runtime_destroy_unity_object_imm(d->obj);
        }
        if (d->pin) runtime_gc_unpin_obj(d->pin);
        memset(d, 0, sizeof(*d));
    }
}

/* ========================================================================= */
/* GPU Texture Readback Cache & White-Luminance Particle Normalization       */
/* ========================================================================= */

#define MAX_TEX_READBACK 512
typedef struct {
    void    *tex;
    uint32_t pin;
    int      w;
    int      h;
    uint8_t *rgba;
    bool     is_chromatic;
    void    *white_tex;
    uint32_t white_pin;
} TexReadbackEntry;

static TexReadbackEntry s_tex_cache[MAX_TEX_READBACK];
static size_t s_tex_cache_evict = 0;

static bool analyze_buffer_chromatic(const uint8_t *rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;
    size_t count = (size_t)w * (size_t)h;
    size_t chrom_px = 0;
    size_t lit_px = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *px = rgba + i * 4u;
        if (px[3] <= 24) continue;
        uint8_t mx = px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2]) : (px[1] > px[2] ? px[1] : px[2]);
        uint8_t mn = px[0] < px[1] ? (px[0] < px[2] ? px[0] : px[2]) : (px[1] < px[2] ? px[1] : px[2]);
        if (mx <= 20) continue;
        ++lit_px;
        if ((int)(mx - mn) * 7 > (int)mx) {
            ++chrom_px;
        }
    }
    if (chrom_px < 4) return false;
    return chrom_px * 40u >= lit_px || chrom_px >= 24;
}

static bool is_generated_white_tex(void *tex) {
    if (!ns_valid_ptr(tex)) return false;
    for (size_t i = 0; i < MAX_TEX_READBACK; ++i) {
        if (s_tex_cache[i].white_tex == tex) return true;
    }
    return runtime_object_name_equals(tex, "NoteSkin/NeutralParticleTex");
}

static TexReadbackEntry *get_tex_readback(void *tex) {
    if (!runtime_unity_object_alive(tex)) return NULL;
    for (size_t i = 0; i < MAX_TEX_READBACK; ++i) {
        if (s_tex_cache[i].tex == tex && s_tex_cache[i].rgba) {
            return &s_tex_cache[i];
        }
    }

    int w = 0, h = 0;
    uint8_t *rgba = runtime_extract_texture_rgba(tex, &w, &h);
    if (!rgba || w <= 0 || h <= 0) {
        runtime_free_extracted_rgba(rgba);
        return NULL;
    }

    size_t slot = MAX_TEX_READBACK;
    for (size_t i = 0; i < MAX_TEX_READBACK; ++i) {
        if (!s_tex_cache[i].rgba || !runtime_unity_object_alive(s_tex_cache[i].tex)) {
            slot = i;
            break;
        }
    }
    if (slot >= MAX_TEX_READBACK) {
        slot = s_tex_cache_evict;
        s_tex_cache_evict = (s_tex_cache_evict + 1) % MAX_TEX_READBACK;
    }
    TexReadbackEntry *e = &s_tex_cache[slot];
    if (e->pin) runtime_gc_unpin_obj(e->pin);
    if (e->white_pin) runtime_gc_unpin_obj(e->white_pin);
    if (e->rgba) runtime_free_extracted_rgba(e->rgba);
    memset(e, 0, sizeof(*e));

    e->tex = tex;
    e->pin = runtime_gc_pin_obj(tex);
    e->w = w;
    e->h = h;
    e->rgba = rgba;
    e->is_chromatic = analyze_buffer_chromatic(rgba, w, h);
    return e;
}

static void rgba_to_white_luminance(const uint8_t *src, uint8_t *dst, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *s = src + i * 4u;
        uint8_t l = s[0] > s[1] ? (s[0] > s[2] ? s[0] : s[2]) : (s[1] > s[2] ? s[1] : s[2]);
        dst[i * 4u + 0] = l;
        dst[i * 4u + 1] = l;
        dst[i * 4u + 2] = l;
        dst[i * 4u + 3] = s[3];
    }
}

static void *get_neutral_white_tex(void *src_tex) {
    if (is_generated_white_tex(src_tex)) return NULL;
    TexReadbackEntry *e = get_tex_readback(src_tex);
    if (!e || !e->is_chromatic) return NULL;
    if (e->white_tex && runtime_unity_object_alive(e->white_tex)) {
        return e->white_tex;
    }

    size_t n = (size_t)e->w * (size_t)e->h;
    uint8_t *lum = (uint8_t *)malloc(n * 4u);
    if (!lum) return NULL;
    rgba_to_white_luminance(e->rgba, lum, n);

    void *wtex = runtime_create_texture2d_rgba(lum, e->w, e->h, "NoteSkin/NeutralParticleTex");
    free(lum);
    if (runtime_unity_object_alive(wtex)) {
        typedef int32_t (ABI *GetWrapFn)(void *t, void *m);
        typedef void    (ABI *SetWrapFn)(void *t, int32_t w, void *m);
        int32_t wrap = ((GetWrapFn)CODE(C_TEX_GET_WRAP))(src_tex, NULL);
        ((SetWrapFn)CODE(C_TEX_SET_WRAP))(wtex, wrap, NULL);
        e->white_tex = wtex;
        e->white_pin = runtime_gc_pin_obj(wtex);
    }
    return e->white_tex;
}

/* ========================================================================= */
/* ParticleSystem Neutralization & Live Per-Note RGB Coloring                */
/* ========================================================================= */

static void (ABI *fn_ps_main_get_col_inj)(void **p_ps, NSMinMaxGradient *out) = NULL;
static void (ABI *fn_ps_main_set_col_inj)(void **p_ps, const NSMinMaxGradient *in) = NULL;
static bool (ABI *fn_ps_col_get_enabled)(void **p_ps) = NULL;
static void (ABI *fn_ps_col_get_color)(void **p_ps, NSMinMaxGradient *out) = NULL;
static void (ABI *fn_ps_col_set_color)(void **p_ps, const NSMinMaxGradient *in) = NULL;
static bool (ABI *fn_mat_has_prop)(void *mat, int32_t prop_id) = NULL;
static void (ABI *fn_mat_get_col_inj)(void *mat, int32_t prop_id, NSColor *out) = NULL;
static void (ABI *fn_mat_set_col_inj)(void *mat, int32_t prop_id, const NSColor *in) = NULL;

static bool    s_icalls_probed = false;
static int32_t s_prop_tint_color = 0;
static int32_t s_prop_color      = 0;
static int32_t s_prop_base_color = 0;

static void ensure_particle_icalls(void) {
    if (s_icalls_probed) return;
    s_icalls_probed = true;

    fn_ps_main_get_col_inj = (void (ABI *)(void **, NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/MainModule::get_startColor_Injected");
    fn_ps_main_set_col_inj = (void (ABI *)(void **, const NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/MainModule::set_startColor_Injected");
    if (!fn_ps_main_get_col_inj && s_module) fn_ps_main_get_col_inj = (void *)(s_module + RVA_PS_MAIN_GET_COL_INJ);
    if (!fn_ps_main_set_col_inj && s_module) fn_ps_main_set_col_inj = (void *)(s_module + RVA_PS_MAIN_SET_COL_INJ);

    fn_ps_col_get_enabled = (bool (ABI *)(void **))
        runtime_resolve_icall("UnityEngine.ParticleSystem/ColorOverLifetimeModule::get_enabled_Injected");
    fn_ps_col_get_color = (void (ABI *)(void **, NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/ColorOverLifetimeModule::get_color_Injected");
    fn_ps_col_set_color = (void (ABI *)(void **, const NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/ColorOverLifetimeModule::set_color_Injected");

    fn_mat_has_prop = (bool (ABI *)(void *, int32_t))
        runtime_resolve_icall("UnityEngine.Material::HasProperty(System.Int32)");
    if (!fn_mat_has_prop) {
        fn_mat_has_prop = (bool (ABI *)(void *, int32_t))
            runtime_resolve_icall("UnityEngine.Material::HasProperty");
    }
    fn_mat_get_col_inj = (void (ABI *)(void *, int32_t, NSColor *))
        runtime_resolve_icall("UnityEngine.Material::GetColorImpl_Injected");
    fn_mat_set_col_inj = (void (ABI *)(void *, int32_t, const NSColor *))
        runtime_resolve_icall("UnityEngine.Material::SetColorImpl_Injected");

    typedef int32_t (ABI *PropIdFn)(void *str, void *m);
    PropIdFn prop_to_id = (PropIdFn)CODE(C_SHADER_PROP_TO_ID);
    void *s_tint = runtime_il2cpp_string("_TintColor");
    void *s_col  = runtime_il2cpp_string("_Color");
    void *s_base = runtime_il2cpp_string("_BaseColor");
    if (s_tint) s_prop_tint_color = prop_to_id(s_tint, NULL);
    if (s_col)  s_prop_color      = prop_to_id(s_col, NULL);
    if (s_base) s_prop_base_color = prop_to_id(s_base, NULL);
}

#define MAX_MAT_CACHE 512
typedef struct {
    void    *orig_mat;
    uint32_t orig_pin;
    void    *neutral_mat;
    uint32_t neutral_pin;
    bool     checked;
    bool     non_chromatic;
} MatNeutralEntry;

static MatNeutralEntry s_mat_cache[MAX_MAT_CACHE];
static size_t s_mat_evict = 0;

static bool color_is_chromatic(NSColor c) {
    float mx = fmaxf(c.r, fmaxf(c.g, c.b));
    float mn = fminf(c.r, fminf(c.g, c.b));
    return mx > 0.05f && (mx - mn) > 0.08f * mx;
}

static void clear_mat_entry(MatNeutralEntry *m) {
    if (!m) return;
    if (m->orig_pin) runtime_gc_unpin_obj(m->orig_pin);
    if (m->neutral_pin) runtime_gc_unpin_obj(m->neutral_pin);
    memset(m, 0, sizeof(*m));
}

static void *get_neutral_material(void *orig_mat) {
    if (!runtime_unity_object_alive(orig_mat)) return orig_mat;
    for (size_t i = 0; i < MAX_MAT_CACHE; ++i) {
        if (s_mat_cache[i].orig_mat == orig_mat && s_mat_cache[i].checked) {
            if (s_mat_cache[i].non_chromatic) return orig_mat;
            if (runtime_unity_object_alive(s_mat_cache[i].neutral_mat)) {
                return s_mat_cache[i].neutral_mat;
            }
            clear_mat_entry(&s_mat_cache[i]);
            break;
        }
        if (s_mat_cache[i].neutral_mat == orig_mat) {
            return orig_mat;
        }
    }

    ensure_particle_icalls();
    typedef void * (ABI *MatGetTexFn)(void *mat, void *m);
    typedef void   (ABI *MatSetTexFn)(void *mat, void *t, void *m);

    void *main_tex = ((MatGetTexFn)CODE(C_MAT_GET_MAIN_TEX))(orig_mat, NULL);
    void *wtex = runtime_unity_object_alive(main_tex) ? get_neutral_white_tex(main_tex) : NULL;

    int32_t props[3] = { s_prop_tint_color, s_prop_color, s_prop_base_color };
    bool need_col_neutral = false;
    if (fn_mat_has_prop && fn_mat_get_col_inj) {
        for (int i = 0; i < 3; ++i) {
            if (!props[i] || !fn_mat_has_prop(orig_mat, props[i])) continue;
            NSColor c = {1, 1, 1, 1};
            fn_mat_get_col_inj(orig_mat, props[i], &c);
            if (color_is_chromatic(c)) {
                need_col_neutral = true;
                break;
            }
        }
    }

    void *neutral_mat = NULL;
    if (wtex || need_col_neutral) {
        void *cloned = runtime_clone_material_raw(orig_mat);
        if (runtime_unity_object_alive(cloned)) {
            if (wtex) {
                ((MatSetTexFn)CODE(C_MAT_SET_MAIN_TEX))(cloned, wtex, NULL);
            }
            if (need_col_neutral && fn_mat_has_prop && fn_mat_get_col_inj && fn_mat_set_col_inj) {
                for (int i = 0; i < 3; ++i) {
                    if (!props[i] || !fn_mat_has_prop(cloned, props[i])) continue;
                    NSColor c = {1, 1, 1, 1};
                    fn_mat_get_col_inj(cloned, props[i], &c);
                    if (color_is_chromatic(c)) {
                        NSColor white = {1.0f, 1.0f, 1.0f, c.a};
                        fn_mat_set_col_inj(cloned, props[i], &white);
                    }
                }
            }
            neutral_mat = cloned;
        }
    }

    if ((wtex || need_col_neutral) && !neutral_mat) {
        return orig_mat;
    }

    size_t slot = MAX_MAT_CACHE;
    for (size_t i = 0; i < MAX_MAT_CACHE; ++i) {
        if (!s_mat_cache[i].checked || !runtime_unity_object_alive(s_mat_cache[i].orig_mat)) {
            slot = i;
            break;
        }
    }
    if (slot >= MAX_MAT_CACHE) {
        slot = s_mat_evict;
        s_mat_evict = (s_mat_evict + 1) % MAX_MAT_CACHE;
    }
    MatNeutralEntry *m = &s_mat_cache[slot];
    clear_mat_entry(m);
    m->orig_mat = orig_mat;
    m->orig_pin = runtime_gc_pin_obj(orig_mat);
    if (neutral_mat) {
        m->neutral_mat = neutral_mat;
        m->neutral_pin = runtime_gc_pin_obj(neutral_mat);
    } else {
        m->non_chromatic = true;
    }
    m->checked = true;
    return neutral_mat ? neutral_mat : orig_mat;
}

#define MAX_PS_CACHE 8192
#define PS_HASH_CAP  16384
typedef struct {
    void            *ps;
    bool             persistent;
    bool             is_root;
    bool             has_last_rgb;
    float            last_r, last_g, last_b;
    int32_t          orig_main_mode;
    float            orig_alpha_min;
    float            orig_alpha_max;
    bool             col_neutralized;
    NSMinMaxGradient white_col;
    uint32_t         pin_white_col_min;
    uint32_t         pin_white_col_max;
    void            *psr;
    void            *neutral_mat;
    bool             mat_prepared;
} PSCacheEntry;

static PSCacheEntry s_ps_cache[MAX_PS_CACHE];
static size_t       s_ps_evict = 0;
static int32_t      s_ps_hash[PS_HASH_CAP];
static bool         s_ps_hash_inited = false;
static size_t       s_ps_tombstones = 0;

static void rebuild_ps_hash(void) {
    for (size_t i = 0; i < PS_HASH_CAP; ++i) s_ps_hash[i] = -1;
    s_ps_tombstones = 0;
    s_ps_hash_inited = true;
    const size_t mask = PS_HASH_CAP - 1u;
    for (size_t idx = 0; idx < MAX_PS_CACHE; ++idx) {
        void *ps = s_ps_cache[idx].ps;
        if (!ps) continue;
        size_t pos = (size_t)ns_ptr_hash(ps) & mask;
        while (s_ps_hash[pos] >= 0) pos = (pos + 1u) & mask;
        s_ps_hash[pos] = (int32_t)idx;
    }
}

static void clear_ps_entry_at(size_t idx) {
    PSCacheEntry *e = &s_ps_cache[idx];
    if (e->ps && s_ps_hash_inited) {
        const size_t mask = PS_HASH_CAP - 1u;
        size_t pos = (size_t)ns_ptr_hash(e->ps) & mask;
        for (size_t probe = 0; probe < PS_HASH_CAP; ++probe) {
            int32_t h = s_ps_hash[pos];
            if (h == -1) break;
            if (h == (int32_t)idx) {
                s_ps_hash[pos] = -2;
                ++s_ps_tombstones;
                break;
            }
            pos = (pos + 1u) & mask;
        }
    }
    if (e->pin_white_col_min) runtime_gc_unpin_obj(e->pin_white_col_min);
    if (e->pin_white_col_max) runtime_gc_unpin_obj(e->pin_white_col_max);
    memset(e, 0, sizeof(*e));
}

static bool ns_color_near_white(NSColor c) {
    return fabsf(c.r - 1.0f) < 0.04f && fabsf(c.g - 1.0f) < 0.04f && fabsf(c.b - 1.0f) < 0.04f;
}

static PSCacheEntry *get_ps_entry(void *ps, bool persistent) {
    if (!ns_valid_ptr(ps)) return NULL;
    ensure_particle_icalls();
    if (!fn_ps_main_get_col_inj || !fn_ps_main_set_col_inj) return NULL;
    if (!s_ps_hash_inited) rebuild_ps_hash();

    const size_t mask = PS_HASH_CAP - 1u;
    size_t pos = (size_t)ns_ptr_hash(ps) & mask;
    size_t first_tomb = PS_HASH_CAP;
    for (size_t probe = 0; probe < PS_HASH_CAP; ++probe) {
        int32_t h = s_ps_hash[pos];
        if (h == -1) break;
        if (h == -2) {
            if (first_tomb >= PS_HASH_CAP) first_tomb = pos;
        } else if (s_ps_cache[h].ps == ps) {
            if (runtime_unity_object_alive(ps)) {
                if (persistent) s_ps_cache[h].persistent = true;
                return &s_ps_cache[h];
            }
            clear_ps_entry_at((size_t)h);
            first_tomb = pos;
            break;
        }
        pos = (pos + 1u) & mask;
    }
    if (!runtime_unity_object_alive(ps)) return NULL;

    size_t free_idx = MAX_PS_CACHE;
    for (size_t i = 0; i < MAX_PS_CACHE; ++i) {
        if (!s_ps_cache[i].ps) {
            free_idx = i;
            break;
        }
    }
    if (free_idx >= MAX_PS_CACHE) {
        for (size_t i = 0; i < MAX_PS_CACHE; ++i) {
            if (!runtime_unity_object_alive(s_ps_cache[i].ps)) {
                clear_ps_entry_at(i);
                free_idx = i;
                break;
            }
        }
    }
    if (free_idx >= MAX_PS_CACHE) {
        for (size_t probe = 0; probe < MAX_PS_CACHE; ++probe) {
            size_t idx = (s_ps_evict + probe) % MAX_PS_CACHE;
            if (!s_ps_cache[idx].persistent) {
                s_ps_evict = (idx + 1u) % MAX_PS_CACHE;
                clear_ps_entry_at(idx);
                free_idx = idx;
                break;
            }
        }
    }
    if (free_idx >= MAX_PS_CACHE) {
        size_t idx = s_ps_evict;
        s_ps_evict = (s_ps_evict + 1u) % MAX_PS_CACHE;
        clear_ps_entry_at(idx);
        free_idx = idx;
    }

    PSCacheEntry *free_slot = &s_ps_cache[free_idx];
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->ps = ps;
    free_slot->persistent = persistent;
    if (s_ps_tombstones > PS_HASH_CAP / 4u) {
        rebuild_ps_hash();
    } else {
        size_t ins_pos = (first_tomb < PS_HASH_CAP) ? first_tomb : pos;
        if (s_ps_hash[ins_pos] == -2) --s_ps_tombstones;
        s_ps_hash[ins_pos] = (int32_t)free_idx;
    }
    if (runtime_object_name_equals(ps, "root")) {
        free_slot->is_root = true;
        return free_slot;
    }

    /* Record original startColor mode & alphas. */
    NSMinMaxGradient orig_main;
    memset(&orig_main, 0, sizeof(orig_main));
    fn_ps_main_get_col_inj(&ps, &orig_main);
    free_slot->orig_main_mode = orig_main.m_Mode;
    float a_min = (orig_main.m_Mode == 2) ? orig_main.m_ColorMin.a : orig_main.m_ColorMax.a;
    float a_max = orig_main.m_ColorMax.a;
    if (orig_main.m_Mode == 1 || orig_main.m_Mode == 3 || orig_main.m_Mode == 4 || a_max <= 0.001f) {
        a_min = 1.0f;
        a_max = 1.0f;
    }
    free_slot->orig_alpha_min = a_min;
    free_slot->orig_alpha_max = a_max;

    /* Neutralize ColorOverLifetime once so startColor controls hue while preserving alpha fade curves. */
    if (fn_ps_col_get_enabled && fn_ps_col_get_color && fn_ps_col_set_color &&
        fn_ps_col_get_enabled(&ps)) {
        NSMinMaxGradient orig_col;
        memset(&orig_col, 0, sizeof(orig_col));
        fn_ps_col_get_color(&ps, &orig_col);
        NSMinMaxGradient wcol = orig_col;
        bool touched = false;

        if (orig_col.m_Mode == 0 && !ns_color_near_white(orig_col.m_ColorMax)) {
            wcol.m_ColorMax = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMax.a};
            touched = true;
        } else if (orig_col.m_Mode == 2 &&
                   (!ns_color_near_white(orig_col.m_ColorMin) || !ns_color_near_white(orig_col.m_ColorMax))) {
            wcol.m_ColorMin = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMin.a};
            wcol.m_ColorMax = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMax.a};
            touched = true;
        } else if (orig_col.m_Mode == 1 || orig_col.m_Mode == 3 || orig_col.m_Mode == 4) {
            if (ns_valid_ptr(orig_col.m_GradientMax)) {
                void *gmax = runtime_clone_gradient_solid_color(orig_col.m_GradientMax, 1.0f, 1.0f, 1.0f);
                if (ns_valid_ptr(gmax)) {
                    wcol.m_GradientMax = gmax;
                    free_slot->pin_white_col_max = runtime_gc_pin_obj(gmax);
                    touched = true;
                }
            }
            if (orig_col.m_Mode == 3 && ns_valid_ptr(orig_col.m_GradientMin)) {
                void *gmin = runtime_clone_gradient_solid_color(orig_col.m_GradientMin, 1.0f, 1.0f, 1.0f);
                if (ns_valid_ptr(gmin)) {
                    wcol.m_GradientMin = gmin;
                    free_slot->pin_white_col_min = runtime_gc_pin_obj(gmin);
                    touched = true;
                }
            }
        }
        if (touched) {
            free_slot->white_col = wcol;
            free_slot->col_neutralized = true;
            fn_ps_col_set_color(&ps, &free_slot->white_col);
        }
    }

    /* Neutralize chromatic particle material once to white luminance. */
    typedef void * (ABI *GetSharedMatFn)(void *r, void *m);
    typedef void   (ABI *SetSharedMatFn)(void *r, void *mat, void *m);
    void *psr = runtime_get_particle_system_renderer(ps);
    if (runtime_unity_object_alive(psr)) {
        void *orig_mat = ((GetSharedMatFn)CODE(C_REND_GET_SHARED_MAT))(psr, NULL);
        void *nmat = runtime_unity_object_alive(orig_mat) ? get_neutral_material(orig_mat) : NULL;
        if (runtime_unity_object_alive(nmat) && nmat != orig_mat) {
            free_slot->psr = psr;
            free_slot->neutral_mat = nmat;
            free_slot->mat_prepared = true;
            ((SetSharedMatFn)CODE(C_REND_SET_SHARED_MAT))(psr, nmat, NULL);
        }
    }

    return free_slot;
}

static void apply_rgb_to_ps_entry(PSCacheEntry *e, float r, float g, float b) {
    if (!e || e->is_root || !e->ps || !fn_ps_main_set_col_inj) return;

    if (e->has_last_rgb &&
        fabsf(e->last_r - r) < 0.004f &&
        fabsf(e->last_g - g) < 0.004f &&
        fabsf(e->last_b - b) < 0.004f) {
        return;
    }

    void *ps = e->ps;
    NSMinMaxGradient mg;
    memset(&mg, 0, sizeof(mg));
    if (e->orig_main_mode == 2) {
        /* Preserve random highlight/base variance when prefab uses RandomBetweenTwoColors. */
        mg.m_Mode = 2;
        mg.m_ColorMin = (NSColor){
            r + (1.0f - r) * 0.35f,
            g + (1.0f - g) * 0.35f,
            b + (1.0f - b) * 0.35f,
            e->orig_alpha_min
        };
        mg.m_ColorMax = (NSColor){r, g, b, e->orig_alpha_max};
    } else {
        mg.m_Mode = 0;
        mg.m_ColorMin = (NSColor){r, g, b, e->orig_alpha_min};
        mg.m_ColorMax = (NSColor){r, g, b, e->orig_alpha_max};
    }
    fn_ps_main_set_col_inj(&ps, &mg);

    if (e->col_neutralized && fn_ps_col_set_color) {
        fn_ps_col_set_color(&ps, &e->white_col);
    }
    if (e->mat_prepared && runtime_unity_object_alive(e->psr) && runtime_unity_object_alive(e->neutral_mat)) {
        typedef void (ABI *SetSharedMatFn)(void *r, void *mat, void *m);
        ((SetSharedMatFn)CODE(C_REND_SET_SHARED_MAT))(e->psr, e->neutral_mat, NULL);
    }

    e->last_r = r;
    e->last_g = g;
    e->last_b = b;
    e->has_last_rgb = true;
}

static void apply_rgb_to_ps(void *ps, float r, float g, float b) {
    apply_rgb_to_ps_entry(get_ps_entry(ps, false), r, g, b);
}

void runtime_note_skins_apply_ps_list_rgb(void *ps_list, float r, float g, float b) {
    if (!ns_should_apply()) return;
    int32_t count = 0;
    void **items = ns_managed_ptr_seq(ps_list, &count);
    if (!items || count <= 0) return;
    for (int32_t i = 0; i < count; ++i) {
        apply_rgb_to_ps(items[i], r, g, b);
    }
}

/* ========================================================================= */
/* Emitter Component Cache (ParticleSystems + SpriteRenderers)               */
/* ========================================================================= */

#define MAX_EMITTER_CACHE 4096
#define EMITTER_HASH_CAP  8192
#define MAX_PS_PER_EMITTER 32
#define MAX_SR_PER_EMITTER 16

typedef struct {
    void         *owner;
    void         *ps_list_hint;
    void         *root_xform;
    bool          persistent;
    bool          empty;
    bool          has_applied;
    int           last_category;
    float         last_r, last_g, last_b;
    void         *last_tag;
    int           ps_count;
    void         *ps[MAX_PS_PER_EMITTER];
    PSCacheEntry *ps_entries[MAX_PS_PER_EMITTER];
    int           sr_count;
    void         *sr[MAX_SR_PER_EMITTER];
} EmitterCacheEntry;

static EmitterCacheEntry s_emitter_cache[MAX_EMITTER_CACHE];
static size_t            s_emitter_evict = 0;
static int32_t           s_emitter_hash[EMITTER_HASH_CAP];
static bool              s_emitter_hash_inited = false;
static size_t            s_emitter_tombstones = 0;

static void rebuild_emitter_hash(void) {
    for (size_t i = 0; i < EMITTER_HASH_CAP; ++i) s_emitter_hash[i] = -1;
    s_emitter_tombstones = 0;
    s_emitter_hash_inited = true;
    const size_t mask = EMITTER_HASH_CAP - 1u;
    for (size_t idx = 0; idx < MAX_EMITTER_CACHE; ++idx) {
        void *owner = s_emitter_cache[idx].owner;
        if (!owner) continue;
        size_t pos = (size_t)ns_ptr_hash(owner) & mask;
        while (s_emitter_hash[pos] >= 0) pos = (pos + 1u) & mask;
        s_emitter_hash[pos] = (int32_t)idx;
    }
}

static void clear_emitter_entry_at(size_t idx) {
    EmitterCacheEntry *e = &s_emitter_cache[idx];
    if (e->owner && s_emitter_hash_inited) {
        const size_t mask = EMITTER_HASH_CAP - 1u;
        size_t pos = (size_t)ns_ptr_hash(e->owner) & mask;
        for (size_t probe = 0; probe < EMITTER_HASH_CAP; ++probe) {
            int32_t h = s_emitter_hash[pos];
            if (h == -1) break;
            if (h == (int32_t)idx) {
                s_emitter_hash[pos] = -2;
                ++s_emitter_tombstones;
                break;
            }
            pos = (pos + 1u) & mask;
        }
    }
    memset(e, 0, sizeof(*e));
}

static void reset_emitter_cache(void) {
    memset(s_emitter_cache, 0, sizeof(s_emitter_cache));
    s_emitter_evict = 0;
    rebuild_emitter_hash();
}

static void add_unique_ptr(void **arr, int *cnt, int max_cnt, void *val) {
    if (!runtime_unity_object_alive(val) || *cnt >= max_cnt) return;
    for (int i = 0; i < *cnt; ++i) {
        if (arr[i] == val) return;
    }
    arr[(*cnt)++] = val;
}

static void collect_transform_components(void *xform, int depth, EmitterCacheEntry *out) {
    if (!runtime_unity_object_alive(xform) || depth > 5) return;

    typedef void *   (ABI *GetGOFn)(void *c, void *m);
    typedef int32_t  (ABI *ChildCntFn)(void *t, void *m);
    typedef void *   (ABI *GetChildFn)(void *t, int32_t idx, void *m);

    void *go = ((GetGOFn)CODE(C_COMP_GET_GAMEOBJECT))(xform, NULL);
    if (runtime_unity_object_alive(go)) {
        void *ps = runtime_get_component_by_name(go, "UnityEngine.ParticleSystem", "ParticleSystem");
        if (ps) add_unique_ptr(out->ps, &out->ps_count, MAX_PS_PER_EMITTER, ps);
        void *sr = runtime_get_component_by_name(go, "UnityEngine.SpriteRenderer", "SpriteRenderer");
        if (sr) add_unique_ptr(out->sr, &out->sr_count, MAX_SR_PER_EMITTER, sr);
    }

    int32_t n = ((ChildCntFn)CODE(C_XFORM_CHILD_COUNT))(xform, NULL);
    if (n < 0 || n > 64) return;
    for (int32_t i = 0; i < n; ++i) {
        void *child = ((GetChildFn)CODE(C_XFORM_GET_CHILD))(xform, i, NULL);
        collect_transform_components(child, depth + 1, out);
    }
}

static void prepare_emitter_sr_assets(EmitterCacheEntry *entry) {
    ensure_particle_icalls();
    typedef void * (ABI *GetSharedMatFn)(void *r, void *m);
    typedef void   (ABI *SetSharedMatFn)(void *r, void *mat, void *m);

    for (int i = 0; i < entry->ps_count; ++i) {
        entry->ps_entries[i] = get_ps_entry(entry->ps[i], entry->persistent);
    }

    for (int i = 0; i < entry->sr_count; ++i) {
        void *sr = entry->sr[i];
        if (!runtime_unity_object_alive(sr)) continue;

        void *orig_mat = ((GetSharedMatFn)CODE(C_REND_GET_SHARED_MAT))(sr, NULL);
        void *nmat = runtime_unity_object_alive(orig_mat) ? get_neutral_material(orig_mat) : NULL;
        if (runtime_unity_object_alive(nmat) && nmat != orig_mat) {
            ((SetSharedMatFn)CODE(C_REND_SET_SHARED_MAT))(sr, nmat, NULL);
        }
    }
}

static size_t find_emitter_hash_pos(const void *owner, size_t *out_first_tomb, int32_t *out_idx) {
    if (!s_emitter_hash_inited) rebuild_emitter_hash();
    const size_t mask = EMITTER_HASH_CAP - 1u;
    size_t pos = (size_t)ns_ptr_hash(owner) & mask;
    size_t first_tomb = EMITTER_HASH_CAP;
    for (size_t probe = 0; probe < EMITTER_HASH_CAP; ++probe) {
        int32_t h = s_emitter_hash[pos];
        if (h == -1) break;
        if (h == -2) {
            if (first_tomb >= EMITTER_HASH_CAP) first_tomb = pos;
        } else if (s_emitter_cache[h].owner == owner) {
            if (out_first_tomb) *out_first_tomb = first_tomb;
            if (out_idx) *out_idx = h;
            return pos;
        }
        pos = (pos + 1u) & mask;
    }
    if (out_first_tomb) *out_first_tomb = first_tomb;
    if (out_idx) *out_idx = -1;
    return pos;
}

static void clear_emitter_hold_tag(void *owner) {
    if (!owner) return;
    int32_t idx = -1;
    find_emitter_hash_pos(owner, NULL, &idx);
    if (idx >= 0) {
        s_emitter_cache[idx].last_tag = NULL;
    }
}

static void apply_category_to_emitter_tagged(void *owner, void *root_xform, void *ps_list,
                                             int category, void *tag, bool force, bool persistent) {
    if (!ns_should_apply()) return;
    if (category < 0 || category >= THEATRHYTHM_NOTE_COUNT) return;
    if (!ns_valid_ptr(owner)) return;

    float r, g, b;
    theatrhythm_note_skin_get_rgb(category, &r, &g, &b);

    size_t first_tomb = EMITTER_HASH_CAP;
    int32_t existing_idx = -1;
    size_t pos = find_emitter_hash_pos(owner, &first_tomb, &existing_idx);

    EmitterCacheEntry *entry = NULL;
    size_t alloc_idx = MAX_EMITTER_CACHE;
    if (existing_idx >= 0) {
        EmitterCacheEntry *c = &s_emitter_cache[existing_idx];
        if (c->ps_list_hint == ps_list && c->root_xform == root_xform) {
            if (persistent) c->persistent = true;
            if (c->empty) {
                return;
            }
            if (!force && c->has_applied &&
                c->last_category == category &&
                fabsf(c->last_r - r) < 0.004f &&
                fabsf(c->last_g - g) < 0.004f &&
                fabsf(c->last_b - b) < 0.004f) {
                c->last_tag = tag;
                return;
            }
            entry = c;
        } else {
            clear_emitter_entry_at((size_t)existing_idx);
            alloc_idx = (size_t)existing_idx;
            first_tomb = pos;
        }
    }

    if (!entry) {
        if (alloc_idx >= MAX_EMITTER_CACHE) {
            for (size_t i = 0; i < MAX_EMITTER_CACHE; ++i) {
                if (!s_emitter_cache[i].owner) {
                    alloc_idx = i;
                    break;
                }
            }
        }
        if (alloc_idx >= MAX_EMITTER_CACHE) {
            for (size_t probe = 0; probe < MAX_EMITTER_CACHE; ++probe) {
                size_t idx = (s_emitter_evict + probe) % MAX_EMITTER_CACHE;
                if (!s_emitter_cache[idx].persistent) {
                    s_emitter_evict = (idx + 1u) % MAX_EMITTER_CACHE;
                    clear_emitter_entry_at(idx);
                    alloc_idx = idx;
                    break;
                }
            }
        }
        if (alloc_idx >= MAX_EMITTER_CACHE) {
            size_t idx = s_emitter_evict;
            s_emitter_evict = (s_emitter_evict + 1u) % MAX_EMITTER_CACHE;
            clear_emitter_entry_at(idx);
            alloc_idx = idx;
        }

        EmitterCacheEntry *free_slot = &s_emitter_cache[alloc_idx];
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->owner = owner;
        free_slot->ps_list_hint = ps_list;
        free_slot->root_xform = root_xform;
        free_slot->persistent = persistent;
        if (s_emitter_tombstones > EMITTER_HASH_CAP / 4u) {
            rebuild_emitter_hash();
        } else {
            size_t ins_pos = (first_tomb < EMITTER_HASH_CAP) ? first_tomb : pos;
            if (s_emitter_hash[ins_pos] == -2) --s_emitter_tombstones;
            s_emitter_hash[ins_pos] = (int32_t)alloc_idx;
        }

        if (ns_valid_ptr(ps_list)) {
            int32_t cnt = 0;
            void **items = ns_managed_ptr_seq(ps_list, &cnt);
            if (items) {
                for (int32_t i = 0; i < cnt; ++i) {
                    add_unique_ptr(free_slot->ps, &free_slot->ps_count, MAX_PS_PER_EMITTER, items[i]);
                }
            }
        }
        if (runtime_unity_object_alive(root_xform)) {
            collect_transform_components(root_xform, 0, free_slot);
        }
        if (free_slot->ps_count == 0 && free_slot->sr_count == 0) {
            free_slot->empty = true;
            return;
        }
        prepare_emitter_sr_assets(free_slot);
        entry = free_slot;
    } else if (force) {
        prepare_emitter_sr_assets(entry);
    }

    for (int i = 0; i < entry->ps_count; ++i) {
        PSCacheEntry *pe = entry->ps_entries[i];
        if (!pe || pe->ps != entry->ps[i]) {
            pe = get_ps_entry(entry->ps[i], entry->persistent);
            entry->ps_entries[i] = pe;
        }
        apply_rgb_to_ps_entry(pe, r, g, b);
    }
    if (entry->sr_count > 0) {
        typedef void (ABI *SrSetColFn)(void *sr, NSColor col, void *m);
        SrSetColFn set_col = (SrSetColFn)CODE(C_SR_SET_COLOR);
        NSColor col = {r, g, b, 1.0f};
        for (int i = 0; i < entry->sr_count; ++i) {
            if (!entry->sr[i]) continue;
            set_col(entry->sr[i], col, NULL);
        }
    }
    entry->has_applied = true;
    entry->last_category = category;
    entry->last_r = r;
    entry->last_g = g;
    entry->last_b = b;
    entry->last_tag = tag;
    entry->root_xform = root_xform;
}

static void apply_category_to_emitter(void *owner, void *root_xform, void *ps_list,
                                      int category, bool persistent) {
    apply_category_to_emitter_tagged(owner, root_xform, ps_list, category, NULL, false, persistent);
}

static void apply_category_to_effect_player(void *ep, int category) {
    if (!ns_mem_readable(ep, 0x30)) return;
    /* JudgeEffect/EffectPlayer stores Transform* at +0x10 (+0x18 is GameObject*) and List<ParticleSystem> at +0x28. */
    void *xform   = *(void **)((char *)ep + 0x10);
    void *ps_list = *(void **)((char *)ep + 0x28);
    apply_category_to_emitter_tagged(ep, xform, ps_list, category, NULL, false, true);
}

static void apply_category_to_loop_emitter_opts(void *le, int category, void *tag, bool force) {
    if (!ns_mem_readable(le, 0x28)) return;
    void *xform   = *(void **)((char *)le + 0x18);
    void *ps_list = *(void **)((char *)le + 0x20);
    apply_category_to_emitter_tagged(le, xform, ps_list, category, tag, force, true);
}

static void apply_category_to_loop_emitter(void *le, int category) {
    apply_category_to_loop_emitter_opts(le, category, NULL, false);
}

static void apply_category_to_loop_emitter_hold(void *le, int category, void *hold_tag) {
    apply_category_to_loop_emitter_opts(le, category, hold_tag, false);
}

static void *select_next_ep(void *judge_effect) {
    if (!ns_mem_readable(judge_effect, 0x20)) return NULL;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return NULL;
    for (int32_t i = 0; i < cnt; ++i) {
        void *ep = eps[i];
        if (ns_mem_readable(ep, 0x39) && *(uint8_t *)((char *)ep + 0x38) == 0) {
            return ep;
        }
    }
    void *best_ep = NULL;
    float best_elapsed = -1e9f;
    for (int32_t i = 0; i < cnt; ++i) {
        void *ep = eps[i];
        if (!ns_mem_readable(ep, 0x38)) continue;
        float elapsed = *(float *)((char *)ep + 0x30) - *(float *)((char *)ep + 0x34);
        if (!best_ep || elapsed > best_elapsed) {
            best_ep = ep;
            best_elapsed = elapsed;
        }
    }
    return best_ep;
}

static void apply_category_to_two_eps(void *judge_effect, int category) {
    if (!ns_mem_readable(judge_effect, 0x20)) return;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return;

    int applied = 0;
    for (int32_t i = 0; i < cnt && applied < 2; ++i) {
        void *ep = eps[i];
        if (ns_mem_readable(ep, 0x39) && *(uint8_t *)((char *)ep + 0x38) == 0) {
            apply_category_to_effect_player(ep, category);
            ++applied;
        }
    }
    if (applied < 2) {
        apply_category_to_effect_player(select_next_ep(judge_effect), category);
    }
}

/* ========================================================================= */
/* Native Note Sprite & Texture Template Capture / Recoloring                */
/* ========================================================================= */

typedef struct {
    void    *orig_spr;
    uint32_t orig_pin;
    uint8_t *orig_pixels;
    int      w;
    int      h;
    float    piv_x;
    float    piv_y;
    float    ppu;
    NSVec4   border;
    void    *active_spr;
    uint32_t active_pin;
    void    *active_tex;
    uint32_t active_tex_pin;
    int      built_hue;
    int      built_sat;
    bool     has_template;
} NoteSlotState;

typedef struct {
    void    *orig_tex;
    uint32_t orig_pin;
    uint8_t *orig_pixels;
    int      w;
    int      h;
    int32_t  wrap_mode;
    void    *active_tex;
    uint32_t active_pin;
    int      built_hue;
    int      built_sat;
    bool     has_template;
} HoldTexState;

static NoteSlotState s_slots[SLOT_SPRITE_COUNT];
static HoldTexState  s_hold_tex;
static void         *s_active_mst = NULL;
static uint32_t      s_stage_skin_ver = 0;

/* History of generated Sprite* pointers for each slot (so we recognize our own replacements even across live edits). */
#define MAX_KNOWN_GENERATED 64
typedef struct {
    void *spr;
    int   slot;
} KnownGenSprite;
static KnownGenSprite s_known_gen[MAX_KNOWN_GENERATED];
static size_t s_known_gen_idx = 0;

static void record_known_sprite(void *spr, int slot) {
    if (!spr) return;
    for (size_t i = 0; i < MAX_KNOWN_GENERATED; ++i) {
        if (s_known_gen[i].spr == spr) {
            s_known_gen[i].slot = slot;
            return;
        }
    }
    s_known_gen[s_known_gen_idx].spr = spr;
    s_known_gen[s_known_gen_idx].slot = slot;
    s_known_gen_idx = (s_known_gen_idx + 1) % MAX_KNOWN_GENERATED;
}

static int find_slot_for_sprite(void *spr) {
    if (!spr) return -1;
    for (int i = 0; i < SLOT_SPRITE_COUNT; ++i) {
        if (s_slots[i].orig_spr == spr || s_slots[i].active_spr == spr) return i;
    }
    for (size_t i = 0; i < MAX_KNOWN_GENERATED; ++i) {
        if (s_known_gen[i].spr == spr) return s_known_gen[i].slot;
    }
    return -1;
}

static bool capture_sprite_template(int slot_idx, void *native_spr) {
    NoteSlotState *st = &s_slots[slot_idx];
    if (!runtime_unity_object_alive(native_spr)) return false;

    /* Ignore sprites that we already generated. */
    for (size_t i = 0; i < MAX_KNOWN_GENERATED; ++i) {
        if (s_known_gen[i].spr == native_spr) return st->has_template;
    }
    if (st->has_template && st->orig_spr == native_spr && st->orig_pixels &&
        runtime_unity_object_alive(st->orig_spr)) {
        return true;
    }

    typedef void *  (ABI *SprGetTexFn)(void *spr, void *m);
    typedef NSRect  (ABI *SprGetRectFn)(void *spr, void *m);
    typedef NSVec2  (ABI *SprGetVec2Fn)(void *spr, void *m);
    typedef float   (ABI *SprGetPpuFn)(void *spr, void *m);
    typedef NSVec4  (ABI *SprGetBorderFn)(void *spr, void *m);

    void *atlas_tex = ((SprGetTexFn)CODE(C_SPR_GET_TEX))(native_spr, NULL);
    TexReadbackEntry *tb = get_tex_readback(atlas_tex);
    if (!tb || !tb->rgba) return false;

    NSRect tex_rect  = ((SprGetRectFn)CODE(C_SPR_GET_TEX_RECT))(native_spr, NULL);
    NSVec2 tex_off   = ((SprGetVec2Fn)CODE(C_SPR_GET_TEX_OFFSET))(native_spr, NULL);
    NSVec2 pix_pivot = ((SprGetVec2Fn)CODE(C_SPR_GET_PIVOT))(native_spr, NULL);
    float  ppu       = ((SprGetPpuFn)CODE(C_SPR_GET_PPU))(native_spr, NULL);
    NSVec4 border    = ((SprGetBorderFn)CODE(C_SPR_GET_BORDER))(native_spr, NULL);

    int tx = (int)floorf(tex_rect.x + 0.5f);
    int ty = (int)floorf(tex_rect.y + 0.5f);
    int tw = (int)floorf(tex_rect.width + 0.5f);
    int th = (int)floorf(tex_rect.height + 0.5f);
    if (tx < 0) { tw += tx; tx = 0; }
    if (ty < 0) { th += ty; ty = 0; }
    if (tx + tw > tb->w) tw = tb->w - tx;
    if (ty + th > tb->h) th = tb->h - ty;
    if (tw <= 0 || th <= 0) return false;

    size_t row_bytes = (size_t)tw * 4u;
    uint8_t *crop = (uint8_t *)malloc(row_bytes * (size_t)th);
    if (!crop) return false;
    for (int r = 0; r < th; ++r) {
        memcpy(crop + (size_t)r * row_bytes,
               tb->rgba + (((size_t)(ty + r) * (size_t)tb->w + (size_t)tx) * 4u),
               row_bytes);
    }

    if (st->orig_pixels) free(st->orig_pixels);
    if (st->orig_pin) runtime_gc_unpin_obj(st->orig_pin);

    st->orig_spr = native_spr;
    st->orig_pin = runtime_gc_pin_obj(native_spr);
    st->orig_pixels = crop;
    st->w = tw;
    st->h = th;
    st->piv_x = (tw > 0) ? ((pix_pivot.x - tex_off.x) / (float)tw) : 0.5f;
    st->piv_y = (th > 0) ? ((pix_pivot.y - tex_off.y) / (float)th) : 0.5f;
    st->ppu = (ppu > 0.001f) ? ppu : 100.0f;
    st->border = border;
    st->built_hue = -1;
    st->built_sat = -1;
    st->has_template = true;
    return true;
}

static bool capture_hold_tex_template(void *native_tex) {
    if (!runtime_unity_object_alive(native_tex)) return false;
    if (s_hold_tex.active_tex == native_tex) return s_hold_tex.has_template;
    if (s_hold_tex.has_template && s_hold_tex.orig_tex == native_tex && s_hold_tex.orig_pixels &&
        runtime_unity_object_alive(s_hold_tex.orig_tex)) {
        return true;
    }

    TexReadbackEntry *tb = get_tex_readback(native_tex);
    if (!tb || !tb->rgba) return false;

    size_t bytes = (size_t)tb->w * (size_t)tb->h * 4u;
    uint8_t *copy = (uint8_t *)malloc(bytes);
    if (!copy) return false;
    memcpy(copy, tb->rgba, bytes);

    typedef int32_t (ABI *GetWrapFn)(void *t, void *m);
    int32_t wrap = ((GetWrapFn)CODE(C_TEX_GET_WRAP))(native_tex, NULL);

    if (s_hold_tex.orig_pixels) free(s_hold_tex.orig_pixels);
    if (s_hold_tex.orig_pin) runtime_gc_unpin_obj(s_hold_tex.orig_pin);

    s_hold_tex.orig_tex = native_tex;
    s_hold_tex.orig_pin = runtime_gc_pin_obj(native_tex);
    s_hold_tex.orig_pixels = copy;
    s_hold_tex.w = tb->w;
    s_hold_tex.h = tb->h;
    s_hold_tex.wrap_mode = wrap;
    s_hold_tex.built_hue = -1;
    s_hold_tex.built_sat = -1;
    s_hold_tex.has_template = true;
    return true;
}

static void rebuild_trigger_sprite_slot(void *ts, int slot_idx) {
    NoteSlotState *st = &s_slots[slot_idx];
    if (!st->has_template || !st->orig_pixels) return;

    int hue = 0, sat = 100;
    int cat = kSlotCategory[slot_idx];
    theatrhythm_note_skin_get(cat, &hue, &sat);

    if (!runtime_unity_object_alive(st->active_spr) || st->built_hue != hue || st->built_sat != sat) {
        size_t bytes = (size_t)st->w * (size_t)st->h * 4u;
        uint8_t *buf = (uint8_t *)malloc(bytes);
        if (!buf) return;
        memcpy(buf, st->orig_pixels, bytes);
        runtime_note_skins_recolor_rgba(buf, st->w, st->h, cat);

        void *new_spr = runtime_create_sprite_from_rgba_metrics(
            buf, st->w, st->h,
            0.0f, 0.0f, (float)st->w, (float)st->h,
            st->piv_x, st->piv_y, st->ppu,
            st->border.x, st->border.y, st->border.z, st->border.w,
            kSlotTags[slot_idx]);
        free(buf);

        if (runtime_unity_object_alive(new_spr)) {
            typedef void * (ABI *SprGetTexFn)(void *spr, void *m);
            void *new_tex = ((SprGetTexFn)CODE(C_SPR_GET_TEX))(new_spr, NULL);
            if (st->active_spr) enqueue_deferred_destroy(st->active_spr, st->active_pin);
            if (st->active_tex) enqueue_deferred_destroy(st->active_tex, st->active_tex_pin);

            st->active_spr = new_spr;
            st->active_pin = runtime_gc_pin_obj(new_spr);
            st->active_tex = new_tex;
            st->active_tex_pin = runtime_unity_object_alive(new_tex) ? runtime_gc_pin_obj(new_tex) : 0;
            st->built_hue = hue;
            st->built_sat = sat;
            record_known_sprite(new_spr, slot_idx);
        }
    }

    if (ns_valid_ptr(ts) && runtime_unity_object_alive(st->active_spr)) {
        *(void **)((char *)ts + kSlotOffset[slot_idx]) = st->active_spr;
    }
}

static void rebuild_hold_line_tex(void *ts) {
    if (!s_hold_tex.has_template || !s_hold_tex.orig_pixels) return;

    int hue = 0, sat = 100;
    theatrhythm_note_skin_get(THEATRHYTHM_NOTE_HOLD, &hue, &sat);

    if (!runtime_unity_object_alive(s_hold_tex.active_tex) ||
        s_hold_tex.built_hue != hue || s_hold_tex.built_sat != sat) {
        size_t bytes = (size_t)s_hold_tex.w * (size_t)s_hold_tex.h * 4u;
        uint8_t *buf = (uint8_t *)malloc(bytes);
        if (!buf) return;
        memcpy(buf, s_hold_tex.orig_pixels, bytes);
        runtime_note_skins_recolor_rgba(buf, s_hold_tex.w, s_hold_tex.h, THEATRHYTHM_NOTE_HOLD);

        void *new_tex = runtime_create_texture2d_rgba(buf, s_hold_tex.w, s_hold_tex.h, "NoteSkin/HoldLineTex");
        free(buf);
        if (runtime_unity_object_alive(new_tex)) {
            typedef void (ABI *SetWrapFn)(void *t, int32_t w, void *m);
            ((SetWrapFn)CODE(C_TEX_SET_WRAP))(new_tex, s_hold_tex.wrap_mode, NULL);
            if (s_hold_tex.active_tex) {
                enqueue_deferred_destroy(s_hold_tex.active_tex, s_hold_tex.active_pin);
            }
            s_hold_tex.active_tex = new_tex;
            s_hold_tex.active_pin = runtime_gc_pin_obj(new_tex);
            s_hold_tex.built_hue = hue;
            s_hold_tex.built_sat = sat;
        }
    }

    if (ns_valid_ptr(ts) && runtime_unity_object_alive(s_hold_tex.active_tex)) {
        *(void **)((char *)ts + HOLD_LINE_TEX_OFFSET) = s_hold_tex.active_tex;
    }
}

static void sync_trigger_sprite_object(void *ts) {
    if (!ns_mem_readable(ts, 0x68)) return;

    for (int i = 0; i < SLOT_SPRITE_COUNT; ++i) {
        void *cur = *(void **)((char *)ts + kSlotOffset[i]);
        if (runtime_unity_object_alive(cur)) {
            capture_sprite_template(i, cur);
        }
        rebuild_trigger_sprite_slot(ts, i);
    }

    void *cur_tex = *(void **)((char *)ts + HOLD_LINE_TEX_OFFSET);
    if (runtime_unity_object_alive(cur_tex)) {
        capture_hold_tex_template(cur_tex);
    }
    rebuild_hold_line_tex(ts);
}

static void refresh_sr_by_known_slot(void *sr) {
    if (!runtime_unity_object_alive(sr)) return;
    typedef void * (ABI *SrGetSprFn)(void *s, void *m);
    typedef void   (ABI *SrSetSprFn)(void *s, void *spr, void *m);
    void *cur = ((SrGetSprFn)CODE(C_SR_GET_SPRITE))(sr, NULL);
    int slot = find_slot_for_sprite(cur);
    if (slot >= 0 && runtime_unity_object_alive(s_slots[slot].active_spr) && cur != s_slots[slot].active_spr) {
        ((SrSetSprFn)CODE(C_SR_SET_SPRITE))(sr, s_slots[slot].active_spr, NULL);
    }
}

static void refresh_xform_sprite_renderers(void *xform, int depth) {
    if (!runtime_unity_object_alive(xform) || depth > 4) return;
    typedef void *  (ABI *GetGOFn)(void *c, void *m);
    typedef int32_t (ABI *ChildCntFn)(void *t, void *m);
    typedef void *  (ABI *GetChildFn)(void *t, int32_t idx, void *m);

    void *go = ((GetGOFn)CODE(C_COMP_GET_GAMEOBJECT))(xform, NULL);
    if (runtime_unity_object_alive(go)) {
        void *sr = runtime_get_component_by_name(go, "UnityEngine.SpriteRenderer", "SpriteRenderer");
        refresh_sr_by_known_slot(sr);
    }
    int32_t n = ((ChildCntFn)CODE(C_XFORM_CHILD_COUNT))(xform, NULL);
    if (n < 0 || n > 32) return;
    for (int32_t i = 0; i < n; ++i) {
        refresh_xform_sprite_renderers(((GetChildFn)CODE(C_XFORM_GET_CHILD))(xform, i, NULL), depth + 1);
    }
}

static int slot_for_trig_type(uint8_t trig_type) {
    switch (trig_type) {
    case NS_TRIG_TOUCH:         return SLOT_TOUCH;
    case NS_TRIG_SLIDE:         return SLOT_SLIDE;
    case NS_TRIG_LINHOLD_START:
    case NS_TRIG_CRVHOLD_START: return SLOT_HOLD_START;
    case NS_TRIG_LINHOLD_PASS:
    case NS_TRIG_CRVHOLD_PASS:  return SLOT_HOLD_PASS;
    case NS_TRIG_LINHOLD_END_T:
    case NS_TRIG_CRVHOLD_END_T: return SLOT_HOLD_END_TOUCH;
    case NS_TRIG_LINHOLD_END_S:
    case NS_TRIG_CRVHOLD_END_S: return SLOT_HOLD_END_SLIDE;
    default:                    return -1;
    }
}

static void    *s_simple_colors_synced_arr  = NULL;
static int      s_simple_colors_synced_type = -1;
static uint32_t s_simple_colors_synced_ver  = 0;

void runtime_note_skins_sync_simple_colors(void) {
    if (!s_installed || !s_module || !s_is_game_104 || RVA_TRIGGER_OBJECT_TYPEINFO == 0ULL) return;
    void **p_ti = (void **)(s_module + RVA_TRIGGER_OBJECT_TYPEINFO);
    if (!ns_mem_readable(p_ti, sizeof(void *))) return;
    void *klass = *p_ti;
    if (!ns_mem_readable(klass, 0xe8)) return;
    if (*(int32_t *)((char *)klass + 0xe0) == 0) {
        typedef void (ABI *ClassInitFn)(void *);
        ((ClassInitFn)(s_module + RVA_IL2CPP_CLASS_INIT))(klass);
    }
    void *static_fields = *(void **)((char *)klass + 0xb8);
    if (!ns_mem_readable(static_fields, 0x18)) return;
    void *arr = *(void **)((char *)static_fields + 0x10);
    if (!ns_mem_readable(arr, 0x60)) return;

    uint32_t skin_ver = theatrhythm_note_skin_version();
    if (arr == s_simple_colors_synced_arr &&
        s_active_color_type == s_simple_colors_synced_type &&
        skin_ver == s_simple_colors_synced_ver) {
        return;
    }

    float *touch_col = (float *)((char *)arr + 0x20);
    float *hold_col  = (float *)((char *)arr + 0x30);
    if (ns_should_apply()) {
        int th = 0, ts = 100, hh = 120, hs = 100;
        theatrhythm_note_skin_get(THEATRHYTHM_NOTE_TOUCH, &th, &ts);
        theatrhythm_note_skin_get(THEATRHYTHM_NOTE_HOLD,  &hh, &hs);
        float tr, tg, tb, hr, hg, hb;
        /* Preserve vanilla Simple Mode value/saturation envelope:
         * Touch default {0.9607843, 0.18039216, 0.18039216} (H=0, S=0.8125, V=0.9607843)
         * Hold  default {0.0,       0.79607844, 0.0       } (H=120, S=1.0,  V=0.79607844) */
        ns_hsv_to_rgb((float)th, ns_clamp01((float)ts / 100.0f) * 0.8125f, 0.9607843f, &tr, &tg, &tb);
        ns_hsv_to_rgb((float)hh, ns_clamp01((float)hs / 100.0f),           0.79607844f, &hr, &hg, &hb);
        touch_col[0] = tr; touch_col[1] = tg; touch_col[2] = tb; touch_col[3] = 1.0f;
        hold_col[0]  = hr; hold_col[1]  = hg; hold_col[2]  = hb; hold_col[3]  = 1.0f;
    } else {
        touch_col[0] = 0.9607843f; touch_col[1] = 0.18039216f; touch_col[2] = 0.18039216f; touch_col[3] = 1.0f;
        hold_col[0]  = 0.0f;       hold_col[1]  = 0.79607844f; hold_col[2]  = 0.0f;        hold_col[3]  = 1.0f;
    }
    s_simple_colors_synced_arr  = arr;
    s_simple_colors_synced_type = s_active_color_type;
    s_simple_colors_synced_ver  = skin_ver;
}

void runtime_note_skins_on_set_simple_type(void *tobj) {
    if (!s_installed || !s_is_game_104 || !ns_should_apply()) return;
    if (!ns_mem_readable(tobj, TOBJ_TRIG_TYPE_OFF + 1u)) return;
    if (runtime_slidehold_is_registered_trigger_obj(tobj)) return;

    void *simple_arr = *(void **)((char *)tobj + 0x68);
    if (!ns_mem_readable(simple_arr, 0x28) || *(void **)((char *)simple_arr + 0x10) != NULL) return;
    int32_t sc = *(int32_t *)((char *)simple_arr + 0x18);
    if (sc < 1 || sc > 4 || !ns_mem_readable((char *)simple_arr + 0x20, (size_t)sc * sizeof(void *))) return;

    uint8_t trig_type = *(uint8_t *)((char *)tobj + TOBJ_TRIG_TYPE_OFF);
    void **slots = (void **)((char *)simple_arr + 0x20);
    for (int32_t k = 0; k < sc; ++k) {
        int sidx = find_slot_for_sprite(slots[k]);
        if (sidx < 0) {
            if (k == 0) {
                sidx = slot_for_trig_type(trig_type);
            } else if (k == 1 && (trig_type == NS_TRIG_LINHOLD_START || trig_type == NS_TRIG_CRVHOLD_START)) {
                /* CreateTriggers (0x271a5da) assigns TriggerSprite+0x40 (HoldEndTouch) into _simpleSprites[1]. */
                sidx = SLOT_HOLD_END_TOUCH;
            }
        }
        if (sidx >= 0 && runtime_unity_object_alive(s_slots[sidx].active_spr)) {
            slots[k] = s_slots[sidx].active_spr;
        }
    }
}

static void refresh_stage_visuals(void *mst) {
    if (!runtime_unity_object_alive(mst) || !ns_mem_readable(mst, 0x98)) return;
    runtime_note_skins_sync_simple_colors();

    void *ts = *(void **)((char *)mst + 0x40);
    if (ns_mem_readable(ts, 0x68)) {
        sync_trigger_sprite_object(ts);
    }

    typedef void (ABI *JointSetFn)(void *jl, void *spr, void *m);
    typedef void (ABI *LHoldSetFn)(void *lh, void *spr, void *m);
    typedef void (ABI *CHoldSetFn)(void *ch, void *tex, void *m);
    typedef void (ABI *SetSimpleColFn)(void *tobj, int32_t color_type);

    /* Update live TriggerObjects (excluding registered SlideHold notes, updated separately below). */
    int32_t trig_cnt = 0;
    void **trigs = ns_managed_ptr_seq(*(void **)((char *)mst + 0x70), &trig_cnt);
    for (int32_t i = 0; i < trig_cnt; ++i) {
        void *tobj = trigs[i];
        if (!runtime_unity_object_alive(tobj) || !ns_mem_readable(tobj, 0x40)) continue;
        if (runtime_slidehold_is_registered_trigger_obj(tobj)) continue;

        runtime_note_skins_on_set_simple_type(tobj);
        refresh_sr_by_known_slot(*(void **)((char *)tobj + 0x38));

        if (s_is_game_104 && RVA_TOBJ_SET_SIMPLE_COLOR != 0ULL &&
            ns_mem_readable(tobj, 0x64) && *(int32_t *)((char *)tobj + 0x60) == 1) {
            /* Re-apply _simpleColors[0, kind] into tobj->_baseColor and _spriteRenderer.color. */
            ((SetSimpleColFn)(s_module + RVA_TOBJ_SET_SIMPLE_COLOR))(tobj, 0);
        }

        void *twin_sub = *(void **)((char *)tobj + 0x30);
        if (runtime_unity_object_alive(twin_sub) && ns_mem_readable(twin_sub, 0x108)) {
            refresh_sr_by_known_slot(*(void **)((char *)twin_sub + 0xf8));
            int32_t arrow_cnt = 0;
            void **arrows = ns_managed_ptr_seq(*(void **)((char *)twin_sub + 0x100), &arrow_cnt);
            for (int32_t a = 0; a < arrow_cnt; ++a) refresh_sr_by_known_slot(arrows[a]);
        } else {
            refresh_xform_sprite_renderers(*(void **)((char *)tobj + 0x28), 0);
        }
    }

    /* Update live JointLines */
    if (runtime_unity_object_alive(s_slots[SLOT_JOINT_LINE].active_spr)) {
        JointSetFn set_joint = (JointSetFn)CODE(C_JOINT_SET_SPRITE);
        int32_t jl_cnt = 0;
        void **jls = ns_managed_ptr_seq(*(void **)((char *)mst + 0x80), &jl_cnt);
        for (int32_t i = 0; i < jl_cnt; ++i) {
            if (runtime_unity_object_alive(jls[i])) {
                set_joint(jls[i], s_slots[SLOT_JOINT_LINE].active_spr, NULL);
            }
        }
    }

    /* Update live LinearHoldLines (skip SlideHold ribbons managed by runtime_slidehold.c) */
    if (runtime_unity_object_alive(s_slots[SLOT_HOLD_LINE].active_spr)) {
        LHoldSetFn set_lh = (LHoldSetFn)CODE(C_LHOLD_SET_SPRITE);
        int32_t lh_cnt = 0;
        void **lhs = ns_managed_ptr_seq(*(void **)((char *)mst + 0x88), &lh_cnt);
        for (int32_t i = 0; i < lh_cnt; ++i) {
            void *lh = lhs[i];
            if (!runtime_unity_object_alive(lh) || !ns_mem_readable(lh, 0x28)) continue;
            void *sr = *(void **)((char *)lh + 0x20);
            if (runtime_unity_object_alive(sr)) {
                typedef void * (ABI *SrGetSprFn)(void *s, void *m);
                void *cur = ((SrGetSprFn)CODE(C_SR_GET_SPRITE))(sr, NULL);
                if (find_slot_for_sprite(cur) == SLOT_HOLD_LINE) {
                    set_lh(lh, s_slots[SLOT_HOLD_LINE].active_spr, NULL);
                }
            }
        }
    }

    /* Update live CurveHoldLines (FMS winding hold ribbons) */
    if (runtime_unity_object_alive(s_hold_tex.active_tex)) {
        CHoldSetFn set_ch = (CHoldSetFn)CODE(C_CHOLD_SET_TEX);
        int32_t ch_cnt = 0;
        void **chs = ns_managed_ptr_seq(*(void **)((char *)mst + 0x90), &ch_cnt);
        for (int32_t i = 0; i < ch_cnt; ++i) {
            if (runtime_unity_object_alive(chs[i])) {
                set_ch(chs[i], s_hold_tex.active_tex, NULL);
            }
        }
    }

    /* Rebuild & refresh any active SlideHold notes in the stage. */
    runtime_slidehold_on_skin_changed(mst);
}

void *runtime_note_skins_active_hold_start_sprite(void) {
    if (!ns_should_apply()) return NULL;
    return runtime_unity_object_alive(s_slots[SLOT_HOLD_START].active_spr)
         ? s_slots[SLOT_HOLD_START].active_spr : NULL;
}

/* ========================================================================= */
/* Cross-Module Hit & Hold Particle Callbacks                                */
/* ========================================================================= */

static void    *s_warmed_pool = NULL;
static int32_t  s_warmed_type_idx = -1;
static bool     s_warmed_is_ems = false;
static uint32_t s_warmed_skin_ver = 0;

static void    *s_active_tlc = NULL;
static void    *s_warmed_tlc = NULL;
static uint32_t s_warmed_tlc_skin_ver = 0;

static void *resolve_loaded_effect_pool(int32_t *out_type_idx, bool *out_is_ems) {
    if (!s_module) return NULL;
    void **p_ti = (void **)(s_module + RVA_EFFECT_POOL_TYPEINFO);
    if (!ns_mem_readable(p_ti, sizeof(void *))) return NULL;
    void *klass = *p_ti;
    if (!ns_mem_readable(klass, 0xe8) || *(int32_t *)((char *)klass + 0xe0) == 0) return NULL;
    void *static_fields = *(void **)((char *)klass + 0xb8);
    if (!ns_mem_readable(static_fields, 0x18)) return NULL;
    void *pool = *(void **)((char *)static_fields + 0x10);
    if (!ns_mem_readable(pool, 0x140) || *(uint8_t *)((char *)pool + 0x138) == 0) return NULL;
    int32_t type_idx = *(int32_t *)((char *)pool + 0x128);
    if (type_idx == 0 || type_idx == 1) {
        runtime_note_skins_set_color_type(type_idx);
        runtime_slidehold_set_color_type(type_idx);
    }
    if (out_type_idx) *out_type_idx = type_idx;
    if (out_is_ems)   *out_is_ems   = (*(uint8_t *)((char *)pool + 0x12c) != 0);
    return pool;
}

static int32_t resolve_judge_idx(const void *controller, uint8_t judge) {
    if (!ns_mem_readable(controller, 0x46)) return -1;
    bool timing_adj = (*(const uint8_t *)((const char *)controller + 0x45) != 0);
    int32_t judge_idx = timing_adj ? ((judge == NS_JUDGE_MISS_PASS) ? 5 : 0) : ((int32_t)judge - 1);
    return (judge_idx >= 0 && judge_idx <= 5) ? judge_idx : -1;
}

static void *get_pool_je_slot(void *arr_obj, int32_t idx) {
    int32_t arr_cnt = 0;
    void **items = ns_managed_ptr_seq(arr_obj, &arr_cnt);
    return (items && idx >= 0 && idx < arr_cnt) ? items[idx] : NULL;
}

static void *get_pool_strided_slot(void *arr_obj, int32_t type_idx, int32_t sub_idx) {
    int32_t arr_cnt = 0;
    void **items = ns_managed_ptr_seq(arr_obj, &arr_cnt);
    if (!items || arr_cnt < 2) return NULL;
    int32_t stride = arr_cnt / 2;
    if (sub_idx < 0 || sub_idx >= stride) return NULL;
    int32_t slot = stride * type_idx + sub_idx;
    return (slot >= 0 && slot < arr_cnt) ? items[slot] : NULL;
}

static void prewarm_judge_effect(void *judge_effect, int default_cat) {
    if (!ns_mem_readable(judge_effect, 0x20)) return;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return;
    for (int32_t i = 0; i < cnt; ++i) {
        apply_category_to_effect_player(eps[i], default_cat);
    }
}

static void prewarm_pool_stride_array(void *arr_obj, int32_t type_idx, int default_cat) {
    int32_t arr_cnt = 0;
    void **items = ns_managed_ptr_seq(arr_obj, &arr_cnt);
    if (!items || arr_cnt < 2) return;
    int32_t stride = arr_cnt / 2;
    int32_t base = stride * type_idx;
    if (base < 0 || base >= arr_cnt) return;
    for (int32_t j = 0; j < stride && (base + j) < arr_cnt; ++j) {
        prewarm_judge_effect(items[base + j], default_cat);
    }
}

static bool prewarm_fms_lane_emitters(void *lane) {
    if (!ns_mem_readable(lane, 0xe0)) return false;
    bool warmed = false;
    void *single = *(void **)((char *)lane + 0x28);
    if (single) { prewarm_judge_effect(single, THEATRHYTHM_NOTE_TOUCH); warmed = true; }

    static const size_t kJeArrOffs[3] = { 0x30, 0x40, 0x48 };
    for (int k = 0; k < 3; ++k) {
        int32_t cnt = 0;
        void **arr = ns_managed_ptr_seq(*(void **)((char *)lane + kJeArrOffs[k]), &cnt);
        for (int32_t i = 0; i < cnt; ++i) {
            prewarm_judge_effect(arr[i], THEATRHYTHM_NOTE_TOUCH);
            warmed = true;
        }
    }

    static const size_t kLeArrOffs[2] = { 0xa0, 0xd8 };
    for (int k = 0; k < 2; ++k) {
        int32_t cnt = 0;
        void **les = ns_managed_ptr_seq(*(void **)((char *)lane + kLeArrOffs[k]), &cnt);
        for (int32_t i = 0; i < cnt; ++i) {
            apply_category_to_loop_emitter(les[i], THEATRHYTHM_NOTE_HOLD);
            warmed = true;
        }
    }
    return warmed;
}

static void prewarm_lanes_if_ready(void) {
    if (!s_installed || !ns_should_apply() || !ns_mem_readable(s_active_tlc, 0x38)) return;
    uint32_t skin_ver = theatrhythm_note_skin_version();
    if (s_active_tlc == s_warmed_tlc && skin_ver == s_warmed_tlc_skin_ver) return;

    int32_t mode = *(int32_t *)((char *)s_active_tlc + 0x18);
    bool warmed_any = false;
    if (mode == 1) {
        void *ctrl = *(void **)((char *)s_active_tlc + 0x20);
        if (ns_mem_readable(ctrl, 0x20)) {
            int32_t cnt = 0;
            void **lanes = ns_managed_ptr_seq(*(void **)((char *)ctrl + 0x18), &cnt);
            for (int32_t i = 0; i < cnt; ++i) {
                void *lane = lanes[i];
                if (!ns_mem_readable(lane, 0x98)) continue;
                void *je0 = *(void **)((char *)lane + 0x20);
                if (je0) { prewarm_judge_effect(je0, THEATRHYTHM_NOTE_TOUCH); warmed_any = true; }
                int32_t je_cnt = 0;
                void **jes = ns_managed_ptr_seq(*(void **)((char *)lane + 0x28), &je_cnt);
                for (int32_t j = 0; j < je_cnt; ++j) {
                    prewarm_judge_effect(jes[j], THEATRHYTHM_NOTE_TOUCH);
                    warmed_any = true;
                }
                void *le_hold = *(void **)((char *)lane + 0x80);
                if (le_hold) { apply_category_to_loop_emitter(le_hold, THEATRHYTHM_NOTE_HOLD); warmed_any = true; }
                void *le_hit = *(void **)((char *)lane + 0x90);
                if (le_hit) { apply_category_to_loop_emitter(le_hit, THEATRHYTHM_NOTE_TOUCH); warmed_any = true; }
            }
        }
    } else if (mode == 2) {
        void *ctrl = *(void **)((char *)s_active_tlc + 0x28);
        if (ns_mem_readable(ctrl, 0x60)) {
            if (prewarm_fms_lane_emitters(*(void **)((char *)ctrl + 0x18))) warmed_any = true;
            int32_t cnt = 0;
            void **lanes = ns_managed_ptr_seq(*(void **)((char *)ctrl + 0x58), &cnt);
            for (int32_t i = 0; i < cnt; ++i) {
                if (prewarm_fms_lane_emitters(lanes[i])) warmed_any = true;
            }
        }
    } else if (mode == 3) {
        void *ctrl = *(void **)((char *)s_active_tlc + 0x30);
        if (ns_mem_readable(ctrl, 0x28)) {
            int32_t cnt = 0;
            void **lanes = ns_managed_ptr_seq(*(void **)((char *)ctrl + 0x20), &cnt);
            for (int32_t i = 0; i < cnt; ++i) {
                void *lane = lanes[i];
                if (!ns_mem_readable(lane, 0x80)) continue;
                void *je0 = *(void **)((char *)lane + 0x20);
                if (je0) { prewarm_judge_effect(je0, THEATRHYTHM_NOTE_TOUCH); warmed_any = true; }
                void *le_hold = *(void **)((char *)lane + 0x78);
                if (le_hold) { apply_category_to_loop_emitter(le_hold, THEATRHYTHM_NOTE_HOLD); warmed_any = true; }
            }
        }
    }

    if (warmed_any) {
        s_warmed_tlc = s_active_tlc;
        s_warmed_tlc_skin_ver = skin_ver;
    }
}

static void prewarm_effect_pool_if_ready(void) {
    prewarm_lanes_if_ready();
    int32_t type_idx = 0;
    bool is_ems = false;
    void *pool = resolve_loaded_effect_pool(&type_idx, &is_ems);
    if (!pool || type_idx != 0 || !ns_should_apply()) return;
    uint32_t skin_ver = theatrhythm_note_skin_version();
    if (pool == s_warmed_pool && type_idx == s_warmed_type_idx &&
        is_ems == s_warmed_is_ems && skin_ver == s_warmed_skin_ver) {
        return;
    }

    s_warmed_pool = pool;
    s_warmed_type_idx = type_idx;
    s_warmed_is_ems = is_ems;
    s_warmed_skin_ver = skin_ver;

    /* Pre-warm all active JudgeEffect.EffectPlayers on stage load or skin change so PlayJudgeEffect never
       runs first-time Graphics.Blit + ReadPixels texture extraction or Material cloning mid-song. */
    static const int kOuterCat[3] = {
        THEATRHYTHM_NOTE_TOUCH, THEATRHYTHM_NOTE_SLIDE, THEATRHYTHM_NOTE_HOLD
    };
    void *outer_obj = *(void **)((char *)pool + (is_ems ? 0xf8 : 0xc8));
    for (int32_t o = 0; o < 3; ++o) {
        prewarm_pool_stride_array(get_pool_je_slot(outer_obj, o), type_idx, kOuterCat[o]);
    }

    static const struct {
        size_t norm_off;
        size_t ems_off;
        int    cat;
        bool   strided;
    } kPoolSlots[] = {
        { 0xd0, 0x100, THEATRHYTHM_NOTE_SLIDE, true  }, /* SlideLine (2 x 6) */
        { 0xd8, 0x108, THEATRHYTHM_NOTE_HOLD,  true  }, /* HoldSlideLine (2 x 6) */
        { 0xe0, 0x110, THEATRHYTHM_NOTE_TOUCH, true  }, /* LastTrigger (2 x 2) */
        { 0xe8, 0x118, THEATRHYTHM_NOTE_TOUCH, false }, /* CriticalHit (2) */
        { 0xf0, 0x120, THEATRHYTHM_NOTE_HOLD,  false }, /* HoldMidpoint (2) */
    };
    for (size_t i = 0; i < sizeof(kPoolSlots) / sizeof(kPoolSlots[0]); ++i) {
        size_t off = is_ems ? kPoolSlots[i].ems_off : kPoolSlots[i].norm_off;
        void *arr = *(void **)((char *)pool + off);
        if (kPoolSlots[i].strided) {
            prewarm_pool_stride_array(arr, type_idx, kPoolSlots[i].cat);
        } else {
            prewarm_judge_effect(get_pool_je_slot(arr, type_idx), kPoolSlots[i].cat);
        }
    }
}

#define NS_LANE_HOLD_CACHE_CAP 8
typedef struct {
    void    *hold_sr;
    void    *last_trig_pd;
    void    *last_spr;
    uint32_t last_skin_ver;
} NSLaneHoldCache;
static NSLaneHoldCache s_ns_lane_hold_cache[NS_LANE_HOLD_CACHE_CAP];

static void reset_stage_instance_caches(void) {
    for (size_t i = 0; i < MAX_PS_CACHE; ++i) {
        clear_ps_entry_at(i);
    }
    s_ps_evict = 0;
    rebuild_ps_hash();
    reset_emitter_cache();
    memset(s_ns_lane_hold_cache, 0, sizeof(s_ns_lane_hold_cache));
    s_warmed_pool = NULL;
    s_warmed_type_idx = -1;
    s_warmed_is_ems = false;
    s_warmed_skin_ver = 0;
    s_warmed_tlc = NULL;
    s_warmed_tlc_skin_ver = 0;
}

void runtime_note_skins_on_play_judge_effect(void *controller, void *trig_pd) {
    if (!s_installed || !s_module ||
        !ns_mem_readable(controller, 0x46) || !ns_mem_readable(trig_pd, 0x50)) return;

    int32_t type_idx = 0;
    bool is_ems = false;
    void *pool = resolve_loaded_effect_pool(&type_idx, &is_ems);
    if (!pool || type_idx != 0 || !ns_should_apply()) return;

    uint8_t trig_type = *(uint8_t *)((char *)trig_pd + 0x18);
    uint8_t judge     = *(uint8_t *)((char *)trig_pd + 0x4c);
    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return;
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return;

    int32_t judge_idx = resolve_judge_idx(controller, judge);

    /* Hold midpoint tick effect (1D array [type_idx]) */
    if (trig_type == NS_TRIG_LINHOLD_PASS || trig_type == NS_TRIG_CRVHOLD_PASS) {
        void *je = get_pool_je_slot(*(void **)((char *)pool + (is_ems ? 0x120 : 0xf0)), type_idx);
        apply_category_to_effect_player(select_next_ep(je), cat);
    }

    /* Main judge hit burst: outer[0]=Touch, [1]=Slide/TwinSlide, [2]=Hold */
    int32_t outer_idx = -1;
    if (trig_type == NS_TRIG_TOUCH) outer_idx = 0;
    else if (trig_type == NS_TRIG_SLIDE || trig_type == NS_TRIG_TWINSLIDE) outer_idx = 1;
    else if (trig_type >= NS_TRIG_LINHOLD_START && trig_type <= NS_TRIG_CRVHOLD_END_S) outer_idx = 2;

    if (outer_idx >= 0 && judge_idx >= 0) {
        void *outer_arr = get_pool_je_slot(*(void **)((char *)pool + (is_ems ? 0xf8 : 0xc8)), outer_idx);
        void *je = get_pool_strided_slot(outer_arr, type_idx, judge_idx);
        apply_category_to_effect_player(select_next_ep(je), cat);
    }

    /* Last trigger flare effect (pool+0xe0 / +0x110, JudgeEffect[2, 2]) or Critical hit flare effect (pool+0xe8 / +0x118) */
    if (*(uint8_t *)((char *)trig_pd + 0x2d) != 0) {
        void *last_arr = *(void **)((char *)pool + (is_ems ? 0x110 : 0xe0));
        int32_t sub = (*(uint8_t *)((char *)trig_pd + 0x2c) != 0) ? 0 : 1;
        void *je = get_pool_strided_slot(last_arr, type_idx, sub);
        if (!je) je = get_pool_je_slot(last_arr, type_idx);
        apply_category_to_effect_player(select_next_ep(je), cat);
    } else if (*(uint8_t *)((char *)trig_pd + 0x2c) != 0) {
        void *je = get_pool_je_slot(*(void **)((char *)pool + (is_ems ? 0x118 : 0xe8)), type_idx);
        apply_category_to_effect_player(select_next_ep(je), cat);
    }

    /* Slide direction line trail particles */
    if (trig_type == NS_TRIG_SLIDE || trig_type == NS_TRIG_TWINSLIDE) {
        void *slide_arr = *(void **)((char *)pool + (is_ems ? 0x100 : 0xd0));
        void *je = (judge_idx >= 0) ? get_pool_strided_slot(slide_arr, type_idx, judge_idx) : NULL;
        if (!je) je = get_pool_je_slot(slide_arr, type_idx);
        if (trig_type == NS_TRIG_TWINSLIDE) apply_category_to_two_eps(je, cat);
        else apply_category_to_effect_player(select_next_ep(je), cat);
    } else if (trig_type == NS_TRIG_LINHOLD_END_S || trig_type == NS_TRIG_CRVHOLD_END_S) {
        void *hslide_arr = *(void **)((char *)pool + (is_ems ? 0x108 : 0xd8));
        void *je = (judge_idx >= 0) ? get_pool_strided_slot(hslide_arr, type_idx, judge_idx) : NULL;
        if (!je) je = get_pool_je_slot(hslide_arr, type_idx);
        apply_category_to_effect_player(select_next_ep(je), cat);
    }
}

void runtime_note_skins_on_bms_holding(void *controller, void *trig_pd) {
    if (!s_installed || !ns_should_apply() ||
        !ns_mem_readable(controller, 0x20) || !ns_mem_readable(trig_pd, 0x50)) return;
    uint8_t type  = *(uint8_t *)((char *)trig_pd + 0x18);
    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (((type & 0xfb) != 3) || judge == NS_JUDGE_MISS_PASS) return;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0) return;

    int32_t lane_idx = *(int32_t *)((char *)trig_pd + 0x1c);
    int32_t lane_cnt = 0;
    void **lanes = ns_managed_ptr_seq(*(void **)((char *)controller + 0x18), &lane_cnt);
    if (!lanes || lane_idx < 0 || lane_idx >= lane_cnt || !ns_mem_readable(lanes[lane_idx], 0x88)) return;

    void *lane = lanes[lane_idx];
    if (cat == THEATRHYTHM_NOTE_HOLD) {
        void *active_spr = s_slots[SLOT_HOLD_START].active_spr;
        void *hold_sr = *(void **)((char *)lane + 0x48);
        NSLaneHoldCache *lc = (lane_idx < NS_LANE_HOLD_CACHE_CAP) ? &s_ns_lane_hold_cache[lane_idx] : NULL;
        if (!lc || lc->hold_sr != hold_sr || lc->last_trig_pd != trig_pd ||
            lc->last_spr != active_spr || lc->last_skin_ver != s_stage_skin_ver) {
            if (runtime_unity_object_alive(active_spr) && runtime_unity_object_alive(hold_sr)) {
                typedef void * (ABI *SrGetSprFn)(void *s, void *m);
                typedef void   (ABI *SrSetSprFn)(void *s, void *spr, void *m);
                void *cur = ((SrGetSprFn)CODE(C_SR_GET_SPRITE))(hold_sr, NULL);
                if (cur != active_spr) {
                    ((SrSetSprFn)CODE(C_SR_SET_SPRITE))(hold_sr, active_spr, NULL);
                }
                if (lc) {
                    lc->hold_sr = hold_sr;
                    lc->last_trig_pd = trig_pd;
                    lc->last_spr = active_spr;
                    lc->last_skin_ver = s_stage_skin_ver;
                }
            }
        }
    }

    apply_category_to_loop_emitter_hold(*(void **)((char *)lane + 0x80), cat, trig_pd);
}

void runtime_note_skins_on_triggers_created(void *mst) {
    if (!s_installed || !ns_mem_readable(mst, 0x98)) return;
    s_active_mst = mst;
    resolve_loaded_effect_pool(NULL, NULL);
    runtime_note_skins_sync_simple_colors();
    if (!ns_should_apply()) return;
    s_stage_skin_ver = theatrhythm_note_skin_version();
    refresh_stage_visuals(mst);
    prewarm_effect_pool_if_ready();
}

/* ========================================================================= */
/* Hook Callbacks                                                            */
/* ========================================================================= */

static ABI int hook_create_triggers(HookRegs *r) {
    void *mst = (void *)r->rdi;
    if (!ns_mem_readable(mst, 0x48)) return 0;
    s_active_mst = mst;
    reset_stage_instance_caches();
    /* CreateTriggers loads TriggerSprite & spawns triggers asynchronously; force sync on completion. */
    s_stage_skin_ver = 0;
    resolve_loaded_effect_pool(NULL, NULL);
    runtime_note_skins_sync_simple_colors();
    if (!ns_should_apply()) return 0;
    void *ts = *(void **)((char *)mst + 0x40);
    if (ns_mem_readable(ts, 0x68)) {
        sync_trigger_sprite_object(ts);
    }
    prewarm_effect_pool_if_ready();
    return 0;
}

static ABI int hook_update_trigger(HookRegs *r) {
    void *mst = (void *)r->rdi;
    if (!ns_mem_readable(mst, 0x48)) return 0;
    s_active_mst = mst;
    tick_deferred_destroys();
    if (!ns_should_apply()) {
        if (s_stage_skin_ver != 0) {
            s_stage_skin_ver = 0;
            runtime_note_skins_sync_simple_colors();
            void *ts = *(void **)((char *)mst + 0x40);
            if (ns_mem_readable(ts, 0x68)) {
                for (int i = 0; i < SLOT_SPRITE_COUNT; ++i) {
                    if (runtime_unity_object_alive(s_slots[i].orig_spr)) {
                        *(void **)((char *)ts + kSlotOffset[i]) = s_slots[i].orig_spr;
                    }
                }
                if (runtime_unity_object_alive(s_hold_tex.orig_tex)) {
                    *(void **)((char *)ts + HOLD_LINE_TEX_OFFSET) = s_hold_tex.orig_tex;
                }
            }
            runtime_slidehold_on_skin_changed(mst);
        }
        return 0;
    }

    uint32_t ver = theatrhythm_note_skin_version();
    if (ver != s_stage_skin_ver) {
        if (s_stage_skin_ver != 0) {
            reset_emitter_cache();
        }
        s_stage_skin_ver = ver;
        refresh_stage_visuals(mst);
        prewarm_effect_pool_if_ready();
    } else if (!s_warmed_pool || s_active_tlc != s_warmed_tlc || ver != s_warmed_skin_ver ||
               ver != s_warmed_tlc_skin_ver) {
        prewarm_effect_pool_if_ready();
    }
    return 0;
}

static ABI int hook_tlc_setup(HookRegs *r) {
    s_active_tlc = (void *)r->rdi;
    s_warmed_tlc = NULL;
    s_warmed_tlc_skin_ver = 0;
    resolve_loaded_effect_pool(NULL, NULL);
    if (!ns_should_apply()) return 0;
    if (runtime_unity_object_alive(s_active_mst) && ns_mem_readable(s_active_mst, 0x48)) {
        void *ts = *(void **)((char *)s_active_mst + 0x40);
        if (ns_mem_readable(ts, 0x68)) sync_trigger_sprite_object(ts);
    }
    return 0;
}

static ABI int hook_bms_set_judge(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *lane    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_mem_readable(lane, 0x98) || !ns_mem_readable(trig_pd, 0x50)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    apply_category_to_effect_player(select_next_ep(*(void **)((char *)lane + 0x20)), cat);
    int32_t je_cnt = 0;
    void **jes = ns_managed_ptr_seq(*(void **)((char *)lane + 0x28), &je_cnt);
    for (int32_t i = 0; i < je_cnt; ++i) apply_category_to_effect_player(select_next_ep(jes[i]), cat);
    apply_category_to_loop_emitter(*(void **)((char *)lane + 0x90), cat);
    return 0;
}

static ABI int hook_fms_play_judge(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *lane       = (void *)r->rdi;
    void *trig_pd    = (void *)r->rsi;
    bool  force_crit = ((r->rcx & 0xff) != 0);
    if (!ns_mem_readable(lane, 0x50) || !ns_mem_readable(trig_pd, 0x50)) return 0;

    uint8_t judge = force_crit ? NS_JUDGE_RAINBOW : *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    bool is_split = (*(uint8_t *)((char *)lane + 0x20) != 0);
    if (is_split) {
        uint8_t sub = *(uint8_t *)((char *)trig_pd + 0x19);
        int32_t je_cnt = 0;
        void **jes = ns_managed_ptr_seq(*(void **)((char *)lane + 0x30), &je_cnt);
        if (jes && (int32_t)sub < je_cnt) {
            apply_category_to_effect_player(select_next_ep(jes[sub]), cat);
        }
        int32_t b_cnt = 0;
        void **bs = ns_managed_ptr_seq(*(void **)((char *)lane + 0x48), &b_cnt);
        for (int32_t i = 0; i < b_cnt; ++i) apply_category_to_effect_player(select_next_ep(bs[i]), cat);
    } else {
        apply_category_to_effect_player(select_next_ep(*(void **)((char *)lane + 0x28)), cat);
        int32_t b_cnt = 0;
        void **bs = ns_managed_ptr_seq(*(void **)((char *)lane + 0x40), &b_cnt);
        for (int32_t i = 0; i < b_cnt; ++i) apply_category_to_effect_player(select_next_ep(bs[i]), cat);
    }
    return 0;
}

static ABI int hook_ems_set_judge(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *lane    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_mem_readable(lane, 0x30) || !ns_mem_readable(trig_pd, 0x50)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    apply_category_to_effect_player(select_next_ep(*(void **)((char *)lane + 0x20)), cat);
    return 0;
}

static void apply_fms_lane_holding(void *lane, int cat, void *trig_pd) {
    if (!ns_mem_readable(lane, 0xa8)) return;

    if (runtime_unity_object_alive(s_slots[SLOT_HOLD_START].active_spr)) {
        int32_t sr_cnt = 0;
        void **srs = ns_managed_ptr_seq(*(void **)((char *)lane + 0x98), &sr_cnt);
        typedef void * (ABI *SrGetSprFn)(void *s, void *m);
        typedef void   (ABI *SrSetSprFn)(void *s, void *spr, void *m);
        for (int32_t i = 0; i < sr_cnt; ++i) {
            if (runtime_unity_object_alive(srs[i])) {
                void *cur = ((SrGetSprFn)CODE(C_SR_GET_SPRITE))(srs[i], NULL);
                if (cur != s_slots[SLOT_HOLD_START].active_spr) {
                    ((SrSetSprFn)CODE(C_SR_SET_SPRITE))(srs[i], s_slots[SLOT_HOLD_START].active_spr, NULL);
                }
            }
        }
    }

    int32_t le_cnt = 0;
    void **les = ns_managed_ptr_seq(*(void **)((char *)lane + 0xa0), &le_cnt);
    for (int32_t i = 0; i < le_cnt; ++i) {
        apply_category_to_loop_emitter_hold(les[i], cat, trig_pd);
    }
    if (ns_mem_readable(lane, 0xe0)) {
        int32_t le_cnt2 = 0;
        void **les2 = ns_managed_ptr_seq(*(void **)((char *)lane + 0xd8), &le_cnt2);
        for (int32_t i = 0; i < le_cnt2; ++i) {
            apply_category_to_loop_emitter_hold(les2[i], cat, trig_pd);
        }
    }
}

static ABI int hook_fms_update_holding(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *ctrl    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_mem_readable(ctrl, 0x20) || !ns_mem_readable(trig_pd, 0x50)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_MISS_PASS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0) cat = THEATRHYTHM_NOTE_HOLD;

    void *single_lane = *(void **)((char *)ctrl + 0x18);
    if (ns_valid_ptr(single_lane)) {
        apply_fms_lane_holding(single_lane, cat, trig_pd);
    }
    if (ns_mem_readable(ctrl, 0x60)) {
        int32_t lane_cnt = 0;
        void **lanes = ns_managed_ptr_seq(*(void **)((char *)ctrl + 0x58), &lane_cnt);
        for (int32_t li = 0; li < lane_cnt; ++li) {
            if (ns_valid_ptr(lanes[li]) && lanes[li] != single_lane) {
                apply_fms_lane_holding(lanes[li], cat, trig_pd);
            }
        }
    }
    return 0;
}

static ABI int hook_ems_set_holding(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *lane    = (void *)r->rdi;
    bool  holding = ((r->rsi & 0xff) != 0);
    if (!ns_mem_readable(lane, 0x80)) return 0;
    if (!holding) {
        clear_emitter_hold_tag(*(void **)((char *)lane + 0x78));
        return 0;
    }

    if (runtime_unity_object_alive(s_slots[SLOT_HOLD_START].active_spr)) {
        void *hold_sr = *(void **)((char *)lane + 0x48);
        if (runtime_unity_object_alive(hold_sr)) {
            typedef void * (ABI *SrGetSprFn)(void *s, void *m);
            typedef void   (ABI *SrSetSprFn)(void *s, void *spr, void *m);
            void *cur = ((SrGetSprFn)CODE(C_SR_GET_SPRITE))(hold_sr, NULL);
            if (cur != s_slots[SLOT_HOLD_START].active_spr) {
                ((SrSetSprFn)CODE(C_SR_SET_SPRITE))(hold_sr, s_slots[SLOT_HOLD_START].active_spr, NULL);
            }
        }
    }
    apply_category_to_loop_emitter_hold(*(void **)((char *)lane + 0x78), THEATRHYTHM_NOTE_HOLD, lane);
    return 0;
}

static ABI int hook_tobj_set_crit(HookRegs *r) {
    if (!ns_should_apply()) return 0;
    void *tobj = (void *)r->rdi;
    void *go   = (void *)r->rsi;
    if (!ns_mem_readable(tobj, TOBJ_TRIG_TYPE_OFF + 1u) || !runtime_unity_object_alive(go)) return 0;

    uint8_t trig_type = *(uint8_t *)((char *)tobj + TOBJ_TRIG_TYPE_OFF);
    int cat = runtime_slidehold_is_registered_trigger_obj(tobj)
            ? THEATRHYTHM_NOTE_SLIDEHOLD
            : ns_trigger_type_to_category(trig_type);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    typedef void * (ABI *GetXformFn)(void *obj, void *m);
    void *xform = ((GetXformFn)CODE(C_GO_GET_TRANSFORM))(go, NULL);
    if (runtime_unity_object_alive(xform)) {
        apply_category_to_emitter(go, xform, NULL, cat, false);
    }
    return 0;
}

/* ========================================================================= */
/* Hook Trampoline Emitter & Installer                                       */
/* ========================================================================= */

typedef ABI int (*NoteSkinHookFn)(HookRegs *);
typedef struct {
    int            site;
    int            floats;
    NoteSkinHookFn fn;
} NSHook;

static const NSHook kHooks[] = {
    {H_CREATE_TRIGGERS,    0, hook_create_triggers},
    {H_UPDATE_TRIGGER,     2, hook_update_trigger},
    {H_TLC_SETUP,          0, hook_tlc_setup},
    {H_BMS_SET_JUDGE,      0, hook_bms_set_judge},
    {H_FMS_PLAY_JUDGE,     0, hook_fms_play_judge},
    {H_EMS_SET_JUDGE,      0, hook_ems_set_judge},
    {H_FMS_UPDATE_HOLDING, 0, hook_fms_update_holding},
    {H_EMS_SET_HOLDING,    0, hook_ems_set_holding},
    {H_TOBJ_SET_CRIT,      0, hook_tobj_set_crit},
};

static unsigned char *emit_bytes(unsigned char *p, const void *bytes, size_t size) {
    memcpy(p, bytes, size);
    return p + size;
}

static unsigned char *rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[] = {
        (unsigned char)(0x48 | (reg >= 8 ? 4 : 0)),
        (unsigned char)(store ? 0x89 : 0x8b),
        (unsigned char)(0x44 | ((reg & 7) << 3)),
        0x24,
        disp
    };
    return emit_bytes(p, bytes, sizeof(bytes));
}

static unsigned char *xmm_slot(unsigned char *p, int store, int reg, int32_t disp) {
    const unsigned char bytes[] = {
        0x0f,
        (unsigned char)(store ? 0x11 : 0x10),
        (unsigned char)(0x84 | (reg << 3)),
        0x24
    };
    p = emit_bytes(p, bytes, sizeof(bytes));
    return emit_bytes(p, &disp, 4);
}

static size_t ns_stub_size(const NSHook *h) {
    return 144 + 16 * (size_t)h->floats;
}

static unsigned char *emit_ns_stub(unsigned char *p, const NSHook *h, unsigned char *function) {
    static const int args[6] = {7, 6, 2, 1, 8, 9};
    const int32_t frame = 72 + 16 * h->floats;
    p = emit_bytes(p, (const unsigned char[]){0x48, 0x81, 0xec}, 3);
    p = emit_bytes(p, &frame, 4);
    for (int i = 0; i < 6; ++i) p = rsp_slot(p, 1, args[i], (uint8_t)(8 * i));
    for (int i = 0; i < h->floats; ++i) p = xmm_slot(p, 1, i, 72 + 16 * i);
    p = emit_bytes(p, (const unsigned char[]){0x48, 0x89, 0xe7}, 3);
    const uint64_t fn = (uint64_t)(uintptr_t)h->fn;
    p = emit_bytes(p, (const unsigned char[]){0x48, 0xb8}, 2);
    p = emit_bytes(p, &fn, 8);
    p = emit_bytes(p, (const unsigned char[]){0xff, 0xd0, 0x85, 0xc0}, 4);
    unsigned char *jnz = p;
    p = emit_bytes(p, (const unsigned char[]){0x75, 0}, 2);
    for (int i = 0; i < 6; ++i) p = rsp_slot(p, 0, args[i], (uint8_t)(8 * i));
    for (int i = 0; i < h->floats; ++i) p = xmm_slot(p, 0, i, 72 + 16 * i);
    p = emit_bytes(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3);
    p = emit_bytes(p, &frame, 4);
    p = emit_bytes(p, function, s_code[h->site].moved);
    const uint64_t back = (uint64_t)(uintptr_t)(function + s_code[h->site].moved);
    p = emit_bytes(p, (const unsigned char[]){0xff, 0x25, 0, 0, 0, 0}, 6);
    p = emit_bytes(p, &back, 8);
    jnz[1] = (unsigned char)(p - (jnz + 2));
    p = rsp_slot(p, 0, 0, 48);
    p = rsp_slot(p, 0, 2, 56);
    p = emit_bytes(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3);
    p = emit_bytes(p, &frame, 4);
    return emit_bytes(p, (const unsigned char[]){0xc3}, 1);
}

int runtime_note_skins_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    s_module = image + module_base;
    if (runtime_game_code_find(kSites, NS_SITES, s_code, "NoteSkins") != 0) {
        printf("NoteSkins: site discovery failed for game %s; custom note colors disabled\n", runtime_game_version());
        return -1;
    }
    const char *ver = runtime_game_version();
    s_is_game_104 = (s_code[H_CREATE_TRIGGERS].offset == 0x27198b0ULL) ||
                    (ver && strstr(ver, "01.04") != NULL);

    size_t need = 0;
    size_t num_hooks = sizeof(kHooks) / sizeof(*kHooks);
    for (size_t i = 0; i < num_hooks; ++i) {
        const unsigned moved = s_code[kHooks[i].site].moved;
        if (moved < 5 || moved > 16) {
            printf("NoteSkins: invalid moved byte count %u for hook %zu\n", moved, i);
            return -1;
        }
        need += ns_stub_size(&kHooks[i]);
        const int64_t rel = (int64_t)(stubs + need - (CODE(kHooks[i].site) + 5));
        if (rel != (int32_t)rel || need > size) {
            printf("NoteSkins: insufficient stub space (need %zu, have %zu)\n", need, size);
            return -1;
        }
    }

    unsigned char *p = stubs;
    for (size_t i = 0; i < num_hooks; ++i) {
        unsigned char *function = CODE(kHooks[i].site);
        unsigned char *stub = p;
        p = emit_ns_stub(p, &kHooks[i], function);
        const int32_t rel32 = (int32_t)(stub - (function + 5));
        function[0] = 0xe9;
        memcpy(function + 1, &rel32, 4);
        memset(function + 5, 0x90, s_code[kHooks[i].site].moved - 5u);
    }

    s_installed = true;
    printf("NoteSkins: installed %zu hooks (%zu bytes stub, v1.04=%d) for game %s\n",
           num_hooks, (size_t)(p - stubs), s_is_game_104, runtime_game_version());
    return 0;
}
