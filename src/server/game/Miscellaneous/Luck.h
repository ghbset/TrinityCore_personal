/*
 * Anti-streak rolls.
 *
 * Combat (miss/dodge/parry/block/crit, spell hit/resist/crit, procs) uses smoothed rolls
 * (roll_prd): the long-run rate is unchanged, droughts and clumps are not. The streak counters
 * live on the player the roll is about: the attacker if it is a player, else the victim.
 * Rolls with no player on either side stay plain random.
 *
 * Quest drops use a per-player marble bag. Legendary drops (quality 5, under 5%) gain
 * +0.05% per failed kill, capped at 10%, stored in character_legendary_luck.
 */

#ifndef TRINITY_LUCK_H
#define TRINITY_LUCK_H

#include "Define.h"
#include <unordered_map>

class Player;
class ProcEventInfo;
class Unit;
class WorldObject;
struct Loot;
struct LootStoreItem;

enum LuckSlot : uint8
{
    LUCK_MISS,
    LUCK_DODGE,
    LUCK_PARRY,
    LUCK_BLOCK,
    LUCK_CRIT,          // melee/ranged crits, including weapon abilities
    LUCK_SPELL_MISS,
    LUCK_SPELL_RESIST,  // mechanic/debuff/binary resists
    LUCK_SPELL_CRIT,
    LUCK_SLOT_COUNT,

    LUCK_NONE = 0xFF    // plain random (glancing, crushing, deflect)
};

struct PlayerLuck
{
    struct Bag { uint8 left = 0; uint8 winsLeft = 0; };

    uint16 outgoing[LUCK_SLOT_COUNT] = {};              // rolls where the player attacks
    uint16 incoming[LUCK_SLOT_COUNT] = {};              // rolls made against the player
    std::unordered_map<uint32, uint16> procs;           // spell id; high bit = proc against the player
    std::unordered_map<uint32, Bag> questBags;          // item id
    std::unordered_map<uint32, uint32> legendaryMisses; // item id -> failed kills (persisted)
};

namespace Luck
{
    TC_GAME_API uint16* Counters(WorldObject const* attacker, WorldObject const* victim);
    TC_GAME_API bool Roll(uint16* counters, LuckSlot slot, float chance);

    // Sequential form of the core's single-roll outcome tables ("roll < (sum += width)"),
    // widths in 1/100 %. Each segment rolls its conditional chance, so smoothing can apply per outcome.
    class TC_GAME_API TableRoll
    {
    public:
        explicit TableRoll(uint16* counters) : _counters(counters) { }
        bool Next(LuckSlot slot, int32 width);
    private:
        uint16* _counters;
        int32 _used = 0;
    };

    TC_GAME_API bool RollProc(Unit* auraOwner, ProcEventInfo& eventInfo, uint32 spellId, float chance);

    // Active while a loot template is processed; tells the rolls who is eligible.
    class TC_GAME_API LootScope
    {
    public:
        LootScope(Loot& loot, Player* owner, bool personal);
        ~LootScope();
    };

    // Quest items: returns true if the per-player bags decided the roll (result in 'drop').
    bool RollQuestItem(LootStoreItem const& item, float chance, bool& drop);
    // Legendaries: the bad-luck-adjusted chance; any other item returns 'chance' unchanged.
    float LegendaryChance(LootStoreItem const& item, float chance);

    void OnItemReceived(Player* player, uint32 itemId);
    void LoadLegendaryMisses(Player* player);
    void CreateTables();
}

#endif
