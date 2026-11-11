/* Theatrhythm Final Bar Line - PC Recomp Custom Charts Loader
 *
 * Implements native PC custom charting support for PS4 version 1.0.4.
 * Loads custom song packages (song.json, PCM 16-bit 48kHz WAV audio, PNG/JPEG jackets,
 * binary trigger charts) from the CustomCharts/ directory.
 * Injects songs into Table.MusicTable, Table.SeriesTable, and Table.SoundTable.
 * Intercepts Addressables to supply Unity AudioClips, Sprites, and TextAssets.
 * Implements the tutorial stage background, practice camera position, and combat
 * monster skip to prevent camera stutters.
 */
#include "runtime.h"
#include "runtime_custom_charts.h"
#include "runtime_judgements.h"
#include "runtime_tffscores.h"
#include "custom_difficulty_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <ctype.h>
#include <sys/stat.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#pragma GCC diagnostic pop

/* Registers saved in hook stubs (SysV x86-64 calling convention, including XMM0..XMM3) */
typedef struct {
    uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out;
    double xmm0, xmm1, xmm2, xmm3;
} HookRegs;
typedef int (ABI *HookFn)(HookRegs *regs);

static inline void set_ret_float(HookRegs *r, float v) {
    uint64_t u = 0;
    memcpy(&u, &v, 4);
    memcpy(&r->xmm0, &u, 8);
}

static unsigned char *module;

enum {
    /* Hook sites (83) */
    H_MUSIC_TABLE_CONVERT = 0,
    H_SERIES_TABLE_CONVERT,
    H_SOUND_TABLE_GET_LABEL,
    H_MUSIC_ID_GET_NAME,
    H_MUSIC_ID_GET_SORT_NAME,
    H_SERIES_ID_GET_NAME,
    H_SERIES_ID_GET_SHORT_NAME,
    H_LANGUAGE_TRANSLATION,
    H_JACKET_GET_ADDR,
    H_JACKET_GET_FILENAME,
    H_SERIES_LOGO_GET_ADDR,
    H_SERIES_LOGO_GET_SUBASSET,
    H_LOAD_ASSET_SYNC,
    H_LOAD_ASSET_ASYNC,
    H_GET_ASSETS,
    H_IS_DONE,
    H_PROGRESS,
    H_RELEASE_ASSET,
    H_TEXT_ASSET_GET_BYTES,
    H_TEXT_ASSET_GET_DATA_PTR,
    H_TEXT_ASSET_GET_DATA_SIZE,
    H_THEATER_BEGIN_START,
    H_THEATER_FINISH,
    H_CHANGE_TO_VIEW_DIFF,
    H_SELECTING_MS_CTOR,
    H_SELECTING_MS_SET_DIFF,
    H_SELECTING_GET_DIFF_VAL,
    H_PANEL_CTOR,
    H_PANEL_SET_DIFF,
    H_MENU_ADD_SONG,
    H_MENU_FILTER_PANEL,
    H_MENU_IS_PLAYABLE,
    H_LIST_CHANGE_VIEW_DIFF,
    H_LIST_SET_DIFF,
    H_LIST_SET_LAST_DIFF,
    H_JACKET_SET_LAST_DIFF,
    H_SORT_REFRESH_BADGES,
    H_JACKET_REFRESH_VIEW,
    H_JACKET_CHANGE_PLAY_DATA,
    H_JACKET_SET_DIFF,
    H_JACKET_EXECUTE,
    H_JACKET_DIFF_REACTION,
    H_PANEL_VIEW_ON_SET_VIEW,
    H_POPUP_ON_READY,
    H_POPUP_SET_STATUS,
    H_POPUP_SUBMIT,
    H_POPUP_EXECUTE,
    H_WINDOW_SETUP_CHANGE_DIFF,
    H_DIFF_LIST_SET_DIFF,
    H_DIFF_LIST_PLAY_SE,
    H_SET_ACTIVE_EDITION_MARK,
    H_LEVEL_BASE_SET_DIFF,
    H_MUSIC_NAME_SET_COLOR,
    H_MUSIC_STATUS_SET_COLOR,
    H_DIFF_ICON_LOADER_SUB,
    H_DIFF_SEMI_LOADER_ADDR,
    H_DIFF_SEMI_LOADER_SUB,
    H_LEVEL_BASE_LOADER_SUB,
    H_STAGE_EFFECT_LOADER_ADDR,
    H_LOAD_TITLE_PLATE_MOVENEXT,
    H_GET_DIFF_ICON_SPRITE,
    H_GET_DIFF_SEMI_SPRITE,
    H_GET_DIFF_BANNER_SPRITE,
    H_STAGE_INTRO_SETUP,
    H_STAGE_MUSIC_INFO_SETUP,
    H_MUSIC_INTRO_SETUP,
    H_CREATE_MS_PARTS,
    H_LIVE_INFO_SETUP_TEXT,
    H_SUPPORT_MGR_SETUP,
    H_SUPPORT_START,
    H_SET_PLAY_SETTING_5,
    H_SET_PLAY_SETTING_10,
    H_TRIGGER_LOADER_GET_ADDR,
    H_RUNTIME_CONTEXT_SETUP,
    H_DAMAGE_CALC_SETUP,
    H_BURST_GAUGE_SETUP,
    H_ABILITY_CONDITION_SETUP,
    H_SUMMON_BAR_UTIL_SETUP,
    H_COPY_MS_PLAY_DATA,
    H_MUSIC_TITLE_DISPLAY_SETUP,
    H_RETRY_WINDOW_ON_UPDATE,
    H_IMAGE_ANIM_PROPS,
    H_IMAGE_SET_SPRITE,
    H_GET_MUSIC_RECORD,
    H_CREATE_MUSIC_RECORD,
    H_LOADED_DATA_MUSIC_RECORDS,
    H_SETUP_DLC_MUSIC_RECORDS,
    H_MUSIC_RECORD_TOTAL_PC,
    H_PROFICA_CLEAR_CTOR,
    H_PROFICA_CREATE_PROFILE,
    H_REGISTER_PLAY_STYLE_REC,
    H_MUSIC_INFO_SERIALIZE,
    H_RHYTHPO_CALC_1,
    H_RHYTHPO_CALC_2,
    H_RHYTHPO_GET_TABLE_BONUS,
    H_RHYTHPO_ADD_BONUS,
    H_RHYTHPO_SET_TOTAL,
    CC_HOOK_SITES,

    /* Patch site inside CountDataList::InitializeList */
    P_COUNT_DATA_LIST_INIT = CC_HOOK_SITES,

    /* Callee sites */
    CC_MEMBERWISE_CLONE,
    CC_MUSIC_ID_IMPLICIT,
    CC_SERIES_ID_IMPLICIT,
    CC_UOBJ_OP_IMPLICIT,
    CC_UOBJ_SET_HIDE_FLAGS,
    CC_UOBJ_DESTROY,
    CC_UOBJ_GET_NAME,
    CC_UOBJ_SET_NAME,
    CC_UOBJ_CLONE_WITH_PARENT,
    CC_AUDIO_CLIP_CREATE,
    CC_AUDIO_CLIP_SET_DATA,
    CC_TEX2D_CTOR,
    CC_TEX_SET_FILTER,
    CC_TEX_SET_WRAP,
    CC_TEX2D_LOAD_RAW,
    CC_TEX2D_APPLY,
    CC_SPRITE_CREATE,
    CC_SPRITE_GET_RECT,
    CC_SPRITE_GET_BORDER,
    CC_SPRITE_GET_PPU,
    CC_SPRITE_GET_PIVOT,
    CC_TEXT_ASSET_INTERNAL_CREATE,
    CC_CAM_SET_FMS_PRACTICE,
    CC_CAM_SET_BMS_PRACTICE,
    CC_BEHAVIOUR_SET_ENABLED,
    CC_COMP_GET_TRANSFORM,
    CC_COMP_GET_GAMEOBJECT,
    CC_GO_GET_COMP_BY_NAME,
    CC_GO_GET_TRANSFORM,
    CC_GO_SET_ACTIVE,
    CC_RT_GET_ANCHORED_POS,
    CC_RT_SET_ANCHORED_POS,
    CC_RT_GET_SIZE_DELTA,
    CC_RT_SET_SIZE_DELTA,
    CC_TR_GET_PARENT,
    CC_TR_SET_LOCAL_SCALE,
    CC_TR_GET_CHILD_COUNT,
    CC_TR_SET_SIBLING_INDEX,
    CC_TR_GET_CHILD,
    CC_IMAGE_GET_SPRITE,
    CC_IMAGE_SET_SPRITE,
    CC_GRAPHIC_GET_COLOR,
    CC_GRAPHIC_SET_COLOR,
    CC_TMP_TEXT_SET_TEXT,
    CC_UI_TEXT_SET_TEXT,
    CC_TMP_GET_SHARED_MAT,
    CC_TMP_SET_SHARED_MAT,
    CC_TMP_SET_FONT_MAT,
    CC_MAT_CTOR,
    CC_MAT_SET_COLOR,
    CC_UIGRAD_SET_COLOR1,
    CC_UIGRAD_SET_COLOR2,
    CC_UIGRAD_SET_COLOR3,
    CC_UIGRAD_SET_COLOR4,
    CC_MAT_GUI_SHADOW_SET_COLOR,
    CC_UI_SHADOW_SET_COLOR,
    CC_LAYOUT_FORCE_REBUILD,
    CC_DIFF_LIST_SET_DIFF,
    CC_DIFF_BTN_PLAY_UNSEL,
    CC_DIFF_BTN_PLAY_SEL,
    CC_DIFF_BASE_SET_ACTIVE,
    CC_DIFF_BTN_SET_ACTIVE,
    CC_DIFF_LIST_PLAY_SE,
    CC_PANEL_CTOR,
    CC_UILIST_ADD_ITEM,
    CC_UIOBJECT_END_ACTION,
    CC_UIOBJECT_ON_CHANGED_FLAG,
    CC_MUSIC_NAME_SET_NAME,
    CC_MUSIC_NAME_SET_TEXT_COLOR,
    CC_MUSIC_STATUS_SET_STATUS,
    CC_MUSIC_REC_CREATE_DIFF_REC,
    CC_MUSIC_INFO_CREATE_REC,
    CC_MUSIC_INFO_GET_PSR,
    CC_PSR_GET_TOTAL_PC,
    CC_PSR_GET_HIGHSCORE,
    CC_PSR_SET_HIGHSCORE,
    CC_PSR_GET_UPDATE_DATE,
    CC_PSR_SET_UPDATE_DATE,
    CC_PSR_GET_BEST_RANK,
    CC_PSR_SET_BEST_RANK,
    CC_PSR_GET_MAX_CHAIN,
    CC_PSR_SET_MAX_CHAIN,
    CC_PSR_GET_BEST_MARK,
    CC_PSR_SET_BEST_MARK,
    CC_PSR_CHECK_BAR_BURIED,
    CC_PSR_SET_TOTAL_PC,
    CC_PSR_GET_TOTAL_CC,
    CC_PSR_SET_TOTAL_CC,
    CC_PSR_GET_TOTAL_FC,
    CC_PSR_SET_TOTAL_FC,
    CC_PSR_GET_MAX_DAMAGE,
    CC_PSR_SET_MAX_DAMAGE,
    CC_PROFICA_DIFF_CLEAR_CTOR,
    CC_SITES
};

static GameCode s_cc_code[CC_SITES];
#define CODE(id) ((void *)(module + s_cc_code[id].offset))

static uint64_t load64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static uint32_t load32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put64(void *p, uint64_t v) { memcpy(p, &v, 8); }
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }

#define MAX_CUSTOM_SONGS 256
#define MAX_SLOTS 6

typedef struct {
    char file[128];
    int rating;
    uint8_t *data;
    size_t size;
} CustomChart;

typedef struct {
    char id[128];
    char title[256];
    char artist[256];
    char folder[512];
    char audio_file[128];
    char jacket_file[128];
    bool has_jacket;
    int mode; /* 1 = BMS, 0 = FMS */
    int duration_ms;
    int preview_begin_ms;
    int preview_length_ms;
    uint32_t music_id;
    uint64_t pcm_offset;
    uint64_t pcm_bytes;
    CustomChart charts[MAX_SLOTS];
    int registered;
    int last_selected_slot;

    void *managed_id;
    void *managed_title;
    void *managed_sort_name;
    void *managed_label;
    void *managed_jacket_addr;
    void *managed_record;
    void *template_id;
    uint32_t root_id;
    uint32_t root_title;
    uint32_t root_sort;
    uint32_t root_label;
    uint32_t root_jacket_addr;
    uint32_t root_record;
    uint32_t root_template;
    uint8_t *decoded_jacket_rgba;
    int decoded_jacket_w;
    int decoded_jacket_h;
    volatile int jacket_decoded;
    uint8_t last_saved_fav;
} CustomSong;

static CustomSong songs[MAX_CUSTOM_SONGS];
static size_t song_count = 0;
static volatile bool s_custom_records_dirty = true;
static volatile bool s_custom_records_loaded_once = false;
static char charts_dir[512];
static bool s_has_series_cover = false;
static char s_series_cover_path[700];
static uint32_t custom_series_id = 189000001;
static void *series_object = NULL;
static void *series_title = NULL;
static void *series_template = NULL;

/* -------------------------------------------------------------------------- */
/* IL2CPP RUNTIME API & GC ROOTING (LAZILY RESOLVED AT RUNTIME)               */
/* -------------------------------------------------------------------------- */

/* In Il2CppClass: name at +0x10, parent at +0x58, methods at +0x98, method_count at +0x11c.
 * In Il2CppMethodInfo (IL2CPP v29 / Unity 2020.3): virtualMethodPointer is at +0x08! */
typedef struct {
    void *pointer;
    void *virtual_pointer;
    void *invoker;
    const char *name;
    void *klass;
    void *return_type;
    void *parameters;
    void *rgctx;
    void *generic;
    uint32_t token;
    uint16_t flags, iflags, slot;
    uint8_t parameter_count, bits;
} Il2CppMethodInfo;

static void *(ABI *il2cpp_domain_get_fn)(void) = NULL;
static void **(ABI *il2cpp_domain_get_assemblies_fn)(void *domain, size_t *count) = NULL;
static void *(ABI *il2cpp_assembly_get_image_fn)(void *assembly) = NULL;
static void *(ABI *il2cpp_class_from_name_fn)(void *image, const char *ns, const char *name) = NULL;
static Il2CppMethodInfo *(ABI *il2cpp_class_get_method_from_name_fn)(void *klass, const char *name, int args) = NULL;
static void *(ABI *il2cpp_object_new_fn)(void *klass) = NULL;
static void *(ABI *il2cpp_array_new_fn)(void *element_klass, uintptr_t length) = NULL;
static void *(ABI *il2cpp_array_new_specific_fn)(void *array_type_info, uintptr_t length) = NULL;
static void *(ABI *il2cpp_class_get_nested_types_fn)(void *klass, void **iter) = NULL;
static void *(ABI *il2cpp_string_new_fn)(const char *str) = NULL;
static void (ABI *il2cpp_runtime_class_init_fn)(void *klass) = NULL;
static uint32_t (ABI *il2cpp_gchandle_new_fn)(void *obj, bool pinned) = NULL;
static void (ABI *il2cpp_gchandle_free_fn)(uint32_t gchandle) = NULL;
static void *(ABI *il2cpp_resolve_icall_fn)(const char *name) = NULL;

static bool s_il2cpp_resolved = false;
static void *s_byte_class = NULL;
static void *s_single_class = NULL;
static void *s_int32_class = NULL;
static void *s_uobject_class = NULL;
static void *s_textasset_class = NULL;
static void *s_texture2d_class = NULL;
static void *s_sprite_class = NULL;
static void *s_addr_async_op_class = NULL;
static void *s_object_array_class = NULL;

static void *find_il2cpp_class(const char *ns, const char *name) {
    if (!il2cpp_domain_get_fn || !il2cpp_domain_get_assemblies_fn ||
        !il2cpp_assembly_get_image_fn || !il2cpp_class_from_name_fn) {
        return NULL;
    }
    void *domain = il2cpp_domain_get_fn();
    if (!domain) return NULL;
    size_t count = 0;
    void **assemblies = il2cpp_domain_get_assemblies_fn(domain, &count);
    if (!assemblies || count == 0 || count > 512) return NULL;
    for (size_t i = 0; i < count; ++i) {
        if (!assemblies[i]) continue;
        void *img = il2cpp_assembly_get_image_fn(assemblies[i]);
        if (!img) continue;
        void *klass = il2cpp_class_from_name_fn(img, ns, name);
        if (klass) {
            if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
            return klass;
        }
    }
    return NULL;
}

static void ensure_il2cpp_api(void) {
    if (!s_il2cpp_resolved) {
        const char *prx = "Il2CppUserAssemblies.prx";
        void *dom = runtime_module_export(prx, "il2cpp_domain_get");
        if (dom) {
            il2cpp_domain_get_fn = (void *(ABI *)(void))dom;
            il2cpp_domain_get_assemblies_fn = (void **(ABI *)(void *, size_t *))runtime_module_export(prx, "il2cpp_domain_get_assemblies");
            il2cpp_assembly_get_image_fn = (void *(ABI *)(void *))runtime_module_export(prx, "il2cpp_assembly_get_image");
            il2cpp_class_from_name_fn = (void *(ABI *)(void *, const char *, const char *))runtime_module_export(prx, "il2cpp_class_from_name");
            il2cpp_class_get_method_from_name_fn = (Il2CppMethodInfo *(ABI *)(void *, const char *, int))runtime_module_export(prx, "il2cpp_class_get_method_from_name");
            il2cpp_object_new_fn = (void *(ABI *)(void *))runtime_module_export(prx, "il2cpp_object_new");
            il2cpp_array_new_fn = (void *(ABI *)(void *, uintptr_t))runtime_module_export(prx, "il2cpp_array_new");
            il2cpp_array_new_specific_fn = (void *(ABI *)(void *, uintptr_t))runtime_module_export(prx, "il2cpp_array_new_specific");
            il2cpp_class_get_nested_types_fn = (void *(ABI *)(void *, void **))runtime_module_export(prx, "il2cpp_class_get_nested_types");
            il2cpp_string_new_fn = (void *(ABI *)(const char *))runtime_module_export(prx, "il2cpp_string_new");
            void *rci = runtime_module_export(prx, "il2cpp_runtime_class_init");
            if (rci) il2cpp_runtime_class_init_fn = (void (ABI *)(void *))rci;
            il2cpp_gchandle_new_fn = (uint32_t (ABI *)(void *, bool))runtime_module_export(prx, "il2cpp_gchandle_new");
            il2cpp_gchandle_free_fn = (void (ABI *)(uint32_t))runtime_module_export(prx, "il2cpp_gchandle_free");
            il2cpp_resolve_icall_fn = (void *(ABI *)(const char *))runtime_module_export(prx, "il2cpp_resolve_icall");
            s_il2cpp_resolved = true;
            printf("CustomCharts: Resolved IL2CPP exports (obj_new=%p arr_new=%p str_new=%p gchandle=%p icall=%p)\n",
                   (void *)il2cpp_object_new_fn, (void *)il2cpp_array_new_fn,
                   (void *)il2cpp_string_new_fn, (void *)il2cpp_gchandle_new_fn,
                   (void *)il2cpp_resolve_icall_fn);
        }
    }
    if (s_il2cpp_resolved && !s_uobject_class) {
        void *dom = il2cpp_domain_get_fn ? il2cpp_domain_get_fn() : NULL;
        if (dom) {
            if (!s_byte_class) s_byte_class = find_il2cpp_class("System", "Byte");
            if (!s_single_class) s_single_class = find_il2cpp_class("System", "Single");
            if (!s_int32_class) s_int32_class = find_il2cpp_class("System", "Int32");
            if (!s_uobject_class) s_uobject_class = find_il2cpp_class("UnityEngine", "Object");
            if (!s_textasset_class) s_textasset_class = find_il2cpp_class("UnityEngine", "TextAsset");
            if (!s_texture2d_class) s_texture2d_class = find_il2cpp_class("UnityEngine", "Texture2D");
            if (!s_sprite_class) s_sprite_class = find_il2cpp_class("UnityEngine", "Sprite");
            if (!s_addr_async_op_class) s_addr_async_op_class = find_il2cpp_class("Gummi.File.AsyncOperations", "AddressableAssetAsyncOperation");
        }
    }
}

static uint32_t gc_pin(void *obj, bool pinned) {
    if (!obj) return 0;
    ensure_il2cpp_api();
    if (il2cpp_gchandle_new_fn) return il2cpp_gchandle_new_fn(obj, pinned);
    return 0;
}

static void gc_unpin(uint32_t handle) {
    if (handle && il2cpp_gchandle_free_fn) {
        il2cpp_gchandle_free_fn(handle);
    }
}

/* In Il2Cpp objects, class pointer is at +0x00 */
static void *object_class(const void *obj) { return obj ? (void *)load64(obj) : NULL; }

static Il2CppMethodInfo *find_method(void *klass, const char *name, int args) {
    if (!klass || !name) return NULL;
    ensure_il2cpp_api();
    for (void *k = klass; k; k = (void *)load64((char *)k + 0x58)) {
        if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(k);
        if (il2cpp_class_get_method_from_name_fn) {
            Il2CppMethodInfo *m = il2cpp_class_get_method_from_name_fn(k, name, args);
            if (m && m->pointer) return m;
        }
        Il2CppMethodInfo **methods = (Il2CppMethodInfo **)load64((char *)k + 0x98);
        uint16_t count = *(uint16_t *)((char *)k + 0x11c);
        if (!methods || count > 4096) continue;
        for (int i = 0; i < count; ++i) {
            if (methods[i] && methods[i]->pointer && methods[i]->parameter_count == args &&
                methods[i]->name && !strcmp(methods[i]->name, name)) {
                return methods[i];
            }
        }
    }
    return NULL;
}

static void *clone_object(void *obj) {
    if (!obj) return NULL;
    typedef void *(ABI *CloneFn)(void *self, void *method);
    CloneFn clone_fn = (CloneFn)CODE(CC_MEMBERWISE_CLONE); /* System.Object::MemberwiseClone */
    return clone_fn(obj, NULL);
}

static void *alloc_managed_object(void *klass) {
    if (!klass || ((uintptr_t)klass & 1u) != 0 || (uintptr_t)klass < 0x10000ULL) return NULL;
    ensure_il2cpp_api();
    if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
    if (il2cpp_object_new_fn) return il2cpp_object_new_fn(klass);
    return NULL;
}

static void *alloc_managed_array_by_elem(void *elem_klass, size_t count) {
    if (!elem_klass) return NULL;
    ensure_il2cpp_api();
    if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(elem_klass);
    if (il2cpp_array_new_fn) return il2cpp_array_new_fn(elem_klass, (uintptr_t)count);
    return NULL;
}

static void *alloc_managed_array_by_arr_class(void *arr_klass, size_t count) {
    if (!arr_klass) return NULL;
    ensure_il2cpp_api();
    if (il2cpp_array_new_specific_fn) return il2cpp_array_new_specific_fn(arr_klass, (uintptr_t)count);
    void *elem_klass = (void *)load64((const char *)arr_klass + 0x48);
    if (elem_klass && il2cpp_array_new_fn) return il2cpp_array_new_fn(elem_klass, (uintptr_t)count);
    return NULL;
}

static bool dict_contains_key(void *dict, void *key) {
    if (!dict || !key) return false;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(dict);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "ContainsKey", 1);
    if (!method) return false;
    s_last_klass = klass;
    s_last_method = method;
    typedef bool (ABI *ContainsFn)(void *self, void *key, void *method);
    return ((ContainsFn)method->pointer)(dict, key, method);
}

static bool dict_add_unchecked(void *dict, void *key, void *value) {
    if (!dict || !key) return false;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(dict);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "Add", 2);
    if (!method) return false;
    s_last_klass = klass;
    s_last_method = method;
    typedef void (ABI *AddFn)(void *self, void *key, void *value, void *method);
    ((AddFn)method->pointer)(dict, key, value, method);
    return true;
}

static bool dict_add_safe(void *dict, void *key, void *value) {
    if (!dict || !key) return false;
    if (dict_contains_key(dict, key)) return false;
    return dict_add_unchecked(dict, key, value);
}

static void *dict_try_get_value(void *dict, void *key) {
    if (!dict || !key) return NULL;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(dict);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "TryGetValue", 2);
    if (!method) return NULL;
    s_last_klass = klass;
    s_last_method = method;
    void *result = NULL;
    typedef bool (ABI *TryGetFn)(void *self, void *key, void **value, void *method);
    return ((TryGetFn)method->pointer)(dict, key, &result, method) ? result : NULL;
}

/* Int32/Enum-keyed Dictionary<TEnum, TValue>: stride 24 (hash +0, next +4, key +8, val +16) */
static bool dict_find_i32(void *dict, int key, void **out_val) {
    if (!dict) return false;
    void *entries = (void *)load64((char *)dict + 0x18);
    int count = (int)load32((char *)dict + 0x20);
    if (!entries || count <= 0 || (uint64_t)count > load64((char *)entries + 0x18)) return false;
    for (int i = 0; i < count; ++i) {
        size_t at = 0x20 + (size_t)i * 24;
        if ((int32_t)load32((char *)entries + at) < 0) continue;
        if ((int32_t)load32((char *)entries + at + 8) != key) continue;
        if (out_val) *out_val = (void *)load64((char *)entries + at + 16);
        return true;
    }
    return false;
}

static bool dict_add_i32(void *dict, int key, void *value) {
    if (!dict) return false;
    if (dict_find_i32(dict, key, NULL)) return true;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(dict);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "Add", 2);
    if (!method) return false;
    s_last_klass = klass;
    s_last_method = method;
    typedef void (ABI *AddEnumFn)(void *self, int32_t key, void *value, void *method);
    ((AddEnumFn)method->pointer)(dict, (int32_t)key, value, method);
    return true;
}

static bool list_add(void *list, void *item) {
    if (!list) return false;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(list);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "Add", 1);
    if (!method) return false;
    s_last_klass = klass;
    s_last_method = method;
    typedef void (ABI *ListAddFn)(void *self, void *item, void *method);
    ((ListAddFn)method->pointer)(list, item, method);
    return true;
}

static void dict_each(void *dict, void (*cb)(void *key, void *value, void *ctx), void *ctx) {
    if (!dict) return;
    void *entries = (void *)load64((char *)dict + 0x18);
    int count = load32((char *)dict + 0x20);
    if (!entries || count <= 0 || (uint64_t)count > load64((char *)entries + 0x18)) return;
    for (int i = 0; i < count; ++i) {
        size_t offset = 0x20 + (size_t)i * 24;
        if ((int32_t)load32((char *)entries + offset) < 0) continue;
        void *k = (void *)load64((char *)entries + offset + 8);
        void *v = (void *)load64((char *)entries + offset + 16);
        cb(k, v, ctx);
    }
}

/* Create an immutable System.String in low memory outside the GC heap (used by uGUI helpers) */
static void *make_string(const void *like, const char *text) {
    if (!text) return NULL;
    ensure_il2cpp_api();
    if (!like && il2cpp_string_new_fn) {
        return il2cpp_string_new_fn(text);
    }
    const size_t n = strlen(text);
    unsigned char *s = (unsigned char *)runtime_low_alloc(0x16 + 2 * n);
    if (!s) return NULL;
    if (like) memcpy(s, like, 8);
    put32(s + 0x10, (int32_t)n);
    for (size_t i = 0; i < n; ++i) {
        uint16_t c = (uint8_t)text[i];
        memcpy(s + 0x14 + 2 * i, &c, 2);
    }
    return s;
}

static void *make_pinned_string(const char *text, uint32_t *out_root) {
    if (!text) return NULL;
    ensure_il2cpp_api();
    void *s = il2cpp_string_new_fn ? il2cpp_string_new_fn(text) : make_string(NULL, text);
    uint32_t r = gc_pin(s, false);
    if (out_root) *out_root = r;
    return s;
}

static void read_guest_string(const void *s, char *out, size_t size) {
    size_t n = 0;
    if (s && (uintptr_t)s > 0x10000 && (uintptr_t)s < 0x00007fffffffffffULL) {
        const int32_t length = load32((const char *)s + 0x10);
        if (length > 0 && length < 2048) {
            const uint16_t *chars = (const uint16_t *)((const char *)s + 0x14);
            for (int32_t i = 0; i < length && n + 1 < size; ++i)
                out[n++] = chars[i] < 128 ? (char)chars[i] : '?';
        }
    }
    out[n] = 0;
}

static uint32_t fnv1a_id(const char *s) {
    uint32_t hash = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c += 32;
        hash = (hash ^ c) * 16777619u;
    }
    return 169000000u + (hash % 1000000u);
}

static void make_sort_key(const char *title, char *out, size_t size) {
    while (*title == ' ' || *title == '\t') ++title;
    size_t len = 0;
    if (*title && !((*title >= 'a' && *title <= 'z') || (*title >= 'A' && *title <= 'Z'))) {
        out[len++] = '#';
    }
    for (; *title && len + 1 < size; ++title) {
        char c = *title;
        if (c >= 'A' && c <= 'Z') c += 32;
        out[len++] = c;
    }
    out[len] = '\0';
}

bool runtime_custom_charts_is_custom_id(uint32_t music_id) {
    return music_id >= 169000000u && music_id < 170000000u;
}

static CustomSong *find_song_by_id(uint32_t id) {
    if (!runtime_custom_charts_is_custom_id(id)) return NULL;
    for (size_t i = 0; i < song_count; ++i) {
        if (songs[i].music_id == id) return &songs[i];
    }
    return NULL;
}

static CustomSong *find_song_by_obj(const void *obj) {
    if (!obj || (uintptr_t)obj < 0x10000ULL) return NULL;
    uint32_t id = load32((const char *)obj + 0x10);
    return find_song_by_id(id);
}

static bool expand_int_array(void *owner, size_t offset) {
    if (!owner) return false;
    void *old_arr = (void *)load64((char *)owner + offset);
    if (!old_arr) return false;
    uint64_t len = load64((char *)old_arr + 0x18);
    if (len >= MAX_SLOTS) return true;
    if (len == 0 || len > MAX_SLOTS) return false;
    void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
    if (!new_arr) return false;
    memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, (size_t)len * sizeof(int32_t));
    memset((char *)new_arr + 0x20 + (size_t)len * sizeof(int32_t), 0, (MAX_SLOTS - (size_t)len) * sizeof(int32_t));
    put64((char *)owner + offset, (uint64_t)new_arr);
    return true;
}

static void expand_ref_array_copy(void *owner, size_t offset) {
    if (!owner) return;
    void *old_arr = (void *)load64((char *)owner + offset);
    if (!old_arr || load64((char *)old_arr + 0x18) != 4) return;
    void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
    if (!new_arr) return;
    memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));
    put64((char *)new_arr + 0x20 + 4 * sizeof(void *), load64((char *)old_arr + 0x20 + 0 * sizeof(void *)));
    put64((char *)new_arr + 0x20 + 5 * sizeof(void *), load64((char *)old_arr + 0x20 + 3 * sizeof(void *)));
    put64((char *)owner + offset, (uint64_t)new_arr);
}

static void expand_ref_array_clone(void *owner, size_t offset) {
    if (!owner) return;
    void *old_arr = (void *)load64((char *)owner + offset);
    if (!old_arr || load64((char *)old_arr + 0x18) != 4) return;
    void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
    if (!new_arr) return;
    uint32_t root = gc_pin(new_arr, false);
    memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));
    void *e0 = (void *)load64((char *)old_arr + 0x20 + 0 * sizeof(void *));
    void *e3 = (void *)load64((char *)old_arr + 0x20 + 3 * sizeof(void *));
    put64((char *)new_arr + 0x20 + 4 * sizeof(void *), (uint64_t)(e0 ? clone_object(e0) : NULL));
    put64((char *)new_arr + 0x20 + 5 * sizeof(void *), (uint64_t)(e3 ? clone_object(e3) : NULL));
    put64((char *)owner + offset, (uint64_t)new_arr);
    gc_unpin(root);
}

/* Minimal JSON parser for song.json */
static char *json_skip_ws(char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    return p;
}

static char *json_find_key(char *p, const char *key) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        char *after = json_skip_ws(p + nlen);
        if (*after == ':') return json_skip_ws(after + 1);
        p += nlen;
    }
    return NULL;
}

static bool json_get_string(char *p, const char *key, char *out, size_t max_out) {
    char *val = json_find_key(p, key);
    if (!val || *val != '"') return false;
    ++val;
    size_t len = 0;
    while (*val && *val != '"' && len + 1 < max_out) {
        if (*val == '\\' && *(val + 1)) ++val;
        out[len++] = *val++;
    }
    out[len] = '\0';
    return true;
}

static bool json_get_int(char *p, const char *key, int *out) {
    char *val = json_find_key(p, key);
    if (!val) return false;
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (end == val) return false;
    *out = (int)v;
    return true;
}

static bool validate_wav(const char *path, uint64_t *pcm_offset, uint64_t *pcm_bytes, int duration_ms) {
    (void)duration_ms;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t head[44];
    if (fread(head, 1, 12, f) != 12) { fclose(f); return false; }
    if (memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) { fclose(f); return false; }

    bool fmt_ok = false, data_ok = false;
    uint32_t chunk_id, chunk_size;
    while (fread(&chunk_id, 1, 4, f) == 4 && fread(&chunk_size, 1, 4, f) == 4) {
        long cur = ftell(f);
        if (!memcmp(&chunk_id, "fmt ", 4)) {
            uint8_t fmt[16];
            if (chunk_size >= 16 && fread(fmt, 1, 16, f) == 16) {
                uint16_t format = (uint16_t)(fmt[0] | (fmt[1] << 8));
                uint16_t channels = (uint16_t)(fmt[2] | (fmt[3] << 8));
                uint32_t rate = (uint32_t)fmt[4] | ((uint32_t)fmt[5] << 8) | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
                uint16_t bits = (uint16_t)(fmt[14] | (fmt[15] << 8));
                if (format == 1 && channels == 2 && rate == 48000 && bits == 16) fmt_ok = true;
            }
        } else if (!memcmp(&chunk_id, "data", 4)) {
            *pcm_offset = (uint64_t)cur;
            *pcm_bytes = chunk_size;
            data_ok = true;
        }
        fseek(f, cur + (long)chunk_size + (long)(chunk_size & 1), SEEK_SET);
    }
    fclose(f);
    return fmt_ok && data_ok;
}

static bool validate_chart(const uint8_t *data, size_t size, int mode, int duration_ms) {
    (void)duration_ms;
    if (size < 44) return false;
    uint32_t m = load32(data);
    uint32_t count = load32(data + 28);
    if ((int)m != mode) return false;
    if (size != 44 + (uint64_t)count * 56) return false;
    return true;
}

static char s_save_dir[512] = "user";

static void scan_custom_charts_dir(const char *root) {
    printf("CustomCharts: Scanning %s\n", root);
    strncpy(charts_dir, root, sizeof(charts_dir) - 1);

    struct stat cover_st;
    snprintf(s_series_cover_path, sizeof(s_series_cover_path), "%s\\CustomCharts.png", root);
    if (stat(s_series_cover_path, &cover_st) == 0 && cover_st.st_size > 8) {
        s_has_series_cover = true;
    } else {
        snprintf(s_series_cover_path, sizeof(s_series_cover_path), "%s\\..\\CustomCharts.png", root);
        if (stat(s_series_cover_path, &cover_st) == 0 && cover_st.st_size > 8) {
            s_has_series_cover = true;
        }
    }

#ifdef _WIN32
    char search_pattern[600];
    snprintf(search_pattern, sizeof(search_pattern), "%s\\*", root);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (song_count >= MAX_CUSTOM_SONGS) break;

        char json_path[700];
        snprintf(json_path, sizeof(json_path), "%s\\%s\\song.json", root, fd.cFileName);
        FILE *f = fopen(json_path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len < 10 || len > 65536) { fclose(f); continue; }

        char *buf = (char *)malloc((size_t)len + 1);
        if (!buf) { fclose(f); continue; }
        if (fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); fclose(f); continue; }
        buf[len] = '\0';
        fclose(f);

        CustomSong song;
        memset(&song, 0, sizeof(song));
        snprintf(song.folder, sizeof(song.folder), "%s\\%s", root, fd.cFileName);

        char mode_str[16] = {0};
        if (!json_get_string(buf, "id", song.id, sizeof(song.id)) ||
            !json_get_string(buf, "title", song.title, sizeof(song.title)) ||
            !json_get_string(buf, "mode", mode_str, sizeof(mode_str)) ||
            !json_get_string(buf, "audio", song.audio_file, sizeof(song.audio_file)) ||
            !json_get_int(buf, "durationMs", &song.duration_ms)) {
            free(buf);
            continue;
        }

        int schema_ver = 1;
        json_get_int(buf, "schemaVersion", &schema_ver);

        json_get_string(buf, "artist", song.artist, sizeof(song.artist));
        json_get_string(buf, "jacket", song.jacket_file, sizeof(song.jacket_file));
        if (song.jacket_file[0]) {
            char jpath[700];
            struct stat jst;
            snprintf(jpath, sizeof(jpath), "%s\\%s", song.folder, song.jacket_file);
            if (stat(jpath, &jst) == 0 && jst.st_size > 8 && jst.st_size <= 8388608) {
                song.has_jacket = true;
            }
        }
        song.mode = (!strcmp(mode_str, "BMS")) ? 1 : 0;
        song.music_id = fnv1a_id(song.id);

        if (find_song_by_id(song.music_id)) {
            printf("CustomCharts: Skipping duplicate/colliding song id [%s] (%u)\n", song.id, song.music_id);
            free(buf);
            continue;
        }

        char *preview_block = strstr(buf, "\"preview\"");
        if (preview_block) {
            json_get_int(preview_block, "beginMs", &song.preview_begin_ms);
            json_get_int(preview_block, "lengthMs", &song.preview_length_ms);
        }
        if (song.preview_length_ms <= 0) song.preview_length_ms = 15000;

        char wav_path[700];
        snprintf(wav_path, sizeof(wav_path), "%s\\%s", song.folder, song.audio_file);
        if (!validate_wav(wav_path, &song.pcm_offset, &song.pcm_bytes, song.duration_ms)) {
            printf("CustomCharts: Invalid WAV in %s\n", song.id);
            free(buf);
            continue;
        }

        int chart_cnt = 0;
        char *charts_block = strstr(buf, "\"charts\"");
        if (charts_block) {
            /* Pre-count chart objects for schemaVersion <= 1 sourceDifficulty remapping */
            int total_charts_in_json = 0;
            char *cp = strchr(charts_block, '[');
            while (cp && (cp = strchr(cp, '{')) != NULL) {
                char *ce = strchr(cp, '}');
                if (!ce) break;
                ++total_charts_in_json;
                cp = ce + 1;
            }

            char *p = strchr(charts_block, '[');
            while (p && (p = strchr(p, '{')) != NULL) {
                char *brace_end = strchr(p, '}');
                if (!brace_end) break;
                char obj[512] = {0};
                size_t olen = (size_t)(brace_end - p + 1);
                if (olen < sizeof(obj)) {
                    memcpy(obj, p, olen);
                    int slot = -1, rating = 0, src_diff = -1;
                    char cfile[128] = {0};
                    json_get_int(obj, "sourceDifficulty", &src_diff);
                    if (json_get_int(obj, "slot", &slot) && json_get_int(obj, "rating", &rating) &&
                        json_get_string(obj, "file", cfile, sizeof(cfile))) {
                        if (schema_ver <= 1) {
                            if (src_diff == 5 && song.charts[5].data == NULL) {
                                slot = 5; /* Chaos */
                            } else if (total_charts_in_json == 1 && src_diff == 0 && song.charts[4].data == NULL) {
                                slot = 4; /* Beginner */
                            }
                        }
                        if (slot >= 0 && slot < MAX_SLOTS) {
                            strncpy(song.charts[slot].file, cfile, sizeof(song.charts[slot].file) - 1);
                            song.charts[slot].rating = rating;

                            char cpath[700];
                            snprintf(cpath, sizeof(cpath), "%s\\%s", song.folder, cfile);
                            FILE *cf = fopen(cpath, "rb");
                            if (cf) {
                                fseek(cf, 0, SEEK_END);
                                long csize = ftell(cf);
                                fseek(cf, 0, SEEK_SET);
                                if (csize >= 44 && csize < 8400044) {
                                    uint8_t *cdata = (uint8_t *)runtime_low_alloc((size_t)csize);
                                    if (cdata && fread(cdata, 1, (size_t)csize, cf) == (size_t)csize &&
                                        validate_chart(cdata, (size_t)csize, song.mode, song.duration_ms)) {
                                        song.charts[slot].data = cdata;
                                        song.charts[slot].size = (size_t)csize;
                                        ++chart_cnt;
                                    }
                                }
                                fclose(cf);
                            }
                        }
                    }
                }
                p = brace_end + 1;
            }
        }

        free(buf);
        if (chart_cnt == 0) {
            printf("CustomCharts: Skipping [%s] (no valid chart files)\n", song.id);
            continue;
        }
        song.last_selected_slot = -1;
        songs[song_count++] = song;
        printf("CustomCharts: Scanned [%s] nativeId=%u mode=%s charts=%d jacket=%d title='%s'\n",
               song.id, song.music_id, song.mode ? "BMS" : "FMS", chart_cnt, (int)song.has_jacket, song.title);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    /* Post-scan pass: merge legacy "<base>-extra-difficulties" single-chart companions into base song slot 5 (Chaos) */
    const char *extra_suffix = "-extra-difficulties";
    const size_t suffix_len = strlen(extra_suffix);
    for (size_t i = 0; i < song_count; ) {
        CustomSong *extra = &songs[i];
        size_t id_len = strlen(extra->id);
        int extra_cnt = 0;
        int extra_slot = -1;
        for (int s = 0; s < MAX_SLOTS; ++s) {
            if (extra->charts[s].data != NULL) {
                ++extra_cnt;
                extra_slot = s;
            }
        }
        bool merged = false;
        if (extra_cnt == 1 && id_len > suffix_len &&
            !strcmp(extra->id + (id_len - suffix_len), extra_suffix)) {
            size_t base_len = id_len - suffix_len;
            for (size_t j = 0; j < song_count; ++j) {
                if (i == j) continue;
                CustomSong *base = &songs[j];
                if (strlen(base->id) == base_len &&
                    !strncmp(base->id, extra->id, base_len) &&
                    base->mode == extra->mode &&
                    base->charts[5].data == NULL) {
                    base->charts[5] = extra->charts[extra_slot];
                    printf("CustomCharts: Merged companion [%s] (rating %d) into [%s] Chaos (slot 5)\n",
                           extra->id, extra->charts[extra_slot].rating, base->id);
                    for (size_t k = i; k + 1 < song_count; ++k) {
                        songs[k] = songs[k + 1];
                    }
                    --song_count;
                    merged = true;
                    break;
                }
            }
        }
        if (!merged) ++i;
    }
