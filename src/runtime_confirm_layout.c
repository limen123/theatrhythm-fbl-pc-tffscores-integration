/* Live confirm/cancel layout sync (menu_confirm_swap / THEATRHYTHM_MENU_CONFIRM_SWAP).
 *
 * On US/EU builds, BuildSettings::GetDefaultPadAssignType returns 2 (South confirm / East cancel)
 * based on the build region before reaching sceSystemServiceParamGetInt(ENTER_BUTTON_ASSIGN).
 * Even if overridden at boot, changing the F10 "Confirm / cancel layout" toggle at runtime requires
 * calling Game.Device.GameInput::SetDecideButtonType(type):
 *   - type = 1 -> InputImplementBase::SetPadAssignType(1) (East confirm / South cancel: Nintendo/JP)
 *   - type = 0 -> InputImplementBase::SetPadAssignType(2) (South confirm / East cancel: Xbox/PlayStation)
 * which updates _padAssignType (+0x18), rebuilds every InputPad's _keyAssignTable (+0x28),
 * _submitAlias (+0x30), and _cancelAlias (+0x34), and invokes InputManager.OnChangePadAssignType
 * so EmojiSwitcher refreshes UI button prompt glyphs immediately.
 *
 * We hook Gummi.Device.InputImplementBase::Update (called every frame by InputManager's UpdateLoop
 * coroutine with rdi = InputImplementBase *this) and call GameInput::SetDecideButtonType whenever
 * this->_padAssignType (+0x18) does not match the current toggle state. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SITE_UPDATE, SITE_SET_DECIDE, SITE_COUNT };
static const GameSite SITES[SITE_COUNT] = {
    {"Gummi.Device.InputImplementBase::Update", 0, 0, 0, {
        "55 48 89 e5 41 57 41 56 41 54 53 | 48 8b 07 48 89 fb 48 8b b0 e0 01 00 00 ff 90 d8 01 00 00"}},
    {"Game.Device.GameInput::SetDecideButtonType", 0, 0, 0, {
        "55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00 89 fb 75 2b 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01"}},
};

static GameCode g_at[SITE_COUNT];
static unsigned char *g_module;
typedef void (ABI *SetDecideButtonTypeFn)(int32_t type, void *method);
static SetDecideButtonTypeFn g_set_decide_button_type;

static int confirm_layout_is_swapped(void) {
    return theatrhythm_gpu_menu_confirm_swap() ? 1 : 0;
}

static ABI void sync_confirm_layout(unsigned char *impl) {
    if (!impl || !g_set_decide_button_type) return;
    int swapped = confirm_layout_is_swapped();
    int32_t want_pad_assign = swapped ? 1 : 2;
    int32_t want_decide_arg = swapped ? 1 : 0;
    int32_t cur_pad_assign = 0;
    memcpy(&cur_pad_assign, impl + 0x18, sizeof(cur_pad_assign));
    if (cur_pad_assign != want_pad_assign) {
        g_set_decide_button_type(want_decide_arg, NULL);
    }
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) {
    memcpy(p, bytes, size);
    return p + size;
}

int runtime_confirm_layout_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    g_module = image + module_base;
    if (runtime_game_code_find(SITES, SITE_COUNT, g_at, "Confirm layout") || size < 64) {
        printf("Confirm layout: game code hooks not found\n");
        return -1;
    }
    g_set_decide_button_type = (SetDecideButtonTypeFn)(void *)(g_module + g_at[SITE_SET_DECIDE].offset);

    unsigned char *site = g_module + g_at[SITE_UPDATE].offset;
    unsigned char *p = stubs, *stub = p;
    const unsigned moved = g_at[SITE_UPDATE].moved;
    const uint64_t fn = (uint64_t)(uintptr_t)sync_confirm_layout;
    const uint64_t back = (uint64_t)(uintptr_t)(site + moved);

    p = emit(p, (const unsigned char[]){0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51}, 8); /* push rdi rsi rdx rcx r8 r9 */
    p = emit(p, (const unsigned char[]){0x48, 0x83, 0xec, 0x08}, 4);                         /* sub rsp,8 (16-aligned) */
    p = emit(p, (const unsigned char[]){0x48, 0xb8}, 2); p = emit(p, &fn, 8);                /* mov rax,sync_confirm_layout */
    p = emit(p, (const unsigned char[]){0xff, 0xd0}, 2);                                     /* call rax (rdi: impl) */
    p = emit(p, (const unsigned char[]){0x48, 0x83, 0xc4, 0x08}, 4);                         /* add rsp,8 */
    p = emit(p, (const unsigned char[]){0x41, 0x59, 0x41, 0x58, 0x59, 0x5a, 0x5e, 0x5f}, 8); /* pop r9 r8 rcx rdx rsi rdi */
    p = emit(p, site, moved);                                                                /* moved prologue */
    p = emit(p, (const unsigned char[]){0xff, 0x25, 0, 0, 0, 0}, 6); p = emit(p, &back, 8);  /* jmp [rip] */
    if ((size_t)(p - stubs) > size) return -1;

    const int32_t jump = (int32_t)(stub - (site + 5));
    site[0] = 0xe9;
    memcpy(site + 1, &jump, 4);
    memset(site + 5, 0x90, moved - 5);
    printf("Confirm layout: live confirm/cancel swap hook installed\n");
    return 0;
}
