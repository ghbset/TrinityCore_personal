#include "Luck.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "ItemTemplate.h"
#include "Loot.h"
#include "LootMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "Unit.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr float LEGENDARY_MAX_BASE_CHANCE = 5.0f;   // rarer than this counts as a legendary drop
    constexpr float LEGENDARY_STEP_PER_MISS = 0.05f;
    constexpr float LEGENDARY_MAX_CHANCE = 10.0f;
    constexpr uint32 PROC_AGAINST_PLAYER = 0x80000000;

    struct LootContext
    {
        Loot* loot;
        std::vector<Player*> questPlayers;      // in the map, like quest loot visibility
        std::vector<Player*> legendaryPlayers;  // also at group reward distance
        std::vector<uint32> legendariesRolled;
        LootContext* previous;
    };

    thread_local LootContext* t_loot = nullptr;

    // One winner in round(1/p) marbles when that is close to the chance (within 2 points and 10% of it),
    // else the smallest bag that is.
    bool BagFor(float chance, uint8& size, uint8& wins)
    {
        double p = chance / 100.0;
        double tolerance = std::min(0.02, p * 0.1) + 1e-9;
        long single = p > 0.0 ? std::lround(1.0 / p) : 0;
        if (single >= 1 && single <= 255 && std::fabs(1.0 / single - p) <= tolerance)
        {
            size = uint8(single);
            wins = 1;
            return true;
        }
        for (uint32 n = 1; n <= 255; ++n)
        {
            uint32 k = std::clamp<uint32>(uint32(std::lround(p * n)), 1, n);
            if (std::fabs(double(k) / n - p) <= tolerance)
            {
                size = uint8(n);
                wins = uint8(k);
                return true;
            }
        }
        return false;   // under ~0.4%: no bag fits, roll normally
    }

    bool IsLegendaryDrop(LootStoreItem const& item)
    {
        if (item.needs_quest || item.reference || item.chance <= 0.0f || item.chance >= LEGENDARY_MAX_BASE_CHANCE)
            return false;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
        return proto && proto->Quality == ITEM_QUALITY_LEGENDARY;
    }
}

uint16* Luck::Counters(WorldObject const* attacker, WorldObject const* victim)
{
    if (attacker && attacker->IsPlayer())
        return const_cast<Player*>(attacker->ToPlayer())->GetLuck().outgoing;
    if (victim && victim->IsPlayer())
        return const_cast<Player*>(victim->ToPlayer())->GetLuck().incoming;
    return nullptr;
}

bool Luck::Roll(uint16* counters, LuckSlot slot, float chance)
{
    if (!counters || slot == LUCK_NONE)
        return roll_chance_f(chance);
    return roll_prd(chance, counters[slot]);
}

bool Luck::TableRoll::Next(LuckSlot slot, int32 width)
{
    if (width <= 0)
        return false;

    int32 remaining = 10000 - _used;
    bool hit = width >= remaining || Roll(_counters, slot, width * 100.0f / remaining);
    if (!hit)
        _used += width;
    return hit;
}

bool Luck::RollProc(Unit* auraOwner, ProcEventInfo& eventInfo, uint32 spellId, float chance)
{
    Player* player = auraOwner->ToPlayer();
    uint32 key = spellId;
    if (!player)
    {
        Unit* other = eventInfo.GetActor() == auraOwner ? eventInfo.GetActionTarget() : eventInfo.GetActor();
        player = other ? other->ToPlayer() : nullptr;
        key |= PROC_AGAINST_PLAYER;
    }

    if (!player || chance <= 0.0f || chance >= 100.0f)
        return roll_chance_f(chance);
    return roll_prd(chance, player->GetLuck().procs[key]);
}

Luck::LootScope::LootScope(Loot& loot, Player* owner, bool personal)
{
    LootContext* ctx = new LootContext{ &loot, {}, {}, {}, t_loot };
    Group* group = personal ? nullptr : owner->GetGroup();
    if (!group)
    {
        ctx->questPlayers.push_back(owner);
        ctx->legendaryPlayers.push_back(owner);
    }
    else
    {
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || !member->IsInMap(owner))
                continue;
            ctx->questPlayers.push_back(member);
            if (member->IsAtGroupRewardDistance(owner))
                ctx->legendaryPlayers.push_back(member);
        }
    }
    t_loot = ctx;
}

