/* Custom note skin recoloring & per-note particle color matching (Theatrhythm Final Bar Line PS4 v1.0.4). */

#include "runtime_note_skins.h"
#include "runtime.h"
#include "runtime_slidehold.h"
#include "gpu/theatrhythm_gpu.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    NS_JUDGE_CRITICAL  = 1,
    NS_JUDGE_GREAT     = 2,
    NS_JUDGE_GOOD      = 3,
    NS_JUDGE_BAD       = 4,
    NS_JUDGE_MISS      = 5,
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

#define RVA_EFFECT_POOL_TYPEINFO 0x609a4e8ULL
#define RVA_PS_MAIN_GET_COL_INJ  0x44c25b0ULL
#define RVA_PS_MAIN_SET_COL_INJ  0x44c2610ULL

/* ========================================================================= */
/* Small Safe Pointer & IL2CPP Helpers                                       */
/* ========================================================================= */

static inline bool ns_valid_ptr(const void *p) {
    uintptr_t u = (uintptr_t)p;
    return u >= 0x10000ULL && u <= 0x00007fffffffffffULL && (u & 1ULL) == 0;
}

static void **ns_managed_ptr_seq(void *seq, int32_t *out_count) {
    if (!ns_valid_ptr(seq)) {
        if (out_count) *out_count = 0;
        return NULL;
    }
    void *p10 = *(void **)((char *)seq + 0x10);
    int32_t n18 = *(int32_t *)((char *)seq + 0x18);
    if (p10 == NULL) {
        if (n18 < 0 || n18 > 65536) n18 = 0;
        if (out_count) *out_count = n18;
        return (void **)((char *)seq + 0x20);
    }
    if (ns_valid_ptr(p10)) {
        int32_t cap = *(int32_t *)((char *)p10 + 0x18);
        if (n18 < 0 || n18 > cap || cap > 65536) n18 = 0;
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
    if (!ns_valid_ptr(trig_pd)) return -1;
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

    runtime_note_skins_recolor_rgba(src, out_w, out_h, category);
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

#define MAX_TEX_READBACK 64
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

static TexReadbackEntry *get_tex_readback(void *tex) {
    if (!runtime_unity_object_alive(tex)) return NULL;
    for (size_t i = 0; i < MAX_TEX_READBACK; ++i) {
        if (s_tex_cache[i].tex == tex && s_tex_cache[i].rgba && runtime_unity_object_alive(tex)) {
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

static void *get_neutral_white_tex(void *src_tex) {
    TexReadbackEntry *e = get_tex_readback(src_tex);
    if (!e || !e->is_chromatic) return NULL;
    if (e->white_tex && runtime_unity_object_alive(e->white_tex)) {
        return e->white_tex;
    }

    size_t n = (size_t)e->w * (size_t)e->h;
    uint8_t *lum = (uint8_t *)malloc(n * 4u);
    if (!lum) return NULL;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t *s = e->rgba + i * 4u;
        uint8_t l = s[0] > s[1] ? (s[0] > s[2] ? s[0] : s[2]) : (s[1] > s[2] ? s[1] : s[2]);
        lum[i * 4u + 0] = l;
        lum[i * 4u + 1] = l;
        lum[i * 4u + 2] = l;
        lum[i * 4u + 3] = s[3];
    }

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
static int32_t s_prop_tint = 0;
static int32_t s_prop_color = 0;
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
    fn_ps_col_get_color   = (void (ABI *)(void **, NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/ColorOverLifetimeModule::get_color_Injected");
    fn_ps_col_set_color   = (void (ABI *)(void **, const NSMinMaxGradient *))
        runtime_resolve_icall("UnityEngine.ParticleSystem/ColorOverLifetimeModule::set_color_Injected");

    fn_mat_has_prop    = (bool (ABI *)(void *, int32_t))runtime_resolve_icall("UnityEngine.Material::HasProperty(System.Int32)");
    if (!fn_mat_has_prop) {
        fn_mat_has_prop = (bool (ABI *)(void *, int32_t))runtime_resolve_icall("UnityEngine.Material::HasProperty");
    }
    fn_mat_get_col_inj = (void (ABI *)(void *, int32_t, NSColor *))runtime_resolve_icall("UnityEngine.Material::GetColorImpl_Injected");
    fn_mat_set_col_inj = (void (ABI *)(void *, int32_t, const NSColor *))runtime_resolve_icall("UnityEngine.Material::SetColorImpl_Injected");

    typedef int32_t (ABI *PropIdFn)(void *str, void *m);
    PropIdFn prop_to_id = (PropIdFn)CODE(C_SHADER_PROP_TO_ID);
    void *s_tint  = runtime_il2cpp_string("_TintColor");
    void *s_color = runtime_il2cpp_string("_Color");
    void *s_base  = runtime_il2cpp_string("_BaseColor");
    if (s_tint)  s_prop_tint       = prop_to_id(s_tint, NULL);
    if (s_color) s_prop_color      = prop_to_id(s_color, NULL);
    if (s_base)  s_prop_base_color = prop_to_id(s_base, NULL);
}

#define MAX_MAT_CACHE 64
typedef struct {
    void    *orig_mat;
    uint32_t orig_pin;
    void    *neutral_mat;
    uint32_t neutral_pin;
    bool     checked;
} MatNeutralEntry;

static MatNeutralEntry s_mat_cache[MAX_MAT_CACHE];
static size_t s_mat_evict = 0;

static void neutralize_mat_color_prop(void *mat, int32_t prop_id, bool *io_changed) {
    if (!mat || !prop_id || !fn_mat_has_prop || !fn_mat_get_col_inj || !fn_mat_set_col_inj) return;
    if (!fn_mat_has_prop(mat, prop_id)) return;
    NSColor c = {1, 1, 1, 1};
    fn_mat_get_col_inj(mat, prop_id, &c);
    float mx = fmaxf(c.r, fmaxf(c.g, c.b));
    float mn = fminf(c.r, fminf(c.g, c.b));
    if (mx > 0.05f && (mx - mn) > 0.08f * mx) {
        NSColor white = {1.0f, 1.0f, 1.0f, c.a};
        fn_mat_set_col_inj(mat, prop_id, &white);
        *io_changed = true;
    }
}

static void *get_neutral_material(void *orig_mat) {
    if (!runtime_unity_object_alive(orig_mat)) return orig_mat;
    for (size_t i = 0; i < MAX_MAT_CACHE; ++i) {
        if (s_mat_cache[i].orig_mat == orig_mat && runtime_unity_object_alive(orig_mat)) {
            return s_mat_cache[i].neutral_mat ? s_mat_cache[i].neutral_mat : orig_mat;
        }
    }

    ensure_particle_icalls();

    typedef void * (ABI *MatGetTexFn)(void *mat, void *m);
    typedef void   (ABI *MatSetTexFn)(void *mat, void *tex, void *m);
    void *main_tex = ((MatGetTexFn)CODE(C_MAT_GET_MAIN_TEX))(orig_mat, NULL);
    void *white_tex = runtime_unity_object_alive(main_tex) ? get_neutral_white_tex(main_tex) : NULL;

    bool need_tint_neutral = false;
    const int32_t props[3] = {s_prop_tint, s_prop_color, s_prop_base_color};
    for (int i = 0; i < 3; ++i) {
        if (!props[i] || !fn_mat_has_prop || !fn_mat_get_col_inj) continue;
        if (!fn_mat_has_prop(orig_mat, props[i])) continue;
        NSColor c = {1, 1, 1, 1};
        fn_mat_get_col_inj(orig_mat, props[i], &c);
        float mx = fmaxf(c.r, fmaxf(c.g, c.b));
        float mn = fminf(c.r, fminf(c.g, c.b));
        if (mx > 0.05f && (mx - mn) > 0.08f * mx) {
            need_tint_neutral = true;
            break;
        }
    }

    void *neutral_mat = NULL;
    if (white_tex || need_tint_neutral) {
        void *cloned = runtime_clone_material_raw(orig_mat);
        if (runtime_unity_object_alive(cloned)) {
            if (white_tex) {
                ((MatSetTexFn)CODE(C_MAT_SET_MAIN_TEX))(cloned, white_tex, NULL);
            }
            bool touched = false;
            for (int i = 0; i < 3; ++i) neutralize_mat_color_prop(cloned, props[i], &touched);
            neutral_mat = cloned;
        }
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
    if (m->orig_pin) runtime_gc_unpin_obj(m->orig_pin);
    if (m->neutral_pin) runtime_gc_unpin_obj(m->neutral_pin);
    memset(m, 0, sizeof(*m));
    m->orig_mat = orig_mat;
    m->orig_pin = runtime_gc_pin_obj(orig_mat);
    if (neutral_mat) {
        m->neutral_mat = neutral_mat;
        m->neutral_pin = runtime_gc_pin_obj(neutral_mat);
    }
    m->checked = true;
    return neutral_mat ? neutral_mat : orig_mat;
}

#define MAX_PS_CACHE 512
typedef struct {
    void            *ps;
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
static size_t s_ps_evict = 0;

static void clear_ps_entry(PSCacheEntry *e) {
    if (!e) return;
    if (e->pin_white_col_min) runtime_gc_unpin_obj(e->pin_white_col_min);
    if (e->pin_white_col_max) runtime_gc_unpin_obj(e->pin_white_col_max);
    memset(e, 0, sizeof(*e));
}

static bool ns_color_near_white(NSColor c) {
    return fabsf(c.r - 1.0f) < 0.04f && fabsf(c.g - 1.0f) < 0.04f && fabsf(c.b - 1.0f) < 0.04f;
}

static PSCacheEntry *get_ps_entry(void *ps) {
    if (!runtime_unity_object_alive(ps)) return NULL;
    ensure_particle_icalls();
    if (!fn_ps_main_get_col_inj || !fn_ps_main_set_col_inj) return NULL;

    PSCacheEntry *free_slot = NULL;
    for (size_t i = 0; i < MAX_PS_CACHE; ++i) {
        if (s_ps_cache[i].ps == ps) {
            if (runtime_unity_object_alive(ps)) return &s_ps_cache[i];
            clear_ps_entry(&s_ps_cache[i]);
            free_slot = &s_ps_cache[i];
            break;
        }
        if (!free_slot && (!s_ps_cache[i].ps || !runtime_unity_object_alive(s_ps_cache[i].ps))) {
            if (s_ps_cache[i].ps) clear_ps_entry(&s_ps_cache[i]);
            free_slot = &s_ps_cache[i];
        }
    }
    if (!free_slot) {
        free_slot = &s_ps_cache[s_ps_evict];
        s_ps_evict = (s_ps_evict + 1) % MAX_PS_CACHE;
        clear_ps_entry(free_slot);
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->ps = ps;
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
    if (orig_main.m_Mode == 1 || orig_main.m_Mode == 3 || a_max <= 0.001f) {
        a_min = 1.0f;
        a_max = 1.0f;
    }
    free_slot->orig_alpha_min = a_min;
    free_slot->orig_alpha_max = a_max;

    /* Neutralize chromatic ColorOverLifetime gradients once to (1,1,1,orig_alpha) so startColor controls hue cleanly. */
    if (fn_ps_col_get_enabled && fn_ps_col_get_color && fn_ps_col_set_color && fn_ps_col_get_enabled(&ps)) {
        NSMinMaxGradient orig_col;
        memset(&orig_col, 0, sizeof(orig_col));
        fn_ps_col_get_color(&ps, &orig_col);
        NSMinMaxGradient wcol = orig_col;
        bool touched = false;
        if (orig_col.m_Mode == 0 && !ns_color_near_white(orig_col.m_ColorMax)) {
            wcol.m_ColorMax = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMax.a};
            touched = true;
        } else if (orig_col.m_Mode == 2 && (!ns_color_near_white(orig_col.m_ColorMin) || !ns_color_near_white(orig_col.m_ColorMax))) {
            wcol.m_ColorMin = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMin.a};
            wcol.m_ColorMax = (NSColor){1.0f, 1.0f, 1.0f, orig_col.m_ColorMax.a};
            touched = true;
        } else if (orig_col.m_Mode == 1 || orig_col.m_Mode == 3) {
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
            free_slot->col_neutralized = true;
            free_slot->white_col = wcol;
            fn_ps_col_set_color(&ps, &free_slot->white_col);
        }
    }

    /* Neutralize chromatic mainTexture / material tint once to white luminance. */
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

static void apply_rgb_to_ps(void *ps, float r, float g, float b) {
    PSCacheEntry *e = get_ps_entry(ps);
    if (!e || e->is_root) return;

    if (e->has_last_rgb &&
        fabsf(e->last_r - r) < 0.004f &&
        fabsf(e->last_g - g) < 0.004f &&
        fabsf(e->last_b - b) < 0.004f) {
        return;
    }

    if (e->mat_prepared && runtime_unity_object_alive(e->psr) && runtime_unity_object_alive(e->neutral_mat)) {
        typedef void (ABI *SetSharedMatFn)(void *rend, void *mat, void *m);
        ((SetSharedMatFn)CODE(C_REND_SET_SHARED_MAT))(e->psr, e->neutral_mat, NULL);
    }
    if (e->col_neutralized && fn_ps_col_set_color) {
        fn_ps_col_set_color(&ps, &e->white_col);
    }

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

    e->last_r = r;
    e->last_g = g;
    e->last_b = b;
    e->has_last_rgb = true;
}

void runtime_note_skins_apply_ps_list_rgb(void *ps_list, float r, float g, float b) {
    int32_t count = 0;
    void **items = ns_managed_ptr_seq(ps_list, &count);
    if (!items || count <= 0) return;
    for (int32_t i = 0; i < count; ++i) {
        apply_rgb_to_ps(items[i], r, g, b);
    }
}

/* ========================================================================= */
/* Emitter Component Cache (collects ParticleSystems once per pooled object) */
/* ========================================================================= */

#define MAX_EMITTER_CACHE 256
#define MAX_PS_PER_EMITTER 24
#define MAX_SR_PER_EMITTER 8

typedef struct {
    void *owner;
    void *ps_list_hint;
    int   ps_count;
    void *ps[MAX_PS_PER_EMITTER];
    int   sr_count;
    void *sr[MAX_SR_PER_EMITTER];
} EmitterCacheEntry;

static EmitterCacheEntry s_emitter_cache[MAX_EMITTER_CACHE];
static size_t s_emitter_evict = 0;

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

static void apply_category_to_emitter(void *owner, void *root_xform, void *ps_list, int category) {
    if (category < 0 || category >= THEATRHYTHM_NOTE_COUNT) return;
    if (!ns_valid_ptr(owner)) return;

    EmitterCacheEntry *entry = NULL;
    EmitterCacheEntry *free_slot = NULL;
    for (size_t i = 0; i < MAX_EMITTER_CACHE; ++i) {
        if (s_emitter_cache[i].owner == owner) {
            /* Validate first object is still alive before reusing pooled cache entry. */
            if (!ns_valid_ptr(root_xform) || runtime_unity_object_alive(root_xform)) {
                entry = &s_emitter_cache[i];
                break;
            }
            memset(&s_emitter_cache[i], 0, sizeof(s_emitter_cache[i]));
            free_slot = &s_emitter_cache[i];
            break;
        }
        if (!free_slot && !s_emitter_cache[i].owner) {
            free_slot = &s_emitter_cache[i];
        }
    }

    if (!entry) {
        if (!free_slot) {
            free_slot = &s_emitter_cache[s_emitter_evict];
            s_emitter_evict = (s_emitter_evict + 1) % MAX_EMITTER_CACHE;
        }
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->owner = owner;
        free_slot->ps_list_hint = ps_list;

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
        entry = free_slot;
    }

    float r, g, b;
    theatrhythm_note_skin_get_rgb(category, &r, &g, &b);
    for (int i = 0; i < entry->ps_count; ++i) {
        apply_rgb_to_ps(entry->ps[i], r, g, b);
    }
    if (entry->sr_count > 0) {
        typedef void (ABI *SrSetColFn)(void *sr, NSColor col, void *m);
        SrSetColFn set_col = (SrSetColFn)CODE(C_SR_SET_COLOR);
        NSColor col = {r, g, b, 1.0f};
        for (int i = 0; i < entry->sr_count; ++i) {
            if (runtime_unity_object_alive(entry->sr[i])) {
                set_col(entry->sr[i], col, NULL);
            }
        }
    }
}

static void apply_category_to_effect_player(void *ep, int category) {
    if (!ns_valid_ptr(ep)) return;
    void *xform   = *(void **)((char *)ep + 0x10);
    void *ps_list = *(void **)((char *)ep + 0x28);
    apply_category_to_emitter(ep, xform, ps_list, category);
}

static void apply_category_to_loop_emitter(void *le, int category) {
    if (!ns_valid_ptr(le)) return;
    void *xform   = *(void **)((char *)le + 0x18);
    void *ps_list = *(void **)((char *)le + 0x20);
    apply_category_to_emitter(le, xform, ps_list, category);
}

static void *select_next_ep(void *judge_effect) {
    if (!ns_valid_ptr(judge_effect)) return NULL;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return NULL;
    for (int32_t i = 0; i < cnt; ++i) {
        void *ep = eps[i];
        if (ns_valid_ptr(ep) && *(uint8_t *)((char *)ep + 0x38) == 0) {
            return ep;
        }
    }
    void *best_ep = NULL;
    float best_elapsed = -1e9f;
    for (int32_t i = 0; i < cnt; ++i) {
        void *ep = eps[i];
        if (!ns_valid_ptr(ep)) continue;
        float elapsed = *(float *)((char *)ep + 0x30) - *(float *)((char *)ep + 0x34);
        if (!best_ep || elapsed > best_elapsed) {
            best_ep = ep;
            best_elapsed = elapsed;
        }
    }
    return best_ep;
}

static void apply_category_to_idle_or_next_ep(void *judge_effect, int category) {
    if (!ns_valid_ptr(judge_effect)) return;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return;

    bool any_idle = false;
    for (int32_t i = 0; i < cnt; ++i) {
        void *ep = eps[i];
        if (ns_valid_ptr(ep) && *(uint8_t *)((char *)ep + 0x38) == 0) {
            apply_category_to_effect_player(ep, category);
            any_idle = true;
        }
    }
    if (!any_idle) {
        apply_category_to_effect_player(select_next_ep(judge_effect), category);
    }
}

static void apply_category_to_two_eps(void *judge_effect, int category) {
    if (!ns_valid_ptr(judge_effect)) return;
    int32_t cnt = 0;
    void **eps = ns_managed_ptr_seq(*(void **)((char *)judge_effect + 0x18), &cnt);
    if (!eps || cnt <= 0) return;

    int applied = 0;
    for (int32_t i = 0; i < cnt && applied < 2; ++i) {
        void *ep = eps[i];
        if (ns_valid_ptr(ep) && *(uint8_t *)((char *)ep + 0x38) == 0) {
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
    if (!runtime_unity_object_alive(ts)) return;

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

static void refresh_stage_visuals(void *mst) {
    if (!runtime_unity_object_alive(mst)) return;
    void *ts = *(void **)((char *)mst + 0x40);
    if (runtime_unity_object_alive(ts)) {
        sync_trigger_sprite_object(ts);
    }

    typedef void (ABI *JointSetFn)(void *jl, void *spr, void *m);
    typedef void (ABI *LHoldSetFn)(void *lh, void *spr, void *m);
    typedef void (ABI *CHoldSetFn)(void *ch, void *tex, void *m);

    /* Update live TriggerObjects (excluding registered SlideHold notes, updated separately below). */
    int32_t trig_cnt = 0;
    void **trigs = ns_managed_ptr_seq(*(void **)((char *)mst + 0x70), &trig_cnt);
    for (int32_t i = 0; i < trig_cnt; ++i) {
        void *tobj = trigs[i];
        if (!runtime_unity_object_alive(tobj)) continue;
        if (runtime_slidehold_is_registered_trigger_obj(tobj)) continue;

        refresh_sr_by_known_slot(*(void **)((char *)tobj + 0x38));
        void *twin_sub = *(void **)((char *)tobj + 0x30);
        if (runtime_unity_object_alive(twin_sub)) {
            refresh_sr_by_known_slot(*(void **)((char *)twin_sub + 0xf8));
            int32_t arrow_cnt = 0;
            void **arrows = ns_managed_ptr_seq(*(void **)((char *)twin_sub + 0x100), &arrow_cnt);
            for (int32_t a = 0; a < arrow_cnt; ++a) refresh_sr_by_known_slot(arrows[a]);
        } else {
            refresh_xform_sprite_renderers(*(void **)((char *)tobj + 0x28), 0);
        }

        void *simple_arr = *(void **)((char *)tobj + 0x68);
        if (ns_valid_ptr(simple_arr)) {
            int32_t sc = *(int32_t *)((char *)simple_arr + 0x18);
            if (sc >= 1 && sc <= 4) {
                void **slots = (void **)((char *)simple_arr + 0x20);
                for (int32_t k = 0; k < sc; ++k) {
                    int sidx = find_slot_for_sprite(slots[k]);
                    if (sidx >= 0 && runtime_unity_object_alive(s_slots[sidx].active_spr)) {
                        slots[k] = s_slots[sidx].active_spr;
                    }
                }
            }
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
            if (!runtime_unity_object_alive(lh)) continue;
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
    return runtime_unity_object_alive(s_slots[SLOT_HOLD_START].active_spr)
         ? s_slots[SLOT_HOLD_START].active_spr : NULL;
}

/* ========================================================================= */
/* Cross-Module Hit & Hold Particle Callbacks                                */
/* ========================================================================= */

void runtime_note_skins_on_play_judge_effect(void *controller, void *trig_pd) {
    if (!s_installed || !s_module || !ns_valid_ptr(controller) || !ns_valid_ptr(trig_pd)) return;

    uint8_t trig_type = *(uint8_t *)((char *)trig_pd + 0x18);
    uint8_t judge     = *(uint8_t *)((char *)trig_pd + 0x4c);
    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return;
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return;

    void **p_ti = (void **)(s_module + RVA_EFFECT_POOL_TYPEINFO);
    void *klass = *p_ti;
    if (!ns_valid_ptr(klass) || *(int32_t *)((char *)klass + 0xe0) == 0) return;
    void *static_fields = *(void **)((char *)klass + 0xb8);
    if (!ns_valid_ptr(static_fields)) return;
    void *pool = *(void **)((char *)static_fields + 0x10);
    if (!ns_valid_ptr(pool) || *(uint8_t *)((char *)pool + 0x138) == 0) return;

    int32_t type_idx = *(int32_t *)((char *)pool + 0x128);
    bool is_ems      = (*(uint8_t *)((char *)pool + 0x12c) != 0);

    /* Hold midpoint tick effect */
    if (trig_type == NS_TRIG_LINHOLD_PASS || trig_type == NS_TRIG_CRVHOLD_PASS) {
        int32_t arr_cnt = 0;
        void **items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0x120 : 0xf0)), &arr_cnt);
        if (items && type_idx >= 0 && type_idx < arr_cnt) {
            apply_category_to_effect_player(select_next_ep(items[type_idx]), cat);
        }
    }

    /* Main judge hit burst: outer[0]=Touch, [1]=Slide/TwinSlide, [2]=Hold */
    int32_t outer_idx = -1;
    if (trig_type == NS_TRIG_TOUCH) outer_idx = 0;
    else if (trig_type == NS_TRIG_SLIDE || trig_type == NS_TRIG_TWINSLIDE) outer_idx = 1;
    else if (trig_type >= NS_TRIG_LINHOLD_START && trig_type <= NS_TRIG_CRVHOLD_END_S) outer_idx = 2;

    if (outer_idx >= 0) {
        bool timing_adj = (*(uint8_t *)((char *)controller + 0x45) != 0);
        int32_t judge_idx = timing_adj ? ((judge == NS_JUDGE_MISS_PASS) ? 5 : 0) : ((int32_t)judge - 1);
        if (judge_idx >= 0 && judge_idx <= 5) {
            int32_t outer_cnt = 0;
            void **outer_items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0xf8 : 0xc8)), &outer_cnt);
            if (outer_items && outer_idx < outer_cnt && ns_valid_ptr(outer_items[outer_idx])) {
                int32_t inner_cnt = 0;
                void **inner_items = ns_managed_ptr_seq(outer_items[outer_idx], &inner_cnt);
                int32_t slot = (inner_cnt / 2) * type_idx + judge_idx;
                if (inner_items && slot >= 0 && slot < inner_cnt) {
                    apply_category_to_effect_player(select_next_ep(inner_items[slot]), cat);
                }
            }
        }
    }

    /* Critical hit & last trigger flare effects */
    if (*(uint8_t *)((char *)trig_pd + 0x2d) != 0) {
        int32_t arr_cnt = 0;
        void **items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0x110 : 0xe0)), &arr_cnt);
        if (items && type_idx >= 0 && type_idx < arr_cnt) {
            apply_category_to_effect_player(select_next_ep(items[type_idx]), cat);
        }
    } else if (*(uint8_t *)((char *)trig_pd + 0x2c) != 0) {
        int32_t arr_cnt = 0;
        void **items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0x118 : 0xe8)), &arr_cnt);
        if (items && type_idx >= 0 && type_idx < arr_cnt) {
            apply_category_to_effect_player(select_next_ep(items[type_idx]), cat);
        }
    }

    /* Slide direction line trail particles */
    if (trig_type == NS_TRIG_SLIDE || trig_type == NS_TRIG_TWINSLIDE) {
        int32_t arr_cnt = 0;
        void **items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0x100 : 0xd0)), &arr_cnt);
        if (items && type_idx >= 0 && type_idx < arr_cnt) {
            if (trig_type == NS_TRIG_TWINSLIDE) apply_category_to_two_eps(items[type_idx], cat);
            else apply_category_to_effect_player(select_next_ep(items[type_idx]), cat);
        }
    } else if (trig_type == NS_TRIG_LINHOLD_END_S || trig_type == NS_TRIG_CRVHOLD_END_S) {
        int32_t arr_cnt = 0;
        void **items = ns_managed_ptr_seq(*(void **)((char *)pool + (is_ems ? 0x108 : 0xd8)), &arr_cnt);
        if (items && type_idx >= 0 && type_idx < arr_cnt) {
            apply_category_to_effect_player(select_next_ep(items[type_idx]), cat);
        }
    }
}