#endif
    printf("CustomCharts: Scan complete (%zu valid songs, seriesCover=%d)\n", song_count, (int)s_has_series_cover);
    if (song_count > 0) {
        extern void *predecode_jackets_worker_entry(void *arg);
        pthread_t th;
        if (pthread_create(&th, NULL, predecode_jackets_worker_entry, NULL) == 0) {
            pthread_detach(th);
        }
    }
}

void runtime_custom_charts_configure(const char *game_dir, const char *user_dir) {
    if (user_dir && user_dir[0]) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", user_dir);
    }
    char candidate[512];
    snprintf(candidate, sizeof(candidate), "%s\\CustomCharts", user_dir);
    struct stat st;
    if (stat(candidate, &st) == 0 && (st.st_mode & S_IFDIR)) {
        scan_custom_charts_dir(candidate);
        return;
    }
    snprintf(candidate, sizeof(candidate), "CustomCharts");
    if (stat(candidate, &st) == 0 && (st.st_mode & S_IFDIR)) {
        scan_custom_charts_dir(candidate);
        return;
    }
    if (game_dir) {
        snprintf(candidate, sizeof(candidate), "%s\\CustomCharts", game_dir);
        if (stat(candidate, &st) == 0 && (st.st_mode & S_IFDIR)) {
            scan_custom_charts_dir(candidate);
            return;
        }
    }
    scan_custom_charts_dir("CustomCharts");
}

void *predecode_jackets_worker_entry(void *arg) {
    (void)arg;
    for (size_t i = 0; i < song_count; ++i) {
        CustomSong *song = &songs[i];
        if (!song->has_jacket || song->jacket_decoded) continue;
        char png_path[700];
        snprintf(png_path, sizeof(png_path), "%s\\%s", song->folder, song->jacket_file);
        FILE *f = fopen(png_path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 8 || size > 8388608) { fclose(f); continue; }
        uint8_t *data = (uint8_t *)malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
            if (data) free(data);
            fclose(f);
            continue;
        }
        fclose(f);

        int width = 0, height = 0, channels = 0;
        uint8_t *raw = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
        free(data);
        if (!raw || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            if (raw) stbi_image_free(raw);
            continue;
        }

        /* Flip vertically in-place (Unity Texture2D bottom-up row order) without touching global stbi flag */
        size_t row_bytes = (size_t)width * 4;
        uint8_t *tmp_row = (uint8_t *)malloc(row_bytes);
        if (tmp_row) {
            for (int y = 0; y < height / 2; ++y) {
                uint8_t *r0 = raw + (size_t)y * row_bytes;
                uint8_t *r1 = raw + (size_t)(height - 1 - y) * row_bytes;
                memcpy(tmp_row, r0, row_bytes);
                memcpy(r0, r1, row_bytes);
                memcpy(r1, tmp_row, row_bytes);
            }
            free(tmp_row);
        }

        /* Downscale oversized jackets (>512px) on the background thread so main-thread GPU upload is <1MB */
        while ((width > 512 || height > 512) && width >= 2 && height >= 2) {
            int nw = width / 2;
            int nh = height / 2;
            uint8_t *ds = (uint8_t *)malloc((size_t)nw * (size_t)nh * 4);
            if (!ds) break;
            for (int y = 0; y < nh; ++y) {
                const uint8_t *s0 = raw + (size_t)(2 * y) * (size_t)width * 4;
                const uint8_t *s1 = raw + (size_t)(2 * y + 1) * (size_t)width * 4;
                uint8_t *d = ds + (size_t)y * (size_t)nw * 4;
                for (int x = 0; x < nw; ++x) {
                    const uint8_t *p00 = s0 + 8 * x;
                    const uint8_t *p01 = p00 + 4;
                    const uint8_t *p10 = s1 + 8 * x;
                    const uint8_t *p11 = p10 + 4;
                    d[4 * x + 0] = (uint8_t)(((unsigned)p00[0] + p01[0] + p10[0] + p11[0] + 2u) >> 2);
                    d[4 * x + 1] = (uint8_t)(((unsigned)p00[1] + p01[1] + p10[1] + p11[1] + 2u) >> 2);
                    d[4 * x + 2] = (uint8_t)(((unsigned)p00[2] + p01[2] + p10[2] + p11[2] + 2u) >> 2);
                    d[4 * x + 3] = (uint8_t)(((unsigned)p00[3] + p01[3] + p10[3] + p11[3] + 2u) >> 2);
                }
            }
            stbi_image_free(raw);
            raw = ds;
            width = nw;
            height = nh;
        }

        song->decoded_jacket_rgba = raw;
        song->decoded_jacket_w = width;
        song->decoded_jacket_h = height;
        song->jacket_decoded = 1;
    }
    return NULL;
}

typedef void *(ABI *MusicIDFromIntFn)(uint32_t id, void *method);
typedef void *(ABI *SeriesIDFromIntFn)(uint32_t id, void *method);

static void ensure_series_managed_objects(void) {
    if (series_object && series_title) return;
    ensure_il2cpp_api();
    if (!series_object) {
        SeriesIDFromIntFn make_series_id = (SeriesIDFromIntFn)CODE(CC_SERIES_ID_IMPLICIT);
        series_object = make_series_id(custom_series_id, NULL);
        (void)gc_pin(series_object, false);
    }
    if (!series_title) {
        series_title = make_pinned_string("Custom Charts", NULL);
    }
}

static void ensure_song_managed_objects(CustomSong *song) {
    if (!song) return;
    ensure_il2cpp_api();
    if (!song->managed_id) {
        MusicIDFromIntFn make_music_id = (MusicIDFromIntFn)CODE(CC_MUSIC_ID_IMPLICIT);
        song->managed_id = make_music_id(song->music_id, NULL);
        song->root_id = gc_pin(song->managed_id, false);
    }
    if (!song->managed_title) {
        song->managed_title = make_pinned_string(song->title, &song->root_title);
    }
    if (!song->managed_sort_name) {
        char sort_key[256];
        make_sort_key(song->title, sort_key, sizeof(sort_key));
        song->managed_sort_name = make_pinned_string(sort_key, &song->root_sort);
    }
    if (!song->managed_label) {
        char label[32];
        snprintf(label, sizeof(label), "MUSIC%07u", song->music_id - 160000000u);
        song->managed_label = make_pinned_string(label, &song->root_label);
    }
    if (!song->managed_jacket_addr && song->has_jacket) {
        char key[256];
        snprintf(key, sizeof(key), "FBL.CustomCharts/%s/jacket", song->id);
        song->managed_jacket_addr = make_pinned_string(key, &song->root_jacket_addr);
    }
}

typedef struct {
    bool has_base;
    void *templates[2];
    void *tutorials[2];
} MusicTableScanCtx;

static void inspect_music_table_entry(void *key, void *value, void *ctx_ptr) {
    if (!key || !value) return;
    MusicTableScanCtx *ctx = (MusicTableScanCtx *)ctx_ptr;
    expand_int_array(value, 0x98); /* Expand stock Ratings int[] from 4 to 6 */
    uint32_t id = load32((char *)key + 0x10);
    int mode = (int)load32((char *)value + 0x68);
    if (id == 160000100) ctx->has_base = true;
    if (mode >= 0 && mode <= 1) {
        if (!ctx->templates[mode] || id == 160000100) ctx->templates[mode] = value;
    }
    if (id == 160000552) ctx->tutorials[1] = value; /* BMS tutorial */
    if (id == 160000553) ctx->tutorials[0] = value; /* FMS tutorial */
}

static void *original_music_convert = NULL;
static void *original_series_convert = NULL;
static void *original_sound_labels = NULL;

static ABI int wrap_music_table_convert(HookRegs *r) {
    ensure_il2cpp_api();
    typedef void *(ABI *ConvertFn)(void *self, void *pack, void *method);
    void *dict = ((ConvertFn)original_music_convert)((void *)r->rdi, (void *)r->rsi, (void *)r->rdx);
    if (!dict || song_count == 0) {
        r->rax = (uint64_t)dict;
        return 1;
    }

    uint32_t dict_root = gc_pin(dict, false);
    MusicTableScanCtx scan = {false, {NULL, NULL}, {NULL, NULL}};
    dict_each(dict, inspect_music_table_entry, &scan);

    /* DLC packs convert separately; register custom songs once on the base table */
    if (!scan.has_base || (!scan.templates[0] && !scan.templates[1])) {
        gc_unpin(dict_root);
        r->rax = (uint64_t)dict;
        return 1;
    }

    ensure_series_managed_objects();
    int added = 0;

    for (size_t i = 0; i < song_count; ++i) {
        CustomSong *song = &songs[i];
        void *tpl = scan.templates[song->mode];
        if (!tpl) continue;

        ensure_song_managed_objects(song);
        if (!song->managed_id || dict_contains_key(dict, song->managed_id)) {
            song->registered = 1;
            continue;
        }

        void *row = clone_object(tpl);
        if (!row) continue;
        uint32_t row_root = gc_pin(row, false);

        if (!song->template_id) {
            song->template_id = (void *)load64((char *)row + 0x10);
            song->root_template = gc_pin(song->template_id, false);
        }

        put64((char *)row + 0x10, (uint64_t)song->managed_id);
        put32((char *)row + 0x18, 2); /* PurchaseStatus = Available */
        put32((char *)row + 0x1c, 0);
        put32((char *)row + 0x40, (uint32_t)(9000000 + added)); /* SortOrder */
        if (series_object) {
            put64((char *)row + 0x48, (uint64_t)series_object);
            put64((char *)row + 0x50, (uint64_t)series_object);
        }
        *(bool *)((char *)row + 0x70) = false;

        /* Apply tutorial background fields to eliminate monster battle wave */
        void *tut = scan.tutorials[song->mode];
        if (tut) {
            put64((char *)row + 0x78, load64((char *)tut + 0x78));
            put32((char *)row + 0x80, load32((char *)tut + 0x80));
            put32((char *)row + 0x84, load32((char *)tut + 0x84));
            put64((char *)row + 0x88, load64((char *)tut + 0x88));
            put64((char *)row + 0x90, load64((char *)tut + 0x90));
            put64((char *)row + 0xf0, load64((char *)tut + 0xf0));
            put64((char *)row + 0xf8, load64((char *)tut + 0xf8));
            put32((char *)row + 0x108, load32((char *)tut + 0x108));
        }

        /* Ratings int[6] array at +0x98 */
        void *src_ratings = (void *)load64((char *)row + 0x98);
        void *ratings = src_ratings ? alloc_managed_array_by_arr_class(object_class(src_ratings), MAX_SLOTS)
                                    : alloc_managed_array_by_elem(s_int32_class, MAX_SLOTS);
        if (ratings) {
            for (int s = 0; s < MAX_SLOTS; ++s) {
                put32((char *)ratings + 0x20 + 4 * s, (uint32_t)song->charts[s].rating);
            }
            put64((char *)row + 0x98, (uint64_t)ratings);
        }

        *(bool *)((char *)row + 0xa0) = (song->charts[3].data != NULL); /* HasSupreme */
        float p_begin = (float)song->preview_begin_ms / 1000.0f;
        float p_end = (float)(song->preview_begin_ms + song->preview_length_ms) / 1000.0f;
        uint32_t pb_bits = 0, pe_bits = 0, dur_bits = 0;
        float dur = (float)song->duration_ms / 1000.0f;
        memcpy(&pb_bits, &p_begin, 4);
        memcpy(&pe_bits, &p_end, 4);
        memcpy(&dur_bits, &dur, 4);
        put32((char *)row + 0xa4, pb_bits);
        put32((char *)row + 0xa8, pe_bits);
        put64((char *)row + 0xb0, (uint64_t)song->managed_label);
        put32((char *)row + 0xc0, dur_bits);
        *(bool *)((char *)row + 0xc4) = true;
        *(bool *)((char *)row + 0xc5) = true;

        if (dict_add_safe(dict, song->managed_id, row)) {
            song->registered = 1;
            ++added;
        }
        gc_unpin(row_root);
    }

    printf("CustomCharts: Injected %d/%zu custom songs into base MusicTable\n", added, song_count);
    gc_unpin(dict_root);
    r->rax = (uint64_t)dict;
    return 1;
}

/* Table.SeriesTable::ConvertData (0x0b60660) */
typedef struct {
    void *sample_row;
    void *sample_key;
    bool occupied;
    int max_order;
} SeriesTableScanCtx;

static void inspect_series_entry(void *key, void *value, void *ctx_ptr) {
    if (!key || !value) return;
    SeriesTableScanCtx *ctx = (SeriesTableScanCtx *)ctx_ptr;
    uint32_t id = load32((char *)key + 0x10);
    if (id == 180000009) { /* Square Enix Titles (present only in base SeriesTable) */
        ctx->sample_row = value;
        ctx->sample_key = key;
    }
    if (id == custom_series_id) {
        ctx->occupied = true;
    } else {
        int order = (int)load32((char *)value + 0x18);
        if (order > ctx->max_order) ctx->max_order = order;
    }
}

static ABI int wrap_series_table_convert(HookRegs *r) {
    ensure_il2cpp_api();
    typedef void *(ABI *ConvertFn)(void *self, void *pack, void *method);
    void *dict = ((ConvertFn)original_series_convert)((void *)r->rdi, (void *)r->rsi, (void *)r->rdx);
    if (!dict || song_count == 0) {
        r->rax = (uint64_t)dict;
        return 1;
    }

    uint32_t dict_root = gc_pin(dict, false);
    SeriesTableScanCtx scan = {NULL, NULL, false, 0};
    dict_each(dict, inspect_series_entry, &scan);

    if (scan.sample_row && !scan.occupied) {
        ensure_series_managed_objects();
        if (!series_template && scan.sample_key) {
            series_template = scan.sample_key;
            (void)gc_pin(series_template, false);
        }
        void *row = clone_object(scan.sample_row);
        if (row && series_object) {
            uint32_t row_root = gc_pin(row, false);
            put64((char *)row + 0x10, (uint64_t)series_object);
            put64((char *)row + 0x20, (uint64_t)series_object);
            put32((char *)row + 0x18, (uint32_t)(scan.max_order + 1));
            *(bool *)((char *)row + 0x30) = false;
            *(bool *)((char *)row + 0x31) = false;
            put32((char *)row + 0x34, 0);
            put32((char *)row + 0x38, 0);
            put32((char *)row + 0x3c, 0);
            if (dict_add_safe(dict, series_object, row)) {
                printf("CustomCharts: Injected Series [Custom Charts] (id=%u order=%d cover=%d)\n",
                       custom_series_id, scan.max_order + 1, (int)s_has_series_cover);
            }
            gc_unpin(row_root);
        }
    }

    gc_unpin(dict_root);
    r->rax = (uint64_t)dict;
    return 1;
}

/* Table.SoundTable::GetLabelData (0x0b60ad0) */
static void inspect_sound_entry(void *key, void *value, void *ctx_ptr) {
    if (!key || !value) return;
    void **sample = (void **)ctx_ptr;
    char text[64];
    read_guest_string(key, text, sizeof(text));
    if (!strcmp(text, "MUSIC0000100")) {
        *sample = value;
    }
}

static ABI int wrap_sound_labels(HookRegs *r) {
    ensure_il2cpp_api();
    typedef void *(ABI *LabelsFn)(void *self, void *method);
    void *dict = ((LabelsFn)original_sound_labels)((void *)r->rdi, (void *)r->rsi);
    if (!dict || song_count == 0) {
        r->rax = (uint64_t)dict;
        return 1;
    }

    uint32_t dict_root = gc_pin(dict, false);
    void *sample = NULL;
    dict_each(dict, inspect_sound_entry, &sample);

    if (sample) {
        int added = 0;
        for (size_t i = 0; i < song_count; ++i) {
            CustomSong *song = &songs[i];
            ensure_song_managed_objects(song);
            void *info = clone_object(sample);
            if (info && song->managed_label) {
                uint32_t info_root = gc_pin(info, false);
                if (dict_add_safe(dict, song->managed_label, info)) {
                    ++added;
                }
                gc_unpin(info_root);
            }
        }
        printf("CustomCharts: Injected %d/%zu sound labels into SoundTable\n", added, song_count);
    }

    gc_unpin(dict_root);
    r->rax = (uint64_t)dict;
    return 1;
}

/* Native SoundLoader::GetAddress (0x1720ca0) is intentionally NOT hooked so it formats
 * /Music/MUSICxxxxxxx/music.wav vs /music_preview.wav, which match_asset_key distinguishes. */

/* -------------------------------------------------------------------------- */
/* NAME RESOLUTION, LOCALIZATION, & JACKET / SERIES SPRITE LOADERS            */
/* -------------------------------------------------------------------------- */