Luck::LootScope::~LootScope()
{
    LootContext* ctx = t_loot;
    t_loot = ctx->previous;

    std::sort(ctx->legendariesRolled.begin(), ctx->legendariesRolled.end());
    ctx->legendariesRolled.erase(std::unique(ctx->legendariesRolled.begin(), ctx->legendariesRolled.end()), ctx->legendariesRolled.end());

    for (uint32 itemId : ctx->legendariesRolled)
    {
        bool dropped = std::any_of(ctx->loot->items.begin(), ctx->loot->items.end(), [itemId](LootItem const& li) { return li.itemid == itemId; });
        if (dropped)
            continue;   // the counter resets when someone actually receives it

        for (Player* player : ctx->legendaryPlayers)
        {
            uint32 misses = ++player->GetLuck().legendaryMisses[itemId];
            CharacterDatabase.PExecute("REPLACE INTO character_legendary_luck (guid, item, misses) VALUES ({}, {}, {})",
                player->GetGUID().GetCounter(), itemId, misses);
        }
    }
    delete ctx;
}

bool Luck::RollQuestItem(LootStoreItem const& item, float chance, bool& drop)
{
    uint8 size, wins;
    if (!t_loot || !BagFor(chance, size, wins))
        return false;

    GuidSet winners;
    bool anyNeeds = false;
    for (Player* player : t_loot->questPlayers)
    {
        if (!player->HasQuestForItem(item.itemid))
            continue;
        anyNeeds = true;
        PlayerLuck::Bag& bag = player->GetLuck().questBags[item.itemid];
        if (roll_bag(size, wins, bag.left, bag.winsLeft))
            winners.insert(player->GetGUID());
    }

    if (!anyNeeds)
        return false;   // nobody on the quest: keep stock behaviour

    drop = !winners.empty();
    if (drop)
        t_loot->loot->questBagWinners[item.itemid].insert(winners.begin(), winners.end());
    return true;
}

float Luck::LegendaryChance(LootStoreItem const& item, float chance)
{
    if (!t_loot || !IsLegendaryDrop(item))
        return chance;

    t_loot->legendariesRolled.push_back(item.itemid);

    uint32 misses = 0;
    for (Player* player : t_loot->legendaryPlayers)
    {
        auto const& known = player->GetLuck().legendaryMisses;
        auto itr = known.find(item.itemid);
        if (itr != known.end())
            misses = std::max(misses, itr->second);   // the unluckiest member carries the group
    }
    return std::min(chance + LEGENDARY_STEP_PER_MISS * misses, std::max(chance, LEGENDARY_MAX_CHANCE));
}

void Luck::OnItemReceived(Player* player, uint32 itemId)
{
    auto& known = player->GetLuck().legendaryMisses;
    if (known.erase(itemId))
        CharacterDatabase.PExecute("DELETE FROM character_legendary_luck WHERE guid = {} AND item = {}", player->GetGUID().GetCounter(), itemId);
}

void Luck::LoadLegendaryMisses(Player* player)
{
    auto& known = player->GetLuck().legendaryMisses;
    known.clear();
    if (QueryResult result = CharacterDatabase.PQuery("SELECT item, misses FROM character_legendary_luck WHERE guid = {}", player->GetGUID().GetCounter()))
    {
        do
        {
            Field* fields = result->Fetch();
            known[fields[0].GetUInt32()] = fields[1].GetUInt32();
        } while (result->NextRow());
    }
}

void Luck::CreateTables()
{
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS character_legendary_luck ("
        "guid INT UNSIGNED NOT NULL, item INT UNSIGNED NOT NULL, misses INT UNSIGNED NOT NULL DEFAULT 0, "
        "PRIMARY KEY (guid, item)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}