void runtime_note_skins_on_bms_holding(void *controller, void *trig_pd) {
    if (!s_installed || !ns_valid_ptr(controller) || !ns_valid_ptr(trig_pd)) return;
    uint8_t type  = *(uint8_t *)((char *)trig_pd + 0x18);
    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (((type & 0xfb) != 3) || judge == NS_JUDGE_MISS_PASS) return;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0) return;

    int32_t lane_idx = *(int32_t *)((char *)trig_pd + 0x1c);
    int32_t lane_cnt = 0;
    void **lanes = ns_managed_ptr_seq(*(void **)((char *)controller + 0x18), &lane_cnt);
    if (!lanes || lane_idx < 0 || lane_idx >= lane_cnt || !ns_valid_ptr(lanes[lane_idx])) return;

    void *lane = lanes[lane_idx];
    if (cat == THEATRHYTHM_NOTE_HOLD && runtime_unity_object_alive(s_slots[SLOT_HOLD_START].active_spr)) {
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

    apply_category_to_loop_emitter(*(void **)((char *)lane + 0x80), cat);
}

/* ========================================================================= */
/* Hook Callbacks                                                            */
/* ========================================================================= */

static ABI int hook_create_triggers(HookRegs *r) {
    void *mst = (void *)r->rdi;
    if (!ns_valid_ptr(mst)) return 0;
    s_active_mst = mst;
    /* CreateTriggers loads TriggerSprite & spawns triggers asynchronously; force frame-1 UpdateTrigger to sync. */
    s_stage_skin_ver = 0;
    void *ts = *(void **)((char *)mst + 0x40);
    if (runtime_unity_object_alive(ts)) {
        sync_trigger_sprite_object(ts);
    }
    return 0;
}

