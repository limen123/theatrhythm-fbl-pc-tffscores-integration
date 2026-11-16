/* Summonstone finder and giver display (found by / received from).
 *
 * In the PS4 build of the game, SummonStoneData has split fields:
 * - 0x20: DiscovererName (string, MessagePack key 2, saved in backup.inf)
 * - 0x28: DiscovererNamePS4 (string, [IgnoreMember], in-memory cache)
 * - 0x30: DiscovererInternalAccountID (ulong, MessagePack key 3)
 * - 0x38: GiverName (string, MessagePack key 4, saved in backup.inf)
 * - 0x40: GiverNamePS4 (string, [IgnoreMember], in-memory cache)
 * - 0x48: GiverInternalAccountID (ulong, MessagePack key 5)
 *
 * On PS4, the game queries Sony PSN at startup using the account IDs to populate the
 * PS4 in-memory cached names (0x28 and 0x40). In the offline PC recomp, the PSN lookup
 * never completes / returns empty, leaving DiscovererNamePS4 and GiverNamePS4 as NULL.
 * DetailPanelSummonStoneStatusBehaviour::UpdateSummonStoneStatus and the getters in
 * SummonStoneData read the PS4 fields (0x28 / 0x40) and fall back to "---" when null.
 *
 * By patching the field offsets from 0x28 -> 0x20 and 0x40 -> 0x38, the UI reads the
 * real names deserialized from the save file (backup.inf).
 */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

enum {
    UPDATE_FINDER,
    UPDATE_PROVIDER,
    GET_DISCOVERER_NAME,
    GET_GIVER_NAME,
    GET_DISCOVERER_NAME_PS4,
    GET_GIVER_NAME_PS4,
    SITES
};

static const GameSite sites[SITES] = {
    {"Game.Scene.PartyEdit.DetailPanelSummonStoneStatusBehaviour::UpdateSummonStoneStatus", 1, 0, 0,
     {"49 8b 74 24 28 | 48 85 f6 74 1c 83 7e 10 00 74 16"}},
    {"Game.Scene.PartyEdit.DetailPanelSummonStoneStatusBehaviour::UpdateSummonStoneStatus", 1, 0, 0,
     {"49 8b 74 24 40 | 48 85 f6 74 1c 83 7e 10 00 74 16"}},
    {"Game.Backup.SummonStoneInfo/SummonStoneData::GetDiscovererName", 0, 0, 0,
     {"48 8b 47 28 c3"}},
    {"Game.Backup.SummonStoneInfo/SummonStoneData::GetGiverName", 0, 0, 0,
     {"48 8b 47 40 c3"}},
    {"Game.Backup.SummonStoneInfo/SummonStoneData::get_DiscovererNamePS4", 0, 0, 0,
     {"48 8b 47 28 c3"}},
    {"Game.Backup.SummonStoneInfo/SummonStoneData::get_GiverNamePS4", 0, 0, 0,
     {"48 8b 47 40 c3"}},
};

int runtime_summonstone_install(unsigned char *module) {
    GameCode at[SITES];
    if (runtime_game_code_find(sites, SITES, at, "Summonstone names")) {
        printf("Summonstone: names not patched (game code differs)\n");
        return -1;
    }

    /* UpdateSummonStoneStatus finder: mov rsi, [r12 + 0x28] -> mov rsi, [r12 + 0x20] */
    unsigned char *p_finder = module + at[UPDATE_FINDER].offset;
    if (p_finder[0] == 0x49 && p_finder[1] == 0x8b && p_finder[2] == 0x74 &&
        p_finder[3] == 0x24 && p_finder[4] == 0x28) {
        p_finder[4] = 0x20;
    }

    /* UpdateSummonStoneStatus provider: mov rsi, [r12 + 0x40] -> mov rsi, [r12 + 0x38] */
    unsigned char *p_provider = module + at[UPDATE_PROVIDER].offset;
    if (p_provider[0] == 0x49 && p_provider[1] == 0x8b && p_provider[2] == 0x74 &&
        p_provider[3] == 0x24 && p_provider[4] == 0x40) {
        p_provider[4] = 0x38;
    }

    /* GetDiscovererName: mov rax, [rdi + 0x28] -> mov rax, [rdi + 0x20] */
    unsigned char *p_get_disc = module + at[GET_DISCOVERER_NAME].offset;
    if (p_get_disc[0] == 0x48 && p_get_disc[1] == 0x8b && p_get_disc[2] == 0x47 &&
        p_get_disc[3] == 0x28) {
        p_get_disc[3] = 0x20;
    }

    /* GetGiverName: mov rax, [rdi + 0x40] -> mov rax, [rdi + 0x38] */
    unsigned char *p_get_giver = module + at[GET_GIVER_NAME].offset;
    if (p_get_giver[0] == 0x48 && p_get_giver[1] == 0x8b && p_get_giver[2] == 0x47 &&
        p_get_giver[3] == 0x40) {
        p_get_giver[3] = 0x38;
    }

    /* get_DiscovererNamePS4: mov rax, [rdi + 0x28] -> mov rax, [rdi + 0x20] */
    unsigned char *p_prop_disc = module + at[GET_DISCOVERER_NAME_PS4].offset;
    if (p_prop_disc[0] == 0x48 && p_prop_disc[1] == 0x8b && p_prop_disc[2] == 0x47 &&
        p_prop_disc[3] == 0x28) {
        p_prop_disc[3] = 0x20;
    }

    /* get_GiverNamePS4: mov rax, [rdi + 0x40] -> mov rax, [rdi + 0x38] */
    unsigned char *p_prop_giver = module + at[GET_GIVER_NAME_PS4].offset;
    if (p_prop_giver[0] == 0x48 && p_prop_giver[1] == 0x8b && p_prop_giver[2] == 0x47 &&
        p_prop_giver[3] == 0x40) {
        p_prop_giver[3] = 0x38;
    }

    printf("Runtime: Summonstone names redirected to save data (DiscovererName / GiverName)\n");
    return 0;
}