static ABI int hook_music_id_get_name(HookRegs *r) {
    CustomSong *s = find_song_by_obj((const void *)r->rdi);
    if (s) {
        ensure_song_managed_objects(s);
        if (s->managed_title) {
            r->rax = (uint64_t)s->managed_title;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_music_id_get_sort_name(HookRegs *r) {
    CustomSong *s = find_song_by_obj((const void *)r->rdi);
    if (s) {
        ensure_song_managed_objects(s);
        if (s->managed_sort_name) {
            r->rax = (uint64_t)s->managed_sort_name;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_series_id_get_name(HookRegs *r) {
    if (r->rdi && load32((const char *)r->rdi + 0x10) == custom_series_id) {
        ensure_series_managed_objects();
        if (series_title) {
            r->rax = (uint64_t)series_title;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_language_translation(HookRegs *r) {
    const void *term_str = (const void *)r->rsi;
    void **out_trans = (void **)r->rdx;
    if (!term_str || !out_trans) return 0;

    char term[128];
    read_guest_string(term_str, term, sizeof(term));

    if (!strcmp(term, "Table/Table_Series/SeriesName/SERIES9000001")) {
        ensure_series_managed_objects();
        if (series_title) {
            *out_trans = series_title;
            r->rax = 1;
            return 1;
        }
    }

    const char *prefix = "Table/MusicTable/MusicName/MUSIC";
    if (!strncmp(term, prefix, strlen(prefix))) {
        uint32_t num = 0;
        if (sscanf(term + strlen(prefix), "%u", &num) == 1) {
            CustomSong *s = find_song_by_id(160000000u + num);
            if (s) {
                ensure_song_managed_objects(s);
                if (s->managed_title) {
                    *out_trans = s->managed_title;
                    r->rax = 1;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void *s_str_custom_jacket_name = NULL;
static void *s_str_custom_series_addr = NULL;
static void *s_str_custom_series_name = NULL;

static ABI int hook_jacket_loader_get_address(HookRegs *r) {
    CustomSong *song = find_song_by_obj((const void *)r->rsi);
    if (!song) return 0;
    if (song->has_jacket) {
        ensure_song_managed_objects(song);
        if (song->managed_jacket_addr) {
            r->rax = (uint64_t)song->managed_jacket_addr;
            return 1;
        }
    }
    if (song->template_id) r->rsi = (uint64_t)song->template_id;
    return 0;
}

static ABI int hook_jacket_loader_get_filename(HookRegs *r) {
    CustomSong *song = find_song_by_obj((const void *)r->rsi);
    if (!song) return 0;
    if (song->has_jacket) {
        if (!s_str_custom_jacket_name) s_str_custom_jacket_name = make_pinned_string("FBL_CustomJacket", NULL);
        r->rax = (uint64_t)s_str_custom_jacket_name;
        return 1;
    }
    if (song->template_id) r->rsi = (uint64_t)song->template_id;
    return 0;
}

static ABI int hook_series_logo_get_address(HookRegs *r) {
    if (r->rsi && load32((const char *)r->rsi + 0x10) == custom_series_id) {
        if (s_has_series_cover) {
            if (!s_str_custom_series_addr) s_str_custom_series_addr = make_pinned_string("FBL.CustomCharts/series", NULL);
            r->rax = (uint64_t)s_str_custom_series_addr;
            return 1;
        }
        if (series_template) r->rsi = (uint64_t)series_template;
    }
    return 0;
}

static ABI int hook_series_logo_get_subasset(HookRegs *r) {
    if (r->rsi && load32((const char *)r->rsi + 0x10) == custom_series_id) {
        if (s_has_series_cover) {
            if (!s_str_custom_series_name) s_str_custom_series_name = make_pinned_string("FBL_CustomSeries", NULL);
            r->rax = (uint64_t)s_str_custom_series_name;
            return 1;
        }
        if (series_template) r->rsi = (uint64_t)series_template;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* ADDRESSABLE ASSET CACHE, TEXTASSET ICALL, & UNITY OBJECT LIFECYCLE         */
/* -------------------------------------------------------------------------- */

/* kind values:
 *   0..5 : chart TextAsset for slot 0..5
 *   -1   : full song AudioClip
 *   -2   : song jacket Sprite
 *   -3   : Custom Charts series cover Sprite
 *   -4   : song preview AudioClip
 */
#define MAX_CACHED_ASSETS 384
#define MAX_ACTIVE_OPERATIONS 256

typedef struct {
    bool used;
    CustomSong *song;
    int kind;
    void *object;
    void *texture;
    void *bytes;
    uint32_t obj_root;
    uint32_t tex_root;
    uint32_t bytes_root;
    int refs;
    uint64_t last_use;
} CachedCustomAsset;

typedef struct {
    bool active;
    void *op_object;
    void *assets_array;
    uint32_t op_root;
    uint32_t arr_root;
    CachedCustomAsset *asset;
} CustomOperation;

static CachedCustomAsset s_asset_cache[MAX_CACHED_ASSETS];
static CustomOperation operations[MAX_ACTIVE_OPERATIONS];
static uint64_t s_cache_clock = 0;

static bool unity_native_alive(const void *obj) {
    if (!obj || (uintptr_t)obj < 0x10000ULL || (uintptr_t)obj > 0x00007fffffffffffULL) return false;
    if (load64((const char *)obj + 0x10) == 0) return false;
    typedef bool (ABI *OpImplicitFn)(const void *obj, void *method);
    OpImplicitFn op_imp = (OpImplicitFn)CODE(CC_UOBJ_OP_IMPLICIT);
    return op_imp(obj, NULL);
}

static void protect_cached_unity_object(void *obj) {
    if (!unity_native_alive(obj)) return;
    /* HideFlags.DontUnloadUnusedAsset = 32 */
    typedef void (ABI *SetHideFlagsFn)(void *obj, int32_t flags, void *method);
    SetHideFlagsFn set_hf = (SetHideFlagsFn)CODE(CC_UOBJ_SET_HIDE_FLAGS);
    set_hf(obj, 32, NULL);
}

static void destroy_cached_asset(CachedCustomAsset *a) {
    if (!a || !a->used) return;
    typedef void (ABI *DestroyObjFn)(void *obj, void *method);
    DestroyObjFn destroy_fn = (DestroyObjFn)CODE(CC_UOBJ_DESTROY);
    /* TextAsset Internal_CreateInstance is managed/native peer; Destroy handles Sprite, Texture2D, AudioClip, TextAsset */
    if (unity_native_alive(a->object)) destroy_fn(a->object, NULL);
    if (unity_native_alive(a->texture)) destroy_fn(a->texture, NULL);
    gc_unpin(a->obj_root);
    gc_unpin(a->tex_root);
    gc_unpin(a->bytes_root);
    memset(a, 0, sizeof(*a));
}

static void evict_idle_assets(CachedCustomAsset *keep) {
    /* Bound idle full AudioClips to 4, previews to 32, visuals to 256 (all jackets!), charts to 64 */
    const int kinds_limits[4][2] = {
        {-1, 4},   /* full audio */
        {-4, 32},  /* preview audio */
        {-2, 256}, /* jackets/series cover */
        {0,  64}   /* charts (0..5) */
    };
    for (int g = 0; g < 4; ++g) {
        for (;;) {
            int count = 0;
            CachedCustomAsset *oldest = NULL;
            for (size_t i = 0; i < MAX_CACHED_ASSETS; ++i) {
                CachedCustomAsset *a = &s_asset_cache[i];
                if (!a->used || a->refs > 0) continue;
                bool match = false;
                if (g == 0) match = (a->kind == -1);
                else if (g == 1) match = (a->kind == -4);
                else if (g == 2) match = (a->kind == -2 || a->kind == -3);
                else match = (a->kind >= 0);
                if (!match) continue;
                ++count;
                if (a != keep && (!oldest || a->last_use < oldest->last_use)) {
                    oldest = a;
                }
            }
            if (count <= kinds_limits[g][1] || !oldest) break;
            destroy_cached_asset(oldest);
        }
    }
}

static CachedCustomAsset *find_cached_text_asset(const void *obj) {
    if (!obj) return NULL;
    for (size_t i = 0; i < MAX_CACHED_ASSETS; ++i) {
        if (s_asset_cache[i].used && s_asset_cache[i].kind >= 0 && s_asset_cache[i].object == obj) {
            return &s_asset_cache[i];
        }
    }
    return NULL;
}

static int find_chart_slot_by_bytes(const void *bytes_arr) {
    if (!bytes_arr) return -1;
    for (size_t i = 0; i < MAX_CACHED_ASSETS; ++i) {
        if (s_asset_cache[i].used && s_asset_cache[i].kind >= 0 && s_asset_cache[i].bytes == bytes_arr) {
            return s_asset_cache[i].kind;
        }
    }
    return -1;
}

/* BeginMainLoop inlines UnityEngine.TextAsset::get_bytes via an icall pointer slot resolved from TextAsset::get_bytes! */
typedef void *(ABI *NativeTextBytesFn)(void *text_asset);
static NativeTextBytesFn s_orig_native_text_bytes = NULL;
static NativeTextBytesFn *s_text_asset_bytes_slot = NULL;

static ABI void *stage_native_text_bytes(void *text_asset) {
    CachedCustomAsset *a = find_cached_text_asset(text_asset);
    if (a && a->bytes) {
        return a->bytes;
    }
    return s_orig_native_text_bytes ? s_orig_native_text_bytes(text_asset) : NULL;
}

static void ensure_native_text_reader(void) {
    if (!s_text_asset_bytes_slot) return;
    ensure_il2cpp_api();
    NativeTextBytesFn *slot = s_text_asset_bytes_slot;
    if (*slot == stage_native_text_bytes) return;
    NativeTextBytesFn orig = *slot;
    if (!orig && il2cpp_resolve_icall_fn) {
        orig = (NativeTextBytesFn)il2cpp_resolve_icall_fn("UnityEngine.TextAsset::get_bytes");
        if (!orig) orig = (NativeTextBytesFn)il2cpp_resolve_icall_fn("UnityEngine.TextAsset::get_bytes()");
    }
    if (orig) {
        s_orig_native_text_bytes = orig;
#ifdef _WIN32
        DWORD old_prot = 0;
        if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_prot)) {
            *slot = stage_native_text_bytes;
            VirtualProtect(slot, sizeof(void *), old_prot, &old_prot);
        } else {
            *slot = stage_native_text_bytes;
        }
#else
        *slot = stage_native_text_bytes;
#endif
    }
}

static CustomSong *match_asset_key(const char *key, int *kind) {
    if (!strncmp(key, "FBL.CustomCharts/", 17)) {
        if (!strcmp(key, "FBL.CustomCharts/series") && s_has_series_cover) {
            *kind = -3;
            return song_count ? &songs[0] : NULL;
        }
        const char *rest = key + 17;
        const char *slash = strchr(rest, '/');
        if (!slash) return NULL;
        size_t id_len = (size_t)(slash - rest);
        const char *sub = slash + 1;
        for (size_t i = 0; i < song_count; ++i) {
            if (strncmp(songs[i].id, rest, id_len) == 0 && songs[i].id[id_len] == '\0') {
                if (!strcmp(sub, "jacket")) {
                    *kind = -2;
                    return &songs[i];
                }
                if (!strcmp(sub, "audio")) {
                    *kind = -1;
                    return &songs[i];
                }
                if (!strncmp(sub, "chart/", 6) && sub[6] >= '0' && sub[6] < '0' + MAX_SLOTS && sub[7] == '\0') {
                    *kind = sub[6] - '0';
                    return &songs[i];
                }
                return NULL;
            }
        }
        return NULL;
    }

    const char *pos = strstr(key, "/Music/MUSIC");
    if (!pos || strlen(pos) < 19) return NULL;
    uint32_t n = 0;
    for (size_t i = 12; i < 19; ++i) {
        if (pos[i] < '0' || pos[i] > '9') return NULL;
        n = n * 10 + (uint32_t)(pos[i] - '0');
    }
    CustomSong *song = find_song_by_id(160000000u + n);
    if (!song) return NULL;

    const char *suffix = pos + 19;
    if (!strcmp(suffix, "/music.wav")) *kind = -1;
    else if (!strcmp(suffix, "/music_preview.wav")) *kind = -4;
    else if (strlen(suffix) == 18 && !strncmp(suffix, "/trigger0", 9) &&
             suffix[9] >= '1' && suffix[9] <= '6' && !strcmp(suffix + 10, "01.bytes")) {
        *kind = suffix[9] - '1';
    } else {
        return NULL;
    }

    return song;
}

static void *create_custom_audio_clip(CustomSong *song, bool preview) {
    ensure_il2cpp_api();
    ensure_song_managed_objects(song);
    if (!s_single_class) return NULL;

    typedef void *(ABI *CreateClipFn)(void *name, int lengthSamples, int channels, int freq, bool stream, void *method);
    typedef bool (ABI *SetDataFn)(void *self, void *data, int offset, void *method);
    CreateClipFn create_clip = (CreateClipFn)CODE(CC_AUDIO_CLIP_CREATE);
    SetDataFn set_data = (SetDataFn)CODE(CC_AUDIO_CLIP_SET_DATA);

    char wav_path[700];
    snprintf(wav_path, sizeof(wav_path), "%s\\%s", song->folder, song->audio_file);
    FILE *f = fopen(wav_path, "rb");
    if (!f) return NULL;

    int total_frames = (int)(song->pcm_bytes / 4);
    if (total_frames <= 0) { fclose(f); return NULL; }
    int start_frame = 0;
    int frame_count = total_frames;

    if (preview) {
        int64_t p_start = (int64_t)song->preview_begin_ms * 48;
        int64_t p_len = (int64_t)(song->preview_length_ms > 0 ? song->preview_length_ms : 15000) * 48;
        if (p_start < 0) p_start = 0;
        if (p_start >= total_frames) p_start = 0;
        start_frame = (int)p_start;
        frame_count = (start_frame + p_len <= total_frames) ? (int)p_len : (total_frames - start_frame);
        if (frame_count <= 0) frame_count = total_frames;
    }

    void *clip = create_clip(song->managed_title, frame_count, 2, 48000, false, NULL);
    if (!clip) { fclose(f); return NULL; }
    uint32_t clip_pin = gc_pin(clip, false);

    const int block_frames = 524288;
    static void *s_staging_floats = NULL;
    static int16_t *s_staging_pcm = NULL;
    if (!s_staging_floats) {
        s_staging_floats = alloc_managed_array_by_elem(s_single_class, (size_t)block_frames * 2);
        if (s_staging_floats) (void)gc_pin(s_staging_floats, true);
    }
    if (!s_staging_pcm) {
        s_staging_pcm = (int16_t *)malloc((size_t)block_frames * 4);
    }
    if (!s_staging_floats || !s_staging_pcm) {
        gc_unpin(clip_pin);
        fclose(f);
        return NULL;
    }

    fseek(f, (long)(song->pcm_offset + (uint64_t)start_frame * 4), SEEK_SET);
    bool ok = true;
    const float inv_scale = 1.0f / 32768.0f;
    for (int offset = 0; offset < frame_count; offset += block_frames) {
        int count = frame_count - offset;
        if (count > block_frames) count = block_frames;
        if (fread(s_staging_pcm, 4, (size_t)count, f) != (size_t)count) {
            ok = false;
            break;
        }
        /* Temporarily set managed Single[] length to exact sample count so AudioClip.SetData never wraps */
        put64((char *)s_staging_floats + 0x18, (uint64_t)((size_t)count * 2));
        float *dst = (float *)((char *)s_staging_floats + 0x20);
        int n_samples = count * 2;
        for (int i = 0; i < n_samples; ++i) {
            dst[i] = (float)s_staging_pcm[i] * inv_scale;
        }
        bool wrote = set_data(clip, s_staging_floats, offset, NULL);
        put64((char *)s_staging_floats + 0x18, (uint64_t)((size_t)block_frames * 2));
        if (!wrote) { ok = false; break; }
    }

    fclose(f);

    if (!ok) {
        if (unity_native_alive(clip)) {
            typedef void (ABI *DestroyObjFn)(void *obj, void *method);
            ((DestroyObjFn)CODE(CC_UOBJ_DESTROY))(clip, NULL);
        }
        gc_unpin(clip_pin);
        return NULL;
    }
    gc_unpin(clip_pin);
    return clip;
}

static void *create_custom_sprite(CustomSong *song, int kind, void **out_tex) {
    ensure_il2cpp_api();
    if (!s_texture2d_class) return NULL;
    if (out_tex) *out_tex = NULL;

    uint8_t *pixels = NULL;
    int width = 0, height = 0;
    bool free_pixels = false;

    if (kind != -3 && song && song->jacket_decoded == 1 && song->decoded_jacket_rgba &&
        song->decoded_jacket_w > 0 && song->decoded_jacket_h > 0) {
        pixels = song->decoded_jacket_rgba;
        width = song->decoded_jacket_w;
        height = song->decoded_jacket_h;
        free_pixels = false;
    } else {
        char png_path[700];
        if (kind == -3) {
            if (!s_has_series_cover) return NULL;
            snprintf(png_path, sizeof(png_path), "%s", s_series_cover_path);
        } else {
            if (!song || !song->has_jacket) return NULL;
            snprintf(png_path, sizeof(png_path), "%s\\%s", song->folder, song->jacket_file);
        }

        FILE *f = fopen(png_path, "rb");
        if (!f) return NULL;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size < 8 || size > 8388608) { fclose(f); return NULL; }

        uint8_t *data = (uint8_t *)malloc((size_t)size);
        if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
            if (data) free(data);
            fclose(f);
            return NULL;
        }
        fclose(f);

        int channels = 0;
        stbi_set_flip_vertically_on_load(1);
        pixels = stbi_load_from_memory(data, (int)size, &width, &height, &channels, 4);
        free(data);
        if (!pixels || width <= 0 || height <= 0 || width > 4096 || height > 4096) {
            if (pixels) stbi_image_free(pixels);
            return NULL;
        }
        free_pixels = true;
    }

    typedef struct { float x, y; } SprVec2;
    typedef struct { float x, y, w, h; } SprRect;
    typedef struct { float x, y, z, w; } SprVec4;
    typedef void (ABI *TexCtorFn2)(void *self, int32_t w, int32_t h, int32_t fmt, bool mip, void *method);
    typedef void (ABI *TexSetIntFn2)(void *self, int32_t mode, void *method);
    typedef void (ABI *LoadRawFn2)(void *self, const void *data, int32_t size, void *method);
    typedef void (ABI *ApplyFn2)(void *self, bool updateMipmaps, bool makeNoLongerReadable, void *method);
    typedef void *(ABI *SprCreateFullFn2)(void *tex, SprRect rect, SprVec2 pivot, float ppu,
                                          uint32_t extrude, int32_t meshType, SprVec4 border,
                                          bool genFallback, void *method);
    typedef void (ABI *SetNameFn2)(void *obj, void *str, void *method);

    void *tex = alloc_managed_object(s_texture2d_class);
    if (!tex) { if (free_pixels) stbi_image_free(pixels); return NULL; }
    uint32_t tex_pin = gc_pin(tex, false);

    ((TexCtorFn2)CODE(CC_TEX2D_CTOR))(tex, width, height, 4 /* RGBA32 */, false, NULL);
    ((TexSetIntFn2)CODE(CC_TEX_SET_FILTER))(tex, 1 /* Bilinear */, NULL);
    ((TexSetIntFn2)CODE(CC_TEX_SET_WRAP))(tex, 1 /* Clamp */, NULL);
    ((LoadRawFn2)CODE(CC_TEX2D_LOAD_RAW))(tex, pixels, width * height * 4, NULL);
    ((ApplyFn2)CODE(CC_TEX2D_APPLY))(tex, false, true, NULL);
    if (free_pixels) stbi_image_free(pixels);

    void *sprite = ((SprCreateFullFn2)CODE(CC_SPRITE_CREATE))(
        tex,
        (SprRect){0.0f, 0.0f, (float)width, (float)height},
        (SprVec2){0.5f, 0.5f},
        100.0f, 0, 0,
        (SprVec4){0.0f, 0.0f, 0.0f, 0.0f},
        false, NULL);

    if (!sprite) {
        if (unity_native_alive(tex)) {
            typedef void (ABI *DestroyObjFn)(void *obj, void *method);
            ((DestroyObjFn)CODE(CC_UOBJ_DESTROY))(tex, NULL);
        }
        gc_unpin(tex_pin);
        return NULL;
    }

    uint32_t spr_pin = gc_pin(sprite, false);
    if (!s_str_custom_jacket_name) s_str_custom_jacket_name = make_pinned_string("FBL_CustomJacket", NULL);
    if (!s_str_custom_series_name) s_str_custom_series_name = make_pinned_string("FBL_CustomSeries", NULL);
    ((SetNameFn2)CODE(CC_UOBJ_SET_NAME))(sprite, kind == -3 ? s_str_custom_series_name : s_str_custom_jacket_name, NULL);
    if (out_tex) *out_tex = tex;
    gc_unpin(spr_pin);
    gc_unpin(tex_pin);
    return sprite;
}

static void *create_custom_text_asset(CustomSong *song, int slot, void **out_bytes) {
    ensure_il2cpp_api();
    if (!song || slot < 0 || slot >= MAX_SLOTS || !song->charts[slot].data || !s_byte_class || !s_textasset_class) {
        return NULL;
    }
    if (out_bytes) *out_bytes = NULL;

    size_t size = song->charts[slot].size;
    void *bytes_arr = alloc_managed_array_by_elem(s_byte_class, size);
    if (!bytes_arr) return NULL;
    uint32_t bytes_pin = gc_pin(bytes_arr, true);
    memcpy((char *)bytes_arr + 0x20, song->charts[slot].data, size);

    void *ta = alloc_managed_object(s_textasset_class);
    if (!ta) {
        gc_unpin(bytes_pin);
        return NULL;
    }
    uint32_t ta_pin = gc_pin(ta, false);

    /* Initialize native Unity TextAsset peer so m_CachedPtr (+0x10) is valid */
    typedef void (ABI *InternalCreateFn)(void *self, void *str, void *method);
    ((InternalCreateFn)CODE(CC_TEXT_ASSET_INTERNAL_CREATE))(ta, make_string(NULL, ""), NULL);
    ensure_native_text_reader();

    if (out_bytes) *out_bytes = bytes_arr;
    gc_unpin(ta_pin);
    gc_unpin(bytes_pin);
    return ta;
}

static CachedCustomAsset *load_or_get_custom_asset(CustomSong *song, int kind) {
    for (size_t i = 0; i < MAX_CACHED_ASSETS; ++i) {
        CachedCustomAsset *a = &s_asset_cache[i];
        if (!a->used) continue;
        bool same = (kind == -3) ? (a->kind == -3) : (a->song == song && a->kind == kind);
        if (!same) continue;
        if (!unity_native_alive(a->object) || (a->texture && !unity_native_alive(a->texture))) {
            if (a->refs > 0) return NULL;
            destroy_cached_asset(a);
            break;
        }
        a->last_use = ++s_cache_clock;
        return a;
    }

    /* Find free cache slot (evicting oldest idle if completely full) */
    evict_idle_assets(NULL);
    CachedCustomAsset *slot_ptr = NULL;
    CachedCustomAsset *oldest_idle = NULL;
    for (size_t i = 0; i < MAX_CACHED_ASSETS; ++i) {
        if (!s_asset_cache[i].used) {
            slot_ptr = &s_asset_cache[i];
            break;
        }
        if (s_asset_cache[i].refs == 0 && (!oldest_idle || s_asset_cache[i].last_use < oldest_idle->last_use)) {
            oldest_idle = &s_asset_cache[i];
        }
    }
    if (!slot_ptr && oldest_idle) {
        destroy_cached_asset(oldest_idle);
        slot_ptr = oldest_idle;
    }
    if (!slot_ptr) return NULL;

    void *obj = NULL, *tex = NULL, *bytes = NULL;
    if (kind == -1) obj = create_custom_audio_clip(song, false);
    else if (kind == -4) obj = create_custom_audio_clip(song, true);
    else if (kind == -2 || kind == -3) obj = create_custom_sprite(song, kind, &tex);
    else if (kind >= 0 && kind < MAX_SLOTS) obj = create_custom_text_asset(song, kind, &bytes);

    if (!obj) return NULL;

    slot_ptr->used = true;
    slot_ptr->song = song;
    slot_ptr->kind = kind;
    slot_ptr->object = obj;
    slot_ptr->texture = tex;
    slot_ptr->bytes = bytes;
    slot_ptr->obj_root = gc_pin(obj, false);
    slot_ptr->tex_root = gc_pin(tex, false);
    slot_ptr->bytes_root = gc_pin(bytes, true);
    slot_ptr->refs = 0;
    slot_ptr->last_use = ++s_cache_clock;

    protect_cached_unity_object(obj);
    protect_cached_unity_object(tex);
    return slot_ptr;
}

static void *create_operation(void *key_str, void *callback, CustomSong *song, int kind) {
    ensure_il2cpp_api();
    if (!s_addr_async_op_class) return NULL;

    /* Find free operation slot */
    CustomOperation *op_slot = NULL;
    for (size_t i = 0; i < MAX_ACTIVE_OPERATIONS; ++i) {
        if (!operations[i].active) {
            op_slot = &operations[i];
            break;
        }
    }
    if (!op_slot) return NULL;

    CachedCustomAsset *asset = load_or_get_custom_asset(song, kind);
    if (!asset || !asset->object) return NULL;

    if (kind == -4) {
        /* User is previewing this custom song in Music Select.
         * Pre-cache full audio clip in memory now so stage loading has instant 0ms access. */
        (void)load_or_get_custom_asset(song, -1);
    }

    void *array = s_object_array_class ? alloc_managed_array_by_arr_class(s_object_array_class, 1)
                                       : alloc_managed_array_by_elem(s_uobject_class, 1);
    if (!array) return NULL;
    uint32_t arr_root = gc_pin(array, false);
    put64((char *)array + 0x20, (uint64_t)asset->object);

    void *op = alloc_managed_object(s_addr_async_op_class);
    if (!op) {
        gc_unpin(arr_root);
        return NULL;
    }
    uint32_t op_root = gc_pin(op, false);

    /* FileAsyncOperationBase fields: _key (+0x10), _completedCallback (+0x18).
     * Never invoke callback synchronously here; FileAsyncOperationBase::Update
     * checks IsDone() and invokes [op + 0x18] with (target, key, op, method)! */
    put64((char *)op + 0x10, (uint64_t)key_str);
    put64((char *)op + 0x18, (uint64_t)callback);

    op_slot->active = true;
    op_slot->op_object = op;
    op_slot->assets_array = array;
    op_slot->op_root = op_root;
    op_slot->arr_root = arr_root;
    op_slot->asset = asset;
    ++asset->refs;
    return op;
}

static ABI int hook_load_asset_sync(HookRegs *r) {
    const void *key_str = (const void *)r->rsi;
    if (!key_str) return 0;
    char key[256];
    read_guest_string(key_str, key, sizeof(key));
    int kind = 0;
    CustomSong *song = match_asset_key(key, &kind);
    if (song) {
        void *op = create_operation((void *)key_str, NULL, song, kind);
        if (op) {
            r->rax = (uint64_t)op;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_load_asset_async(HookRegs *r) {
    const void *key_str = (const void *)r->rsi;
    void *callback = (void *)r->rdx;
    if (!key_str) return 0;
    char key[256];
    read_guest_string(key_str, key, sizeof(key));
    int kind = 0;
    CustomSong *song = match_asset_key(key, &kind);
    if (song) {
        void *op = create_operation((void *)key_str, callback, song, kind);
        if (op) {
            r->rax = (uint64_t)op;
            return 1;
        }
    }
    return 0;
}

static void *original_get_assets = NULL;

static ABI int hook_get_assets(HookRegs *r) {
    void *op = (void *)r->rdi;
    for (size_t i = 0; i < MAX_ACTIVE_OPERATIONS; ++i) {
        if (operations[i].active && operations[i].op_object == op) {
            r->rax = (uint64_t)operations[i].assets_array;
            return 1;
        }
    }
    if (original_get_assets) {
        typedef void *(ABI *GetAssetsFn)(void *op, void *method);
        void *res = ((GetAssetsFn)original_get_assets)(op, (void *)r->rsi);
        if (res && !s_object_array_class) {
            s_object_array_class = object_class(res);
        }
        r->rax = (uint64_t)res;
        return 1;
    }
    return 0;
}

static ABI int hook_is_done(HookRegs *r) {
    void *op = (void *)r->rdi;
    for (size_t i = 0; i < MAX_ACTIVE_OPERATIONS; ++i) {
        if (operations[i].active && operations[i].op_object == op) {
            r->rax = 1;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_progress(HookRegs *r) {
    void *op = (void *)r->rdi;
    for (size_t i = 0; i < MAX_ACTIVE_OPERATIONS; ++i) {
        if (operations[i].active && operations[i].op_object == op) {
            set_ret_float(r, 1.0f);
            r->rax = 0;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_release_asset(HookRegs *r) {
    void *op = (void *)r->rsi;
    if (!op) return 0;
    for (size_t i = 0; i < MAX_ACTIVE_OPERATIONS; ++i) {
        if (operations[i].active && operations[i].op_object == op) {
            CachedCustomAsset *asset = operations[i].asset;
            gc_unpin(operations[i].op_root);
            gc_unpin(operations[i].arr_root);
            memset(&operations[i], 0, sizeof(operations[i]));
            if (asset && asset->refs > 0) {
                --asset->refs;
                if (asset->refs == 0) {
                    evict_idle_assets(asset);
                }
            }
            r->rax = 0;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_text_asset_get_bytes(HookRegs *r) {
    CachedCustomAsset *a = find_cached_text_asset((const void *)r->rdi);
    if (a && a->bytes) {
        r->rax = (uint64_t)a->bytes;
        return 1;
    }
    return 0;
}

static ABI int hook_text_asset_get_data_ptr(HookRegs *r) {
    CachedCustomAsset *a = find_cached_text_asset((const void *)r->rdi);
    if (a && a->bytes) {
        r->rax = (uint64_t)((char *)a->bytes + 0x20);
        return 1;
    }
    return 0;
}

static ABI int hook_text_asset_get_data_size(HookRegs *r) {
    CachedCustomAsset *a = find_cached_text_asset((const void *)r->rdi);
    if (a && a->bytes) {
        r->rax = load64((const char *)a->bytes + 0x18);
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* TUTORIAL / ANTI-STUTTER / PRACTICE FIXES                                   */
/* -------------------------------------------------------------------------- */

static int s_pending_chart_slot = -1;
static int s_gameplay_chart_slot = -1;

static ABI int hook_theater_begin_start(HookRegs *r) {
    void *enumerator = (void *)r->rdi;
    if (!enumerator) return 0;
    void *theater = (void *)load64((char *)enumerator + 0x20);
    if (theater) {
        void *setting = (void *)load64((char *)theater + 0x98);
        if (setting) {
            void *mid = (void *)load64((char *)setting + 0x18);
            if (find_song_by_obj(mid)) {
                /* Custom stage: ensure camera has field movement active, then complete immediately */
                void *camera = (void *)load64((char *)theater + 0x190);
                if (camera) {
                    *(uint8_t *)((char *)camera + 0xc8) = 1;
                }
                put32((char *)enumerator + 0x10, (uint32_t)-1);
                put64((char *)enumerator + 0x18, 0);
                r->rax = 0;
                return 1;
            }
        }
    }
    return 0;
}

static bool s_custom_stage_active = false;
static void *s_current_play_setting = NULL;

int runtime_custom_charts_get_results_page(void) {
    return runtime_tffscores_get_results_page();
}

static int check_is_custom_id(uint32_t music_id) {
    return runtime_custom_charts_is_custom_id(music_id) ? 1 : 0;
}

static void on_stage_begin_callback(void *enumerator, int before, bool pending) {
    if (!enumerator) return;
    void *theater = (void *)load64((char *)enumerator + 0x20);
    if (!theater) return;

    void *setting = (void *)load64((char *)theater + 0x98);
    CustomSong *song = NULL;
    if (setting) {
        s_current_play_setting = setting;
        void *mid = (void *)load64((char *)setting + 0x18);
        song = find_song_by_obj(mid);
        if (before == 0) {
            uint32_t music_id = mid ? load32((char *)mid + 0x10) : 0;
            int difficulty = (int)load32((char *)setting + 0x50);
            if (s_gameplay_chart_slot >= 4) difficulty = s_gameplay_chart_slot;
            else if (s_pending_chart_slot >= 4) difficulty = s_pending_chart_slot;
            if (difficulty >= 4) {
                runtime_judgements_begin_stage(music_id, difficulty);
                runtime_tffscores_notify_stage_start(music_id, difficulty);
            }
            s_custom_stage_active = (song != NULL);
        }
    }

    if (song && !pending && before >= 0 && (int)(int32_t)load32((char *)theater + 0xc4) == 2) {
        int mode = (int)load32((char *)theater + 0xd0);
        void *camera = (void *)load64((char *)theater + 0x190);
        if (camera) {
            typedef void (ABI *CamFn)(void *cam, void *method);
            CamFn set_cam = (CamFn)(mode == 0 ? CODE(CC_CAM_SET_FMS_PRACTICE) : CODE(CC_CAM_SET_BMS_PRACTICE));
            set_cam(camera, NULL);
        }
        *(bool *)((char *)theater + 0x211) = false;
    }
}

static ABI int hook_theater_finish(HookRegs *r) {
    void *theater = (void *)r->rdi;
    if (theater) {
        void *setting = (void *)load64((char *)theater + 0x98);
        if (setting) {
            void *mid = (void *)load64((char *)setting + 0x18);
            if (find_song_by_obj(mid)) {
                s_custom_stage_active = false;
                s_custom_records_dirty = true;
                *(bool *)((char *)theater + 0x211) = true;
                r->rax = 0;
                return 1;
            }
        }
    }
    s_custom_stage_active = false;
    return 0;
}

static ABI int hook_support_start(HookRegs *r) {
    if (s_custom_stage_active && r->rdi) {
        *(uint8_t *)((char *)r->rdi + 0x40) = 0;
        r->rax = 0;
        return 1;
    }
    return 0;
}

static int on_input_damage(void *main_loop, void *trigger_data, void *attacked_out, float *damage_out) {
    (void)trigger_data;
    if (main_loop) {
        void *theater = (void *)load64((char *)main_loop + 0xf0);
        if (theater) {
            void *setting = (void *)load64((char *)theater + 0x98);
            if (setting) {
                void *mid = (void *)load64((char *)setting + 0x18);
                if (find_song_by_obj(mid)) {
                    if (attacked_out) *(bool *)attacked_out = false;
                    if (damage_out) *damage_out = 0.0f;
                    return 1;
                }
            }
        }
    }
    return 0;
}

static void on_play_setting_config(void *setting, void *config_info) {
    (void)config_info;
    if (setting) {
        s_current_play_setting = setting;
        void *mid = (void *)load64((char *)setting + 0x18);
        if (find_song_by_obj(mid)) {
            for (size_t off = 0xf6; off <= 0xfc; ++off) {
                *(bool *)((char *)setting + off) = false;
            }
        }
    }
}

typedef struct { float x, y; } UGuiVec2;
typedef struct { float x, y, z; } UGuiVec3;
typedef struct { float x, y, z, w; } UGuiVec4;
typedef struct { float x, y, w, h; } UGuiRect;

typedef void *(ABI *GetGOPtrFn)(void *comp, void *method);
typedef void *(ABI *GetTransformFn)(void *obj, void *method);
typedef void (ABI *SetActiveFn)(void *go, bool act, void *method);
typedef void (ABI *SetEnabledFn)(void *beh, bool en, void *method);
typedef void *(ABI *GetParentFn)(void *t, void *method);
typedef int32_t (ABI *GetChildCountFn)(void *t, void *method);
typedef void *(ABI *GetChildFn)(void *t, int32_t idx, void *method);
typedef void (ABI *SetLocalScaleFn)(void *t, UGuiVec3 v, void *method);
typedef void *(ABI *GetObjNameFn)(void *obj, void *method);
typedef void (ABI *SetObjNameFn)(void *obj, void *str, void *method);
typedef void *(ABI *CloneWithParentFn)(void *obj, void *parent_t, int world_stays);
typedef void (ABI *SetVec2Fn)(void *rt, UGuiVec2 v, void *method);
typedef void (ABI *SetColorFn)(void *graphic, UGuiVec4 c, void *method);
typedef void *(ABI *GetSpriteFn)(void *img, void *method);
typedef void (ABI *SetSpriteFn)(void *img, void *spr, void *method);
typedef void *(ABI *SpriteCreateFullFn)(void *tex, UGuiRect rect, UGuiVec2 pivot, float ppu,
                                        uint32_t extrude, int32_t meshType, UGuiVec4 border,
                                        bool genFallback, void *method);
typedef void (ABI *TexCtorFn)(void *self, int32_t w, int32_t h, int32_t fmt, bool mip, void *method);
typedef void (ABI *TexSetIntFn)(void *self, int32_t mode, void *method);
typedef void (ABI *TexLoadRawFn)(void *self, const void *data, int32_t size, void *method);
typedef void (ABI *TexApplyFn)(void *self, bool updateMip, bool makeUnreadable, void *method);

static bool uobj_alive(const void *obj) {
    if (!obj) return false;
    uintptr_t p = (uintptr_t)obj;
    if (p < 0x10000ULL || p > 0x00007fffffffffffULL) return false;
    return load64((const char *)obj + 0x10) != 0;
}

/* -------------------------------------------------------------------------- */
/* 6-DIFFICULTY, SLOT NORMALIZATION, MENU, JACKET, & POPUP HOOKS              */
/* -------------------------------------------------------------------------- */

/* Presentation order: Beginner(4), Basic(0), Expert(1), Ultimate(2), Supreme(3), Chaos(5) */
static const int s_difficulty_order[6] = {4, 0, 1, 2, 3, 5};
static int s_global_requested_slot = -1;

static int song_has_chart(const CustomSong *song, int slot) {
    if (!song || slot < 0 || slot >= MAX_SLOTS) return 0;
    return song->charts[slot].data != NULL;
}

static int count_song_charts(const CustomSong *song) {
    if (!song) return 0;
    int c = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (song_has_chart(song, i)) ++c;
    }
    return c;
}

static int clamp_extra_slot(int slot) {
    if (slot == 4 || slot < 0) return 0;
    if (slot == 5 || slot > 3) return 3;
    return slot;
}

static int resolve_song_slot(const CustomSong *song, int requested, int prev) {
    if (!song) return clamp_extra_slot(requested);
    if (song_has_chart(song, requested)) return requested;
    if (song_has_chart(song, prev)) return prev;

    int req_idx = 1; /* Default near Basic */
    for (int i = 0; i < 6; ++i) {
        if (s_difficulty_order[i] == requested) {
            req_idx = i;
            break;
        }
    }
    for (int d = 0; d < 6; ++d) {
        if (req_idx - d >= 0 && song_has_chart(song, s_difficulty_order[req_idx - d])) {
            return s_difficulty_order[req_idx - d];
        }
        if (req_idx + d < 6 && song_has_chart(song, s_difficulty_order[req_idx + d])) {
            return s_difficulty_order[req_idx + d];
        }
    }
    return 0;
}

static int resolve_slot_for_id(const void *mid, int slot) {
    CustomSong *song = find_song_by_obj(mid);
    if (song) return resolve_song_slot(song, slot, song->last_selected_slot);
    return clamp_extra_slot(slot);
}

static int step_difficulty(const CustomSong *song, int current_slot, int change_type) {
    if (!song) return current_slot;
    if (!song_has_chart(song, current_slot)) {
        current_slot = resolve_song_slot(song, current_slot, song->last_selected_slot);
    }
    int dir = (change_type == 0) ? 1 : -1;
    int cur_idx = 1;
    for (int i = 0; i < 6; ++i) {
        if (s_difficulty_order[i] == current_slot) {
            cur_idx = i;
            break;
        }
    }
    for (int next_idx = cur_idx + dir; next_idx >= 0 && next_idx < 6; next_idx += dir) {
        int slot = s_difficulty_order[next_idx];
        if (song_has_chart(song, slot)) {
            return slot;
        }
    }
    return current_slot;
}

/* -------------------------------------------------------------------------- */
/* CUSTOM BEGINNER (4) & CHAOS (5) SPRITES, MATERIALS, BADGE, & UI LAYOUT     */
/* -------------------------------------------------------------------------- */

static UGuiVec4 cda_rgba_to_vec4(CustomRgba8 c) {
    return (UGuiVec4){(float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f, (float)c.a / 255.0f};
}

static UGuiVec4 cda_outline_vec4(int slot) {
    return cda_rgba_to_vec4(slot == 4 ? cda_beginner_outline : cda_chaos_outline);
}

static UGuiVec4 cda_shadow_vec4(int slot) {
    return cda_rgba_to_vec4(slot == 4 ? cda_beginner_shadow : cda_chaos_shadow);
}

static void *cda_create_sprite_rgba(const uint8_t *rgba, int width, int height,
                                    UGuiRect rect, UGuiVec2 pivot, float ppu, UGuiVec4 border,
                                    const char *name_tag) {
    ensure_il2cpp_api();
    if (!rgba || width <= 0 || height <= 0 || !s_texture2d_class) return NULL;

    TexCtorFn tex_ctor = (TexCtorFn)CODE(CC_TEX2D_CTOR);
    TexSetIntFn set_filter = (TexSetIntFn)CODE(CC_TEX_SET_FILTER);
    TexSetIntFn set_wrap = (TexSetIntFn)CODE(CC_TEX_SET_WRAP);
    TexLoadRawFn load_raw = (TexLoadRawFn)CODE(CC_TEX2D_LOAD_RAW);
    TexApplyFn apply = (TexApplyFn)CODE(CC_TEX2D_APPLY);
    SpriteCreateFullFn spr_create = (SpriteCreateFullFn)CODE(CC_SPRITE_CREATE);
    SetObjNameFn set_name = (SetObjNameFn)CODE(CC_UOBJ_SET_NAME);

    void *tex = alloc_managed_object(s_texture2d_class);
    if (!tex) return NULL;
    (void)gc_pin(tex, false);

    tex_ctor(tex, width, height, 4 /* RGBA32 */, false, NULL);
    set_filter(tex, 1 /* Bilinear */, NULL);
    set_wrap(tex, 1 /* Clamp */, NULL);
    load_raw(tex, rgba, width * height * 4, NULL);
    apply(tex, false, true, NULL);
    protect_cached_unity_object(tex);

    void *spr = spr_create(tex, rect, pivot, ppu, 0, 0, border, false, NULL);
    if (!spr) return NULL;
    (void)gc_pin(spr, false);
    protect_cached_unity_object(spr);
    if (name_tag) {
        set_name(spr, make_string(NULL, name_tag), NULL);
    }
    return spr;
}

typedef struct {
    const CustomDiffArtwork *art;
    void *sprite;
} CdaArtCacheEntry;

static CdaArtCacheEntry s_cda_art_cache[16];

static void *cda_artwork_sprite(const CustomDiffArtwork *art) {
    if (!art || !art->png || art->png_size == 0) return NULL;
    for (size_t i = 0; i < 16; ++i) {
        if (s_cda_art_cache[i].art == art && unity_native_alive(s_cda_art_cache[i].sprite)) {
            return s_cda_art_cache[i].sprite;
        }
    }
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(1);
    uint8_t *pixels = stbi_load_from_memory(art->png, (int)art->png_size, &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return NULL;
    }
    UGuiRect rect = {
        (float)art->unity_rect.x,
        (float)art->unity_rect.y,
        (float)art->unity_rect.width,
        (float)art->unity_rect.height
    };
    char tag[128];
    snprintf(tag, sizeof(tag), "FBL.CustomDifficulty/%s", art->key ? art->key : "Art");
    void *spr = cda_create_sprite_rgba(pixels, w, h, rect, (UGuiVec2){0.5f, 0.5f}, 100.0f,
                                       (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f}, tag);
    stbi_image_free(pixels);
    if (spr) {
        for (size_t i = 0; i < 16; ++i) {
            if (!s_cda_art_cache[i].art || s_cda_art_cache[i].art == art || !unity_native_alive(s_cda_art_cache[i].sprite)) {
                s_cda_art_cache[i].art = art;
                s_cda_art_cache[i].sprite = spr;
                break;
            }
        }
    }
    return spr;
}

static void *s_cda_plate_sprites[2] = {NULL, NULL};

static void *cda_plate_sprite(int slot) {
    if (slot != 4 && slot != 5) return NULL;
    int idx = (slot == 5) ? 1 : 0;
    if (unity_native_alive(s_cda_plate_sprites[idx])) return s_cda_plate_sprites[idx];

    const unsigned char *png = (slot == 4) ? cda_kBeginnerPlatePng : cda_kChaosPlatePng;
    size_t png_sz = (slot == 4) ? sizeof(cda_kBeginnerPlatePng) : sizeof(cda_kChaosPlatePng);
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(1);
    uint8_t *pixels = stbi_load_from_memory(png, (int)png_sz, &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return NULL;
    }
    void *spr = cda_create_sprite_rgba(pixels, w, h,
                                       (UGuiRect){0.0f, 0.0f, (float)w, (float)h},
                                       (UGuiVec2){0.5f, 0.5f}, 100.0f,
                                       (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f},
                                       slot == 4 ? "FBL.CustomDifficulty/Plate_Beginner" : "FBL.CustomDifficulty/Plate_Chaos");
    stbi_image_free(pixels);
    s_cda_plate_sprites[idx] = spr;
    return spr;
}

static void *s_cda_blue_variants[6][2];

static void *cda_blue_variant_by_name(const char *name, int slot, UGuiVec2 pivot, float ppu, UGuiVec4 border) {
    if (!name || (slot != 4 && slot != 5)) return NULL;
    int sidx = (slot == 5) ? 1 : 0;
    int found_i = -1;
    for (size_t i = 0; i < cda_source_sprite_count && i < 6; ++i) {
        if (!strcmp(cda_source_sprites[i].name, name)) {
            found_i = (int)i;
            break;
        }
    }
    if (found_i < 0) return NULL;
    if (unity_native_alive(s_cda_blue_variants[found_i][sidx])) {
        return s_cda_blue_variants[found_i][sidx];
    }

    const CustomSourceSpriteArt *asset = &cda_source_sprites[found_i];
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(1);
    uint8_t *pixels = stbi_load_from_memory(asset->png, (int)asset->size, &w, &h, &ch, 4);
    if (!pixels || w != asset->width || h != asset->height) {
        if (pixels) stbi_image_free(pixels);
        return NULL;
    }

    CustomRgba8 col = (slot == 4) ? cda_beginner_primary : cda_chaos_primary;
    int total = w * h;
    for (int i = 0; i < total; ++i) {
        uint8_t *p = pixels + 4 * i;
        if ((int)p[2] > (int)p[0] + 12 && (int)p[2] > (int)p[1] + 4) {
            int lo = p[0] < p[1] ? p[0] : p[1];
            if (p[2] < lo) lo = p[2];
            int hi = p[0] > p[1] ? p[0] : p[1];
            if (p[2] > hi) hi = p[2];
            p[0] = (uint8_t)(lo + (hi - lo) * (int)col.r / 255);
            p[1] = (uint8_t)(lo + (hi - lo) * (int)col.g / 255);
            p[2] = (uint8_t)(lo + (hi - lo) * (int)col.b / 255);
        }
    }

    char tag[128];
    snprintf(tag, sizeof(tag), "FBL.CustomDifficulty/%s_%d", name, slot);
    void *spr = cda_create_sprite_rgba(pixels, w, h,
                                       (UGuiRect){0.0f, 0.0f, (float)w, (float)h},
                                       pivot, ppu, border, tag);
    stbi_image_free(pixels);
    s_cda_blue_variants[found_i][sidx] = spr;
    return spr;
}

static void cda_read_sprite_metrics(void *source_spr, int fallback_w, int fallback_h,
                                    UGuiVec2 *out_pivot, float *out_ppu, UGuiVec4 *out_border) {
    *out_pivot = (UGuiVec2){0.5f, 0.5f};
    *out_ppu = 100.0f;
    *out_border = (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f};
    if (!unity_native_alive(source_spr)) return;

    typedef UGuiVec2 (ABI *GetPivotFn)(void *spr, void *method);
    typedef UGuiVec4 (ABI *GetBorderFn)(void *spr, void *method);
    typedef float (ABI *GetPpuFn)(void *spr, void *method);
    typedef UGuiRect (ABI *GetSprRectFn)(void *spr, void *method);

    UGuiVec2 piv = ((GetPivotFn)CODE(CC_SPRITE_GET_PIVOT))(source_spr, NULL);
    UGuiRect r = ((GetSprRectFn)CODE(CC_SPRITE_GET_RECT))(source_spr, NULL);
    float w = (r.w > 0.0f) ? r.w : (float)fallback_w;
    float h = (r.h > 0.0f) ? r.h : (float)fallback_h;
    if (w > 0.0f && h > 0.0f) {
        out_pivot->x = piv.x / w;
        out_pivot->y = piv.y / h;
    }
    *out_border = ((GetBorderFn)CODE(CC_SPRITE_GET_BORDER))(source_spr, NULL);
    float ppu = ((GetPpuFn)CODE(CC_SPRITE_GET_PPU))(source_spr, NULL);
    if (ppu > 0.0f) *out_ppu = ppu;
}

typedef struct {
    void *source_spr;
    int slot;
    void *result_spr;
} CdaSpriteVariantCacheEntry;

static CdaSpriteVariantCacheEntry s_cda_variant_cache[64];

static void *cda_blue_variant(void *source_spr, int slot) {
    if (!unity_native_alive(source_spr) || (slot != 4 && slot != 5)) return source_spr;
    for (int i = 0; i < 64; ++i) {
        if (s_cda_variant_cache[i].source_spr == source_spr &&
            s_cda_variant_cache[i].slot == slot &&
            unity_native_alive(s_cda_variant_cache[i].result_spr)) {
            return s_cda_variant_cache[i].result_spr;
        }
    }

    GetObjNameFn get_name = (GetObjNameFn)CODE(CC_UOBJ_GET_NAME);
    char name[128] = {0};
    read_guest_string(get_name(source_spr, NULL), name, sizeof(name));
    void *out_spr = source_spr;
    if (!strncmp(name, "FBL.CustomDifficulty/", 21)) {
        out_spr = source_spr;
    } else if (strstr(name, "MusicLevelBase") != NULL) {
        out_spr = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerMusicLevelBase : &cda_kChaosMusicLevelBase);
    } else if (!strncmp(name, "DifficultyIcon", 14)) {
        out_spr = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerTrigger : &cda_kChaosTrigger);
    } else if (!strncmp(name, "MusicList_Plate02", 17)) {
        out_spr = cda_plate_sprite(slot);
    } else {
        for (size_t i = 0; i < cda_source_sprite_count && i < 6; ++i) {
            if (!strcmp(cda_source_sprites[i].name, name)) {
                UGuiVec2 piv;
                float ppu;
                UGuiVec4 border;
                cda_read_sprite_metrics(source_spr, cda_source_sprites[i].width, cda_source_sprites[i].height,
                                        &piv, &ppu, &border);
                void *res = cda_blue_variant_by_name(name, slot, piv, ppu, border);
                out_spr = res ? res : source_spr;
                break;
            }
        }
    }

    if (unity_native_alive(out_spr)) {
        int slot_idx = -1;
        for (int i = 0; i < 64; ++i) {
            if ((s_cda_variant_cache[i].source_spr == source_spr && s_cda_variant_cache[i].slot == slot) ||
                !unity_native_alive(s_cda_variant_cache[i].source_spr)) {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx < 0) slot_idx = 0;
        s_cda_variant_cache[slot_idx].source_spr = source_spr;
        s_cda_variant_cache[slot_idx].slot = slot;
        s_cda_variant_cache[slot_idx].result_spr = out_spr;
    }
    return out_spr;
}

static void *s_cda_cx_sprite = NULL;

static void *cda_cx_mark_sprite(void) {
    if (unity_native_alive(s_cda_cx_sprite)) return s_cda_cx_sprite;
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(1);
    uint8_t *pixels = stbi_load_from_memory(cda_kMsMarkCustomPng, (int)sizeof(cda_kMsMarkCustomPng), &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return NULL;
    }
    s_cda_cx_sprite = cda_create_sprite_rgba(pixels, w, h,
                                             (UGuiRect){0.0f, 0.0f, (float)w, (float)h},
                                             (UGuiVec2){0.5f, 0.5f}, 100.0f,
                                             (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f},
                                             "FBL.CustomDifficulty/MsMark_CX");
    stbi_image_free(pixels);
    return s_cda_cx_sprite;
}

typedef struct {
    void *img;
    void *orig_sprite;
    UGuiVec2 orig_size;
} CdaOrigImageEntry;

static CdaOrigImageEntry s_cda_orig_ind_imgs[64];

static void cda_remember_orig_sprite(CdaOrigImageEntry *table, size_t cap, void *img, void *spr, UGuiVec2 sz) {
    if (!uobj_alive(img) || !unity_native_alive(spr)) return;
    for (size_t i = 0; i < cap; ++i) {
        if (table[i].img == img && unity_native_alive(table[i].orig_sprite)) {
            return;
        }
    }
    GetObjNameFn get_name = (GetObjNameFn)CODE(CC_UOBJ_GET_NAME);
    char nm[128] = {0};
    read_guest_string(get_name(spr, NULL), nm, sizeof(nm));
    if (!strncmp(nm, "FBL.CustomDifficulty/", 21)) return;

    for (size_t i = 0; i < cap; ++i) {
        if (table[i].img == img) {
            if (!unity_native_alive(table[i].orig_sprite)) {
                table[i].orig_sprite = spr;
                (void)gc_pin(spr, false);
                table[i].orig_size = sz;
            }
            return;
        }
    }
    for (size_t i = 0; i < cap; ++i) {
        if (!uobj_alive(table[i].img)) {
            table[i].img = img;
            table[i].orig_sprite = spr;
            (void)gc_pin(spr, false);
            table[i].orig_size = sz;
            return;
        }
    }
}

static void *cda_find_orig_sprite(CdaOrigImageEntry *table, size_t cap, void *img) {
    if (!uobj_alive(img)) return NULL;
    for (size_t i = 0; i < cap; ++i) {
        if (table[i].img == img && unity_native_alive(table[i].orig_sprite)) {
            return table[i].orig_sprite;
        }
    }
    return NULL;
}

static void *cda_cached_comp_str(const char *name) {
    if (!name || !*name) return NULL;
    typedef struct { const char *key; void *gstr; } CompStrCache;
    static CompStrCache s_cache[24];
    for (int i = 0; i < 24; ++i) {
        if (s_cache[i].key && (s_cache[i].key == name || !strcmp(s_cache[i].key, name))) {
            return s_cache[i].gstr;
        }
        if (!s_cache[i].key) {
            s_cache[i].key = name;
            s_cache[i].gstr = make_pinned_string(name, NULL);
            return s_cache[i].gstr;
        }
    }
    return make_string(NULL, name);
}

static void *cda_get_comp(void *go, const char *full_name, const char *short_name) {
    if (!uobj_alive(go)) return NULL;
    typedef void *(ABI *GetCompByNameFn)(void *go, void *str, void *method);
    GetCompByNameFn get_comp = (GetCompByNameFn)CODE(CC_GO_GET_COMP_BY_NAME);
    if (full_name && *full_name) {
        void *c = get_comp(go, cda_cached_comp_str(full_name), NULL);
        if (uobj_alive(c)) return c;
    }
    if (short_name && *short_name) {
        void *c = get_comp(go, cda_cached_comp_str(short_name), NULL);
        if (uobj_alive(c)) return c;
    }
    return NULL;
}

static void *cda_find_comp_in_children(void *tr, const char *full_name, const char *short_name, int depth) {
    if (!uobj_alive(tr) || depth > 8) return NULL;
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(CC_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(CC_TR_GET_CHILD);
    void *go = comp_get_go(tr, NULL);
    void *c = cda_get_comp(go, full_name, short_name);
    if (uobj_alive(c)) return c;
    int32_t cc = get_cc(tr, NULL);
    for (int32_t i = 0; i < cc && i < 32; ++i) {
        c = cda_find_comp_in_children(get_ch(tr, i, NULL), full_name, short_name, depth + 1);
        if (uobj_alive(c)) return c;
    }
    return NULL;
}

#define CDA_CUSTOM_MARKS_CAP 256

typedef struct {
    void *ms_mark;
    void *custom_mark;
    bool is_custom;
} CdaCustomMarkEntry;

static CdaCustomMarkEntry s_cda_custom_marks[CDA_CUSTOM_MARKS_CAP];
static void *original_set_active_edition_mark = NULL;

static void cda_configure_custom_mark_hierarchy(void *node_tr) {
    if (!uobj_alive(node_tr)) return;
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    SetSpriteFn set_spr = (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);
    SetEnabledFn set_enabled = (SetEnabledFn)CODE(CC_BEHAVIOUR_SET_ENABLED);
    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(CC_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(CC_TR_GET_CHILD);

    void *go = comp_get_go(node_tr, NULL);
    if (uobj_alive(go)) {
        void *img = cda_get_comp(go, "UnityEngine.UI.Image", "Image");
        if (uobj_alive(img)) {
            void *cx_spr = cda_cx_mark_sprite();
            if (cx_spr) set_spr(img, cx_spr, NULL);
        }
        static const struct {
            const char *full_name;
            const char *short_name;
        } text_types[4] = {
            {"Game.Localization.UI.LocalizedTMProUGUI", "LocalizedTMProUGUI"},
            {"Game.Localization.UI.LocalizedText", "LocalizedText"},
            {"TMPro.TextMeshProUGUI", "TextMeshProUGUI"},
            {"UnityEngine.UI.Text", "Text"}
        };
        static void *s_cx_str = NULL;
        if (!s_cx_str) s_cx_str = make_pinned_string("CX", NULL);
        void *gstr = s_cx_str ? s_cx_str : make_string(NULL, "CX");

        for (int i = 0; i < 4; ++i) {
            void *txt = cda_get_comp(go, text_types[i].full_name, text_types[i].short_name);
            if (uobj_alive(txt)) {
                void *klass = object_class(txt);
                const char *kname = klass ? *(const char **)((char *)klass + 0x10) : "";
                if (i == 0 || i == 1 || (kname && strstr(kname, "Localized"))) {
                    /* Disable LocalizedTMProUGUI / LocalizedText so OnEnable never overwrites "CX" */
                    set_enabled(txt, false, NULL);
                    void *txt_go = comp_get_go(txt, NULL);
                    if (uobj_alive(txt_go)) set_active(txt_go, false, NULL);
                    if (i == 0 || (kname && strstr(kname, "TMPro"))) {
                        put64((char *)txt + 0x798, 0);
                        void *info = (void *)load64((char *)txt + 0x7a0);
                        if (info && (uintptr_t)info > 0x10000ULL) {
                            put32((char *)info + 0x10, 3);
                            put64((char *)info + 0x18, 0);
                        }
                    } else {
                        put64((char *)txt + 0x110, 0);
                    }
                }
                if (i == 0 || i == 2 || (kname && strstr(kname, "TMPro"))) {
                    put64((char *)txt + 0xd0, (uint64_t)gstr);
                    typedef void (ABI *TmpSetTextFn)(void *self, void *str, void *method);
                    ((TmpSetTextFn)CODE(CC_TMP_TEXT_SET_TEXT))(txt, gstr, NULL);
                    if (klass) {
                        TmpSetTextFn fn = (TmpSetTextFn)load64((const char *)klass + 0x558);
                        void *m = (void *)load64((const char *)klass + 0x560);
                        if (fn && gstr) fn(txt, gstr, m);
                    }
                } else {
                    typedef void (ABI *SetTextVirtFn)(void *self, void *str, void *method);
                    ((SetTextVirtFn)CODE(CC_UI_TEXT_SET_TEXT))(txt, gstr, NULL);
                    if (klass) {
                        SetTextVirtFn fn = (SetTextVirtFn)load64((const char *)klass + 0x5e8);
                        void *m = (void *)load64((const char *)klass + 0x5f0);
                        if (fn && gstr) fn(txt, gstr, m);
                    }
                }
            }
        }
    }
    int32_t cc = get_cc(node_tr, NULL);
    for (int32_t i = 0; i < cc && i < 16; ++i) {
        cda_configure_custom_mark_hierarchy(get_ch(node_tr, i, NULL));
    }
}

static void cda_apply_ms_mark(void *self, bool is_custom) {
    if (!uobj_alive(self)) return;
    GetTransformFn go_get_tr = (GetTransformFn)CODE(CC_GO_GET_TRANSFORM);
    void *custom_mark = NULL;
    int entry_idx = -1;
    for (int i = 0; i < CDA_CUSTOM_MARKS_CAP; ++i) {
        if (s_cda_custom_marks[i].ms_mark == self) {
            if (uobj_alive(s_cda_custom_marks[i].custom_mark)) {
                custom_mark = s_cda_custom_marks[i].custom_mark;
            }
            s_cda_custom_marks[i].is_custom = is_custom;
            entry_idx = i;
            break;
        }
    }
    if (!custom_mark && is_custom) {
        /* MSMarkBehaviour: +0x20 = _dlcMark, +0x28 = _dxMark */
        void *dlc = (void *)load64((char *)self + 0x20);
        if (!uobj_alive(dlc)) dlc = (void *)load64((char *)self + 0x28);
        if (uobj_alive(dlc)) {
            GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
            CloneWithParentFn clone_obj = (CloneWithParentFn)CODE(CC_UOBJ_CLONE_WITH_PARENT);
            void *dlc_tr = go_get_tr(dlc, NULL);
            void *par_tr = uobj_alive(dlc_tr) ? get_parent(dlc_tr, NULL) : NULL;
            if (uobj_alive(par_tr)) {
                custom_mark = clone_obj(dlc, par_tr, 0);
                if (uobj_alive(custom_mark)) {
                    (void)gc_pin(custom_mark, false);
                    cda_configure_custom_mark_hierarchy(go_get_tr(custom_mark, NULL));
                    if (entry_idx < 0) {
                        for (int i = 0; i < CDA_CUSTOM_MARKS_CAP; ++i) {
                            if (!uobj_alive(s_cda_custom_marks[i].ms_mark)) { entry_idx = i; break; }
                        }
                        if (entry_idx < 0) entry_idx = 0;
                    }
                    s_cda_custom_marks[entry_idx].ms_mark = self;
                    s_cda_custom_marks[entry_idx].custom_mark = custom_mark;
                    s_cda_custom_marks[entry_idx].is_custom = is_custom;
                }
            }
        }
    }

    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);
    void *dlc = (void *)load64((char *)self + 0x20);
    void *dx = (void *)load64((char *)self + 0x28);
    if (is_custom) {
        if (uobj_alive(dlc)) set_active(dlc, false, NULL);
        if (uobj_alive(dx)) set_active(dx, false, NULL);
        if (uobj_alive(custom_mark)) {
            set_active(custom_mark, true, NULL);
            cda_configure_custom_mark_hierarchy(go_get_tr(custom_mark, NULL));
        } else if (uobj_alive(dlc)) {
            set_active(dlc, true, NULL);
            cda_configure_custom_mark_hierarchy(go_get_tr(dlc, NULL));
        }
    } else {
        if (uobj_alive(custom_mark)) set_active(custom_mark, false, NULL);
    }
}

/* MSMarkBehaviour::SetActiveEditionMark (0x1d96b80) — re-apply CX mark after native SetActiveEditionMark */
static ABI int hook_set_active_edition_mark(HookRegs *r) {
    void *self = (void *)r->rdi;
    int32_t edition = (int32_t)r->rsi;
    void *method = (void *)r->rdx;
    if (!original_set_active_edition_mark) return 0;

    typedef void (ABI *SetEditionFn)(void *self, int32_t ed, void *method);
    ((SetEditionFn)original_set_active_edition_mark)(self, edition, method);

    if (uobj_alive(self)) {
        for (size_t i = 0; i < CDA_CUSTOM_MARKS_CAP; ++i) {
            if (s_cda_custom_marks[i].ms_mark == self) {
                cda_apply_ms_mark(self, s_cda_custom_marks[i].is_custom);
                break;
            }
        }
    }
    r->rax = 0;
    return 1;
}

static void *s_cda_indicator_sprites[64];

static void *cda_indicator_sprite(uint8_t mask) {
    mask &= 0x3fu;
    if (unity_native_alive(s_cda_indicator_sprites[mask])) {
        return s_cda_indicator_sprites[mask];
    }
    /* Fit all 6 difficulty dots cleanly inside the stock 156x28 indicator box so they never
     * overflow the song bar or overlap the favorite star! */
    const int width = 156, height = 28;
    uint8_t pixels[156 * 28 * 4];
    memset(pixels, 0, sizeof(pixels));

    static const int dot_x_starts[6] = {2, 28, 54, 80, 106, 132};
    static const uint8_t colors[6][3] = {
        {247, 205, 13},  /* Basic (0) */
        {37,  169, 230}, /* Expert (1) */
        {250, 95,  40},  /* Ultimate (2) */
        {182, 99,  230}, /* Supreme (3) */
        {51,  204, 51},  /* Beginner (4) */
        {155, 155, 155}  /* Chaos (5) */
    };

    const int dot_w = 22;
    const int dot_h = 23;
    const int y_off = (height - dot_h) / 2;
    for (int pos = 0; pos < 6; ++pos) {
        int slot = s_difficulty_order[pos];
        if (!(mask & (1u << slot))) continue;
        const uint8_t *col = colors[slot];
        int sx = dot_x_starts[pos];
        for (int dy = 0; dy < dot_h; ++dy) {
            int y = y_off + dy;
            int src_y = ((dot_h - 1 - dy) * 27 + (dot_h / 2)) / (dot_h - 1);
            for (int dx = 0; dx < dot_w; ++dx) {
                int src_x = (dx * 26 + (dot_w / 2)) / (dot_w - 1);
                uint8_t a = cda_kDotAlpha[src_y * 27 + src_x];
                if (a == 0) continue;
                int px = sx + dx;
                int idx = 4 * (y * width + px);
                pixels[idx + 0] = col[0];
                pixels[idx + 1] = col[1];
                pixels[idx + 2] = col[2];
                pixels[idx + 3] = a;
            }
        }
    }

    char tag[64];
    snprintf(tag, sizeof(tag), "FBL.CustomDifficulty/Indicator_%u", (unsigned)mask);
    void *spr = cda_create_sprite_rgba(pixels, width, height,
                                       (UGuiRect){0.0f, 0.0f, (float)width, (float)height},
                                       (UGuiVec2){0.5f, 0.5f}, 100.0f,
                                       (UGuiVec4){0.0f, 0.0f, 0.0f, 0.0f}, tag);
    s_cda_indicator_sprites[mask] = spr;
    return spr;
}

static void *s_material_class = NULL;

typedef struct {
    void *src_mat;
    int slot;
    void *cloned_mat;
} CdaMatCacheEntry;

static CdaMatCacheEntry s_cda_mat_cache[32];

static void *cda_clone_tmp_material(void *src_mat, int slot) {
    if (!unity_native_alive(src_mat) || (slot != 4 && slot != 5)) return src_mat;
    for (int i = 0; i < 32; ++i) {
        if (s_cda_mat_cache[i].src_mat == src_mat &&
            s_cda_mat_cache[i].slot == slot &&
            unity_native_alive(s_cda_mat_cache[i].cloned_mat)) {
            return s_cda_mat_cache[i].cloned_mat;
        }
    }
    ensure_il2cpp_api();
    if (!s_material_class) {
        s_material_class = find_il2cpp_class("UnityEngine", "Material");
        if (!s_material_class) s_material_class = object_class(src_mat);
    }
    if (!s_material_class) return src_mat;

    void *mat = alloc_managed_object(s_material_class);
    if (!mat) return src_mat;
    (void)gc_pin(mat, false);

    typedef void (ABI *MatCtorFn)(void *self, void *source, void *method);
    typedef void (ABI *MatSetColorFn)(void *self, void *name_str, UGuiVec4 col, void *method);
    ((MatCtorFn)CODE(CC_MAT_CTOR))(mat, src_mat, NULL);
    protect_cached_unity_object(mat);

    UGuiVec4 outline = cda_outline_vec4(slot);
    UGuiVec4 shadow = cda_shadow_vec4(slot);
    ((MatSetColorFn)CODE(CC_MAT_SET_COLOR))(mat, cda_cached_comp_str("_OutlineColor"), outline, NULL);
    ((MatSetColorFn)CODE(CC_MAT_SET_COLOR))(mat, cda_cached_comp_str("_UnderlayColor"), shadow, NULL);

    for (int i = 0; i < 32; ++i) {
        if (!unity_native_alive(s_cda_mat_cache[i].cloned_mat)) {
            s_cda_mat_cache[i].src_mat = src_mat;
            s_cda_mat_cache[i].slot = slot;
            s_cda_mat_cache[i].cloned_mat = mat;
            break;
        }
    }
    return mat;
}

/* Per-DifficultyListBehaviour context so native RefreshView/OnReady internal calls stay aware of custom songs */
typedef struct {
    void *control;
    const CustomSong *song;
    bool is_popup;
} DiffListCtxEntry;

static DiffListCtxEntry s_diff_list_ctx[16];

static void set_diff_list_ctx(void *control, const CustomSong *song, bool is_popup) {
    if (!control) return;
    for (int i = 0; i < 16; ++i) {
        if (s_diff_list_ctx[i].control == control) {
            s_diff_list_ctx[i].song = song;
            s_diff_list_ctx[i].is_popup = is_popup;
            return;
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (!s_diff_list_ctx[i].control || !uobj_alive(s_diff_list_ctx[i].control)) {
            s_diff_list_ctx[i].control = control;
            s_diff_list_ctx[i].song = song;
            s_diff_list_ctx[i].is_popup = is_popup;
            return;
        }
    }
    s_diff_list_ctx[0].control = control;
    s_diff_list_ctx[0].song = song;
    s_diff_list_ctx[0].is_popup = is_popup;
}

static const DiffListCtxEntry *get_diff_list_ctx(void *control) {
    if (!control) return NULL;
    for (int i = 0; i < 16; ++i) {
        if (s_diff_list_ctx[i].control == control) return &s_diff_list_ctx[i];
    }
    return NULL;
}

static void draw_diff_control(void *control, const CustomSong *song, int slot, bool is_popup) {
    if (!control) return;
    set_diff_list_ctx(control, song, is_popup);
    if (!song || !song_has_chart(song, slot)) return;
    typedef void (ABI *SetDiffFn)(void *self, int32_t diff, bool isSupremeActive, bool isOneDiff, void *method);
    SetDiffFn set_diff = (SetDiffFn)CODE(CC_DIFF_LIST_SET_DIFF);
    bool has_sup = song_has_chart(song, 3);
    bool one_diff = (count_song_charts(song) <= 1);
    set_diff(control, slot, has_sup, one_diff, NULL);
}

/* Map a descendant GameObject from src_root_tr to dst_root_tr by matching child transform indices */
static void *cda_map_cloned_go(void *src_root_tr, void *dst_root_tr, void *src_child_go) {
    if (!uobj_alive(src_root_tr) || !uobj_alive(dst_root_tr) || !uobj_alive(src_child_go)) return NULL;
    GetTransformFn go_get_tr = (GetTransformFn)CODE(CC_GO_GET_TRANSFORM);
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(CC_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(CC_TR_GET_CHILD);

    void *target_tr = go_get_tr(src_child_go, NULL);
    if (!uobj_alive(target_tr)) return NULL;
    if (target_tr == src_root_tr) return comp_get_go(dst_root_tr, NULL);

    int path[8];
    int depth = 0;
    void *cur = target_tr;
    while (uobj_alive(cur) && cur != src_root_tr && depth < 8) {
        void *par = get_parent(cur, NULL);
        if (!uobj_alive(par)) return NULL;
        int32_t cc = get_cc(par, NULL);
        int found = -1;
        for (int32_t i = 0; i < cc; ++i) {
            if (get_ch(par, i, NULL) == cur) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) return NULL;
        path[depth++] = found;
        cur = par;
    }
    if (cur != src_root_tr) return NULL;

    void *dst_cur = dst_root_tr;
    for (int d = depth - 1; d >= 0; --d) {
        int32_t cc = get_cc(dst_cur, NULL);
        if (path[d] >= cc) return NULL;
        dst_cur = get_ch(dst_cur, path[d], NULL);
        if (!uobj_alive(dst_cur)) return NULL;
    }
    return comp_get_go(dst_cur, NULL);
}

typedef struct {
    void *image;
    void *source;
    void *last_resolved;
    void *grad_comp;
    int slot;
    bool is_button;
    bool button_checked;
    bool grad_checked;
} CdaImageSkinEntry;

static CdaImageSkinEntry s_cda_skins[128];
static CdaSpriteVariantCacheEntry s_cda_resolve_cache[64];

static UGuiVec4 cda_recolor_blue(UGuiVec4 color, int slot) {
    if (color.z <= color.x + 0.04f || color.z <= color.y + 0.015f) return color;
    float lo = fminf(color.x, fminf(color.y, color.z));
    float hi = fmaxf(color.x, fmaxf(color.y, color.z));
    float span = hi - lo;
    UGuiVec4 tint = (slot == 4)
                    ? (UGuiVec4){0.20f, 0.80f, 0.20f, 1.0f}
                    : (UGuiVec4){0.61f, 0.61f, 0.61f, 1.0f};
    return (UGuiVec4){lo + span * tint.x, lo + span * tint.y, lo + span * tint.z, color.w};
}

static void *cda_resolve_sprite(void *source_spr, int slot) {
    if (!unity_native_alive(source_spr) || (slot != 4 && slot != 5)) return source_spr;
    for (int i = 0; i < 64; ++i) {
        if (s_cda_resolve_cache[i].source_spr == source_spr &&
            s_cda_resolve_cache[i].slot == slot &&
            unity_native_alive(s_cda_resolve_cache[i].result_spr)) {
            return s_cda_resolve_cache[i].result_spr;
        }
    }
    GetObjNameFn get_name = (GetObjNameFn)CODE(CC_UOBJ_GET_NAME);
    char nm[128] = {0};
    read_guest_string(get_name(source_spr, NULL), nm, sizeof(nm));
    void *out_spr = NULL;
    if (!strncmp(nm, "DifficultyList_Button", 21) && strlen(nm) >= 23) {
        int idx = atoi(nm + 21);
        if (idx >= 1 && idx <= 4) {
            void *rep = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerButtonUnselected : &cda_kChaosButtonUnselected);
            out_spr = rep ? rep : source_spr;
        } else if (idx >= 5 && idx <= 8) {
            void *rep = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerButtonSelected : &cda_kChaosButtonSelected);
            out_spr = rep ? rep : source_spr;
        }
    }
    if (!out_spr) out_spr = cda_blue_variant(source_spr, slot);
    if (unity_native_alive(out_spr)) {
        int slot_idx = -1;
        for (int i = 0; i < 64; ++i) {
            if ((s_cda_resolve_cache[i].source_spr == source_spr && s_cda_resolve_cache[i].slot == slot) ||
                !unity_native_alive(s_cda_resolve_cache[i].source_spr)) {
                slot_idx = i;
                break;
            }
        }
        if (slot_idx < 0) slot_idx = 0;
        s_cda_resolve_cache[slot_idx].source_spr = source_spr;
        s_cda_resolve_cache[slot_idx].slot = slot;
        s_cda_resolve_cache[slot_idx].result_spr = out_spr;
    }
    return out_spr;
}

static bool cda_is_button_sprite(void *spr) {
    if (!unity_native_alive(spr)) return false;
    GetObjNameFn get_name = (GetObjNameFn)CODE(CC_UOBJ_GET_NAME);
    char nm[128] = {0};
    read_guest_string(get_name(spr, NULL), nm, sizeof(nm));
    return (!strncmp(nm, "DifficultyList_Button", 21) ||
            !strncmp(nm, "FBL.CustomDifficulty/DifficultyList_Button", 42));
}

static void cda_register_image_skin(void *img, void *source_spr, int slot) {
    if (!uobj_alive(img) || (slot != 4 && slot != 5)) return;
    for (int i = 0; i < 128; ++i) {
        if (s_cda_skins[i].image == img) {
            if (unity_native_alive(source_spr) && s_cda_skins[i].source != source_spr) {
                s_cda_skins[i].source = source_spr;
                s_cda_skins[i].last_resolved = NULL;
            }
            if (s_cda_skins[i].slot != slot) {
                s_cda_skins[i].slot = slot;
                s_cda_skins[i].last_resolved = NULL;
            }
            return;
        }
    }
    for (int i = 0; i < 128; ++i) {
        if (!uobj_alive(s_cda_skins[i].image)) {
            memset(&s_cda_skins[i], 0, sizeof(s_cda_skins[i]));
            s_cda_skins[i].image = img;
            s_cda_skins[i].source = source_spr;
            s_cda_skins[i].slot = slot;
            return;
        }
    }
    memset(&s_cda_skins[0], 0, sizeof(s_cda_skins[0]));
    s_cda_skins[0].image = img;
    s_cda_skins[0].source = source_spr;
    s_cda_skins[0].slot = slot;
}

static void *original_image_set_sprite = NULL;

static void cda_apply_image_skin(CdaImageSkinEntry *skin) {
    if (!skin || !uobj_alive(skin->image)) return;
    GetSpriteFn get_spr = (GetSpriteFn)CODE(CC_IMAGE_GET_SPRITE);
    SetSpriteFn set_spr = original_image_set_sprite
                          ? (SetSpriteFn)original_image_set_sprite
                          : (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);
    typedef UGuiVec4 (ABI *GetColorFn)(void *graphic, void *method);
    GetColorFn get_color = (GetColorFn)CODE(CC_GRAPHIC_GET_COLOR);
    SetColorFn set_color = (SetColorFn)CODE(CC_GRAPHIC_SET_COLOR);
    SetEnabledFn set_enabled = (SetEnabledFn)CODE(CC_BEHAVIOUR_SET_ENABLED);
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);

    void *cur = get_spr(skin->image, NULL);
    void *rep = cur;
    if (!cur || cur != skin->last_resolved) {
        void *base_spr = unity_native_alive(cur) ? cur : skin->source;
        rep = cda_resolve_sprite(base_spr, skin->slot);
        if (!unity_native_alive(rep)) rep = cda_resolve_sprite(skin->source, skin->slot);
        if (unity_native_alive(rep)) {
            if (rep != cur) set_spr(skin->image, rep, NULL);
            skin->last_resolved = rep;
        }
        if (!skin->button_checked && (unity_native_alive(base_spr) || unity_native_alive(rep))) {
            skin->is_button = cda_is_button_sprite(base_spr) || cda_is_button_sprite(rep);
            skin->button_checked = true;
        }
    }
    if (skin->is_button) {
        UGuiVec4 ic = get_color(skin->image, NULL);
        if (ic.x < 0.99f || ic.y < 0.99f || ic.z < 0.99f || ic.w < 0.99f) {
            set_color(skin->image, (UGuiVec4){1.0f, 1.0f, 1.0f, 1.0f}, NULL);
        }
        if (!skin->grad_checked) {
            void *go = comp_get_go(skin->image, NULL);
            skin->grad_comp = cda_get_comp(go, "Coffee.UIExtensions.UIGradient", "UIGradient");
            skin->grad_checked = true;
            if (uobj_alive(skin->grad_comp)) set_enabled(skin->grad_comp, false, NULL);
        }
    }
}

static void cda_set_button_label(void *tr, const char *label) {
    if (!uobj_alive(tr) || !label) return;
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(CC_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(CC_TR_GET_CHILD);

    void *go = comp_get_go(tr, NULL);
    if (uobj_alive(go)) {
        void *gstr = make_string(NULL, label);
        void *tmp = cda_get_comp(go, "TMPro.TextMeshProUGUI", "TextMeshProUGUI");
        if (uobj_alive(tmp) && gstr) {
            void *klass = object_class(tmp);
            if (klass) {
                typedef void (ABI *SetTmpTextVirtFn)(void *self, void *str, void *method);
                SetTmpTextVirtFn fn = (SetTmpTextVirtFn)load64((const char *)klass + 0x558);
                void *m = (void *)load64((const char *)klass + 0x560);
                if (fn) fn(tmp, gstr, m);
            }
        }
        void *txt = cda_get_comp(go, "UnityEngine.UI.Text", "Text");
        if (uobj_alive(txt) && gstr) {
            void *klass = object_class(txt);
            if (klass) {
                typedef void (ABI *SetTextVirtFn)(void *self, void *str, void *method);
                SetTextVirtFn fn = (SetTextVirtFn)load64((const char *)klass + 0x5e8);
                void *m = (void *)load64((const char *)klass + 0x5f0);
                if (fn) fn(txt, gstr, m);
            }
        }
    }

    int32_t cc = get_cc(tr, NULL);
    for (int32_t i = 0; i < cc && i < 32; ++i) {
        cda_set_button_label(get_ch(tr, i, NULL), label);
    }
}

static void cda_skin_hierarchy(void *tr, void *skip_tr, int slot) {
    if (!uobj_alive(tr) || tr == skip_tr || (slot != 4 && slot != 5)) return;
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetSpriteFn get_spr = (GetSpriteFn)CODE(CC_IMAGE_GET_SPRITE);
    typedef UGuiVec4 (ABI *GetColorFn)(void *graphic, void *method);
    GetColorFn get_color = (GetColorFn)CODE(CC_GRAPHIC_GET_COLOR);
    SetColorFn set_color = (SetColorFn)CODE(CC_GRAPHIC_SET_COLOR);
    GetChildCountFn get_cc = (GetChildCountFn)CODE(CC_TR_GET_CHILD_COUNT);
    GetChildFn get_ch = (GetChildFn)CODE(CC_TR_GET_CHILD);
    typedef void *(ABI *GetTmpMatFn)(void *tmp, void *method);
    typedef void (ABI *SetTmpMatFn)(void *tmp, void *mat, void *method);
    GetTmpMatFn get_tmp_mat = (GetTmpMatFn)CODE(CC_TMP_GET_SHARED_MAT);
    SetTmpMatFn set_shared_mat = (SetTmpMatFn)CODE(CC_TMP_SET_SHARED_MAT);
    SetTmpMatFn set_tmp_mat = (SetTmpMatFn)CODE(CC_TMP_SET_FONT_MAT);

    void *go = comp_get_go(tr, NULL);
    if (uobj_alive(go)) {
        /* 1. Coffee.UIExtensions.UIGradient */
        void *grad = cda_get_comp(go, "Coffee.UIExtensions.UIGradient", "UIGradient");
        if (uobj_alive(grad)) {
            static const size_t grad_offs[4] = {0x24, 0x34, 0x44, 0x54};
            static const int grad_sites[4] = {
                CC_UIGRAD_SET_COLOR1, CC_UIGRAD_SET_COLOR2, CC_UIGRAD_SET_COLOR3, CC_UIGRAD_SET_COLOR4
            };
            for (int k = 0; k < 4; ++k) {
                UGuiVec4 c;
                memcpy(&c, (const char *)grad + grad_offs[k], sizeof(UGuiVec4));
                UGuiVec4 rc = cda_recolor_blue(c, slot);
                ((SetColorFn)CODE(grad_sites[k]))(grad, rc, NULL);
            }
        }

        /* 2. UnityEngine.UI.Image */
        void *img = cda_get_comp(go, "UnityEngine.UI.Image", "Image");
        if (uobj_alive(img)) {
            UGuiVec4 ic = get_color(img, NULL);
            set_color(img, cda_recolor_blue(ic, slot), NULL);
            void *spr = get_spr(img, NULL);
            cda_register_image_skin(img, spr, slot);
            for (int i = 0; i < 128; ++i) {
                if (s_cda_skins[i].image == img) {
                    cda_apply_image_skin(&s_cda_skins[i]);
                    break;
                }
            }
        }

        /* 3. TMPro.TextMeshProUGUI */
        void *tmp = cda_get_comp(go, "TMPro.TextMeshProUGUI", "TextMeshProUGUI");
        if (uobj_alive(tmp)) {
            void *orig_mat = get_tmp_mat(tmp, NULL);
            void *new_mat = cda_clone_tmp_material(orig_mat, slot);
            if (new_mat) {
                set_shared_mat(tmp, new_mat, NULL);
                set_tmp_mat(tmp, new_mat, NULL);
            }
        }

        /* 4. Legacy text/shadow components (MaterialGuiTextShadow, Shadow, Text) */
        void *mshadow = cda_get_comp(go, "Gummi.Graphics.MaterialGuiTextShadow", "MaterialGuiTextShadow");
        if (uobj_alive(mshadow)) {
            UGuiVec4 sc;
            memcpy(&sc, (const char *)mshadow + 0x20, sizeof(UGuiVec4));
            if (sc.z > sc.x + 0.04f && sc.z > sc.y + 0.015f) {
                UGuiVec4 target = cda_outline_vec4(slot);
                target.w = sc.w;
                ((SetColorFn)CODE(CC_MAT_GUI_SHADOW_SET_COLOR))(mshadow, target, NULL);
            }
        }
        void *ushadow = cda_get_comp(go, "UnityEngine.UI.Shadow", "Shadow");
        if (uobj_alive(ushadow)) {
            UGuiVec4 sc;
            memcpy(&sc, (const char *)ushadow + 0x20, sizeof(UGuiVec4));
            if (sc.z > sc.x + 0.04f && sc.z > sc.y + 0.015f) {
                ((SetColorFn)CODE(CC_UI_SHADOW_SET_COLOR))(ushadow, cda_recolor_blue(sc, slot), NULL);
            }
        }
        void *utxt = cda_get_comp(go, "UnityEngine.UI.Text", "Text");
        if (uobj_alive(utxt)) {
            UGuiVec4 tc = get_color(utxt, NULL);
            if (tc.z > tc.x + 0.04f && tc.z > tc.y + 0.015f) {
                set_color(utxt, cda_recolor_blue(tc, slot), NULL);
            }
        }
    }

    int32_t cc = get_cc(tr, NULL);
    for (int32_t i = 0; i < cc && i < 32; ++i) {
        cda_skin_hierarchy(get_ch(tr, i, NULL), skip_tr, slot);
    }
}

static void cda_freeze_button_grid(void *parent_tr) {
    if (!uobj_alive(parent_tr)) return;
    typedef void (ABI *ForceRebuildFn)(void *rt, void *method);
    ((ForceRebuildFn)CODE(CC_LAYOUT_FORCE_REBUILD))(parent_tr, NULL);

    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    SetEnabledFn set_enabled = (SetEnabledFn)CODE(CC_BEHAVIOUR_SET_ENABLED);

    void *go = comp_get_go(parent_tr, NULL);
    if (!uobj_alive(go)) return;
    static const char *const kLayoutFull[3] = {
        "UnityEngine.UI.GridLayoutGroup",
        "UnityEngine.UI.HorizontalLayoutGroup",
        "UnityEngine.UI.ContentSizeFitter"
    };
    static const char *const kLayoutShort[3] = {
        "GridLayoutGroup",
        "HorizontalLayoutGroup",
        "ContentSizeFitter"
    };
    for (int i = 0; i < 3; ++i) {
        void *comp = cda_get_comp(go, kLayoutFull[i], kLayoutShort[i]);
        if (uobj_alive(comp)) set_enabled(comp, false, NULL);
    }
}

static void cda_ensure_diff_list_buttons(void *self) {
    if (!self) return;
    void *old_arr = (void *)load64((char *)self + 0x18);
    if (!old_arr) return;
    uint64_t len = load64((char *)old_arr + 0x18);
    if (len != 4) return;

    GetTransformFn comp_get_tr = (GetTransformFn)CODE(CC_COMP_GET_TRANSFORM);
    GetTransformFn go_get_tr = (GetTransformFn)CODE(CC_GO_GET_TRANSFORM);
    GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
    CloneWithParentFn clone_comp = (CloneWithParentFn)CODE(CC_UOBJ_CLONE_WITH_PARENT);
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    SetObjNameFn set_name = (SetObjNameFn)CODE(CC_UOBJ_SET_NAME);
    typedef UGuiVec2 (ABI *GetVec2Fn)(void *rt, void *method);
    GetVec2Fn get_apos = (GetVec2Fn)CODE(CC_RT_GET_ANCHORED_POS);
    SetVec2Fn set_apos = (SetVec2Fn)CODE(CC_RT_SET_ANCHORED_POS);
    SetLocalScaleFn set_scale = (SetLocalScaleFn)CODE(CC_TR_SET_LOCAL_SCALE);
    typedef void (ABI *SetSiblingIdxFn)(void *tr, int32_t idx, void *method);
    SetSiblingIdxFn set_sib = (SetSiblingIdxFn)CODE(CC_TR_SET_SIBLING_INDEX);

    void *b0 = (void *)load64((char *)old_arr + 0x20 + 0 * sizeof(void *));
    void *b1 = (void *)load64((char *)old_arr + 0x20 + 1 * sizeof(void *));
    void *b3 = (void *)load64((char *)old_arr + 0x20 + 3 * sizeof(void *));
    void *a0 = b0 ? (void *)load64((char *)b0 + 0x10) : NULL;
    void *a1 = b1 ? (void *)load64((char *)b1 + 0x10) : NULL;
    void *a3 = b3 ? (void *)load64((char *)b3 + 0x10) : NULL;
    if (!uobj_alive(a0) || !uobj_alive(a1) || !uobj_alive(a3)) return;

    void *tr0 = comp_get_tr(a0, NULL);
    void *tr1 = comp_get_tr(a1, NULL);
    void *tr3 = comp_get_tr(a3, NULL);
    void *parent_tr = uobj_alive(tr1) ? get_parent(tr1, NULL) : NULL;
    if (!uobj_alive(parent_tr)) return;

    cda_freeze_button_grid(parent_tr);

    UGuiVec2 p0 = get_apos(tr0, NULL);
    UGuiVec2 p3 = get_apos(tr3, NULL);
    float y = p0.y - 8.0f;
    float span = p3.x - p0.x;
    if (span < 50.0f) {
        span = 540.0f;
        p0.x = -270.0f;
        p3.x = 270.0f;
    }

    void *arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
    if (!arr) return;
    (void)gc_pin(arr, false);
    memcpy((char *)arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));

    /* Clone Expert (slot 1) for both Beginner (slot 4) and Chaos (slot 5) */
    const int extra_slots[2] = {4, 5};
    for (int k = 0; k < 2; ++k) {
        int slot = extra_slots[k];
        void *new_btn = clone_object(b1);
        if (!new_btn) continue;
        (void)gc_pin(new_btn, false);

        void *new_anim = clone_comp(a1, parent_tr, 0);
        if (!uobj_alive(new_anim)) continue;
        (void)gc_pin(new_anim, false);

        put64((char *)new_btn + 0x10, (uint64_t)new_anim);
        void *new_tr = comp_get_tr(new_anim, NULL);
        void *new_go = comp_get_go(new_anim, NULL);
        if (uobj_alive(new_go)) {
            set_name(new_go, make_string(NULL, slot == 4 ? "DifficultyButton_Beginner" : "DifficultyButton_Chaos"), NULL);
        }

        void *src_arrow_root = (void *)load64((char *)b1 + 0x18);
        void *src_left_arrow = (void *)load64((char *)b1 + 0x20);
        void *src_right_arrow = (void *)load64((char *)b1 + 0x28);
        void *new_arrow_root = cda_map_cloned_go(tr1, new_tr, src_arrow_root);
        void *new_left_arrow = cda_map_cloned_go(tr1, new_tr, src_left_arrow);
        void *new_right_arrow = cda_map_cloned_go(tr1, new_tr, src_right_arrow);
        put64((char *)new_btn + 0x18, (uint64_t)new_arrow_root);
        put64((char *)new_btn + 0x20, (uint64_t)new_left_arrow);
        put64((char *)new_btn + 0x28, (uint64_t)new_right_arrow);

        cda_set_button_label(new_tr, slot == 4 ? "Beginner" : "Chaos");
        void *skip_tr = uobj_alive(new_arrow_root) ? go_get_tr(new_arrow_root, NULL) : NULL;
        cda_skin_hierarchy(new_tr, skip_tr, slot);

        put64((char *)arr + 0x20 + (size_t)slot * sizeof(void *), (uint64_t)new_btn);
    }

    float center_x = (p0.x + p3.x) * 0.5f;
    float expanded_span = span * 1.07f;
    float start_x = center_x - expanded_span * 0.5f;
    float step = expanded_span / 5.0f;
    float scale = 0.54f;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        int slot = s_difficulty_order[i];
        void *btn = (void *)load64((char *)arr + 0x20 + (size_t)slot * sizeof(void *));
        void *anim = btn ? (void *)load64((char *)btn + 0x10) : NULL;
        void *tr = uobj_alive(anim) ? comp_get_tr(anim, NULL) : NULL;
        if (uobj_alive(tr)) {
            set_apos(tr, (UGuiVec2){start_x + (float)i * step, y}, NULL);
            set_scale(tr, (UGuiVec3){scale, scale, 1.0f}, NULL);
            set_sib(tr, i, NULL);
        }
    }

    put64((char *)self + 0x18, (uint64_t)arr);
}

/* DifficultyListBehaviour::SetDifficulty(this, int diff, bool isSupremeActive, bool isOneDiff)
 * 1:1 port of Switch difficulty_ui_controls.hpp: ALWAYS shows all 6 difficulty tabs
 * (Beginner, Basic, Expert, Ultimate, Supreme, Chaos) and greys out unauthored slots via
 * PlayUnSelectAnim(button, fixed || !hasChart). */
static ABI int hook_diff_list_set_difficulty(HookRegs *r) {
    void *self = (void *)r->rdi;
    int req_slot = (int)r->rsi;
    bool supreme = (r->rdx & 1) != 0;
    bool fixed = (r->rcx & 1) != 0;
    if (!self) return 0;

    cda_ensure_diff_list_buttons(self);
    void *arr = (void *)load64((char *)self + 0x18);
    if (!arr || load64((char *)arr + 0x18) < MAX_SLOTS) {
        r->rsi = (uint64_t)clamp_extra_slot(req_slot);
        return 0;
    }

    const DiffListCtxEntry *ctx = get_diff_list_ctx(self);
    const CustomSong *song = ctx ? ctx->song : NULL;

    uint8_t mask = 0;
    int selected = req_slot;
    if (song) {
        for (int s = 0; s < MAX_SLOTS; ++s) {
            if (song_has_chart(song, s)) mask |= (uint8_t)(1u << s);
        }
        selected = resolve_song_slot(song, req_slot, song->last_selected_slot);
        fixed = fixed || (count_song_charts(song) <= 1);
    } else {
        selected = clamp_extra_slot(req_slot);
        if (!supreme && selected == 3) selected = 2;
        mask = (uint8_t)((supreme ? 0x0Fu : 0x07u) |
                         ((selected >= 0 && selected < MAX_SLOTS) ? (1u << selected) : 0u));
    }

    typedef void (ABI *ButtonBaseSetActiveFn)(void *base, bool active, bool oneDiff, void *method);
    ButtonBaseSetActiveFn base_set_active = (ButtonBaseSetActiveFn)CODE(CC_DIFF_BASE_SET_ACTIVE);
    typedef void (ABI *DiffBtnSetActiveFn)(void *btn, bool active, void *method);
    DiffBtnSetActiveFn btn_set_active = (DiffBtnSetActiveFn)CODE(CC_DIFF_BTN_SET_ACTIVE);
    typedef void (ABI *PlayDiffAnimFn)(void *btn, bool transit, void *method);
    PlayDiffAnimFn play_sel = (PlayDiffAnimFn)CODE(CC_DIFF_BTN_PLAY_SEL);
    PlayDiffAnimFn play_unsel = (PlayDiffAnimFn)CODE(CC_DIFF_BTN_PLAY_UNSEL);
    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);

    void *base_20 = (void *)load64((char *)self + 0x20);
    void *base_28 = (void *)load64((char *)self + 0x28);
    if (base_20) base_set_active(base_20, true, fixed, NULL);
    if (base_28) base_set_active(base_28, false, true, NULL);

    int first = selected;
    int last = selected;
    bool have_first = false;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        int slot = s_difficulty_order[i];
        if (!(mask & (1u << slot))) continue;
        if (!have_first) {
            first = slot;
            have_first = true;
        }
        last = slot;
    }

    bool changed = ((int)load32((char *)self + 0x34) != selected);
    bool arrows = (*(uint8_t *)((char *)self + 0x30) != 0) && !fixed;

    for (int slot = 0; slot < MAX_SLOTS; ++slot) {
        void *btn = (void *)load64((char *)arr + 0x20 + (size_t)slot * sizeof(void *));
        if (!btn) continue;
        btn_set_active(btn, true, NULL);
        bool has_chart = (mask & (1u << slot)) != 0;
        if (slot == selected && has_chart) {
            play_sel(btn, changed, NULL);
        } else {
            play_unsel(btn, fixed || !has_chart, NULL);
        }

        void *arrow_root = (void *)load64((char *)btn + 0x18);
        void *left_arrow = (void *)load64((char *)btn + 0x20);
        void *right_arrow = (void *)load64((char *)btn + 0x28);
        if (uobj_alive(arrow_root)) set_active(arrow_root, arrows && (slot == selected), NULL);
        if (uobj_alive(left_arrow)) set_active(left_arrow, selected != first, NULL);
        if (uobj_alive(right_arrow)) set_active(right_arrow, selected != last, NULL);
    }

    put32((char *)self + 0x34, (uint32_t)selected);
    r->rax = 0;
    return 1;
}

static void *s_cda_level_base_mat_beginner = NULL;
static void *s_cda_level_base_mat_chaos = NULL;

/* MusicLevelBaseBehaviour::SetDifficulty(this, int diff, int level)
 * 1:1 port of Switch difficulty_ui_controls.hpp EnsureLevels + Level */
static ABI int hook_level_base_set_difficulty(HookRegs *r) {
    void *self = (void *)r->rdi;
    int slot = (int)r->rsi;
    int level = (int)r->rdx;
    if (!self) return 0;

    void *old_bases = (void *)load64((char *)self + 0x18);
    void *old_texts = (void *)load64((char *)self + 0x20);
    if (!old_bases || !old_texts) return 0;

    uint64_t blen = load64((char *)old_bases + 0x18);
    uint64_t tlen = load64((char *)old_texts + 0x18);
    if (blen < 4 || tlen < 4) return 0;

    void *bases = old_bases;
    void *texts = old_texts;
    if (blen < MAX_SLOTS) {
        bases = alloc_managed_array_by_arr_class(object_class(old_bases), MAX_SLOTS);
        if (bases) {
            (void)gc_pin(bases, false);
            memcpy((char *)bases + 0x20, (char *)old_bases + 0x20, 4 * sizeof(void *));
            put64((char *)self + 0x18, (uint64_t)bases);
        } else {
            bases = old_bases;
        }
    }
    if (tlen < MAX_SLOTS) {
        texts = alloc_managed_array_by_arr_class(object_class(old_texts), MAX_SLOTS);
        if (texts) {
            (void)gc_pin(texts, false);
            memcpy((char *)texts + 0x20, (char *)old_texts + 0x20, 4 * sizeof(void *));
            put64((char *)self + 0x20, (uint64_t)texts);
        } else {
            texts = old_texts;
        }
    }

    GetTransformFn go_get_tr = (GetTransformFn)CODE(CC_GO_GET_TRANSFORM);
    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
    CloneWithParentFn clone_obj = (CloneWithParentFn)CODE(CC_UOBJ_CLONE_WITH_PARENT);
    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);
    SetSpriteFn set_spr = (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);
    typedef void *(ABI *GetTmpMatFn)(void *tmp, void *method);
    typedef void (ABI *SetTmpMatFn)(void *tmp, void *mat, void *method);
    GetTmpMatFn get_tmp_mat = (GetTmpMatFn)CODE(CC_TMP_GET_SHARED_MAT);
    SetTmpMatFn set_shared_mat = (SetTmpMatFn)CODE(CC_TMP_SET_SHARED_MAT);
    SetTmpMatFn set_tmp_mat = (SetTmpMatFn)CODE(CC_TMP_SET_FONT_MAT);

    if (load64((char *)bases + 0x18) >= MAX_SLOTS && load64((char *)texts + 0x18) >= MAX_SLOTS) {
        void *src_base = (void *)load64((char *)bases + 0x20 + 1 * sizeof(void *)); /* Expert (1) */
        void *src_tmp = (void *)load64((char *)texts + 0x20 + 1 * sizeof(void *));  /* Expert (1) */
        void *src_tr = uobj_alive(src_base) ? go_get_tr(src_base, NULL) : NULL;
        void *parent_tr = uobj_alive(src_tr) ? get_parent(src_tr, NULL) : NULL;

        const int extra_slots[2] = {4, 5};
        for (int k = 0; k < 2; ++k) {
            int es = extra_slots[k];
            void *b_go = (void *)load64((char *)bases + 0x20 + (size_t)es * sizeof(void *));
            void *t_comp = (void *)load64((char *)texts + 0x20 + (size_t)es * sizeof(void *));
            if ((!uobj_alive(b_go) || !uobj_alive(t_comp)) && uobj_alive(src_base) && uobj_alive(parent_tr)) {
                if (!uobj_alive(b_go)) {
                    b_go = clone_obj(src_base, parent_tr, 0);
                }
                if (uobj_alive(b_go)) {
                    (void)gc_pin(b_go, false);
                    void *clone_tr = go_get_tr(b_go, NULL);

                    /* Map the cloned TextMeshProUGUI component from src_tr -> clone_tr */
                    void *src_tmp_go = uobj_alive(src_tmp) ? comp_get_go(src_tmp, NULL) : NULL;
                    void *dst_tmp_go = cda_map_cloned_go(src_tr, clone_tr, src_tmp_go);
                    void *tmp = cda_get_comp(dst_tmp_go, "TMPro.TextMeshProUGUI", "TextMeshProUGUI");
                    if (!uobj_alive(tmp)) {
                        tmp = cda_find_comp_in_children(clone_tr, "TMPro.TextMeshProUGUI", "TextMeshProUGUI", 0);
                    }

                    /* Skin the entire cloned LevelBase hierarchy (Image + TMP + UIGradient) */
                    cda_skin_hierarchy(clone_tr, NULL, es);

                    /* Belt-and-suspenders: explicitly apply LevelBase artwork sprite & TMP material */
                    void *img = cda_find_comp_in_children(clone_tr, "UnityEngine.UI.Image", "Image", 0);
                    void *spr = cda_artwork_sprite(es == 4 ? &cda_kBeginnerMusicLevelBase : &cda_kChaosMusicLevelBase);
                    if (uobj_alive(img) && spr) {
                        cda_register_image_skin(img, spr, es);
                        set_spr(img, spr, NULL);
                    }

                    if (uobj_alive(tmp)) {
                        (void)gc_pin(tmp, false);
                        void *orig_mat = uobj_alive(src_tmp) ? get_tmp_mat(src_tmp, NULL) : get_tmp_mat(tmp, NULL);
                        void *new_mat = (es == 4) ? s_cda_level_base_mat_beginner : s_cda_level_base_mat_chaos;
                        if (!unity_native_alive(new_mat) && uobj_alive(orig_mat)) {
                            new_mat = cda_clone_tmp_material(orig_mat, es);
                            if (es == 4) s_cda_level_base_mat_beginner = new_mat;
                            else s_cda_level_base_mat_chaos = new_mat;
                        }
                        if (new_mat) {
                            set_shared_mat(tmp, new_mat, NULL);
                            set_tmp_mat(tmp, new_mat, NULL);
                        }
                        put64((char *)texts + 0x20 + (size_t)es * sizeof(void *), (uint64_t)tmp);
                    }
                    set_active(b_go, false, NULL);
                    put64((char *)bases + 0x20 + (size_t)es * sizeof(void *), (uint64_t)b_go);
                }
            }
        }
    }

    if (slot < 0 || slot >= MAX_SLOTS) slot = clamp_extra_slot(slot);
    uint64_t cur_blen = load64((char *)bases + 0x18);
    for (uint64_t i = 0; i < cur_blen && i < MAX_SLOTS; ++i) {
        void *b_go = (void *)load64((char *)bases + 0x20 + i * sizeof(void *));
        if (uobj_alive(b_go)) {
            set_active(b_go, (int)i == slot, NULL);
        }
    }

    if ((uint64_t)slot < load64((char *)texts + 0x18)) {
        void *tmp = (void *)load64((char *)texts + 0x20 + (size_t)slot * sizeof(void *));
        if (uobj_alive(tmp)) {
            static void *s_level_strs[32];
            void *gstr = NULL;
            if (level >= 0 && level < 32) {
                if (!s_level_strs[level]) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", level);
                    s_level_strs[level] = make_pinned_string(buf, NULL);
                }
                gstr = s_level_strs[level];
            } else {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", level);
                gstr = make_string(NULL, buf);
            }
            void *klass = object_class(tmp);
            if (klass && gstr) {
                typedef void (ABI *SetTmpTextVirtFn)(void *self, void *str, void *method);
                SetTmpTextVirtFn fn = (SetTmpTextVirtFn)load64((const char *)klass + 0x558);
                void *m = (void *)load64((const char *)klass + 0x560);
                if (fn) fn(tmp, gstr, m);
            }
            if (slot == 4 || slot == 5) {
                void *src_tmp = (void *)load64((char *)texts + 0x20 + 1 * sizeof(void *));
                void *src_mat = uobj_alive(src_tmp) ? get_tmp_mat(src_tmp, NULL) : NULL;
                void *new_mat = (slot == 4) ? s_cda_level_base_mat_beginner : s_cda_level_base_mat_chaos;
                if (!unity_native_alive(new_mat) && uobj_alive(src_mat)) {
                    new_mat = cda_clone_tmp_material(src_mat, slot);
                    if (slot == 4) s_cda_level_base_mat_beginner = new_mat;
                    else s_cda_level_base_mat_chaos = new_mat;
                }
                if (unity_native_alive(new_mat) && get_tmp_mat(tmp, NULL) != new_mat) {
                    set_shared_mat(tmp, new_mat, NULL);
                    set_tmp_mat(tmp, new_mat, NULL);
                }
            }
        }
    }

    r->rax = 0;
    return 1;
}

/* MusicSelectUtility::ChangeToViewDifficulty(int slot, MusicID mid) */
static ABI int hook_change_to_view_difficulty(HookRegs *r) {
    void *mid = (void *)r->rsi;
    CustomSong *song = find_song_by_obj(mid);
    if (song) {
        int req = (s_global_requested_slot >= 0 && s_global_requested_slot < MAX_SLOTS)
                  ? s_global_requested_slot : (int)r->rdi;
        int slot = resolve_song_slot(song, req, song->last_selected_slot);
        song->last_selected_slot = slot;
        r->rax = (uint64_t)slot;
        return 1;
    }
    r->rdi = (uint64_t)clamp_extra_slot((int)r->rdi);
    return 0;
}

/* SelectingMSData::.ctor(this, MusicID mid, int slot, ...) */
static ABI int hook_selecting_ms_ctor(HookRegs *r) {
    s_gameplay_chart_slot = -1;
    void *mid = (void *)r->rsi;
    CustomSong *song = find_song_by_obj(mid);
    int slot = resolve_slot_for_id(mid, (int)r->rdx);
    r->rdx = (uint64_t)slot;
    if (song) song->last_selected_slot = slot;
    return 0;
}

/* SelectingMSData::set_Difficulty(this, int slot) — replaces 4-byte mov [rdi+0x2c], esi; ret */
static ABI int hook_selecting_ms_set_diff(HookRegs *r) {
    void *self = (void *)r->rdi;
    if (self) {
        void *mid = (void *)load64((char *)self + 0x10);
        int prev = (int)load32((char *)self + 0x2c);
        CustomSong *song = find_song_by_obj(mid);
        int slot = song ? resolve_song_slot(song, (int)r->rsi, prev) : clamp_extra_slot((int)r->rsi);
        if (song) song->last_selected_slot = slot;
        put32((char *)self + 0x2c, (uint32_t)slot);
    }
    r->rax = 0;
    return 1;
}

/* SelectingMSData::get_DifficultyValue(this, int diff) */
static ABI int hook_selecting_get_diff_value(HookRegs *r) {
    void *self = (void *)r->rdi;
    int diff = (int)r->rsi;
    int32_t val = 0;
    if (self) {
        void *table = (void *)load64((char *)self + 0x18);
        void *arr = table ? (void *)load64((char *)table + 0x98) : NULL;
        if (arr && diff >= 0 && (uint64_t)diff < load64((char *)arr + 0x18)) {
            val = (int32_t)load32((char *)arr + 0x20 + (size_t)diff * 4);
        } else {
            CustomSong *song = find_song_by_obj((void *)load64((char *)self + 0x10));
            if (song && diff >= 0 && diff < MAX_SLOTS) {
                val = song->charts[diff].rating;
            }
        }
    }
    r->rax = (uint64_t)(uint32_t)val;
    return 1;
}

/* ListItemMusicPanel::.ctor(this, MusicID mid, int slot, int style, bool fixedDiff) */
static ABI int hook_panel_ctor(HookRegs *r) {
    if (!(r->r8 & 1)) {
        r->rdx = (uint64_t)resolve_slot_for_id((void *)r->rsi, (int)r->rdx);
    }
    return 0;
}

/* ListItemMusicPanel::SetDifficulty(this, int slot) */
static ABI int hook_panel_set_diff(HookRegs *r) {
    void *self = (void *)r->rdi;
    if (self) {
        void *mid = (void *)load64((char *)self + 0x18);
        int prev = (int)load32((char *)self + 0x2c);
        CustomSong *song = find_song_by_obj(mid);
        r->rsi = (uint64_t)(song ? resolve_song_slot(song, (int)r->rsi, prev) : clamp_extra_slot((int)r->rsi));
    }
    return 0;
}

static void *original_menu_add_song = NULL;

/* MusicSelectMusicListPartsMain::AddMusicPanel(this, MusicID mid) */
static ABI int hook_menu_add_song(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *mid = (void *)r->rsi;
    typedef bool (ABI *AddSongFn)(void *self, void *mid, void *method);
    bool res = original_menu_add_song ? ((AddSongFn)original_menu_add_song)(self, mid, (void *)r->rdx) : false;

    CustomSong *song = res ? find_song_by_obj(mid) : NULL;
    if (song && self) {
        static void *s_panel_class = NULL;
        if (!s_panel_class) s_panel_class = find_il2cpp_class("Game.Scene.MusicSelect", "ListItemMusicPanel");
        void *panel_class = s_panel_class;
        typedef void (ABI *PanelCtorFn)(void *panel, void *mid, int32_t slot, int32_t style, bool fixed, void *method);
        typedef void (ABI *UIListAddFn)(void *uilist, void *item, void *method);
        PanelCtorFn panel_ctor = (PanelCtorFn)CODE(CC_PANEL_CTOR);
        UIListAddFn uilist_add = (UIListAddFn)CODE(CC_UILIST_ADD_ITEM);

        const int extra_slots[2] = {4, 5};
        for (int k = 0; k < 2; ++k) {
            int slot = extra_slots[k];
            if (!song_has_chart(song, slot) || !panel_class) continue;
            void *panel = alloc_managed_object(panel_class);
            if (!panel) continue;
            uint32_t proot = gc_pin(panel, false);
            panel_ctor(panel, mid, slot, 0, true, NULL);
            void *ui_list = (void *)load64((char *)self + 0x90);
            if (ui_list) uilist_add(ui_list, panel, NULL);
            void *backing_list = (void *)load64((char *)self + 0xb0);
            if (backing_list) list_add(backing_list, panel);
            gc_unpin(proot);
        }
    }
    r->rax = res ? 1 : 0;
    return 1;
}

/* MusicSelectMusicListPartsMain::FilterMusicPanel — hide missing fixed-difficulty slots (e.g. no Basic slot 0) */
static ABI int hook_menu_filter_panel(HookRegs *r) {
    void *panel = (void *)r->rsi;
    if (panel && *(bool *)((char *)panel + 0x40)) {
        CustomSong *song = find_song_by_obj((void *)load64((char *)panel + 0x18));
        int slot = (int)load32((char *)panel + 0x2c);
        if (song && !song_has_chart(song, slot)) {
            r->rax = 0;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_menu_is_playable(HookRegs *r) {
    if (r->rsi) {
        void *mid = *(void **)((char *)r->rsi + 0x18);
        if (find_song_by_obj(mid)) {
            r->rax = 1;
            return 1;
        }
    }
    return 0;
}

static ABI int hook_list_change_view_diff(HookRegs *r) {
    int req = (int)r->rsi;
    if (req >= 0 && req < MAX_SLOTS) {
        s_global_requested_slot = req;
    }
    if (r->rdi) {
        /* Force _viewDifficulty (+0xc0) to -1 so ChangeDifficulty (0x168e160) never early-outs
         * at 0x168e292 when scrolling between custom songs with different difficulty sets. */
        put32((char *)r->rdi + 0xc0, (uint32_t)-1);
    }
    return 0;
}

static ABI int hook_sort_refresh_badges(HookRegs *r) {
    r->rsi = (uint64_t)clamp_extra_slot((int)r->rsi);
    return 0;
}

static void cda_ensure_jacket_ribbons(void *panel) {
    if (!panel) return;
    expand_ref_array_copy(panel, 0x120);
    void *arr = (void *)load64((char *)panel + 0x120);
    if (!arr || load64((char *)arr + 0x18) < MAX_SLOTS) return;

    void *r1 = (void *)load64((char *)arr + 0x20 + 1 * sizeof(void *));
    void *r4 = (void *)load64((char *)arr + 0x20 + 4 * sizeof(void *));
    void *r5 = (void *)load64((char *)arr + 0x20 + 5 * sizeof(void *));
    if (unity_native_alive(r1)) {
        if (!unity_native_alive(r4) || r4 == (void *)load64((char *)arr + 0x20 + 0 * sizeof(void *))) {
            void *b4 = cda_blue_variant(r1, 4);
            if (b4) put64((char *)arr + 0x20 + 4 * sizeof(void *), (uint64_t)b4);
        }
        if (!unity_native_alive(r5) || r5 == (void *)load64((char *)arr + 0x20 + 3 * sizeof(void *))) {
            void *c5 = cda_blue_variant(r1, 5);
            if (c5) put64((char *)arr + 0x20 + 5 * sizeof(void *), (uint64_t)c5);
        }
    }
}

static void *original_jacket_refresh_view = NULL;

/* MusicSelectJacketPartsMain::RefreshView(this, SelectingMSData sel, MusicTableData tbl, bool diffSort) */
static ABI int hook_jacket_refresh_view(HookRegs *r) {
    void *panel = (void *)r->rdi;
    void *selection = (void *)r->rsi;
    void *table_data = (void *)r->rdx;
    bool diff_sort = (r->rcx & 1) != 0;
    void *method = (void *)r->r8;

    if (panel) cda_ensure_jacket_ribbons(panel);
    if (table_data) expand_int_array(table_data, 0x98);

    void *old_sel = panel ? (void *)load64((char *)panel + 0x130) : NULL;
    CustomSong *song = selection ? find_song_by_obj((void *)load64((char *)selection + 0x10)) : NULL;

    if (panel) {
        set_diff_list_ctx((void *)load64((char *)panel + 0xe0), song, false);
        if (!song) {
            cda_apply_ms_mark((void *)load64((char *)panel + 0xc0), false);
        }
    }

    if (song && selection) {
        int cur = (int)load32((char *)selection + 0x2c);
        int slot = resolve_song_slot(song, cur, song->last_selected_slot);
        song->last_selected_slot = slot;
        put32((char *)selection + 0x2c, (uint32_t)slot);
        if (panel) {
            put32((char *)panel + 0x160, (uint32_t)slot);
            if (slot == 4 && old_sel) {
                put32((char *)old_sel + 0x2c, 4);
            }
            draw_diff_control((void *)load64((char *)panel + 0xe0), song, slot, false);
        }
    } else {
        s_global_requested_slot = -1;
        if (selection) {
            int safe = clamp_extra_slot((int)load32((char *)selection + 0x2c));
            put32((char *)selection + 0x2c, (uint32_t)safe);
            if (panel) put32((char *)panel + 0x160, (uint32_t)safe);
        }
        if (old_sel) {
            put32((char *)old_sel + 0x2c, (uint32_t)clamp_extra_slot((int)load32((char *)old_sel + 0x2c)));
        }
    }

    if (original_jacket_refresh_view) {
        typedef void (ABI *JacketRefreshFn)(void *panel, void *sel, void *tbl, bool sort, void *method);
        ((JacketRefreshFn)original_jacket_refresh_view)(panel, selection, table_data, diff_sort, method);
    }

    if (song && panel) {
        if (song->managed_title) {
            void *beh = (void *)load64((char *)panel + 0x100);
            if (beh) {
                typedef void (ABI *SetMusicNameFn)(void *beh, void *str, void *method);
                ((SetMusicNameFn)CODE(CC_MUSIC_NAME_SET_NAME))(beh, song->managed_title, NULL);
            }
        }
        cda_apply_ms_mark((void *)load64((char *)panel + 0xc0), true);
    }
    r->rax = 0;
    return 1;
}

/* MusicSelectJacketPartsMain::ChangePlayData(this, SelectingMSData ms, MusicTableData tbl, int dispDiff) */
static ABI int hook_jacket_change_play_data(HookRegs *r) {
    void *ms_data = (void *)r->rsi;
    void *table_data = (void *)r->rdx;
    int disp = (int)r->rcx;
    if (ms_data && !find_song_by_obj((void *)load64((char *)ms_data + 0x10))) {
        int safe = clamp_extra_slot(disp);
        void *arr = table_data ? (void *)load64((char *)table_data + 0x98) : NULL;
        if (arr) {
            int len = (int)load32((char *)arr + 0x18);
            if (len > 0 && safe >= len) safe = len - 1;
        }
        put32((char *)ms_data + 0x2c, (uint32_t)safe);
        r->rcx = (uint64_t)safe;
    }
    return 0;
}

/* MusicSelectJacketPartsMain::SetDifficulty(this, SelectingMSData sel, bool force) */
static ABI int hook_jacket_set_difficulty(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *selection = (void *)r->rsi;
    if (self) cda_ensure_jacket_ribbons(self);
    if (selection) {
        void *mid = (void *)load64((char *)selection + 0x10);
        CustomSong *song = find_song_by_obj(mid);
        int slot = resolve_slot_for_id(mid, (int)load32((char *)selection + 0x2c));
        put32((char *)selection + 0x2c, (uint32_t)slot);
        if (self) set_diff_list_ctx((void *)load64((char *)self + 0xe0), song, false);
        if (song) {
            song->last_selected_slot = slot;
            if (self) draw_diff_control((void *)load64((char *)self + 0xe0), song, slot, false);
        }
    }
    return 0;
}

static void *allocate_jacket_reaction(int next_slot) {
    static void *s_reaction_class = NULL;
    if (!s_reaction_class) {
        ensure_il2cpp_api();
        void *jacket_class = find_il2cpp_class("Game.Scene.MusicSelect", "MusicSelectJacketPartsMain");
        if (jacket_class && il2cpp_class_get_nested_types_fn) {
            void *iter = NULL;
            void *nested = NULL;
            while ((nested = il2cpp_class_get_nested_types_fn(jacket_class, &iter)) != NULL) {
                const char *nname = *(const char **)((const char *)nested + 0x10);
                if (nname && !strcmp(nname, "SceneReactionData")) {
                    if (il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(nested);
                    s_reaction_class = nested;
                    break;
                }
            }
        }
    }
    void *reaction_class = s_reaction_class;
    void *reaction = alloc_managed_object(reaction_class);
    if (reaction) {
        put32((char *)reaction + 0x10, 0);
        put32((char *)reaction + 0x14, (uint32_t)next_slot);
    }
    return reaction;
}

/* MusicSelectJacketPartsMain::OnExecutedScenePartsMain — v1.0.4 inlines OnChangeDifficultyReaction! */
static ABI int hook_jacket_execute(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *source = (void *)r->rsi;
    if (!self || !source || load32((char *)self + 0x15c) != 0) return 0;

    void *selection = (void *)load64((char *)self + 0x130);
    if (!selection) return 0;
    CustomSong *song = find_song_by_obj((void *)load64((char *)selection + 0x10));
    if (!song) return 0;

    int act = (int)load32((char *)source + 0x20);
    if (act != 0 && act != 1) return 0;

    /* Consume UIObject action identically to native path */
    *(bool *)((char *)source + 0x30) = false;
    typedef void (ABI *ActionFn)(void *action, void *method);
    ((ActionFn)CODE(CC_UIOBJECT_ON_CHANGED_FLAG))(source, NULL); /* UIObject::onChangedActionFlag */
    ((ActionFn)CODE(CC_UIOBJECT_END_ACTION))(source, NULL); /* UIObject::EndAction */

    int change_type = (act == 0) ? 1 : 0;
    int sel_slot = (int)load32((char *)selection + 0x2c);
    int cur = resolve_song_slot(song, sel_slot, song->last_selected_slot);
    int next = step_difficulty(song, cur, change_type);
    if (next == cur) {
        r->rax = 0;
        return 1;
    }

    s_global_requested_slot = next;
    song->last_selected_slot = next;

    typedef void (ABI *PlaySEFn)(int32_t diff, void *method);
    ((PlaySEFn)CODE(CC_DIFF_LIST_PLAY_SE))(next, NULL);

    r->rax = (uint64_t)allocate_jacket_reaction(next);
    return 1;
}

static ABI int hook_jacket_difficulty_reaction(HookRegs *r) {
    void *jacket_parts = (void *)r->rdi;
    int change_type = (int)r->rsi;
    void *closure = (void *)r->rdx;
    if (!jacket_parts || !closure) return 0;

    void *selection = (void *)load64((char *)jacket_parts + 0x130);
    if (!selection) return 0;

    CustomSong *song = find_song_by_obj((void *)load64((char *)selection + 0x10));
    if (!song) return 0;

    int sel_slot = (int)load32((char *)selection + 0x2c);
    int cur_slot = resolve_song_slot(song, sel_slot, song->last_selected_slot);
    int next_slot = step_difficulty(song, cur_slot, change_type);
    if (next_slot == cur_slot) {
        put64((char *)closure + 8, 0);
        r->rax = 0;
        return 1;
    }

    s_global_requested_slot = next_slot;
    song->last_selected_slot = next_slot;

    typedef void (ABI *PlaySEFn)(int32_t diff, void *method);
    ((PlaySEFn)CODE(CC_DIFF_LIST_PLAY_SE))(next_slot, NULL);

    void *reaction = allocate_jacket_reaction(next_slot);
    if (reaction) {
        put64((char *)closure + 8, (uint64_t)reaction);
    }
    r->rax = (uint64_t)reaction;
    return 1;
}

static void *original_panel_view_on_set_view = NULL;

/* ListItemMusicPanelView::OnSetView(this, ItemViewParameter view) */
static void cda_ensure_panel_view_arrays(void *self) {
    if (!self) return;
    expand_ref_array_copy(self, 0xe8);
    expand_ref_array_copy(self, 0xf0);
    expand_ref_array_copy(self, 0xf8);
    expand_ref_array_copy(self, 0x118);

    void *plates = (void *)load64((char *)self + 0xe8);
    if (plates && load64((char *)plates + 0x18) >= MAX_SLOTS) {
        void *p0 = (void *)load64((char *)plates + 0x20 + 0 * sizeof(void *));
        void *p3 = (void *)load64((char *)plates + 0x20 + 3 * sizeof(void *));
        void *p4 = (void *)load64((char *)plates + 0x20 + 4 * sizeof(void *));
        void *p5 = (void *)load64((char *)plates + 0x20 + 5 * sizeof(void *));
        if (!unity_native_alive(p4) || p4 == p0) {
            void *np4 = cda_plate_sprite(4);
            if (np4) put64((char *)plates + 0x20 + 4 * sizeof(void *), (uint64_t)np4);
        }
        if (!unity_native_alive(p5) || p5 == p3) {
            void *np5 = cda_plate_sprite(5);
            if (np5) put64((char *)plates + 0x20 + 5 * sizeof(void *), (uint64_t)np5);
        }
    }
    void *lbases = (void *)load64((char *)self + 0xf0);
    if (lbases && load64((char *)lbases + 0x18) >= MAX_SLOTS) {
        void *b0 = (void *)load64((char *)lbases + 0x20 + 0 * sizeof(void *));
        void *b3 = (void *)load64((char *)lbases + 0x20 + 3 * sizeof(void *));
        void *b4 = (void *)load64((char *)lbases + 0x20 + 4 * sizeof(void *));
        void *b5 = (void *)load64((char *)lbases + 0x20 + 5 * sizeof(void *));
        if (!unity_native_alive(b4) || b4 == b0) {
            void *nb4 = cda_artwork_sprite(&cda_kBeginnerMusicLevelBase);
            if (nb4) put64((char *)lbases + 0x20 + 4 * sizeof(void *), (uint64_t)nb4);
        }
        if (!unity_native_alive(b5) || b5 == b3) {
            void *nb5 = cda_artwork_sprite(&cda_kChaosMusicLevelBase);
            if (nb5) put64((char *)lbases + 0x20 + 5 * sizeof(void *), (uint64_t)nb5);
        }
    }
    void *mats = (void *)load64((char *)self + 0xf8);
    if (mats && load64((char *)mats + 0x18) >= MAX_SLOTS) {
        void *m1 = (void *)load64((char *)mats + 0x20 + 1 * sizeof(void *));
        void *m4 = (void *)load64((char *)mats + 0x20 + 4 * sizeof(void *));
        void *m5 = (void *)load64((char *)mats + 0x20 + 5 * sizeof(void *));
        void *m0 = (void *)load64((char *)mats + 0x20 + 0 * sizeof(void *));
        void *m3 = (void *)load64((char *)mats + 0x20 + 3 * sizeof(void *));
        if (unity_native_alive(m1)) {
            if (!unity_native_alive(m4) || m4 == m0) {
                void *nm4 = cda_clone_tmp_material(m1, 4);
                if (nm4) put64((char *)mats + 0x20 + 4 * sizeof(void *), (uint64_t)nm4);
            }
            if (!unity_native_alive(m5) || m5 == m3) {
                void *nm5 = cda_clone_tmp_material(m1, 5);
                if (nm5) put64((char *)mats + 0x20 + 5 * sizeof(void *), (uint64_t)nm5);
            }
        }
    }
    void *spr118 = (void *)load64((char *)self + 0x118);
    if (spr118 && load64((char *)spr118 + 0x18) >= MAX_SLOTS) {
        void *s1 = (void *)load64((char *)spr118 + 0x20 + 1 * sizeof(void *));
        void *s4 = (void *)load64((char *)spr118 + 0x20 + 4 * sizeof(void *));
        void *s5 = (void *)load64((char *)spr118 + 0x20 + 5 * sizeof(void *));
        void *s0 = (void *)load64((char *)spr118 + 0x20 + 0 * sizeof(void *));
        void *s3 = (void *)load64((char *)spr118 + 0x20 + 3 * sizeof(void *));
        if (unity_native_alive(s1)) {
            if (!unity_native_alive(s4) || s4 == s0) {
                void *ns4 = cda_blue_variant(s1, 4);
                if (ns4) put64((char *)spr118 + 0x20 + 4 * sizeof(void *), (uint64_t)ns4);
            }
            if (!unity_native_alive(s5) || s5 == s3) {
                void *ns5 = cda_blue_variant(s1, 5);
                if (ns5) put64((char *)spr118 + 0x20 + 5 * sizeof(void *), (uint64_t)ns5);
            }
        }
    }
}

static ABI int hook_panel_view_on_set_view(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *view = (void *)r->rsi;
    void *method = (void *)r->rdx;

    cda_ensure_panel_view_arrays(self);

    CustomSong *song = NULL;
    if (view) {
        void *sel = (void *)load64((char *)view + 0x30);
        void *tbl = sel ? (void *)load64((char *)sel + 0x18) : NULL;
        if (tbl) expand_int_array(tbl, 0x98);

        /* On ItemViewParameter, MusicID is +0x18 and Difficulty is +0x28 (+0x2c is Ranking!) */
        void *mid = (void *)load64((char *)view + 0x18);
        int slot = (int)load32((char *)view + 0x28);
        song = find_song_by_obj(mid);
        if (!song) {
            int safe = clamp_extra_slot(slot);
            void *arr = tbl ? (void *)load64((char *)tbl + 0x98) : NULL;
            if (arr && safe == 3 && load64((char *)arr + 0x18) >= 4 && load32((char *)arr + 0x20 + 3 * 4) <= 0) {
                safe = 2;
            }
            put32((char *)view + 0x28, (uint32_t)safe);
        } else if (!song_has_chart(song, slot)) {
            put32((char *)view + 0x28, (uint32_t)resolve_song_slot(song, slot, song->last_selected_slot));
        }
    }

    GetSpriteFn get_spr = (GetSpriteFn)CODE(CC_IMAGE_GET_SPRITE);
    SetSpriteFn set_spr = (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);
    GetTransformFn comp_get_tr = (GetTransformFn)CODE(CC_COMP_GET_TRANSFORM);
    SetVec2Fn set_size = (SetVec2Fn)CODE(CC_RT_SET_SIZE_DELTA);

    if (self && !song) {
        cda_apply_ms_mark((void *)load64((char *)self + 0x60), false);
        void *ind_img = (void *)load64((char *)self + 0x80);
        if (uobj_alive(ind_img)) {
            void *orig_ind = cda_find_orig_sprite(s_cda_orig_ind_imgs, 64, ind_img);
            if (orig_ind) set_spr(ind_img, orig_ind, NULL);
            void *ind_rt = comp_get_tr(ind_img, NULL);
            if (uobj_alive(ind_rt)) {
                UGuiVec2 osz = {156.0f, 28.0f};
                for (size_t i = 0; i < 64; ++i) {
                    if (s_cda_orig_ind_imgs[i].img == ind_img && s_cda_orig_ind_imgs[i].orig_size.x > 1.0f) {
                        osz = s_cda_orig_ind_imgs[i].orig_size;
                        break;
                    }
                }
                set_size(ind_rt, osz, NULL);
            }
        }
    }

    if (original_panel_view_on_set_view) {
        typedef void (ABI *OnSetViewFn)(void *self, void *view, void *method);
        ((OnSetViewFn)original_panel_view_on_set_view)(self, view, method);
        cda_ensure_panel_view_arrays(self);
    }

    if (self && song) {
        cda_apply_ms_mark((void *)load64((char *)self + 0x60), true);
        void *ind_img = (void *)load64((char *)self + 0x80);
        if (uobj_alive(ind_img)) {
            void *cur_spr = get_spr(ind_img, NULL);
            void *ind_rt = comp_get_tr(ind_img, NULL);
            UGuiVec2 cur_sz = {156.0f, 28.0f};
            if (uobj_alive(ind_rt)) {
                typedef UGuiVec2 (ABI *GetVec2Fn)(void *rt, void *method);
                UGuiVec2 sd = ((GetVec2Fn)CODE(CC_RT_GET_SIZE_DELTA))(ind_rt, NULL);
                if (sd.x > 1.0f && sd.x < 200.0f) cur_sz = sd;
            }
            cda_remember_orig_sprite(s_cda_orig_ind_imgs, 64, ind_img, cur_spr, cur_sz);
            uint8_t mask = 0;
            for (int s = 0; s < MAX_SLOTS; ++s) {
                if (song_has_chart(song, s)) mask |= (uint8_t)(1u << s);
            }
            void *ind_spr = cda_indicator_sprite(mask);
            if (ind_spr) set_spr(ind_img, ind_spr, NULL);
            if (uobj_alive(ind_rt)) set_size(ind_rt, cur_sz, NULL);
        }
    }

    r->rax = 0;
    return 1;
}

static void *original_popup_on_ready = NULL;
static void *original_popup_set_status = NULL;

static void refresh_popup_ui(void *window_setup, void *setting, const CustomSong *song) {
    if (!window_setup || !setting || !song) return;
    int slot = (int)load32((char *)setting + 0x18);
    draw_diff_control((void *)load64((char *)window_setup + 0xa8), song, slot, true);
    void *status = (void *)load64((char *)window_setup + 0xb0);
    if (status) {
        typedef void (ABI *SetStatusFn)(void *status, void *setting, bool skipAnim, void *method);
        ((SetStatusFn)CODE(CC_MUSIC_STATUS_SET_STATUS))(status, setting, false, NULL);
    }
}

static ABI int hook_popup_on_ready(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *setting = self ? (void *)load64((char *)self + 0xd0) : NULL;
    CustomSong *song = NULL;
    if (setting) {
        void *mid = (void *)load64((char *)setting + 0x10);
        song = find_song_by_obj(mid);
        int slot = resolve_slot_for_id(mid, (int)load32((char *)setting + 0x18));
        put32((char *)setting + 0x18, (uint32_t)slot);
        if (song) song->last_selected_slot = slot;
    }
    if (self) {
        set_diff_list_ctx((void *)load64((char *)self + 0xa8), song, true);
    }
    if (original_popup_on_ready) {
        typedef void (ABI *ReadyFn)(void *self, void *method);
        ((ReadyFn)original_popup_on_ready)(self, (void *)r->rsi);
    }
    if (self && setting && song) {
        draw_diff_control((void *)load64((char *)self + 0xa8), song, (int)load32((char *)setting + 0x18), true);
    }
    r->rax = 0;
    return 1;
}

static ABI int hook_popup_set_status(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *setting = (void *)r->rsi;
    bool skip_anim = (r->rdx & 1) != 0;
    void *method = (void *)r->rcx;
    CustomSong *song = NULL;
    if (setting) {
        void *mid = (void *)load64((char *)setting + 0x10);
        song = find_song_by_obj(mid);
        int slot = resolve_slot_for_id(mid, (int)load32((char *)setting + 0x18));
        put32((char *)setting + 0x18, (uint32_t)slot);
        if (song) song->last_selected_slot = slot;
    }
    if (original_popup_set_status) {
        typedef void (ABI *SetStatusFn)(void *self, void *setting, bool skip, void *method);
        ((SetStatusFn)original_popup_set_status)(self, setting, skip_anim, method);
        if (self) {
            cda_apply_ms_mark((void *)load64((char *)self + 0x30), song != NULL);
        }
        r->rax = 0;
        return 1;
    }
    return 0;
}

static bool popup_missing_start_chart(void *self, void *source) {
    if (!self || !source) return false;
    int act = (int)load32((char *)source + 0x20);
    if (act < 10 || act > 12) return false;
    void *setting = (void *)load64((char *)self + 0xd0);
    if (!setting) return false;
    CustomSong *song = find_song_by_obj((void *)load64((char *)setting + 0x10));
    return song && !song_has_chart(song, (int)load32((char *)setting + 0x18));
}

static ABI int hook_popup_submit(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *source = (void *)r->rsi;
    if (popup_missing_start_chart(self, source)) {
        *(bool *)((char *)self + 0xd8) = false;
        r->rax = 0;
        return 1;
    }
    return 0;
}

static ABI int hook_popup_execute(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *source = (void *)r->rsi;
    if (popup_missing_start_chart(self, source)) {
        *(bool *)((char *)self + 0xd8) = false;
        void *setting = (void *)load64((char *)self + 0xd0);
        if (setting) {
            CustomSong *song = find_song_by_obj((void *)load64((char *)setting + 0x10));
            if (song) {
                int slot = resolve_song_slot(song, (int)load32((char *)setting + 0x18), song->last_selected_slot);
                put32((char *)setting + 0x18, (uint32_t)slot);
                song->last_selected_slot = slot;
                refresh_popup_ui(self, setting, song);
            }
        }
        r->rax = 0;
        return 1;
    }
    return 0;
}

static ABI int hook_window_setup_change_difficulty(HookRegs *r) {
    void *window_setup = (void *)r->rdi;
    int change_type = (int)r->rsi;
    if (!window_setup) return 0;

    void *setting = (void *)load64((char *)window_setup + 0xd0);
    if (!setting) return 0;

    CustomSong *song = find_song_by_obj((void *)load64((char *)setting + 0x10));
    if (!song) return 0;

    int cur_slot = (int)load32((char *)setting + 0x18);
    int next_slot = song_has_chart(song, cur_slot)
                    ? step_difficulty(song, cur_slot, change_type)
                    : resolve_song_slot(song, cur_slot, song->last_selected_slot);
    if (next_slot == cur_slot || !song_has_chart(song, next_slot)) {
        r->rax = 0;
        return 1;
    }

    put32((char *)setting + 0x18, (uint32_t)next_slot);
    song->last_selected_slot = next_slot;
    refresh_popup_ui(window_setup, setting, song);

    typedef void (ABI *PlaySEFn)(int32_t diff, void *method);
    ((PlaySEFn)CODE(CC_DIFF_LIST_PLAY_SE))(next_slot, NULL);

    r->rax = 0;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* UI / STAGE / RESULTS / RETRY 6-DIFFICULTY COLORS, SPRITES, & ROUND-TRIP    */
/* -------------------------------------------------------------------------- */

static ABI int hook_clamp_rdi_diff(HookRegs *r) {
    r->rdi = (uint64_t)clamp_extra_slot((int)r->rdi);
    return 0;
}

static uint32_t cda_pack_rgba8(CustomRgba8 c) {
    uint32_t v = 0;
    memcpy(&v, &c, 4);
    return v;
}

/* MusicNameBehaviour::SetMusicNameColor (0x1679e10) — expand static Color32[] at TypeInfo+0xb8+0x10
 * and directly call MusicNameBehaviour::setTextColor (0x1685510) for Beginner(4)/Chaos(5). */
static ABI int hook_music_name_set_color(HookRegs *r) {
    void *self = (void *)r->rdi;
    int slot = (int)r->rsi;
    bool unavailable = (r->rdx & 1) != 0;

    void *klass = self ? object_class(self) : find_il2cpp_class("Game.Scene.MusicSelect", "MusicNameBehaviour");
    if (klass && il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
    void *statics = klass ? (void *)load64((char *)klass + 0xb8) : NULL;
    void *old_arr = statics ? (void *)load64((char *)statics + 0x10) : NULL;
    if (old_arr && load64((char *)old_arr + 0x18) == 4) {
        void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
        if (new_arr) {
            (void)gc_pin(new_arr, false);
            memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * 4);
            memcpy((char *)new_arr + 0x20 + 4 * 4, &cda_beginner_nameplate, 4);
            memcpy((char *)new_arr + 0x20 + 5 * 4, &cda_chaos_nameplate, 4);
            put64((char *)statics + 0x10, (uint64_t)new_arr);
        }
    }

    if (self && !unavailable && (slot == 4 || slot == 5)) {
        CustomRgba8 out_c = (slot == 4) ? cda_beginner_outline : cda_chaos_outline;
        CustomRgba8 shd_c = (slot == 4) ? cda_beginner_shadow : cda_chaos_shadow;
        typedef void (ABI *SetTextColorFn)(void *self, uint32_t top, uint32_t outline, uint32_t shadow, void *method);
        ((SetTextColorFn)CODE(CC_MUSIC_NAME_SET_TEXT_COLOR))(self, 0xffffffffu, cda_pack_rgba8(out_c), cda_pack_rgba8(shd_c), NULL);
        r->rax = 0;
        return 1;
    }

    if (slot < 0 || slot >= MAX_SLOTS) r->rsi = (uint64_t)clamp_extra_slot(slot);
    return 0;
}

static void *original_music_status_set_diff_color = NULL;

static void cda_expand_popup_go_array(void *owner, size_t offset) {
    if (!owner) return;
    void *old_arr = (void *)load64((char *)owner + offset);
    if (!old_arr || load64((char *)old_arr + 0x18) != 4) return;

    void *src_go = (void *)load64((char *)old_arr + 0x20 + 1 * sizeof(void *)); /* Expert (1) */
    if (!uobj_alive(src_go)) return;

    GetTransformFn go_get_tr = (GetTransformFn)CODE(CC_GO_GET_TRANSFORM);
    GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
    CloneWithParentFn clone_obj = (CloneWithParentFn)CODE(CC_UOBJ_CLONE_WITH_PARENT);
    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);

    void *src_tr = go_get_tr(src_go, NULL);
    void *parent_tr = uobj_alive(src_tr) ? get_parent(src_tr, NULL) : NULL;
    if (!uobj_alive(parent_tr)) return;

    void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
    if (!new_arr) return;
    (void)gc_pin(new_arr, false);
    memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));

    const int extra_slots[2] = {4, 5};
    for (int k = 0; k < 2; ++k) {
        int slot = extra_slots[k];
        void *clone_go = clone_obj(src_go, parent_tr, 0);
        if (uobj_alive(clone_go)) {
            (void)gc_pin(clone_go, false);
            void *clone_tr = go_get_tr(clone_go, NULL);
            cda_skin_hierarchy(clone_tr, NULL, slot);
            set_active(clone_go, false, NULL);
            put64((char *)new_arr + 0x20 + (size_t)slot * sizeof(void *), (uint64_t)clone_go);
        }
    }
    put64((char *)owner + offset, (uint64_t)new_arr);
}

/* MusicStatusBehaviour.MusicTitle::SetColor (0x135ead0) — expand static Color[] & GameObject[] (+0x30, +0x38) */
static ABI int hook_music_status_set_diff_color(HookRegs *r) {
    void *self = (void *)r->rdi;
    int slot = (int)r->rsi;
    void *method = (void *)r->rdx;

    void *klass = self ? object_class(self) : NULL;
    if (klass && il2cpp_runtime_class_init_fn) il2cpp_runtime_class_init_fn(klass);
    void *statics = klass ? (void *)load64((char *)klass + 0xb8) : NULL;
    if (statics) {
        for (int which = 0; which < 2; ++which) {
            size_t off = (which == 0) ? 0x00 : 0x08;
            void *old_arr = (void *)load64((char *)statics + off);
            if (old_arr && load64((char *)old_arr + 0x18) == 4) {
                void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
                if (new_arr) {
                    (void)gc_pin(new_arr, false);
                    memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(UGuiVec4));
                    UGuiVec4 c4 = (which == 0) ? cda_outline_vec4(4) : cda_shadow_vec4(4);
                    UGuiVec4 c5 = (which == 0) ? cda_outline_vec4(5) : cda_shadow_vec4(5);
                    memcpy((char *)new_arr + 0x20 + 4 * sizeof(UGuiVec4), &c4, sizeof(UGuiVec4));
                    memcpy((char *)new_arr + 0x20 + 5 * sizeof(UGuiVec4), &c5, sizeof(UGuiVec4));
                    put64((char *)statics + off, (uint64_t)new_arr);
                }
            }
        }
    }

    if (self) {
        cda_expand_popup_go_array(self, 0x30);
        cda_expand_popup_go_array(self, 0x38);
    }

    if (slot < 0 || slot >= MAX_SLOTS) slot = clamp_extra_slot(slot);
    if (original_music_status_set_diff_color) {
        typedef void (ABI *SetPopupColorFn)(void *self, int32_t diff, void *method);
        ((SetPopupColorFn)original_music_status_set_diff_color)(self, slot, method);
        if (self) {
            SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);
            static const size_t offs[2] = {0x30, 0x38};
            for (int k = 0; k < 2; ++k) {
                void *arr = (void *)load64((char *)self + offs[k]);
                if (!arr || load64((char *)arr + 0x18) < MAX_SLOTS) continue;
                for (int s = 4; s < MAX_SLOTS; ++s) {
                    void *go = (void *)load64((char *)arr + 0x20 + (size_t)s * sizeof(void *));
                    if (uobj_alive(go)) set_active(go, s == slot, NULL);
                }
            }
        }
        r->rax = 0;
        return 1;
    }

    r->rsi = (uint64_t)slot;
    return 0;
}

static void *original_stage_intro_setup = NULL;

/* MusicStageIntroduction::Setup (0x3175e60) — temporarily expose true slot 4/5 on playSetting+0x50 */
static ABI int hook_stage_intro_setup(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *play_setting = (void *)r->rsi;
    void *method = (void *)r->rdx;
    if (!original_stage_intro_setup) return 0;

    if (play_setting) {
        s_current_play_setting = play_setting;
        void *mid = (void *)load64((char *)play_setting + 0x18);
        s_custom_stage_active = (find_song_by_obj(mid) != NULL);
    }

    int active_extra = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
    int saved_slot = -1;
    if (play_setting && active_extra >= 4 && active_extra < MAX_SLOTS) {
        saved_slot = (int)load32((char *)play_setting + 0x50);
        put32((char *)play_setting + 0x50, (uint32_t)active_extra);
    }

    typedef void (ABI *StageIntroSetupFn)(void *self, void *setting, void *method);
    ((StageIntroSetupFn)original_stage_intro_setup)(self, play_setting, method);

    if (play_setting && saved_slot >= 0) {
        put32((char *)play_setting + 0x50, (uint32_t)clamp_extra_slot(active_extra));
    }
    r->rax = 0;
    return 1;
}

/* Game.Scene.MusicStage.StageMusicInfo::Setup (0x32ebe10, 8 args):
 * (self [rdi], jacket [rsi], title [rdx], series [rcx], msType [r8d], difficulty [r9d], playStyle [stack], method [stack])
 * Expand [self + 0x30] Sprite[] to 6 elements and let native Setup run with all 8 register/stack args intact! */
static ABI int hook_stage_music_info_setup(HookRegs *r) {
    void *self = (void *)r->rdi;
    int diff = (int)r->r9;
    int active_extra = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
    if (active_extra >= 4 && active_extra < MAX_SLOTS) {
        diff = active_extra;
    }

    void *sprites = NULL;
    if (self) {
        expand_ref_array_copy(self, 0x30);
        sprites = (void *)load64((char *)self + 0x30);
        if (sprites && load64((char *)sprites + 0x18) >= MAX_SLOTS) {
            void *s1 = (void *)load64((char *)sprites + 0x20 + 1 * sizeof(void *));
            void *s4 = cda_blue_variant(s1, 4);
            void *s5 = cda_blue_variant(s1, 5);
            if (!unity_native_alive(s4) || s4 == s1) s4 = cda_artwork_sprite(&cda_kBeginnerMusicLevelBase);
            if (!unity_native_alive(s5) || s5 == s1) s5 = cda_artwork_sprite(&cda_kChaosMusicLevelBase);
            if (s4) put64((char *)sprites + 0x20 + 4 * sizeof(void *), (uint64_t)s4);
            if (s5) put64((char *)sprites + 0x20 + 5 * sizeof(void *), (uint64_t)s5);
        }
    }

    if (sprites && load64((char *)sprites + 0x18) >= MAX_SLOTS && diff >= 0 && diff < MAX_SLOTS) {
        r->r9 = (uint64_t)diff;
    } else {
        r->r9 = (uint64_t)clamp_extra_slot(diff);
    }
    return 0;
}

/* MusicIntroduction::Setup (0x3174160, 10 args):
 * (self [rdi], jacket [rsi], plate [rdx], msType [ecx], iconPack [r8], difficulty [r9d], title [stack], artist [stack], levelText [stack], method [stack])
 * Expand [self + 0x28] Material[] to 6 elements, register CX mark on [self + 0x58], and let native Setup run with all 10 args intact! */
static ABI int hook_music_intro_setup(HookRegs *r) {
    void *self = (void *)r->rdi;
    int diff = (int)r->r9;
    int active_extra = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
    if (active_extra >= 4 && active_extra < MAX_SLOTS) {
        diff = active_extra;
    }

    void *mats = NULL;
    if (self) {
        expand_ref_array_copy(self, 0x28);
        mats = (void *)load64((char *)self + 0x28);
        if (mats && load64((char *)mats + 0x18) >= MAX_SLOTS) {
            void *m1 = (void *)load64((char *)mats + 0x20 + 1 * sizeof(void *));
            void *m4 = (void *)load64((char *)mats + 0x20 + 4 * sizeof(void *));
            void *m5 = (void *)load64((char *)mats + 0x20 + 5 * sizeof(void *));
            void *m0 = (void *)load64((char *)mats + 0x20 + 0 * sizeof(void *));
            void *m3 = (void *)load64((char *)mats + 0x20 + 3 * sizeof(void *));
            if (unity_native_alive(m1)) {
                if (!unity_native_alive(m4) || m4 == m0) {
                    void *nm4 = cda_clone_tmp_material(m1, 4);
                    if (nm4) put64((char *)mats + 0x20 + 4 * sizeof(void *), (uint64_t)nm4);
                }
                if (!unity_native_alive(m5) || m5 == m3) {
                    void *nm5 = cda_clone_tmp_material(m1, 5);
                    if (nm5) put64((char *)mats + 0x20 + 5 * sizeof(void *), (uint64_t)nm5);
                }
            }
        }
        cda_apply_ms_mark((void *)load64((char *)self + 0x58), s_custom_stage_active || diff >= 4);
    }

    if (mats && load64((char *)mats + 0x18) >= MAX_SLOTS && diff >= 0 && diff < MAX_SLOTS) {
        r->r9 = (uint64_t)diff;
    } else {
        r->r9 = (uint64_t)clamp_extra_slot(diff);
    }
    return 0;
}

static void *original_live_info_setup_fixed_text = NULL;
static void *original_get_music_record = NULL;
static void *s_cached_music_info = NULL;

static void ensure_extra_difficulty_records(void *record);

/* Game.Scene.MusicStage.LiveInfoController::setupFixedText */
static ABI int hook_live_info_setup_fixed_text(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *method = (void *)r->rsi;
    if (!original_live_info_setup_fixed_text) return 0;

    void *play_setting = s_current_play_setting;
    int active_extra = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
    CustomSong *song = NULL;
    int saved_diff = -1;

    GetGOPtrFn comp_get_go = (GetGOPtrFn)CODE(CC_COMP_GET_GAMEOBJECT);
    GetTransformFn comp_get_tr = (GetTransformFn)CODE(CC_COMP_GET_TRANSFORM);
    GetParentFn get_parent = (GetParentFn)CODE(CC_TR_GET_PARENT);
    CloneWithParentFn clone_obj = (CloneWithParentFn)CODE(CC_UOBJ_CLONE_WITH_PARENT);
    SetActiveFn set_active = (SetActiveFn)CODE(CC_GO_SET_ACTIVE);
    SetSpriteFn set_spr = (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);

    if (self) {
        /* self + 0x78 is UnityEngine.UI.Image[] _difficultyIcons.
         * LiveInfoController::setupFixedText loops over _difficultyIcons calling Behaviour::set_enabled(icons[i], i == diff). */
        expand_ref_array_copy(self, 0x78);
        void *icons = (void *)load64((char *)self + 0x78);
        if (icons && load64((char *)icons + 0x18) >= MAX_SLOTS) {
            void *img0 = (void *)load64((char *)icons + 0x20 + 0 * sizeof(void *));
            void *src_img = (void *)load64((char *)icons + 0x20 + 1 * sizeof(void *)); /* Expert Image */
            void *img3 = (void *)load64((char *)icons + 0x20 + 3 * sizeof(void *));
            void *src_go = uobj_alive(src_img) ? comp_get_go(src_img, NULL) : NULL;
            void *src_tr = uobj_alive(src_img) ? comp_get_tr(src_img, NULL) : NULL;
            void *par_tr = uobj_alive(src_tr) ? get_parent(src_tr, NULL) : NULL;

            const int extra_slots[2] = {4, 5};
            for (int k = 0; k < 2; ++k) {
                int es = extra_slots[k];
                void *cur_img = (void *)load64((char *)icons + 0x20 + (size_t)es * sizeof(void *));
                if ((!uobj_alive(cur_img) || cur_img == img0 || cur_img == img3 || cur_img == src_img) &&
                    uobj_alive(src_go) && uobj_alive(par_tr)) {
                    void *clone_go = clone_obj(src_go, par_tr, 0);
                    if (uobj_alive(clone_go)) {
                        (void)gc_pin(clone_go, false);
                        set_active(clone_go, true, NULL);
                        void *c_img = cda_get_comp(clone_go, "UnityEngine.UI.Image", "Image");
                        if (uobj_alive(c_img)) {
                            (void)gc_pin(c_img, false);
                            void *spr = cda_artwork_sprite(es == 4 ? &cda_kBeginnerTrigger : &cda_kChaosTrigger);
                            if (spr) set_spr(c_img, spr, NULL);
                            put64((char *)icons + 0x20 + (size_t)es * sizeof(void *), (uint64_t)c_img);
                        }
                    }
                }
            }
        }
    }

    if (play_setting && (uintptr_t)play_setting > 0x10000ULL) {
        void *mid = (void *)load64((char *)play_setting + 0x18);
        song = find_song_by_obj(mid);
        if (song) {
            if (s_cached_music_info) {
                void *dict = *(void **)((char *)s_cached_music_info + 0x20);
                void *mrec = song->managed_record;
                if (!mrec && original_get_music_record && song->managed_id) {
                    typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);
                    mrec = ((GetRecFn)original_get_music_record)(s_cached_music_info, song->managed_id, NULL);
                }
                if (!mrec && song->managed_id) {
                    typedef void *(ABI *CreateRecFn)(void *self, void *mid, void *method);
                    CreateRecFn create_rec = (CreateRecFn)CODE(CC_MUSIC_INFO_CREATE_REC);
                    if (create_rec) mrec = create_rec(s_cached_music_info, song->managed_id, NULL);
                }
                if (mrec) {
                    ensure_extra_difficulty_records(mrec);
                    if (!song->managed_record) {
                        song->managed_record = mrec;
                        song->root_record = gc_pin(mrec, false);
                    }
                    if (dict && song->managed_id && !dict_contains_key(dict, song->managed_id)) {
                        dict_add_safe(dict, song->managed_id, mrec);
                    }
                }
            } else if (song->managed_record) {
                ensure_extra_difficulty_records(song->managed_record);
            }
        }
        if (active_extra >= 4 && active_extra < MAX_SLOTS) {
            void *tbl = (void *)load64((char *)play_setting + 0x20);
            if (tbl) {
                expand_int_array(tbl, 0x98);
                void *scores = (void *)load64((char *)tbl + 0x98);
                if (song && scores && load64((char *)scores + 0x18) >= MAX_SLOTS) {
                    put32((char *)scores + 0x20 + (size_t)active_extra * 4, (uint32_t)song->charts[active_extra].rating);
                }
            }
            saved_diff = (int)load32((char *)play_setting + 0x50);
            put32((char *)play_setting + 0x50, (uint32_t)active_extra);
        }
    }

    typedef void (ABI *LiveInfoSetupFn)(void *self, void *method);
    ((LiveInfoSetupFn)original_live_info_setup_fixed_text)(self, method);

    if (play_setting && saved_diff >= 0) {
        put32((char *)play_setting + 0x50, (uint32_t)saved_diff);
    }

    r->rax = 0;
    return 1;
}

/* MusicStageMainLoop::createMSParts */
static ABI int hook_create_ms_parts(HookRegs *r) {
    void *play_setting = (void *)r->rsi;
    if (play_setting) {
        s_current_play_setting = play_setting;
        int diff = (int)load32((char *)play_setting + 0x50);
        if (diff == 4 || diff == 5) {
            s_gameplay_chart_slot = diff;
            put32((char *)play_setting + 0x50, (uint32_t)clamp_extra_slot(diff));
        } else if (s_pending_chart_slot == 4 || s_pending_chart_slot == 5) {
            s_gameplay_chart_slot = s_pending_chart_slot;
        }
    }
    return 0;
}

static void *original_get_diff_semicircle = NULL;

/* GetDifficultyIconSprite (0x1f0d860) */
static ABI int hook_get_diff_icon_sprite(HookRegs *r) {
    int slot = (int)r->rdi;
    if (slot < 4) {
        int active = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
        if (active >= 4 && active < MAX_SLOTS) slot = active;
    }
    if (slot == 4 || slot == 5) {
        void *spr = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerTrigger : &cda_kChaosTrigger);
        if (spr) {
            r->rax = (uint64_t)spr;
            return 1;
        }
    }
    r->rdi = (uint64_t)clamp_extra_slot(slot);
    return 0;
}

/* GetDifficultySemicircleIconSprite (0x1f0d900) */
static ABI int hook_get_diff_semicircle_sprite(HookRegs *r) {
    int slot = (int)r->rdi;
    if (slot < 4) {
        int active = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
        if (active >= 4 && active < MAX_SLOTS) slot = active;
    }
    if (slot == 4 || slot == 5) {
        typedef void *(ABI *GetSprByDiffFn)(int32_t diff, void *method);
        void *ref_spr = original_get_diff_semicircle
                        ? ((GetSprByDiffFn)original_get_diff_semicircle)(1 /* Expert */, (void *)r->rsi)
                        : NULL;
        void *spr = cda_blue_variant(ref_spr, slot);
        if (spr && spr != ref_spr) {
            r->rax = (uint64_t)spr;
            return 1;
        }
        UGuiVec2 piv;
        float ppu;
        UGuiVec4 border;
        cda_read_sprite_metrics(ref_spr, 118, 92, &piv, &ppu, &border);
        spr = cda_blue_variant_by_name("DifficultySemicircle_Difficulty02", slot, piv, ppu, border);
        r->rax = (uint64_t)(spr ? spr : ref_spr);
        return 1;
    }
    r->rdi = (uint64_t)clamp_extra_slot(slot);
    return 0;
}

/* GetDifficultyBannerSprite (0x1f0d9a0) */
static ABI int hook_get_diff_banner_sprite(HookRegs *r) {
    int slot = (int)r->rdi;
    if (slot < 4) {
        int active = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : s_pending_chart_slot;
        if (active >= 4 && active < MAX_SLOTS) slot = active;
    }
    if (slot == 4 || slot == 5) {
        void *spr = cda_artwork_sprite(slot == 4 ? &cda_kBeginnerMusicLevelBase : &cda_kChaosMusicLevelBase);
        if (spr) {
            r->rax = (uint64_t)spr;
            return 1;
        }
    }
    r->rdi = (uint64_t)clamp_extra_slot(slot);
    return 0;
}

static ABI int hook_support_manager_setup(HookRegs *r) {
    void *play_setting = (void *)r->rsi;
    if (play_setting) {
        int slot = (int)load32((char *)play_setting + 0x50);
        if (slot >= 4 || slot < 0) {
            put32((char *)play_setting + 0x50, (uint32_t)clamp_extra_slot(slot));
        }
    }
    return 0;
}

static ABI int hook_set_play_setting(HookRegs *r) {
    if (r->rdi) s_current_play_setting = (void *)r->rdi;
    void *mid = (void *)r->rsi;
    int diff = (int)r->rdx;
    uint32_t music_id = mid ? load32((char *)mid + 0x10) : 0;
    CustomSong *song = find_song_by_obj(mid);
    s_custom_stage_active = (song != NULL);
    if (song) {
        (void)load_or_get_custom_asset(song, -1);
        if (s_cached_music_info) {
            void *dict = *(void **)((char *)s_cached_music_info + 0x20);
            void *mrec = song->managed_record;
            if (!mrec && original_get_music_record && song->managed_id) {
                typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);
                mrec = ((GetRecFn)original_get_music_record)(s_cached_music_info, song->managed_id, NULL);
            }
            if (!mrec && song->managed_id) {
                typedef void *(ABI *CreateRecFn)(void *self, void *mid, void *method);
                CreateRecFn create_rec = (CreateRecFn)CODE(CC_MUSIC_INFO_CREATE_REC);
                if (create_rec) mrec = create_rec(s_cached_music_info, song->managed_id, NULL);
            }
            if (mrec) {
                ensure_extra_difficulty_records(mrec);
                if (!song->managed_record) {
                    song->managed_record = mrec;
                    song->root_record = gc_pin(mrec, false);
                }
                if (dict && song->managed_id && !dict_contains_key(dict, song->managed_id)) {
                    dict_add_safe(dict, song->managed_id, mrec);
                }
            }
        }
    }
    if (music_id != 0) {
        runtime_judgements_begin_stage(music_id, diff);
        extern void runtime_tffscores_notify_stage_start(uint32_t music_id, int difficulty);
        runtime_tffscores_notify_stage_start(music_id, diff);
    }
    if (diff >= 4) {
        s_pending_chart_slot = diff;
        s_gameplay_chart_slot = diff;
        r->rdx = (uint64_t)clamp_extra_slot(diff);
    } else {
        s_pending_chart_slot = -1;
        s_gameplay_chart_slot = -1;
    }
    return 0;
}

static ABI int hook_trigger_data_loader_get_address(HookRegs *r) {
    void *option = (void *)r->rdx;
    if (option) {
        int slot = (int)load32((char *)option + 0x10);
        void *mid = (void *)r->rsi;
        CustomSong *song = find_song_by_obj(mid);
        if (song) {
            if (s_pending_chart_slot >= 4) slot = s_pending_chart_slot;
            else if (s_gameplay_chart_slot >= 4) slot = s_gameplay_chart_slot;
            slot = resolve_song_slot(song, slot, song->last_selected_slot);
            char path[256];
            snprintf(path, sizeof(path), "FBL.CustomCharts/%s/chart/%d", song->id, slot);
            r->rax = (uint64_t)make_string(NULL, path);
            return 1;
        }
        if (slot >= 4 || slot < 0) {
            put32((char *)option + 0x10, (uint32_t)clamp_extra_slot(slot));
        }
    }
    return 0;
}

static ABI int hook_runtime_context_setup(HookRegs *r) {
    int slot = find_chart_slot_by_bytes((const void *)r->rsi);
    if (slot >= 0 && slot < MAX_SLOTS) {
        s_gameplay_chart_slot = slot;
        s_pending_chart_slot = -1;
        /* Vanilla judge levels: 0=Beginner, 1=Basic, 2=Expert, 3=Ultimate, 4=Supreme, 5=Chaos */
        int judge_level = (slot == 4) ? 0 : ((slot == 5) ? 5 : (slot + 1));
        r->rdx = (uint64_t)judge_level;
    } else {
        s_gameplay_chart_slot = -1;
        if (r->rdx == 5) r->rdx = 0;
        else if (r->rdx == 6) r->rdx = 5;
    }
    return 0;
}

static ABI int hook_ability_condition_setup(HookRegs *r) {
    r->rdx = (uint64_t)clamp_extra_slot((int)r->rdx);
    return 0;
}

static ABI int hook_summon_bar_util_setup(HookRegs *r) {
    if (s_gameplay_chart_slot == 4 || r->rdi == 5) r->rdi = 0;
    else if (s_gameplay_chart_slot == 5 || r->rdi >= 6) r->rdi = 5;
    return 0;
}

static void *original_copy_ms_play_data = NULL;

/* Restore true Beginner(4)/Chaos(5) slot onto playSetting (+0x50) before Record save,
 * but clamp MusicResultPlayData._difficulty (+0x48) to a safe stock slot (0..3)
 * so native Results scene UI components (length-4 arrays, GetDifficultyIconSprite) never crash! */
static ABI int hook_copy_ms_play_data(HookRegs *r) {
    void *result_play_data = (void *)r->rdi;
    void *play_setting = (void *)r->rcx;
    if (play_setting && s_gameplay_chart_slot >= 4) {
        put32((char *)play_setting + 0x50, (uint32_t)s_gameplay_chart_slot);
    }
    s_custom_records_dirty = true;

    if (original_copy_ms_play_data) {
        typedef void (ABI *CopyFn)(void *this_ptr, int32_t rsi, void *rdx, void *play_setting, int8_t r8b);
        ((CopyFn)original_copy_ms_play_data)(result_play_data, (int32_t)r->rsi, (void *)r->rdx, (void *)r->rcx, (int8_t)r->r8);
        if (result_play_data && s_gameplay_chart_slot >= 4) {
            put32((char *)result_play_data + 0x48, (uint32_t)clamp_extra_slot(s_gameplay_chart_slot));
        }
        r->rax = 0;
        return 1;
    }
    return 0;
}

static void cda_expand_title_color_info(void *owner) {
    if (!owner) return;
    expand_ref_array_clone(owner, 0x10);
    void *arr = (void *)load64((char *)owner + 0x10);
    if (!arr || load64((char *)arr + 0x18) < MAX_SLOTS) return;

    void *info1 = (void *)load64((char *)arr + 0x20 + 1 * sizeof(void *));
    void *spr1 = info1 ? (void *)load64((char *)info1 + 0x10) : NULL;
    const int extra_slots[2] = {4, 5};
    for (int k = 0; k < 2; ++k) {
        int s = extra_slots[k];
        void *info = (void *)load64((char *)arr + 0x20 + (size_t)s * sizeof(void *));
        if (!info) continue;
        void *custom_icon = unity_native_alive(spr1) ? cda_blue_variant(spr1, s) : NULL;
        if (!unity_native_alive(custom_icon) || custom_icon == spr1) {
            custom_icon = cda_artwork_sprite(s == 4 ? &cda_kBeginnerTrigger : &cda_kChaosTrigger);
        }
        if (custom_icon) put64((char *)info + 0x10, (uint64_t)custom_icon);
        UGuiVec4 outline = cda_outline_vec4(s);
        UGuiVec4 shadow = cda_shadow_vec4(s);
        memcpy((char *)info + 0x18, &outline, sizeof(UGuiVec4));
        memcpy((char *)info + 0x28, &shadow, sizeof(UGuiVec4));
    }
}

typedef struct {
    void *image;
    int slot;
} CdaIconTargetEntry;

static CdaIconTargetEntry s_cda_icon_targets[32];

static void cda_register_icon_target(void *img, int slot) {
    if (!uobj_alive(img)) return;
    for (int i = 0; i < 32; ++i) {
        if (s_cda_icon_targets[i].image == img) {
            s_cda_icon_targets[i].slot = slot;
            return;
        }
    }
    for (int i = 0; i < 32; ++i) {
        if (!uobj_alive(s_cda_icon_targets[i].image)) {
            s_cda_icon_targets[i].image = img;
            s_cda_icon_targets[i].slot = slot;
            return;
        }
    }
    s_cda_icon_targets[0].image = img;
    s_cda_icon_targets[0].slot = slot;
}

/* MusicTitleDisplay::Setup (0x12c6610) — returns 16-byte UniTask in rax:rdx.
 * Prepare TitleColorInfo[] (+0x10), register icon target (+0x48), and register CX mark (+0x70),
 * then return 0 so native Setup executes and returns the full 16-byte UniTask in rax:rdx! */
static ABI int hook_music_title_display_setup(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *mid = (void *)r->rsi;
    int slot = (int)r->rdx;

    if (s_gameplay_chart_slot >= 4 && s_gameplay_chart_slot < MAX_SLOTS) {
        slot = s_gameplay_chart_slot;
    }
    if (self) {
        cda_expand_title_color_info(self);
        cda_register_icon_target((void *)load64((char *)self + 0x48), slot);
        cda_apply_ms_mark((void *)load64((char *)self + 0x70), find_song_by_obj(mid) != NULL);
    }
    if (slot < 0 || slot >= 4) {
        r->rdx = (uint64_t)clamp_extra_slot(slot);
    }
    return 0;
}

/* Subasset loaders (0x1710c90, 0x1710fa0, 0x1711160) & MusicTitlePlateLoader (0x171bc60, 0x171c0d0):
 * clamp 4/5 -> 1 (Expert blue) so async subasset loads return the blue Expert template for recoloring! */
static ABI int hook_title_plate_clamp_expert(HookRegs *r) {
    int slot = (int)r->rsi;
    if (slot == 4 || slot == 5) {
        r->rsi = 1;
    } else if (slot < 0 || slot > 3) {
        r->rsi = (uint64_t)clamp_extra_slot(slot);
    }
    return 0;
}

static void *original_load_title_plate_move_next = NULL;

/* MusicTitleDisplay::<LoadTitlePlateAsync>d__MoveNext (0x12c7cc0) */
static ABI int hook_load_title_plate_move_next(HookRegs *r) {
    void *sm = (void *)r->rdi;
    void *method = (void *)r->rsi;
    if (!sm || !original_load_title_plate_move_next) return 0;

    typedef void (ABI *MoveNextVoidFn)(void *sm, void *method);
    ((MoveNextVoidFn)original_load_title_plate_move_next)(sm, method);

    int32_t state = (int32_t)load32(sm);
    if (state == -2) {
        void *closure = (void *)load64((char *)sm + 0x18);
        if (closure) {
            int diff = (s_gameplay_chart_slot >= 4) ? s_gameplay_chart_slot : (int)load32((char *)closure + 0x30);
            if (diff == 4 || diff == 5) {
                void *disp = (void *)load64((char *)closure + 0x18);
                void *img = disp ? (void *)load64((char *)disp + 0x48) : NULL;
                if (uobj_alive(img)) {
                    GetSpriteFn get_spr = (GetSpriteFn)CODE(CC_IMAGE_GET_SPRITE);
                    SetSpriteFn set_spr = (SetSpriteFn)CODE(CC_IMAGE_SET_SPRITE);
                    void *cur = get_spr(img, NULL);
                    void *rep = cda_blue_variant(cur, diff);
                    if (rep && rep != cur) {
                        set_spr(img, rep, NULL);
                    }
                }
            }
        }
    }
    r->rax = 0;
    return 1;
}

/* RetryWindowMusicNameBehaviour::OnUpdate (0x183a270):
 * Signature: (self [rdi], slot [esi], mid [rdx], method [rcx])
 * Expand Setting[] at +0x10 before inlined lookup at 0x183a2ee and register CX mark on +0x20! */
static ABI int hook_retry_window_on_update(HookRegs *r) {
    void *self = (void *)r->rdi;
    int slot = (int)r->rsi;
    void *mid = (void *)r->rdx;

    if (s_gameplay_chart_slot >= 4 && s_gameplay_chart_slot < MAX_SLOTS) {
        slot = s_gameplay_chart_slot;
    }
    if (self) {
        cda_expand_title_color_info(self);
        cda_apply_ms_mark((void *)load64((char *)self + 0x20), find_song_by_obj(mid) != NULL);
    }
    if (slot < 0 || slot >= MAX_SLOTS) slot = clamp_extra_slot(slot);
    r->rsi = (uint64_t)slot;
    return 0;
}

static void *original_image_anim_props = NULL;

/* UnityEngine.UI.Image::OnDidApplyAnimationProperties (0x2d00430) */
static ABI int hook_image_anim_props(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *method = (void *)r->rsi;
    if (!original_image_anim_props) return 0;

    typedef void (ABI *AnimPropsFn)(void *self, void *method);
    ((AnimPropsFn)original_image_anim_props)(self, method);

    if (self) {
        for (int i = 0; i < 128; ++i) {
            if (s_cda_skins[i].image == self) {
                cda_apply_image_skin(&s_cda_skins[i]);
                break;
            }
        }
    }
    r->rax = 0;
    return 1;
}

/* UnityEngine.UI.Image::set_sprite (0x2cd5560) */
static ABI int hook_image_set_sprite(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *sprite = (void *)r->rsi;
    if (self && unity_native_alive(sprite)) {
        for (int i = 0; i < 32; ++i) {
            if (s_cda_icon_targets[i].image == self &&
                (s_cda_icon_targets[i].slot == 4 || s_cda_icon_targets[i].slot == 5)) {
                void *rep = cda_blue_variant(sprite, s_cda_icon_targets[i].slot);
                if (unity_native_alive(rep)) r->rsi = (uint64_t)rep;
                break;
            }
        }
        for (int i = 0; i < 128; ++i) {
            if (s_cda_skins[i].image == self &&
                (s_cda_skins[i].slot == 4 || s_cda_skins[i].slot == 5)) {
                s_cda_skins[i].source = sprite;
                void *rep = cda_resolve_sprite(sprite, s_cda_skins[i].slot);
                if (unity_native_alive(rep)) r->rsi = (uint64_t)rep;
                break;
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* RECORD PERSISTENCE, EXTRA DIFFICULTY RECORDS, & PLAY STYLE CACHE           */
/* -------------------------------------------------------------------------- */

#define CUSTOM_RECORDS_MAGIC   0x31435243u /* 'CRC1' */
#define CUSTOM_RECORDS_VERSION 2u

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t song_count;
    uint32_t reserved;
} CustomRecordsHeader;

typedef struct {
    int32_t highscore;
    int64_t update_date;
    int32_t best_rank;
    int32_t max_chain;
    int32_t best_clear_mark;
    int32_t total_play_count;
    int32_t total_clear_count;
    int32_t total_full_chain_count;
    int32_t max_damage;
} CustomSavedPlayStyleRecordV1;

typedef struct {
    uint32_t music_id;
    uint8_t is_favorite;
    uint8_t reserved[3];
    CustomSavedPlayStyleRecordV1 styles[MAX_SLOTS][3];
} CustomSavedSongRecordV1;

typedef struct {
    int32_t highscore;
    int64_t update_date;
    int32_t best_rank;
    int32_t max_chain;
    int32_t best_clear_mark;
    int32_t total_play_count;
    int32_t total_clear_count;
    int32_t total_full_chain_count;
    int32_t max_damage;
    float critical_bar_zones[20];
} CustomSavedPlayStyleRecord;

typedef struct {
    uint32_t music_id;
    uint8_t is_favorite;
    uint8_t reserved[3];
    CustomSavedPlayStyleRecord styles[MAX_SLOTS][3];
} CustomSavedSongRecord;
#pragma pack(pop)

static bool dict_remove_safe(void *dict, void *key) {
    if (!dict || !key) return false;
    static void *s_last_klass = NULL;
    static Il2CppMethodInfo *s_last_method = NULL;
    void *klass = object_class(dict);
    Il2CppMethodInfo *method = (klass && klass == s_last_klass && s_last_method)
                               ? s_last_method
                               : find_method(klass, "Remove", 1);
    if (!method) return false;
    s_last_klass = klass;
    s_last_method = method;
    typedef bool (ABI *RemoveFn)(void *self, void *key, void *method);
    return ((RemoveFn)method->pointer)(dict, key, method);
}

static void ensure_extra_difficulty_records(void *record) {
    if (!record) return;
    *(bool *)((char *)record + 0x10) = true; /* isUnlocked = true */
    void *dict = (void *)load64((char *)record + 0x28);
    if (!dict) return;

    typedef void *(ABI *CreateDiffRecFn)(void *record, void *method);
    CreateDiffRecFn create_diff = (CreateDiffRecFn)CODE(CC_MUSIC_REC_CREATE_DIFF_REC);

    const int extra_keys[2] = {4, 5};
    for (int i = 0; i < 2; ++i) {
        int key = extra_keys[i];
        if (!dict_find_i32(dict, key, NULL)) {
            void *fresh = create_diff(record, NULL);
            if (fresh) {
                uint32_t froot = gc_pin(fresh, false);
                dict_add_i32(dict, key, fresh);
                gc_unpin(froot);
            }
        }
    }
}

static void *original_create_music_record = NULL;
static void *original_loaded_data_music_records = NULL;
static void *original_setup_dlc_music_records = NULL;
static void *original_music_info_serialize = NULL;

typedef void *(ABI *GetPlayStyleRecFn)(void *music_info, void *mid, int diff, int style, void *method);
typedef int32_t (ABI *PsrGetI32Fn)(void *psr, void *method);
typedef void (ABI *PsrSetI32Fn)(void *psr, int32_t val, void *method);
typedef int64_t (ABI *PsrGetI64Fn)(void *psr, void *method);
typedef void (ABI *PsrSetI64Fn)(void *psr, int64_t val, void *method);

static void save_custom_records_to_path(const char *path, const CustomRecordsHeader *hdr, const CustomSavedSongRecord *recs, size_t count) {
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, sizeof(*hdr), 1, f);
    if (count > 0 && recs) {
        fwrite(recs, sizeof(CustomSavedSongRecord), count, f);
    }
    fclose(f);
}

typedef struct {
    CustomRecordsHeader hdr;
    CustomSavedSongRecord *recs;
    size_t count;
    char path_primary[560];
    char path_slot[560];
} AsyncSaveCustomRecordsJob;

static void *async_save_custom_records_worker(void *arg) {
    AsyncSaveCustomRecordsJob *job = (AsyncSaveCustomRecordsJob *)arg;
    if (!job) return NULL;
    if (job->path_primary[0]) {
        save_custom_records_to_path(job->path_primary, &job->hdr, job->recs, job->count);
    }
    if (job->path_slot[0]) {
        save_custom_records_to_path(job->path_slot, &job->hdr, job->recs, job->count);
    }
    if (job->recs) free(job->recs);
    free(job);
    return NULL;
}

static void save_custom_records(void *music_info) {
    if (!music_info || song_count == 0) return;

    typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);

    /* Fast path: skip serializing all 123*6*3 PlayStyleRecords if neither scores nor favorites changed */
    if (!s_custom_records_dirty) {
        bool fav_changed = false;
        for (size_t i = 0; i < song_count; ++i) {
            CustomSong *song = &songs[i];
            void *mrec = song->managed_record;
            if (!mrec && song->managed_id && original_get_music_record) {
                mrec = ((GetRecFn)original_get_music_record)(music_info, song->managed_id, NULL);
            }
            if (mrec) {
                uint8_t fav = *(uint8_t *)((char *)mrec + 0x14) ? 1u : 0u;
                if (fav != song->last_saved_fav) {
                    fav_changed = true;
                    break;
                }
            }
        }
        if (!fav_changed) return;
    }

    GetPlayStyleRecFn get_psr = (GetPlayStyleRecFn)CODE(CC_MUSIC_INFO_GET_PSR);
    PsrGetI32Fn get_hs = (PsrGetI32Fn)CODE(CC_PSR_GET_HIGHSCORE);
    PsrGetI64Fn get_dt = (PsrGetI64Fn)CODE(CC_PSR_GET_UPDATE_DATE);
    PsrGetI32Fn get_rk = (PsrGetI32Fn)CODE(CC_PSR_GET_BEST_RANK);
    PsrGetI32Fn get_mc = (PsrGetI32Fn)CODE(CC_PSR_GET_MAX_CHAIN);
    PsrGetI32Fn get_cm = (PsrGetI32Fn)CODE(CC_PSR_GET_BEST_MARK);
    PsrGetI32Fn get_pc = (PsrGetI32Fn)CODE(CC_PSR_GET_TOTAL_PC);
    PsrGetI32Fn get_cc = (PsrGetI32Fn)CODE(CC_PSR_GET_TOTAL_CC);
    PsrGetI32Fn get_fc = (PsrGetI32Fn)CODE(CC_PSR_GET_TOTAL_FC);
    PsrGetI32Fn get_md = (PsrGetI32Fn)CODE(CC_PSR_GET_MAX_DAMAGE);

    CustomSavedSongRecord *recs = (CustomSavedSongRecord *)calloc(song_count, sizeof(CustomSavedSongRecord));
    if (!recs) return;

    size_t saved_count = 0;
    for (size_t i = 0; i < song_count; ++i) {
        CustomSong *song = &songs[i];
        ensure_song_managed_objects(song);
        if (!song->managed_id) continue;

        void *mrec = song->managed_record;
        if (!mrec && original_get_music_record) {
            mrec = ((GetRecFn)original_get_music_record)(music_info, song->managed_id, NULL);
        }
        if (!mrec) continue;
        ensure_extra_difficulty_records(mrec);

        CustomSavedSongRecord *dst = &recs[saved_count++];
        dst->music_id = song->music_id;
        dst->is_favorite = *(uint8_t *)((char *)mrec + 0x14) ? 1u : 0u;
        song->last_saved_fav = dst->is_favorite;

        for (int d = 0; d < MAX_SLOTS; ++d) {
            for (int st = 0; st < 3; ++st) {
                void *psr = get_psr(music_info, song->managed_id, d, st, NULL);
                if (!psr) continue;
                CustomSavedPlayStyleRecord *s = &dst->styles[d][st];
                s->highscore = get_hs(psr, NULL);
                s->update_date = get_dt(psr, NULL);
                s->best_rank = get_rk(psr, NULL);
                s->max_chain = get_mc(psr, NULL);
                s->best_clear_mark = get_cm(psr, NULL);
                s->total_play_count = get_pc(psr, NULL);
                s->total_clear_count = get_cc(psr, NULL);
                s->total_full_chain_count = get_fc(psr, NULL);
                s->max_damage = get_md(psr, NULL);

                void *zones_arr = (void *)load64((char *)psr + 0x38);
                if (zones_arr && load64((char *)zones_arr + 0x18) >= 20) {
                    for (int z = 0; z < 20; ++z) {
                        void *elem = (void *)load64((char *)zones_arr + 0x20 + (size_t)z * sizeof(void *));
                        if (elem) {
                            uint32_t fbits = load32((char *)elem + 0x18);
                            memcpy(&s->critical_bar_zones[z], &fbits, sizeof(float));
                        }
                    }
                }
            }
        }
    }

    s_custom_records_dirty = false;

    AsyncSaveCustomRecordsJob *job = (AsyncSaveCustomRecordsJob *)calloc(1, sizeof(AsyncSaveCustomRecordsJob));
    if (!job) {
        free(recs);
        return;
    }
    job->hdr.magic = CUSTOM_RECORDS_MAGIC;
    job->hdr.version = CUSTOM_RECORDS_VERSION;
    job->hdr.song_count = (uint32_t)saved_count;
    job->hdr.reserved = 0;
    job->recs = recs;
    job->count = saved_count;
    snprintf(job->path_primary, sizeof(job->path_primary), "%s/customRecords.dat", s_save_dir);

    char active_dir[512];
    if (runtime_savedata_get_active_save_dir(active_dir, sizeof(active_dir)) == 0 && active_dir[0]) {
        snprintf(job->path_slot, sizeof(job->path_slot), "%s/customRecords.dat", active_dir);
    }

    pthread_t th;
    if (pthread_create(&th, NULL, async_save_custom_records_worker, job) == 0) {
        pthread_detach(th);
    } else {
        async_save_custom_records_worker(job);
    }
}

static void load_custom_records(void *music_info) {
    if (!music_info || song_count == 0) return;

    char path_primary[560];
    snprintf(path_primary, sizeof(path_primary), "%s/customRecords.dat", s_save_dir);
    FILE *f = fopen(path_primary, "rb");
    if (!f) {
        char active_dir[512];
        if (runtime_savedata_get_active_save_dir(active_dir, sizeof(active_dir)) == 0 && active_dir[0]) {
            char path_slot[560];
            snprintf(path_slot, sizeof(path_slot), "%s/customRecords.dat", active_dir);
            f = fopen(path_slot, "rb");
        }
    }
    if (!f) return;

    CustomRecordsHeader hdr = {0};
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 ||
        hdr.magic != CUSTOM_RECORDS_MAGIC ||
        (hdr.version != 1u && hdr.version != CUSTOM_RECORDS_VERSION) ||
        hdr.song_count == 0 ||
        hdr.song_count > 4096) {
        fclose(f);
        return;
    }

    CustomSavedSongRecord *recs = (CustomSavedSongRecord *)calloc(hdr.song_count, sizeof(CustomSavedSongRecord));
    if (!recs) {
        fclose(f);
        return;
    }

    size_t nread = 0;
    if (hdr.version == 1u) {
        CustomSavedSongRecordV1 *v1 = (CustomSavedSongRecordV1 *)calloc(hdr.song_count, sizeof(CustomSavedSongRecordV1));
        if (v1) {
            nread = fread(v1, sizeof(CustomSavedSongRecordV1), hdr.song_count, f);
            for (size_t i = 0; i < nread; ++i) {
                recs[i].music_id = v1[i].music_id;
                recs[i].is_favorite = v1[i].is_favorite;
                for (int d = 0; d < MAX_SLOTS; ++d) {
                    for (int st = 0; st < 3; ++st) {
                        const CustomSavedPlayStyleRecordV1 *sv1 = &v1[i].styles[d][st];
                        CustomSavedPlayStyleRecord *sv2 = &recs[i].styles[d][st];
                        sv2->highscore = sv1->highscore;
                        sv2->update_date = sv1->update_date;
                        sv2->best_rank = sv1->best_rank;
                        sv2->max_chain = sv1->max_chain;
                        sv2->best_clear_mark = sv1->best_clear_mark;
                        sv2->total_play_count = sv1->total_play_count;
                        sv2->total_clear_count = sv1->total_clear_count;
                        sv2->total_full_chain_count = sv1->total_full_chain_count;
                        sv2->max_damage = sv1->max_damage;
                    }
                }
            }
            free(v1);
        }
    } else {
        nread = fread(recs, sizeof(CustomSavedSongRecord), hdr.song_count, f);
    }
    fclose(f);

    typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);
    GetPlayStyleRecFn get_psr = (GetPlayStyleRecFn)CODE(CC_MUSIC_INFO_GET_PSR);
    PsrSetI32Fn set_hs = (PsrSetI32Fn)CODE(CC_PSR_SET_HIGHSCORE);
    PsrSetI64Fn set_dt = (PsrSetI64Fn)CODE(CC_PSR_SET_UPDATE_DATE);
    PsrSetI32Fn set_rk = (PsrSetI32Fn)CODE(CC_PSR_SET_BEST_RANK);
    PsrSetI32Fn set_mc = (PsrSetI32Fn)CODE(CC_PSR_SET_MAX_CHAIN);
    PsrSetI32Fn set_cm = (PsrSetI32Fn)CODE(CC_PSR_SET_BEST_MARK);
    PsrSetI32Fn set_pc = (PsrSetI32Fn)CODE(CC_PSR_SET_TOTAL_PC);
    PsrSetI32Fn set_cc = (PsrSetI32Fn)CODE(CC_PSR_SET_TOTAL_CC);
    PsrSetI32Fn set_fc = (PsrSetI32Fn)CODE(CC_PSR_SET_TOTAL_FC);
    PsrSetI32Fn set_md = (PsrSetI32Fn)CODE(CC_PSR_SET_MAX_DAMAGE);
    typedef bool (ABI *CheckBarBuriedFn)(void *psr, void *method);
    CheckBarBuriedFn check_buried = (CheckBarBuriedFn)CODE(CC_PSR_CHECK_BAR_BURIED);

    int restored = 0;
    for (size_t i = 0; i < nread; ++i) {
        const CustomSavedSongRecord *src = &recs[i];
        CustomSong *song = find_song_by_id(src->music_id);
        if (!song) continue;
        ensure_song_managed_objects(song);
        if (!song->managed_id) continue;

        void *mrec = song->managed_record;
        if (!mrec && original_get_music_record) {
            mrec = ((GetRecFn)original_get_music_record)(music_info, song->managed_id, NULL);
        }
        if (!mrec) continue;
        ensure_extra_difficulty_records(mrec);
        *(uint8_t *)((char *)mrec + 0x14) = src->is_favorite ? 1u : 0u;
        song->last_saved_fav = src->is_favorite ? 1u : 0u;

        for (int d = 0; d < MAX_SLOTS; ++d) {
            for (int st = 0; st < 3; ++st) {
                const CustomSavedPlayStyleRecord *s = &src->styles[d][st];
                if (s->total_play_count <= 0 && s->highscore <= 0) continue;
                void *psr = get_psr(music_info, song->managed_id, d, st, NULL);
                if (!psr) continue;
                set_hs(psr, s->highscore, NULL);
                set_dt(psr, s->update_date, NULL);
                set_rk(psr, s->best_rank, NULL);
                set_mc(psr, s->max_chain, NULL);
                set_cm(psr, s->best_clear_mark, NULL);
                set_pc(psr, s->total_play_count, NULL);
                set_cc(psr, s->total_clear_count, NULL);
                set_fc(psr, s->total_full_chain_count, NULL);
                set_md(psr, s->max_damage, NULL);

                if (hdr.version >= 2u) {
                    void *zones_arr = (void *)load64((char *)psr + 0x38);
                    if (zones_arr && load64((char *)zones_arr + 0x18) >= 20) {
                        for (int z = 0; z < 20; ++z) {
                            void *elem = (void *)load64((char *)zones_arr + 0x20 + (size_t)z * sizeof(void *));
                            if (elem) {
                                uint32_t fbits = 0;
                                memcpy(&fbits, &s->critical_bar_zones[z], sizeof(float));
                                put32((char *)elem + 0x18, fbits);
                            }
                        }
                        {
                            bool buried = check_buried(psr, NULL);
                            *(uint8_t *)((char *)psr + 0x40) = buried ? 1u : 0u;
                        }
                    }
                }
            }
        }
        ++restored;
    }

    free(recs);
    s_custom_records_loaded_once = true;
    s_custom_records_dirty = (hdr.version == 1u);
    if (restored > 0) {
        printf("CustomCharts: Restored %d custom song records from customRecords.dat (v%u)\n", restored, hdr.version);
    }
}

typedef struct {
    void *key;
    void *val;
    uint32_t kroot;
    uint32_t vroot;
} RemovedCustomRec;

typedef struct {
    RemovedCustomRec items[MAX_CUSTOM_SONGS * 2];
    size_t count;
} RemovedCustomRecList;

static void collect_custom_dict_entry(void *key, void *value, void *ctx) {
    RemovedCustomRecList *list = (RemovedCustomRecList *)ctx;
    if (!key || !list || list->count >= (sizeof(list->items) / sizeof(list->items[0]))) return;
    uint32_t id_val = load32((const char *)key + 0x10);
    if (runtime_custom_charts_is_custom_id(id_val) || find_song_by_id(id_val) != NULL) {
        RemovedCustomRec *entry = &list->items[list->count++];
        entry->key = key;
        entry->val = value;
        entry->kroot = gc_pin(key, false);
        entry->vroot = value ? gc_pin(value, false) : 0;
    }
}

/* MusicInfoFormatter::Serialize(this, ref MessagePackWriter, MusicInfo value, MessagePackSerializerOptions, method) (0x19da2e0)
 * Isolates placeholder custom chart IDs (169xxxxxx) out of stock musicBackup.inf while saving customRecords.dat! */
static ABI int hook_music_info_serialize(HookRegs *r) {
    void *formatter = (void *)r->rdi;
    void *writer = (void *)r->rsi;
    void *music_info = (void *)r->rdx;
    void *options = (void *)r->rcx;
    void *method = (void *)r->r8;
    if (!original_music_info_serialize || !music_info) return 0;

    save_custom_records(music_info);

    void *dict = *(void **)((char *)music_info + 0x20);
    RemovedCustomRecList *removed = (RemovedCustomRecList *)calloc(1, sizeof(RemovedCustomRecList));
    if (dict && removed) {
        dict_each(dict, collect_custom_dict_entry, removed);
        for (size_t i = 0; i < removed->count; ++i) {
            dict_remove_safe(dict, removed->items[i].key);
        }
    }

    typedef void (ABI *SerializeFn)(void *fmt, void *wr, void *val, void *opt, void *m);
    ((SerializeFn)original_music_info_serialize)(formatter, writer, music_info, options, method);

    if (dict && removed) {
        for (size_t i = 0; i < removed->count; ++i) {
            if (find_song_by_obj(removed->items[i].key) != NULL) {
                dict_add_safe(dict, removed->items[i].key, removed->items[i].val);
            }
            if (removed->items[i].kroot) gc_unpin(removed->items[i].kroot);
            if (removed->items[i].vroot) gc_unpin(removed->items[i].vroot);
        }
    }
    if (removed) free(removed);

    r->rax = 0;
    return 1;
}

static ABI int hook_create_music_record(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *mid = (void *)r->rsi;
    typedef void *(ABI *CreateRecFn)(void *self, void *mid, void *method);
    void *record = original_create_music_record ? ((CreateRecFn)original_create_music_record)(self, mid, (void *)r->rdx) : NULL;
    CustomSong *song = find_song_by_obj(mid);
    if (record && song) {
        ensure_extra_difficulty_records(record);
        if (self == s_cached_music_info && !song->managed_record) {
            song->managed_record = record;
            song->root_record = gc_pin(record, false);
        }
    }
    r->rax = (uint64_t)record;
    return 1;
}

static ABI int hook_get_music_record(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *mid = (void *)r->rsi;
    CustomSong *song = find_song_by_obj(mid);
    if (!song) return 0;

    if (self && self == s_cached_music_info && song->managed_record) {
        r->rax = (uint64_t)song->managed_record;
        return 1;
    }

    typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);
    void *record = original_get_music_record ? ((GetRecFn)original_get_music_record)(self, mid, (void *)r->rdx) : NULL;
    if (!record && self) {
        typedef void *(ABI *CreateRecFn)(void *self, void *mid, void *method);
        CreateRecFn create_rec = (CreateRecFn)CODE(CC_MUSIC_INFO_CREATE_REC);
        record = create_rec(self, mid, NULL);
        if (record) {
            uint32_t rroot = gc_pin(record, false);
            void *dict = *(void **)((char *)self + 0x20);
            if (dict) dict_add_safe(dict, mid, record);
            gc_unpin(rroot);
        }
    }
    if (record) {
        ensure_extra_difficulty_records(record);
        if (self && self == s_cached_music_info && !song->managed_record) {
            song->managed_record = record;
            song->root_record = gc_pin(record, false);
        }
    }
    r->rax = (uint64_t)record;
    return 1;
}

static void unlock_all_custom_records(void *self) {
    if (!self || song_count == 0) return;
    ensure_il2cpp_api();

    if (self != s_cached_music_info) {
        for (size_t i = 0; i < song_count; ++i) {
            if (songs[i].root_record) {
                gc_unpin(songs[i].root_record);
                songs[i].root_record = 0;
            }
            songs[i].managed_record = NULL;
        }
        s_cached_music_info = self;
        s_custom_records_loaded_once = false;
    }

    void *dict = *(void **)((char *)self + 0x20);
    typedef void *(ABI *GetRecFn)(void *self, void *mid, void *method);
    typedef void *(ABI *CreateRecFn)(void *self, void *mid, void *method);
    CreateRecFn create_rec = (CreateRecFn)CODE(CC_MUSIC_INFO_CREATE_REC);

    int unlocked_count = 0;
    for (size_t i = 0; i < song_count; ++i) {
        CustomSong *song = &songs[i];
        ensure_song_managed_objects(song);
        if (!song->managed_id) continue;

        void *record = song->managed_record;
        if (!record && original_get_music_record) {
            record = ((GetRecFn)original_get_music_record)(self, song->managed_id, NULL);
        }
        if (!record) {
            record = create_rec(self, song->managed_id, NULL);
            if (record) {
                uint32_t rroot = gc_pin(record, false);
                if (dict) dict_add_safe(dict, song->managed_id, record);
                gc_unpin(rroot);
            }
        }
        if (record) {
            ensure_extra_difficulty_records(record);
            if (!song->managed_record) {
                song->managed_record = record;
                song->root_record = gc_pin(record, false);
            }
            ++unlocked_count;
        }
    }
    if (!s_custom_records_loaded_once) {
        load_custom_records(self);
    }
    printf("CustomCharts: Records ready, ensured %d custom songs unlocked (with 6-difficulty records)\n", unlocked_count);
}

static ABI int hook_loaded_data_music_records(HookRegs *r) {
    void *self = (void *)r->rdi;
    if (original_loaded_data_music_records) {
        typedef void (ABI *LoadedFn)(void *self, void *method);
        ((LoadedFn)original_loaded_data_music_records)(self, (void *)r->rsi);
    }
    unlock_all_custom_records(self);
    r->rax = 0;
    return 1;
}

static ABI int hook_setup_dlc_music_records(HookRegs *r) {
    void *self = (void *)r->rdi;
    if (original_setup_dlc_music_records) {
        typedef void (ABI *SetupDlcFn)(void *self, void *dlc_ids, void *method);
        ((SetupDlcFn)original_setup_dlc_music_records)(self, (void *)r->rsi, (void *)r->rdx);
    }
    unlock_all_custom_records(self);
    r->rax = 0;
    return 1;
}

static ABI int hook_register_play_style_record(HookRegs *r) {
    void *cache = (void *)r->rdi;
    void *mid = (void *)r->rdx;
    int diff = (int)r->rcx;
    if (diff == 4 || diff == 5) {
        if (!find_song_by_obj(mid)) {
            r->rax = 0;
            return 1; /* Ignore invalid extra difficulty on stock songs */
        }
        bool ready = false;
        if (cache) {
            expand_int_array(cache, 0x20);
            void *score_arr = (void *)load64((char *)cache + 0x20);
            bool arr_ok = (score_arr && load64((char *)score_arr + 0x18) >= MAX_SLOTS);
            void *ranking = (void *)load64((char *)cache + 0x30);
            void *dict = ranking ? (void *)load64((char *)ranking + 0x20) : NULL;
            void *list = dict ? dict_try_get_value(dict, mid) : NULL;
            if (list) {
                int count = (int)load32((char *)list + 0x18);
                void *items = (void *)load64((char *)list + 0x10);
                if (items && count >= 3 && count < MAX_SLOTS && load64((char *)items + 0x18) >= 1) {
                    void *first = (void *)load64((char *)items + 0x20);
                    if (first) {
                        int32_t base_prio = (int32_t)load32((char *)first + 0x10);
                        void *all_list = (void *)load64((char *)ranking + 0x10);
                        for (int slot = count; slot < MAX_SLOTS; ++slot) {
                            void *fresh = clone_object(first);
                            if (!fresh) break;
                            uint32_t froot = gc_pin(fresh, false);
                            put32((char *)fresh + 0x10, (uint32_t)(base_prio + slot));
                            put32((char *)fresh + 0x14, 0);
                            put64((char *)fresh + 0x18, (uint64_t)mid);
                            put32((char *)fresh + 0x20, (uint32_t)slot);
                            list_add(list, fresh);
                            if (all_list) list_add(all_list, fresh);
                            gc_unpin(froot);
                        }
                    }
                }
                ready = arr_ok && ((int)load32((char *)list + 0x18) > diff);
            }
        }
        if (!ready) {
            r->rax = 0;
            return 1; /* Never allow OriginalRegister to index out-of-bounds if cache isn't expanded */
        }
    }
    return 0;
}

static void *original_music_record_total_play_count = NULL;
static void *original_profica_clear_ctor = NULL;
static void *original_profica_create_profile = NULL;
static int s_profile_build_depth = 0;

/* MusicRecord::GetTotalPlayCount (0x45fddc0) — include Beginner(4) & Chaos(5) play counts for custom songs */
static ABI int hook_music_record_total_play_count(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *method = (void *)r->rsi;
    if (!original_music_record_total_play_count) return 0;

    typedef int32_t (ABI *GetTotalPCFn)(void *self, void *method);
    typedef int32_t (ABI *GetPCFn)(void *psr, void *method);
    int32_t total = ((GetTotalPCFn)original_music_record_total_play_count)(self, method);
    GetPCFn get_pc = (GetPCFn)CODE(CC_PSR_GET_TOTAL_PC);

    if (self) {
        void *mid = (void *)load64((char *)self + 0x18);
        void *dict = (void *)load64((char *)self + 0x28);
        if (find_song_by_obj(mid) && dict) {
            const int extra_slots[2] = {4, 5};
            for (int k = 0; k < 2; ++k) {
                void *diff_rec = NULL;
                if (!dict_find_i32(dict, extra_slots[k], &diff_rec) || !diff_rec) continue;
                void *style_dict = (void *)load64((char *)diff_rec + 0x18);
                if (!style_dict) continue;
                for (int st = 0; st < 3; ++st) {
                    void *psr = NULL;
                    if (dict_find_i32(style_dict, st, &psr) && psr) {
                        total += get_pc(psr, NULL);
                    }
                }
            }
        }
    }
    r->rax = (uint64_t)(uint32_t)total;
    return 1;
}

/* ProficaClearData::.ctor (0x0acb760) — temporarily expand [self + 0x18] from 4 to 6 elements
 * during ProficaData::CreateProfile so Beginner(4) & Chaos(5) records never crash at 0x0ac8f7f! */
static ABI int hook_profica_clear_ctor(HookRegs *r) {
    void *self = (void *)r->rdi;
    void *method = (void *)r->rsi;
    if (!original_profica_clear_ctor) return 0;

    typedef void (ABI *CtorFn)(void *self, void *method);
    ((CtorFn)original_profica_clear_ctor)(self, method);

    if (s_profile_build_depth > 0 && self) {
        void *old_arr = (void *)load64((char *)self + 0x18);
        if (old_arr && load64((char *)old_arr + 0x18) == 4) {
            void *first = (void *)load64((char *)old_arr + 0x20);
            void *elem_klass = object_class(first);
            void *new_arr = alloc_managed_array_by_arr_class(object_class(old_arr), MAX_SLOTS);
            if (new_arr && elem_klass) {
                uint32_t aroot = gc_pin(new_arr, false);
                memcpy((char *)new_arr + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));
                CtorFn diff_clear_ctor = (CtorFn)CODE(CC_PROFICA_DIFF_CLEAR_CTOR); /* ProficaDifficultyClearData::.ctor */
                for (int slot = 4; slot < MAX_SLOTS; ++slot) {
                    void *fresh = alloc_managed_object(elem_klass);
                    if (!fresh) break;
                    uint32_t froot = gc_pin(fresh, false);
                    diff_clear_ctor(fresh, NULL);
                    put64((char *)new_arr + 0x20 + (size_t)slot * sizeof(void *), (uint64_t)fresh);
                    gc_unpin(froot);
                }
                put64((char *)self + 0x18, (uint64_t)new_arr);
                gc_unpin(aroot);
            }
        }
    }
    r->rax = 0;
    return 1;
}

/* ProficaData::CreateProfile (0x0ac70c0) — bracket with s_profile_build_depth and shrink [clear + 0x18]
 * back to 4 elements afterwards so stock Profica serialization stays 100% pure! */
static ABI int hook_profica_create_profile(HookRegs *r) {
    void *self = (void *)r->rdi;
    int32_t clear_stone = (int32_t)r->rsi;
    void *method = (void *)r->rdx;
    if (!original_profica_create_profile) return 0;

    typedef void *(ABI *CreateProfFn)(void *self, int32_t clear_stone, void *method);
    ++s_profile_build_depth;
    void *result = ((CreateProfFn)original_profica_create_profile)(self, clear_stone, method);
    --s_profile_build_depth;

    if (result) {
        void *clear = (void *)load64((char *)result + 0xc8);
        void *old_arr = clear ? (void *)load64((char *)clear + 0x18) : NULL;
        if (old_arr && load64((char *)old_arr + 0x18) > 4) {
            void *shrunk = alloc_managed_array_by_arr_class(object_class(old_arr), 4);
            if (shrunk) {
                memcpy((char *)shrunk + 0x20, (char *)old_arr + 0x20, 4 * sizeof(void *));
                put64((char *)clear + 0x18, (uint64_t)shrunk);
            }
        }
    }
    r->rax = (uint64_t)result;
    return 1;
}

/* -------------------------------------------------------------------------- */
/* RHYTHMIA (TOTALRHYTHPOVALUE) 4-LAYER PROTECTION HOOKS                      */
/* -------------------------------------------------------------------------- */

/* Layer 1: RhythpoCalculator::calculateMSPlayBonus (0x45bded0 & wrapper 0x45c03b0)
 * Clamp msDifficulty (r->rdx) from 4/5 -> 0..3 so all 28 inlined getTableBonus sites
 * never read out-of-bounds float[4] and never produce x86-64 vcvttss2si NaN -> INT_MIN! */
static ABI int hook_rhythpo_calc_ms_bonus(HookRegs *r) {
    int diff = (int)r->rdx;
    if (diff < 0 || diff > 3) {
        r->rdx = (uint64_t)clamp_extra_slot(diff);
    }
    return 0;
}

/* Layer 2: RhythpoCalculator::getTableBonus (0x45c0c50) — clamp msDifficulty (r->rsi) to 0..3 */
static ABI int hook_rhythpo_get_table_bonus(HookRegs *r) {
    int diff = (int)r->rsi;
    if (diff < 0 || diff > 3) {
        r->rsi = (uint64_t)clamp_extra_slot(diff);
    }
    return 0;
}

/* Layer 3: RhythpoResultData::AddBonus (0x45bca40) — reject negative or corrupt bonus values */
static ABI int hook_rhythpo_add_bonus(HookRegs *r) {
    int32_t bonus = (int32_t)r->rdx;
    if (bonus < 0 || bonus > 100000) {
        r->rdx = 0;
    }
    return 0;
}

/* Layer 4: RhythpoInfo::set_TotalRhythpoValue (0x0ace990) — monotonic guard so TotalRhythpoValue
 * can NEVER decrease below its current stored value! */
static ABI int hook_rhythpo_set_total(HookRegs *r) {
    void *self = (void *)r->rdi;
    int32_t new_val = (int32_t)r->rsi;
    if (self) {
        void *info = (void *)load64((char *)self + 0x20);
        void *clamper = info ? (void *)load64((char *)info + 0x10) : NULL;
        if (clamper) {
            int32_t cur_val = (int32_t)load32((char *)clamper + 0x18);
            if (new_val < cur_val) {
                r->rsi = (uint64_t)(uint32_t)cur_val;
            }
        }
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* HOOK INSTALLATION                                                          */
/* -------------------------------------------------------------------------- */

static const GameSite s_cc_sites[CC_SITES] = {
    /* H_MUSIC_TABLE_CONVERT */ {"Table.MusicTable::ConvertData", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 48 83 e4 e0 48 83 ec 40 4c 8b 25 ?? ?? ?? ??"}},
    /* H_SERIES_TABLE_CONVERT */ {"Table.SeriesTable::ConvertData", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 e4 e0 48 83 ec 40 4c 8b 3d ?? ?? ?? ??"}},
    /* H_SOUND_TABLE_GET_LABEL */ {"Table.SoundTable::GetLabelData", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_MUSIC_ID_GET_NAME */ {"Game.MusicID::GetName", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 48 83 ec 30 4c 8b 35 ?? ?? ?? ?? 48 89 fb"}},
    /* H_MUSIC_ID_GET_SORT_NAME */ {"Game.MusicID::GetSortName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ??"}},
    /* H_SERIES_ID_GET_NAME */ {"Game.SeriesID::GetName", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 48 83 ec 30 4c 8b 35 ?? ?? ?? ?? 48 89 fb"}},
    /* H_SERIES_ID_GET_SHORT_NAME */ {"Game.SeriesID::GetShortName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ??"}},
    /* H_LANGUAGE_TRANSLATION */ {"I2.Loc.LanguageSourceData::TryGetTranslation", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ??"}},
    /* H_JACKET_GET_ADDR */ {"Game.File.JacketSpriteLoader::GetAddressImplement", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_JACKET_GET_FILENAME */ {"Game.File.JacketSpriteLoader::getFileName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_SERIES_LOGO_GET_ADDR */ {"Game.File.SeriesLogoSpriteLoader::GetAddressImplement", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ??"}},
    /* H_SERIES_LOGO_GET_SUBASSET */ {"Game.File.SeriesLogoSpriteLoader::GetSubAssetName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 48 83 ec 48 4c 8b 3d ?? ?? ?? ??"}},
    /* H_LOAD_ASSET_SYNC */ {"Gummi.File.FileAccessor/AddressableAccessor::LoadAssetSync", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ??"}},
    /* H_LOAD_ASSET_ASYNC */ {"Gummi.File.FileAccessor/AddressableAccessor::LoadAssetAsync", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 81 ec 88 00 00 00 4c 8b 3d ?? ?? ?? ??"}},
    /* H_GET_ASSETS */ {"Gummi.File.AsyncOperations.AddressableAssetAsyncOperation::GetAssets", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 48 83 e4 e0 48 83 ec 60 4c 8b 3d ?? ?? ?? ??"}},
    /* H_IS_DONE */ {"Gummi.File.AsyncOperations.AddressableAssetAsyncOperation::IsDone", 0, 0, 0,
     {"48 83 c7 20 31 f6 | e9 ?? ?? ?? ?? 0f 1f 44 00 00 55 48 89 e5"}},
    /* H_PROGRESS */ {"Gummi.File.AsyncOperations.AddressableAssetAsyncOperation::Progress", 0, 0, 0,
     {"55 48 89 e5 53 | 50 48 89 fb 48 83 c3 20 80 3d ?? ?? ?? ?? 00"}},
    /* H_RELEASE_ASSET */ {"Gummi.File.FileAccessor/AddressableAccessor::ReleaseAsset", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 48 83 ec 40 4c 8b 35 ?? ?? ?? ?? 48 89 f3"}},
    /* H_TEXT_ASSET_GET_BYTES */ {"UnityEngine.TextAsset::get_bytes", 0, 0, 0,
     {"55 48 89 e5 53 50 | 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 85 c0 75 18"}},
    /* H_TEXT_ASSET_GET_DATA_PTR */ {"UnityEngine.TextAsset::GetDataPtr", 0, 0, 0,
     {"55 48 89 e5 53 | 50 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 85 c0 75 18"}},
    /* H_TEXT_ASSET_GET_DATA_SIZE */ {"UnityEngine.TextAsset::GetDataSize", 0, 0, 0,
     {"55 48 89 e5 53 | 50 48 8b 05 ?? ?? ?? ?? 48 89 fb 48 85 c0 75 18"}},
    /* H_THEATER_BEGIN_START */ {"Game.Scene.MusicStage.TheaterController/<BeginTheaterStart>d__128::MoveNext", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_THEATER_FINISH */ {"Game.Scene.MusicStage.TheaterController::FinishTheater", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 48 83 ec 10 8b 87 c4 00 00 00"}},
    /* H_CHANGE_TO_VIEW_DIFF */ {NULL, 0, 1, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 49 89 f6 89 fb 75 13 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 8b 35 ?? ?? ?? ?? 4c 89 f7"}},
    /* H_SELECTING_MS_CTOR */ {"Game.Scene.MusicSelect.SelectingMSData::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_SELECTING_MS_SET_DIFF */ {"Game.Scene.MusicSelect.SelectingMSData::SetDifficulty", 0, 0, 0,
     {"89 77 2c c3 66 66 66 2e 0f 1f 84 00 00 00 00 00 | 89 77 30 c3"}},
    /* H_SELECTING_GET_DIFF_VAL */ {"Game.Scene.MusicSelect.SelectingTableData::GetDifficultyValue", 0, 0, 0,
     {"48 8b 47 18 48 63 ce | 48 8b 80 98 00 00 00 8b 44 88 20 c3 66 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* H_PANEL_CTOR */ {"Game.Scene.MusicSelect.ListItemMusicPanel::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_PANEL_SET_DIFF */ {"Game.Scene.MusicSelect.ListItemMusicPanel::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 80 7f 40 00 74 09 5b 41 5c"}},
    /* H_MENU_ADD_SONG */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::addListItemMusicPanel", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_MENU_FILTER_PANEL */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::filterItem", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_MENU_IS_PLAYABLE */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::isPlayable", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 49 89 f6 48 89 fb"}},
    /* H_LIST_CHANGE_VIEW_DIFF */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::ChangeViewDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 41 89 f6"}},
    /* H_LIST_SET_DIFF */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 48 4c 8b 3d ?? ?? ?? ??",
      "55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 48 4c 8b 25 ?? ?? ?? ??"}},
    /* H_LIST_SET_LAST_DIFF */ {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::set_lastSelectedDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 41 89 f6 48 89 fb"}},
    /* H_JACKET_SET_LAST_DIFF */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::set_lastSelectedDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 41 89 f6 48 89 fb"}},
    /* H_SORT_REFRESH_BADGES */ {"Game.Scene.MusicSelect.MusicSelectSortListPartsMain::RefreshBadges(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_JACKET_REFRESH_VIEW */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::RefreshView", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 28 80 3d ?? ?? ?? ?? 00"}},
    /* H_JACKET_CHANGE_PLAY_DATA */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::ChangePlayData", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_JACKET_SET_DIFF */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_JACKET_EXECUTE */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::OnExecutedScenePartsMain", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3"}},
    /* H_JACKET_DIFF_REACTION */ {"Game.Scene.MusicSelect.MusicSelectJacketPartsMain::<OnExecutedScenePartsMain>g__getChangeDifficultyReaction|59_0", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 49 89 d6"}},
    /* H_PANEL_VIEW_ON_SET_VIEW */ {"Game.Scene.MusicSelect.ListItemMusicPanelView::OnSetView", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ??"}},
    /* H_POPUP_ON_READY */ {"Game.Scene.WindowSetup.WindowSetupBodyPartsMain::OnReady", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 48 89 fb"}},
    /* H_POPUP_SET_STATUS */ {"Game.Scene.WindowSetup.MusicStatusBehaviour::SetMusicStatus", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 58 48 8b 05 ?? ?? ?? ??",
      "55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ??"}},
    /* H_POPUP_SUBMIT */ {"Game.Scene.WindowSetup.WindowSetupBodyPartsMain::OnUIImmediateSubmit", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3"}},
    /* H_POPUP_EXECUTE */ {"Game.Scene.WindowSetup.WindowSetupBodyPartsMain::OnExecutedScenePartsMain", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 f7"}},
    /* H_WINDOW_SETUP_CHANGE_DIFF */ {"Game.Scene.WindowSetup.WindowSetupBodyPartsMain::<OnExecutedScenePartsMain>g__changeDifficulty|20_0", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 41 89 f6 48 89 fb"}},
    /* H_DIFF_LIST_SET_DIFF */ {"Game.Prefab.DifficultyListBehaviour::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 48 8b 5f 20"}},
    /* H_DIFF_LIST_PLAY_SE */ {"Game.Prefab.DifficultyListBehaviour::PlayChangeSE", 0, 0, 0,
     {"55 48 89 e5 53 50 | 80 3d ?? ?? ?? ?? 00 89 fb 75 13 48 8d 3d ?? ?? ?? ??"}},
    /* H_SET_ACTIVE_EDITION_MARK */ {"Game.Prefab.MSMarkBehaviour::SetActiveEditionMark", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 48 8b 05 ?? ?? ?? ?? 4c 8b 7f 20"}},
    /* H_LEVEL_BASE_SET_DIFF */ {"Game.Prefab.MusicLevelBaseBehaviour::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 48 8b 47 18 41 89 d6 41 89 f7"}},
    /* H_MUSIC_NAME_SET_COLOR */ {"Game.Scene.MusicSelect.MusicNameBehaviour::SetMusicNameColor", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 38 80 3d ?? ?? ?? ?? 00"}},
    /* H_MUSIC_STATUS_SET_COLOR */ {"Game.Scene.WindowSetup.MusicStatusBehaviour/MusicTitle::SetColor", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 80 3d ?? ?? ?? ?? 00 41 89 f6"}},
    /* H_DIFF_ICON_LOADER_SUB */ {"Game.File.DifficultyIconSpriteLoader::GetSubAssetName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 58 4c 8b 25 ?? ?? ?? ??"}},
    /* H_DIFF_SEMI_LOADER_ADDR */ {"Game.File.DifficultySemicircleSpriteLoader::GetAddressImplement", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 58 4c 8b 25 ?? ?? ?? ??"}},
    /* H_DIFF_SEMI_LOADER_SUB */ {"Game.File.DifficultySemicircleSpriteLoader::GetSubAssetName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 58 4c 8b 25 ?? ?? ?? ??"}},
    /* H_LEVEL_BASE_LOADER_SUB */ {"Game.File.MusicLevelBaseSpriteLoader::GetSubAssetName", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 58 4c 8b 25 ?? ?? ?? ??"}},
    /* H_STAGE_EFFECT_LOADER_ADDR */ {"Game.File.MusicStageEffectPrefabLoader::GetAddressImplement", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 41 89 f6"}},
    /* H_LOAD_TITLE_PLATE_MOVENEXT */ {"Game.Scene.MusicResult.MusicResultTitlePartsMain/MusicTitleDisplay/<>c__DisplayClass18_0/<<Setup>g__setDifficultyIconSprite|2>d::MoveNext", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 28 48 8b 05 ?? ?? ?? ??"}},
    /* H_GET_DIFF_ICON_SPRITE */ {"Game.Scene.MusicStage.MusicStageLoader::GetDifficultyIconSprite", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 41 89 fe 75 2b 48 8d 3d ?? ?? ?? ??"}},
    /* H_GET_DIFF_SEMI_SPRITE */ {"Game.Scene.MusicStage.MusicStageLoader::GetDifficultySemicircleIconSprite", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 41 89 fe 75 2b 48 8d 3d ?? ?? ?? ??"}},
    /* H_GET_DIFF_BANNER_SPRITE */ {"Game.Scene.MusicStage.MusicStageLoader::GetDifficultyBannerSprite", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 41 89 fe 75 2b 48 8d 3d ?? ?? ?? ??"}},
    /* H_STAGE_INTRO_SETUP */ {"Game.Scene.MusicStage.MusicStageTelop::setupMusicIntroTelop", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 38 80 3d ?? ?? ?? ?? 00"}},
    /* H_STAGE_MUSIC_INFO_SETUP */ {"Game.Scene.MusicStage.MusicInfo::Setup", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_MUSIC_INTRO_SETUP */ {"Game.Scene.MusicStage.MusicIntroduction::Setup", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 44 89 4d d0"}},
    /* H_CREATE_MS_PARTS */ {"Game.Scene.MusicStage.MusicStageMainLoop::createMSParts", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 48 83 e4 e0 48 83 ec 60 4c 8b 25 ?? ?? ?? ??"}},
    /* H_LIVE_INFO_SETUP_TEXT */ {"Game.Scene.MusicStage.LiveInfoController::setupFixedText", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ??"}},
    /* H_SUPPORT_MGR_SETUP */ {"Game.Scene.MusicStage.SupportManager::Setup", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 68 48 8b 05 ?? ?? ?? ??"}},
    /* H_SUPPORT_START */ {"Game.Scene.MusicStage.SupportManager::SupportStart", 0, 0, 0,
     {"55 48 89 e5 53 | 50 c6 47 40 01 48 8b 0d ?? ?? ?? ?? 48 8b 5f 30"}},
    /* H_SET_PLAY_SETTING_5 */ {"Game.Scene.MusicStage.MusicStagePlaySetting::SetPlaySetting(5)", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_SET_PLAY_SETTING_10 */ {"Game.Scene.MusicStage.MusicStagePlaySetting::SetPlaySetting(10)", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    /* H_TRIGGER_LOADER_GET_ADDR */ {"Game.File.TriggerDataLoader::GetAddressImplement", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ??"}},
    /* H_RUNTIME_CONTEXT_SETUP */ {"Game.Rhythm.RuntimeContext::Setup", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ??"}},
    /* H_DAMAGE_CALC_SETUP */ {"Game.Scene.MusicStage.DamageCalculator::Setup", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 83 ec 10 80 3d ?? ?? ?? ?? 00 49 89 f6"}},
    /* H_BURST_GAUGE_SETUP */ {"Game.Scene.MusicStage.BurstGaugeUtil::Setup", 0, 0, 0,
     {"55 48 89 e5 53 50 | 80 3d ?? ?? ?? ?? 00 89 fb 75 1f 48 8d 3d ?? ?? ?? ??"}},
    /* H_ABILITY_CONDITION_SETUP */ {"Game.Scene.MusicStage.AbilityTriggerCondition::SetupCondition", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 80 3d ?? ?? ?? ?? 00"}},
    /* H_SUMMON_BAR_UTIL_SETUP */ {"Game.Scene.MusicStage.SummonBarUtil::Setup", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 83 ec 10 80 3d ?? ?? ?? ?? 00 49 89 f6"}},
    /* H_COPY_MS_PLAY_DATA */ {"Game.Scene.MusicResult.MusicResultPlayData::CopyMSPlayData", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 ec 48 48 8b 05 ?? ?? ?? ??"}},
    /* H_MUSIC_TITLE_DISPLAY_SETUP */ {"Game.Scene.MusicResult.MusicResultTitlePartsMain/MusicTitleDisplay::Setup", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 48 83 e4 e0 48 81 ec 80 00 00 00"}},
    /* H_RETRY_WINDOW_ON_UPDATE */ {"Game.Scene.MusicResult.RetryWindowMusicNameBehaviour::OnUpdate", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 d6"}},
    /* H_IMAGE_ANIM_PROPS */ {"UnityEngine.UI.Image::OnDidApplyAnimationProperties", 0, 0, 0,
     {"55 48 89 e5 53 50 | 48 8b 07 48 89 fb 48 8b b0 10 03 00 00 ff 90 08 03 00 00"}},
    /* H_IMAGE_SET_SPRITE */ {"UnityEngine.UI.Image::set_sprite", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 4c 8b 25 ?? ?? ?? ??"}},
    /* H_GET_MUSIC_RECORD */ {"Game.Backup.MusicInfo::GetMusicRecord", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 49 89 f6 48 89 fb"}},
    /* H_CREATE_MUSIC_RECORD */ {"Game.Backup.MusicInfo::createMusicRecord", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 f6"}},
    /* H_LOADED_DATA_MUSIC_RECORDS */ {"Game.Backup.MusicInfo::loadedDataMusicRecords", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 48 83 e4 e0 48 83 ec 40 4c 8b 3d ?? ?? ?? ??"}},
    /* H_SETUP_DLC_MUSIC_RECORDS */ {"Game.Backup.MusicInfo::SetupDataByDLCPacks", 0, 0, 0,
     {"55 48 89 e5 53 | 50 80 3d ?? ?? ?? ?? 00 48 89 fb 75 13 48 8d 3d ?? ?? ?? ??"}},
    /* H_MUSIC_RECORD_TOTAL_PC */ {"Game.Backup.MusicInfo/MusicRecord::GetTotalPlayCount", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 fe"}},
    /* H_PROFICA_CLEAR_CTOR */ {"Game.Backup.ProficaInfo/ClearMusicNumber::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_PROFICA_CREATE_PROFILE */ {"Game.Backup.ProficaInfo::CreateMyProficaData", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 e4 e0 48 81 ec 80 01 00 00"}},
    /* H_REGISTER_PLAY_STYLE_REC */ {"Game.Scene.Record.RecordBackupCache/MusicInfoCache/PlayStyleCache::RegisterMusicPlayStyleRecord", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 48 8b 46 50 41 89 cf"}},
    /* H_MUSIC_INFO_SERIALIZE */ {"Game.MessagePackFormatter.Formatters.Game.Backup.MusicInfoFormatter::Serialize", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* H_RHYTHPO_CALC_1 */ {NULL, 0, 1, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 38 80 3d ?? ?? ?? ?? 00 4c 8b 7d 68 89 cb c5 fa 11 45 c4"}},
    /* H_RHYTHPO_CALC_2 */ {NULL, 0, 1, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 81 ec 88 00 00 00 48 8b 05 ?? ?? ?? ?? c5 fa 11 45 98 44 89 45 a0 89 8d 64 ff ff ff"}},
    /* H_RHYTHPO_GET_TABLE_BONUS */ {"Game.Logic.RhythpoCalculator::getTableBonus", 0, 0, 0,
     {"48 8b 47 18 48 63 ce | c5 fa 10 44 88 20 c3 66 90 c3 66 66 66 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* H_RHYTHPO_ADD_BONUS */ {"Game.Logic.RhythpoBonusResultData::Add", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 41 89 d6"}},
    /* H_RHYTHPO_SET_TOTAL */ {"Game.Backup.RhythpoInfo::set_TotalRhythpoValue", 0, 0, 0,
     {"48 8b 47 20 48 8b 48 10 | 8b 51 10 8b 49 14 39 f1 0f 4d ce 39 f2"}},

    /* P_COUNT_DATA_LIST_INIT */ {"Game.Scene.MusicSelect.CountDataList::InitializeList", 1, 0, 0,
     {"83 f8 04 48 89 45 c8 0f 84 ?? ?? ?? ?? 48 8b 3d ?? ?? ?? ??"}},

    /* CC_MEMBERWISE_CLONE */ {"System.Object::MemberwiseClone", 0, 0, 0,
     {"e9 ?? ?? ?? ?? 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_MUSIC_ID_IMPLICIT */ {"Game.MusicID::op_Implicit", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_SERIES_ID_IMPLICIT */ {"Game.SeriesID::op_Implicit", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_UOBJ_OP_IMPLICIT */ {"UnityEngine.Object::op_Implicit", 0, 0, 0,
     {"55 48 89 e5 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_UOBJ_SET_HIDE_FLAGS */ {"UnityEngine.Object::set_hideFlags", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_UOBJ_DESTROY */ {"UnityEngine.Object::Destroy(2)", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_UOBJ_GET_NAME */ {"UnityEngine.Object::get_name", 0, 0, 0,
     {"55 48 89 e5 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_UOBJ_SET_NAME */ {"UnityEngine.Object::set_name", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_UOBJ_CLONE_WITH_PARENT */ {"UnityEngine.Object::Internal_CloneSingleWithParent", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_AUDIO_CLIP_CREATE */ {"UnityEngine.AudioClip::Create(5)", 0, 0, 0,
     {"55 48 89 e5 48 83 ec 10 45 31 c9 48 c7 04 24 00 00 00 00"}},
    /* CC_AUDIO_CLIP_SET_DATA */ {"UnityEngine.AudioClip::SetData(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 54 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_TEX2D_CTOR */ {NULL, 0, 1, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00 45 89 c6 41 89 cf 41 89 d4 41 89 f5 48 89 fb 75 1f"}},
    /* CC_TEX_SET_FILTER */ {"UnityEngine.Texture::set_filterMode", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_TEX_SET_WRAP */ {"UnityEngine.Texture::set_wrapMode", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_TEX2D_LOAD_RAW */ {"UnityEngine.Texture2D::LoadRawTextureData(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_TEX2D_APPLY */ {"UnityEngine.Texture2D::Apply(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 48 8b 07"}},
    /* CC_SPRITE_CREATE */ {"UnityEngine.Sprite::CreateSprite", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_SPRITE_GET_RECT */ {"UnityEngine.Sprite::get_rect", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 20 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_SPRITE_GET_BORDER */ {"UnityEngine.Sprite::get_border", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 20 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_SPRITE_GET_PPU */ {"UnityEngine.Sprite::get_pixelsPerUnit", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_SPRITE_GET_PIVOT */ {"UnityEngine.Sprite::get_pivot", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 10 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_TEXT_ASSET_INTERNAL_CREATE */ {"UnityEngine.TextAsset::Internal_CreateInstance", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_CAM_SET_FMS_PRACTICE */ {"Game.Scene.MusicStage.CameraController::SetFmsPracticePos", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_CAM_SET_BMS_PRACTICE */ {"Game.Scene.MusicStage.CameraController::SetBmsPracticePos", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_BEHAVIOUR_SET_ENABLED */ {"UnityEngine.Behaviour::set_enabled", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_COMP_GET_TRANSFORM */ {"UnityEngine.Component::get_transform", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_COMP_GET_GAMEOBJECT */ {"UnityEngine.Component::get_gameObject", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_GO_GET_COMP_BY_NAME */ {"UnityEngine.GameObject::GetComponentByName", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_GO_GET_TRANSFORM */ {"UnityEngine.GameObject::get_transform", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_GO_SET_ACTIVE */ {"UnityEngine.GameObject::SetActive", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_RT_GET_ANCHORED_POS */ {"UnityEngine.RectTransform::get_anchoredPosition", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 10 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_RT_SET_ANCHORED_POS */ {"UnityEngine.RectTransform::set_anchoredPosition", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 10 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_RT_GET_SIZE_DELTA */ {"UnityEngine.RectTransform::get_sizeDelta", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 10 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_RT_SET_SIZE_DELTA */ {"UnityEngine.RectTransform::set_sizeDelta", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 10 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_TR_GET_PARENT */ {"UnityEngine.Transform::get_parent", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_TR_SET_LOCAL_SCALE */ {"UnityEngine.Transform::set_localScale", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 20 4c 8b 35 ?? ?? ?? ??"}},
    /* CC_TR_GET_CHILD_COUNT */ {"UnityEngine.Transform::get_childCount", 0, 0, 0,
     {"55 48 89 e5 53 50 48 8b 05 ?? ?? ?? ??"}},
    /* CC_TR_SET_SIBLING_INDEX */ {"UnityEngine.Transform::SetSiblingIndex", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_TR_GET_CHILD */ {"UnityEngine.Transform::GetChild", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_IMAGE_GET_SPRITE */ {"UnityEngine.UI.Image::get_sprite", 0, 0, 0,
     {"48 8b 87 d0 00 00 00 c3 0f 1f 84 00 00 00 00 00"}},
    /* CC_IMAGE_SET_SPRITE */ {"UnityEngine.UI.Image::set_sprite", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_GRAPHIC_GET_COLOR */ {"UnityEngine.UI.Graphic::get_color", 0, 0, 0,
     {"c5 fb 10 47 20 c5 fb 10 4f 28 c3 0f 1f 44 00 00"}},
    /* CC_GRAPHIC_SET_COLOR */ {"UnityEngine.UI.Graphic::set_color", 0, 0, 0,
     {"c5 fa 10 57 20 c5 f8 2e d0 75 32 7a 30"}},
    /* CC_TMP_TEXT_SET_TEXT */ {"TMPro.TMP_Text::set_text", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 bf d8 00 00 00 00"}},
    /* CC_UI_TEXT_SET_TEXT */ {"UnityEngine.UI.Text::set_text", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_TMP_GET_SHARED_MAT */ {"TMPro.TMP_Text::get_fontSharedMaterial", 0, 0, 0,
     {"48 8b 87 08 01 00 00 c3 0f 1f 84 00 00 00 00 00"}},
    /* CC_TMP_SET_SHARED_MAT */ {"TMPro.TMP_Text::set_fontSharedMaterial", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_TMP_SET_FONT_MAT */ {"TMPro.TMP_Text::set_fontMaterial", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_MAT_CTOR */ {NULL, 0, 1, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00 49 89 f6 48 89 fb 75 13 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 8b 3d ?? ?? ?? ?? 83 bf e0 00 00 00 00 75 05 e8 ?? ?? ?? ?? 48 8b 05 ?? ?? ?? ?? 48 85 c0 75 18 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 85 c0 74 13 48 89 05 ?? ?? ?? ?? 48 89 df 4c 89 f6 5b 41 5e 5d ff e0 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 89 c7 31 f6 e8 ?? ?? ?? ?? 0f 0b 66 2e 0f 1f 84 00 00 00 00 00 55 48 89 e5 48 8b 05 ?? ?? ?? ??"}},
    /* CC_MAT_SET_COLOR */ {NULL, 0, 4, 1,
     {"55 48 89 e5 41 57 41 56 53 48 83 ec 38 4c 8b 3d ?? ?? ?? ?? c5 f8 29 4d b0 c5 f8 29 45 c0 48 89 f3 49 89 fe 49 8b 07 48 89 45 e0 48 8b 0d ?? ?? ?? ?? 48 85 c9 75 1b 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 85 c0 74 6a 48 89 c1 48 89 05 ?? ?? ?? ?? 48 89 df ff d1 c5 f8 28 45 c0 c5 f9 14 45 b0 89 c3 48 8b 05 ?? ?? ?? ?? 48 85 c0 c5 f8 11 45 d0 75 18 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 85 c0 74 36 48 89 05 ?? ?? ?? ?? 48 8d 55 d0 4c 89 f7 89 de ff d0"}},
    /* CC_UIGRAD_SET_COLOR1 */ {"Coffee.UIExtensions.UIGradient::set_color1", 0, 0, 0,
     {"55 48 89 e5 c5 fb 10 57 24 c5 fb 10 5f 2c"}},
    /* CC_UIGRAD_SET_COLOR2 */ {"Coffee.UIExtensions.UIGradient::set_color2", 0, 0, 0,
     {"55 48 89 e5 c5 fb 10 57 34 c5 fb 10 5f 3c"}},
    /* CC_UIGRAD_SET_COLOR3 */ {"Coffee.UIExtensions.UIGradient::set_color3", 0, 0, 0,
     {"55 48 89 e5 c5 fb 10 57 44 c5 fb 10 5f 4c"}},
    /* CC_UIGRAD_SET_COLOR4 */ {"Coffee.UIExtensions.UIGradient::set_color4", 0, 0, 0,
     {"55 48 89 e5 c5 fb 10 57 54 c5 fb 10 5f 5c"}},
    /* CC_MAT_GUI_SHADOW_SET_COLOR */ {"Gummi.Graphics.MaterialGuiTextShadow::set_EffectColor", 0, 0, 0,
     {"55 48 89 e5 c5 f8 16 c1 31 f6 c5 f8 11 47 20"}},
    /* CC_UI_SHADOW_SET_COLOR */ {"UnityEngine.UI.Shadow::set_effectColor", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 83 ec 20 80 3d ?? ?? ?? ?? 00"}},
    /* CC_LAYOUT_FORCE_REBUILD */ {"UnityEngine.UI.LayoutRebuilder::ForceRebuildLayoutImmediate", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_DIFF_LIST_SET_DIFF */ {"Game.Prefab.DifficultyListBehaviour::SetDifficulty", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_DIFF_BTN_PLAY_UNSEL */ {"Game.Prefab.DifficultyListBehaviour/DifficultyButton::PlayUnSelectAnim", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_DIFF_BTN_PLAY_SEL */ {"Game.Prefab.DifficultyListBehaviour/DifficultyButton::PlaySelectAnim", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_DIFF_BASE_SET_ACTIVE */ {"Game.Prefab.DifficultyListBehaviour/ButtonBase::SetActive", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 54 53 48 8b 05 ?? ?? ?? ??"}},
    /* CC_DIFF_BTN_SET_ACTIVE */ {"Game.Prefab.DifficultyListBehaviour/DifficultyButton::SetActive", 0, 0, 0,
     {"55 48 89 e5 41 56 53 48 8b 0d ?? ?? ?? ??"}},
    /* CC_DIFF_LIST_PLAY_SE */ {"Game.Prefab.DifficultyListBehaviour::PlayChangeSE", 0, 0, 0,
     {"55 48 89 e5 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_PANEL_CTOR */ {"Game.Scene.MusicSelect.ListItemMusicPanel::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_UILIST_ADD_ITEM */ {"Gummi.Scene.UI.UIList::AddListItem", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 80 3d ?? ?? ?? ?? 00"}},
    /* CC_UIOBJECT_END_ACTION */ {"Gummi.Scene.UI.UIObject::EndAction", 0, 0, 0,
     {"55 48 89 e5 41 56 53 c6 47 30 00 49 89 fe"}},
    /* CC_UIOBJECT_ON_CHANGED_FLAG */ {"Gummi.Scene.UI.UIObject::onChangedActionFlag", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_MUSIC_NAME_SET_NAME */ {"Game.Scene.MusicSelect.MusicNameBehaviour::set_MusicName", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_MUSIC_NAME_SET_TEXT_COLOR */ {"Game.Scene.MusicSelect.MusicNameBehaviour::setTextColor", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_MUSIC_STATUS_SET_STATUS */ {"Game.Scene.WindowSetup.MusicStatusBehaviour::SetMusicStatus", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
    /* CC_MUSIC_REC_CREATE_DIFF_REC */ {"Game.Backup.MusicInfo/MusicRecord::createDifficultyRecord", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_MUSIC_INFO_CREATE_REC */ {"Game.Backup.MusicInfo::createMusicRecord", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 54 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_MUSIC_INFO_GET_PSR */ {"Game.Backup.MusicInfo::GetPlayStyleRecord", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 54 53 80 3d ?? ?? ?? ?? 00"}},
    /* CC_PSR_GET_TOTAL_PC */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_TotalPlayCount", 0, 0, 0,
     {"48 8b 47 50 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_GET_HIGHSCORE */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_Highscore", 0, 0, 0,
     {"48 8b 47 10 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_SET_HIGHSCORE */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_Highscore", 0, 0, 0,
     {"48 8b 47 10 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PSR_GET_UPDATE_DATE */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_UpdateDateUniversal", 0, 0, 0,
     {"48 8b 47 18 c3 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_SET_UPDATE_DATE */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_UpdateDateUniversal", 0, 0, 0,
     {"48 89 77 18 c3 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_GET_BEST_RANK */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_BestPlayRank", 0, 0, 0,
     {"8b 47 20 c3 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_SET_BEST_RANK */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_BestPlayRank", 0, 0, 0,
     {"89 77 20 c3 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_GET_MAX_CHAIN */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_MaxChainCount", 0, 0, 0,
     {"48 8b 47 28 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_SET_MAX_CHAIN */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_MaxChainCount", 0, 0, 0,
     {"48 8b 47 28 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PSR_GET_BEST_MARK */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_BestClearMark", 0, 0, 0,
     {"8b 47 30 c3 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_SET_BEST_MARK */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_BestClearMark", 0, 0, 0,
     {"89 77 30 c3 66 66 66 2e 0f 1f 84 00 00 00 00 00"}},
    /* CC_PSR_CHECK_BAR_BURIED */ {"Game.Backup.MusicInfo/PlayStyleRecord::checkBarBuried", 0, 0, 0,
     {"55 48 89 e5 48 8b 7f 38 31 f6 e8 ?? ?? ?? ??"}},
    /* CC_PSR_SET_TOTAL_PC */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_TotalPlayCount", 0, 0, 0,
     {"48 8b 47 50 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PSR_GET_TOTAL_CC */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_TotalClearCount", 0, 0, 0,
     {"48 8b 47 58 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_SET_TOTAL_CC */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_TotalClearCount", 0, 0, 0,
     {"48 8b 47 58 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PSR_GET_TOTAL_FC */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_TotalFullChainClearCount", 0, 0, 0,
     {"48 8b 47 60 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_SET_TOTAL_FC */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_TotalFullChainClearCount", 0, 0, 0,
     {"48 8b 47 60 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PSR_GET_MAX_DAMAGE */ {"Game.Backup.MusicInfo/PlayStyleRecord::get_MaxDamage", 0, 0, 0,
     {"48 8b 47 68 48 8b 48 10 8b 50 18 8b 41 14"}},
    /* CC_PSR_SET_MAX_DAMAGE */ {"Game.Backup.MusicInfo/PlayStyleRecord::set_MaxDamage", 0, 0, 0,
     {"48 8b 47 68 48 8b 48 10 8b 51 10 8b 49 14"}},
    /* CC_PROFICA_DIFF_CLEAR_CTOR */ {"Game.Backup.ProficaInfo/DifficultyRecord::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54"}},
};

typedef struct {
    int site;
    HookFn fn;
    void **orig;
} CcHookEntry;

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p, bytes, size); return p + size; }

static unsigned char *rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[] = {(unsigned char)(0x48 | (reg >= 8 ? 4 : 0)), store ? 0x89 : 0x8b, (unsigned char)(0x44 | (reg & 7) << 3), 0x24, disp};
    return emit(p, bytes, sizeof bytes);
}

static unsigned char *rsp_xmm_slot(unsigned char *p, int store, int xmm, uint8_t disp) {
    const unsigned char bytes[] = {0x0f, (unsigned char)(store ? 0x11 : 0x10), (unsigned char)(0x44 | ((xmm & 7) << 3)), 0x24, disp};
    return emit(p, bytes, sizeof bytes);
}

static unsigned char *emit_hook_stub(unsigned char *p, HookFn fn_ptr, unsigned char *function, unsigned moved, void **orig_call) {
    static const int args[6] = {7, 6, 2, 1, 8, 9};
    const int32_t frame = 136; /* 16-byte aligned (8-byte ret + 136 = 144), holds HookRegs (128 bytes) */
    p = emit(p, (const unsigned char[]){0x48, 0x81, 0xec}, 3); p = emit(p, &frame, 4);
    for (int i = 0; i < 6; ++i) p = rsp_slot(p, 1, args[i], (uint8_t)(8 * i));
    for (int x = 0; x < 4; ++x) p = rsp_xmm_slot(p, 1, x, (uint8_t)(64 + 16 * x));
    p = emit(p, (const unsigned char[]){0x48, 0x89, 0xe7}, 3);
    const uint64_t fn = (uint64_t)(uintptr_t)fn_ptr;
    p = emit(p, (const unsigned char[]){0x48, 0xb8}, 2); p = emit(p, &fn, 8);
    p = emit(p, (const unsigned char[]){0xff, 0xd0, 0x85, 0xc0}, 4);
    unsigned char *jnz = p; p = emit(p, (const unsigned char[]){0x75, 0}, 2);
    for (int i = 0; i < 6; ++i) p = rsp_slot(p, 0, args[i], (uint8_t)(8 * i));
    for (int x = 0; x < 4; ++x) p = rsp_xmm_slot(p, 0, x, (uint8_t)(64 + 16 * x));
    p = emit(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3); p = emit(p, &frame, 4);

    if (orig_call) *orig_call = p;
    p = emit(p, function, moved);
    const uint64_t back = (uint64_t)(uintptr_t)(function + moved);
    p = emit(p, (const unsigned char[]){0xff, 0x25, 0, 0, 0, 0}, 6); p = emit(p, &back, 8);

    jnz[1] = (unsigned char)(p - (jnz + 2));
    p = rsp_slot(p, 0, 0, 48); p = rsp_slot(p, 0, 2, 56);
    p = rsp_xmm_slot(p, 0, 0, 64);
    p = emit(p, (const unsigned char[]){0x48, 0x81, 0xc4}, 3); p = emit(p, &frame, 4);
    return emit(p, (const unsigned char[]){0xc3}, 1);
}

int runtime_custom_charts_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    static const CcHookEntry custom_hooks[CC_HOOK_SITES] = {
        {H_MUSIC_TABLE_CONVERT, wrap_music_table_convert, &original_music_convert},
        {H_SERIES_TABLE_CONVERT, wrap_series_table_convert, &original_series_convert},
        {H_SOUND_TABLE_GET_LABEL, wrap_sound_labels, &original_sound_labels},
        {H_MUSIC_ID_GET_NAME, hook_music_id_get_name, NULL},
        {H_MUSIC_ID_GET_SORT_NAME, hook_music_id_get_sort_name, NULL},
        {H_SERIES_ID_GET_NAME, hook_series_id_get_name, NULL},
        {H_SERIES_ID_GET_SHORT_NAME, hook_series_id_get_name, NULL},
        {H_LANGUAGE_TRANSLATION, hook_language_translation, NULL},
        {H_JACKET_GET_ADDR, hook_jacket_loader_get_address, NULL},
        {H_JACKET_GET_FILENAME, hook_jacket_loader_get_filename, NULL},
        {H_SERIES_LOGO_GET_ADDR, hook_series_logo_get_address, NULL},
        {H_SERIES_LOGO_GET_SUBASSET, hook_series_logo_get_subasset, NULL},
        {H_LOAD_ASSET_SYNC, hook_load_asset_sync, NULL},
        {H_LOAD_ASSET_ASYNC, hook_load_asset_async, NULL},
        {H_GET_ASSETS, hook_get_assets, &original_get_assets},
        {H_IS_DONE, hook_is_done, NULL},
        {H_PROGRESS, hook_progress, NULL},
        {H_RELEASE_ASSET, hook_release_asset, NULL},
        {H_TEXT_ASSET_GET_BYTES, hook_text_asset_get_bytes, NULL},
        {H_TEXT_ASSET_GET_DATA_PTR, hook_text_asset_get_data_ptr, NULL},
        {H_TEXT_ASSET_GET_DATA_SIZE, hook_text_asset_get_data_size, NULL},
        {H_THEATER_BEGIN_START, hook_theater_begin_start, NULL},
        {H_THEATER_FINISH, hook_theater_finish, NULL},
        {H_CHANGE_TO_VIEW_DIFF, hook_change_to_view_difficulty, NULL},
        {H_SELECTING_MS_CTOR, hook_selecting_ms_ctor, NULL},
        {H_SELECTING_MS_SET_DIFF, hook_selecting_ms_set_diff, NULL},
        {H_SELECTING_GET_DIFF_VAL, hook_selecting_get_diff_value, NULL},
        {H_PANEL_CTOR, hook_panel_ctor, NULL},
        {H_PANEL_SET_DIFF, hook_panel_set_diff, NULL},
        {H_MENU_ADD_SONG, hook_menu_add_song, &original_menu_add_song},
        {H_MENU_FILTER_PANEL, hook_menu_filter_panel, NULL},
        {H_MENU_IS_PLAYABLE, hook_menu_is_playable, NULL},
        {H_LIST_CHANGE_VIEW_DIFF, hook_list_change_view_diff, NULL},
        {H_LIST_SET_DIFF, hook_list_change_view_diff, NULL},
        {H_LIST_SET_LAST_DIFF, hook_sort_refresh_badges, NULL},
        {H_JACKET_SET_LAST_DIFF, hook_sort_refresh_badges, NULL},
        {H_SORT_REFRESH_BADGES, hook_sort_refresh_badges, NULL},
        {H_JACKET_REFRESH_VIEW, hook_jacket_refresh_view, &original_jacket_refresh_view},
        {H_JACKET_CHANGE_PLAY_DATA, hook_jacket_change_play_data, NULL},
        {H_JACKET_SET_DIFF, hook_jacket_set_difficulty, NULL},
        {H_JACKET_EXECUTE, hook_jacket_execute, NULL},
        {H_JACKET_DIFF_REACTION, hook_jacket_difficulty_reaction, NULL},
        {H_PANEL_VIEW_ON_SET_VIEW, hook_panel_view_on_set_view, &original_panel_view_on_set_view},
        {H_POPUP_ON_READY, hook_popup_on_ready, &original_popup_on_ready},
        {H_POPUP_SET_STATUS, hook_popup_set_status, &original_popup_set_status},
        {H_POPUP_SUBMIT, hook_popup_submit, NULL},
        {H_POPUP_EXECUTE, hook_popup_execute, NULL},
        {H_WINDOW_SETUP_CHANGE_DIFF, hook_window_setup_change_difficulty, NULL},
        {H_DIFF_LIST_SET_DIFF, hook_diff_list_set_difficulty, NULL},
        {H_DIFF_LIST_PLAY_SE, hook_clamp_rdi_diff, NULL},
        {H_SET_ACTIVE_EDITION_MARK, hook_set_active_edition_mark, &original_set_active_edition_mark},
        {H_LEVEL_BASE_SET_DIFF, hook_level_base_set_difficulty, NULL},
        {H_MUSIC_NAME_SET_COLOR, hook_music_name_set_color, NULL},
        {H_MUSIC_STATUS_SET_COLOR, hook_music_status_set_diff_color, &original_music_status_set_diff_color},
        {H_DIFF_ICON_LOADER_SUB, hook_title_plate_clamp_expert, NULL},
        {H_DIFF_SEMI_LOADER_ADDR, hook_title_plate_clamp_expert, NULL},
        {H_DIFF_SEMI_LOADER_SUB, hook_title_plate_clamp_expert, NULL},
        {H_LEVEL_BASE_LOADER_SUB, hook_title_plate_clamp_expert, NULL},
        {H_STAGE_EFFECT_LOADER_ADDR, hook_title_plate_clamp_expert, NULL},
        {H_LOAD_TITLE_PLATE_MOVENEXT, hook_load_title_plate_move_next, &original_load_title_plate_move_next},
        {H_GET_DIFF_ICON_SPRITE, hook_get_diff_icon_sprite, NULL},
        {H_GET_DIFF_SEMI_SPRITE, hook_get_diff_semicircle_sprite, &original_get_diff_semicircle},
        {H_GET_DIFF_BANNER_SPRITE, hook_get_diff_banner_sprite, NULL},
        {H_STAGE_INTRO_SETUP, hook_stage_intro_setup, &original_stage_intro_setup},
        {H_STAGE_MUSIC_INFO_SETUP, hook_stage_music_info_setup, NULL},
        {H_MUSIC_INTRO_SETUP, hook_music_intro_setup, NULL},
        {H_CREATE_MS_PARTS, hook_create_ms_parts, NULL},
        {H_LIVE_INFO_SETUP_TEXT, hook_live_info_setup_fixed_text, &original_live_info_setup_fixed_text},
        {H_SUPPORT_MGR_SETUP, hook_support_manager_setup, NULL},
        {H_SUPPORT_START, hook_support_start, NULL},
        {H_SET_PLAY_SETTING_5, hook_set_play_setting, NULL},
        {H_SET_PLAY_SETTING_10, hook_set_play_setting, NULL},
        {H_TRIGGER_LOADER_GET_ADDR, hook_trigger_data_loader_get_address, NULL},
        {H_RUNTIME_CONTEXT_SETUP, hook_runtime_context_setup, NULL},
        {H_DAMAGE_CALC_SETUP, hook_clamp_rdi_diff, NULL},
        {H_BURST_GAUGE_SETUP, hook_clamp_rdi_diff, NULL},
        {H_ABILITY_CONDITION_SETUP, hook_ability_condition_setup, NULL},
        {H_SUMMON_BAR_UTIL_SETUP, hook_summon_bar_util_setup, NULL},
        {H_COPY_MS_PLAY_DATA, hook_copy_ms_play_data, &original_copy_ms_play_data},
        {H_MUSIC_TITLE_DISPLAY_SETUP, hook_music_title_display_setup, NULL},
        {H_RETRY_WINDOW_ON_UPDATE, hook_retry_window_on_update, NULL},
        {H_IMAGE_ANIM_PROPS, hook_image_anim_props, &original_image_anim_props},
        {H_IMAGE_SET_SPRITE, hook_image_set_sprite, &original_image_set_sprite},
        {H_GET_MUSIC_RECORD, hook_get_music_record, &original_get_music_record},
        {H_CREATE_MUSIC_RECORD, hook_create_music_record, &original_create_music_record},
        {H_LOADED_DATA_MUSIC_RECORDS, hook_loaded_data_music_records, &original_loaded_data_music_records},
        {H_SETUP_DLC_MUSIC_RECORDS, hook_setup_dlc_music_records, &original_setup_dlc_music_records},
        {H_MUSIC_RECORD_TOTAL_PC, hook_music_record_total_play_count, &original_music_record_total_play_count},
        {H_PROFICA_CLEAR_CTOR, hook_profica_clear_ctor, &original_profica_clear_ctor},
        {H_PROFICA_CREATE_PROFILE, hook_profica_create_profile, &original_profica_create_profile},
        {H_REGISTER_PLAY_STYLE_REC, hook_register_play_style_record, NULL},
        {H_MUSIC_INFO_SERIALIZE, hook_music_info_serialize, &original_music_info_serialize},
        {H_RHYTHPO_CALC_1, hook_rhythpo_calc_ms_bonus, NULL},
        {H_RHYTHPO_CALC_2, hook_rhythpo_calc_ms_bonus, NULL},
        {H_RHYTHPO_GET_TABLE_BONUS, hook_rhythpo_get_table_bonus, NULL},
        {H_RHYTHPO_ADD_BONUS, hook_rhythpo_add_bonus, NULL},
        {H_RHYTHPO_SET_TOTAL, hook_rhythpo_set_total, NULL},
    };

    module = image + module_base;
    if (runtime_game_code_find(s_cc_sites, CC_SITES, s_cc_code, "CustomCharts")) {
        printf("CustomCharts: hooks not installed for game %s\n", runtime_game_version());
        return -1;
    }

    /* Resolve TextAsset::get_bytes icall pointer slot from `mov rax, [rip + disp32]` at +6 */
    {
        unsigned char *ta_fn = module + s_cc_code[H_TEXT_ASSET_GET_BYTES].offset;
        if (ta_fn[6] == 0x48 && ta_fn[7] == 0x8b && ta_fn[8] == 0x05) {
            int32_t disp = (int32_t)load32(ta_fn + 9);
            s_text_asset_bytes_slot = (NativeTextBytesFn *)(ta_fn + 13 + disp);
        }
    }

    /* Expand CountDataList::InitializeList score loop (0..3 -> 0..5) so Beginner(4) & Chaos(5)
     * never throw ArgumentOutOfRangeException on CountDataList.ScoreList[difficulty]. */
    {
        unsigned char *cmp_site = module + s_cc_code[P_COUNT_DATA_LIST_INIT].offset;
        if (memcmp(cmp_site, "\x83\xf8\x04", 3) == 0) {
            cmp_site[2] = 0x06;
        }
    }

    unsigned char *p = stubs;
    size_t installed = 0;
    for (size_t i = 0; i < CC_HOOK_SITES; ++i) {
        const GameCode *gc = &s_cc_code[custom_hooks[i].site];
        if (gc->moved < 5 || gc->moved > 16 || (size_t)(p - stubs) + 224 > size) {
            printf("CustomCharts: stub buffer or prologue invalid at hook %zu!\n", i);
            return -1;
        }
        unsigned char *function = module + gc->offset;
        unsigned char *stub = p;
        p = emit_hook_stub(p, custom_hooks[i].fn, function, gc->moved, custom_hooks[i].orig);
        const int32_t rel32 = (int32_t)(stub - (function + 5));
        function[0] = 0xe9;
        memcpy(function + 1, &rel32, 4);
        memset(function + 5, 0x90, gc->moved - 5u);
        ++installed;
    }

    runtime_tffscores_set_custom_id_checker(check_is_custom_id);
    runtime_tffscores_add_play_setting_config_callback(on_play_setting_config);
    runtime_tffscores_add_input_damage_callback(on_input_damage);
    runtime_judgements_add_stage_begin_callback(on_stage_begin_callback);

    printf("CustomCharts: Installed %zu/%d native hooks and %d callees for game %s (%zu custom songs scanned)\n",
           installed, CC_HOOK_SITES, CC_SITES - CC_HOOK_SITES, runtime_game_version(), song_count);
    return 0;
}