static ABI int hook_update_trigger(HookRegs *r) {
    void *mst = (void *)r->rdi;
    if (!ns_valid_ptr(mst)) return 0;
    s_active_mst = mst;
    tick_deferred_destroys();

    uint32_t ver = theatrhythm_note_skin_version();
    if (ver != s_stage_skin_ver) {
        s_stage_skin_ver = ver;
        refresh_stage_visuals(mst);
    }
    return 0;
}

static ABI int hook_tlc_setup(HookRegs *r) {
    (void)r;
    if (runtime_unity_object_alive(s_active_mst)) {
        void *ts = *(void **)((char *)s_active_mst + 0x40);
        if (runtime_unity_object_alive(ts)) sync_trigger_sprite_object(ts);
    }
    return 0;
}

static ABI int hook_bms_set_judge(HookRegs *r) {
    void *lane    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_valid_ptr(lane) || !ns_valid_ptr(trig_pd)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    apply_category_to_idle_or_next_ep(*(void **)((char *)lane + 0x20), cat);
    int32_t je_cnt = 0;
    void **jes = ns_managed_ptr_seq(*(void **)((char *)lane + 0x28), &je_cnt);
    for (int32_t i = 0; i < je_cnt; ++i) apply_category_to_idle_or_next_ep(jes[i], cat);
    apply_category_to_loop_emitter(*(void **)((char *)lane + 0x90), cat);
    return 0;
}

static ABI int hook_fms_play_judge(HookRegs *r) {
    void *lane       = (void *)r->rdi;
    void *trig_pd    = (void *)r->rsi;
    bool  force_crit = ((r->rcx & 0xff) != 0);
    if (!ns_valid_ptr(lane) || !ns_valid_ptr(trig_pd)) return 0;

    uint8_t judge = force_crit ? NS_JUDGE_CRITICAL : *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    bool is_split = (*(uint8_t *)((char *)lane + 0x20) != 0);
    if (is_split) {
        uint8_t sub = *(uint8_t *)((char *)trig_pd + 0x19);
        int32_t je_cnt = 0;
        void **jes = ns_managed_ptr_seq(*(void **)((char *)lane + 0x30), &je_cnt);
        if (jes && (int32_t)sub < je_cnt) {
            apply_category_to_idle_or_next_ep(jes[sub], cat);
        }
        int32_t b_cnt = 0;
        void **bs = ns_managed_ptr_seq(*(void **)((char *)lane + 0x48), &b_cnt);
        for (int32_t i = 0; i < b_cnt; ++i) apply_category_to_idle_or_next_ep(bs[i], cat);
    } else {
        apply_category_to_idle_or_next_ep(*(void **)((char *)lane + 0x28), cat);
        int32_t b_cnt = 0;
        void **bs = ns_managed_ptr_seq(*(void **)((char *)lane + 0x40), &b_cnt);
        for (int32_t i = 0; i < b_cnt; ++i) apply_category_to_idle_or_next_ep(bs[i], cat);
    }
    return 0;
}

static ABI int hook_ems_set_judge(HookRegs *r) {
    void *lane    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_valid_ptr(lane) || !ns_valid_ptr(trig_pd)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_NONE || judge >= NS_JUDGE_MISS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    apply_category_to_idle_or_next_ep(*(void **)((char *)lane + 0x20), cat);
    return 0;
}

static ABI int hook_fms_update_holding(HookRegs *r) {
    void *ctrl    = (void *)r->rdi;
    void *trig_pd = (void *)r->rsi;
    if (!ns_valid_ptr(ctrl) || !ns_valid_ptr(trig_pd)) return 0;

    uint8_t judge = *(uint8_t *)((char *)trig_pd + 0x4c);
    if (judge == NS_JUDGE_MISS_PASS) return 0;

    int cat = ns_classify_trig_pd(trig_pd);
    if (cat < 0) cat = THEATRHYTHM_NOTE_HOLD;

    int32_t lane_cnt = 0;
    void **lanes = ns_managed_ptr_seq(*(void **)((char *)ctrl + 0x58), &lane_cnt);
    if (!lanes || lane_cnt <= 0) return 0;

    for (int32_t li = 0; li < lane_cnt; ++li) {
        void *lane = lanes[li];
        if (!ns_valid_ptr(lane)) continue;

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
            apply_category_to_loop_emitter(les[i], cat);
        }
    }
    return 0;
}

static ABI int hook_ems_set_holding(HookRegs *r) {
    void *lane    = (void *)r->rdi;
    bool  holding = ((r->rsi & 0xff) != 0);
    if (!ns_valid_ptr(lane) || !holding) return 0;

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
    apply_category_to_loop_emitter(*(void **)((char *)lane + 0x78), THEATRHYTHM_NOTE_HOLD);
    return 0;
}

static ABI int hook_tobj_set_crit(HookRegs *r) {
    void *tobj = (void *)r->rdi;
    void *ep   = (void *)r->rsi;
    if (!ns_valid_ptr(tobj) || !ns_valid_ptr(ep)) return 0;

    int cat = runtime_slidehold_is_registered_trigger_obj(tobj)
            ? THEATRHYTHM_NOTE_SLIDEHOLD
            : ns_trigger_type_to_category(*(uint8_t *)((char *)tobj + 0xd0));
    if (cat < 0 || cat == THEATRHYTHM_NOTE_JOINT) return 0;

    void *xform = *(void **)((char *)ep + 0x10);
    void *ps_all = *(void **)((char *)ep + 0x38);
    apply_category_to_emitter(ep, xform, ps_all, cat);
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
    printf("NoteSkins: installed %zu hooks (%zu bytes stub) for game %s\n",
           num_hooks, (size_t)(p - stubs), runtime_game_version());
    return 0;
}
