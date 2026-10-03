/*
 * forever_classes racials that need code (datascripts/racials/<race>/<race>_racials.ts).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the datascript
 * writes, and read every related id / value from spell data.
 */

#include "ScriptMgr.h"
#include "ObjectMgr.h"
#include "GridNotifiersImpl.h"
#include "GameTime.h"
#include "GameObject.h"
#include "DBCStores.h"
#include "CellImpl.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "Unit.h"

// Plainsrunning (Tauren): every e1 seconds spent moving adds a stack of e0's TriggerSpell (up to e2 / e0 stacks);
// every e1 seconds standing still, and every hit taken, removes one.
class spell_forever_racial_plainsrunning : public AuraScript
{
    PrepareAuraScript(spell_forever_racial_plainsrunning);

    uint32 _moving = 0;
    uint32 _still = 0;

    uint32 BuffId() const { return GetSpellInfo()->GetEffect(EFFECT_0).TriggerSpell; }
    uint32 Step() const { return std::max<uint32>(1, GetEffect(EFFECT_1) ? GetEffect(EFFECT_1)->GetAmount() : 5); }
    uint32 MaxStacks() const
    {
        int32 per = std::max<int32>(1, GetEffect(EFFECT_0)->GetAmount());
        return std::max<uint32>(1, GetEffect(EFFECT_2) ? GetEffect(EFFECT_2)->GetAmount() / per : 30);
    }

    void RemoveStack(Unit* target)
    {
        if (Aura* buff = target->GetAura(BuffId()))
        {
            if (buff->GetStackAmount() > 1)
                buff->ModStackAmount(-1);
            else
                buff->Remove();
        }
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (target->isMoving())
        {
            _still = 0;
            if (++_moving < Step())
                return;
            _moving = 0;
            Aura* buff = target->GetAura(BuffId());
            if (!buff)
                target->CastSpell(target, BuffId(), true);
            else if (buff->GetStackAmount() < MaxStacks())
                buff->ModStackAmount(1);
        }
        else
        {
            _moving = 0;
            if (++_still < Step())
                return;
            _still = 0;
            RemoveStack(target);
        }
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return damage && damage->GetDamage();
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        RemoveStack(GetTarget());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_forever_racial_plainsrunning::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        DoCheckProc += AuraCheckProcFn(spell_forever_racial_plainsrunning::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_racial_plainsrunning::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Blood Fury (Orc): e2 DUMMY = % more spell power for the duration (3.3.5a has no spell power % aura). The bonus is
// taken from the player's spell power when the buff lands and given back when it ends.
class spell_forever_racial_blood_fury : public AuraScript
{
    PrepareAuraScript(spell_forever_racial_blood_fury);

    int32 _bonus = 0;

    void HandleApply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;
        int32 const power = std::max(player->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC),
                                     player->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_MAGIC));
        _bonus = CalculatePct(power, aurEff->GetAmount());
        if (_bonus > 0)
            player->ApplySpellPowerBonus(_bonus, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            if (_bonus > 0)
                player->ApplySpellPowerBonus(_bonus, false);
        _bonus = 0;
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_forever_racial_blood_fury::HandleApply, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_forever_racial_blood_fury::HandleRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Touch of the Grave (Undead): the drain deals e0 % of the caster's maximum health (the leech heals for it).
class spell_forever_racial_touch_of_the_grave : public SpellScript
{
    PrepareSpellScript(spell_forever_racial_touch_of_the_grave);

    void HandleLaunch(SpellEffIndex /*effIndex*/)
    {
        if (Unit* caster = GetCaster())
            SetEffectValue(int32(caster->CountPctFromMaxHealth(GetEffectValue())));
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_forever_racial_touch_of_the_grave::HandleLaunch, EFFECT_0, SPELL_EFFECT_ANY);
    }
};

// Shadowmeld (Night Elf): Forever's cooldown is short out of combat; used in combat it is e1's BasePoints (ms). The core
// starts the cooldown when the aura ends (before this hook), so the hook lengthens it to e1 when the cast was in combat.
class spell_forever_racial_shadowmeld : public AuraScript
{
    PrepareAuraScript(spell_forever_racial_shadowmeld);

    bool _inCombat = false;

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        _inCombat = GetTarget()->IsInCombat();
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (!_inCombat)
            return;
        Unit* target = GetTarget();
        int32 const want = GetSpellInfo()->GetEffect(EFFECT_1).BasePoints;
        int32 const left = int32(target->GetSpellHistory()->GetRemainingCooldown(GetSpellInfo()));
        if (want > left)
            target->GetSpellHistory()->ModifyCooldown(GetId(), want - left);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_forever_racial_shadowmeld::HandleApply, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_forever_racial_shadowmeld::HandleRemove, EFFECT_0, SPELL_AURA_ANY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Cultivation (Tauren): grows a gatherable copy of a herb within e0's radius. The copy is the herb's "cultivated twin"
// (a chest template with no lock whose Data23 names the original herb entry; racials/tauren/tauren_racials.ts).
// Level >= the herb's Herbalism skill / 5 (max 60). A cultivated herb spawn cannot be cultivated again for e0 hours;
// the copy lasts e1 seconds.
namespace
{
    std::unordered_map<uint32, uint32> const& CultivatedTwins()        // original herb entry -> twin entry
    {
        static std::unordered_map<uint32, uint32> twins;
        static bool built = false;
        if (!built)
        {
            for (auto const& [entry, info] : sObjectMgr->GetGameObjectTemplates())
                if (info.type == GAMEOBJECT_TYPE_CHEST && info.raw.data[23])
                    twins[info.raw.data[23]] = entry;
            built = true;
        }
        return twins;
    }

    std::unordered_map<ObjectGuid::LowType, time_t> CultivatedUntil;   // herb spawn -> when it may be cultivated again

    uint32 HerbSkill(GameObjectTemplate const* info)
    {
        if (LockEntry const* lock = sLockStore.LookupEntry(info->GetLockId()))
            for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
                if (lock->Type[i] == LOCK_KEY_SKILL && lock->Index[i] == LOCKTYPE_HERBALISM)
                    return lock->Skill[i];
        return 0;
    }

    struct HerbInRange
    {
        WorldObject const* _source;
        float _range;
        bool operator()(GameObject* go) const
        {
            return go->isSpawned() && go->GetSpawnId() && CultivatedTwins().count(go->GetEntry())
                && _source->IsWithinDistInMap(go, _range);
        }
    };
}

class spell_forever_racial_cultivation : public SpellScript
{
    PrepareSpellScript(spell_forever_racial_cultivation);

    GameObject* FindHerb() const
    {
        Unit* caster = GetCaster();
        float const range = GetSpellInfo()->GetEffect(EFFECT_0).CalcRadius(caster);
        std::list<GameObject*> herbs;
        HerbInRange check{ caster, range };
        Trinity::GameObjectListSearcher<HerbInRange> searcher(caster, herbs, check);
        Cell::VisitGridObjects(caster, searcher, range);
        GameObject* best = nullptr;
        for (GameObject* go : herbs)
            if (!best || caster->GetDistance(go) < caster->GetDistance(best))
                best = go;
        return best;
    }

    SpellCastResult CheckCast()
    {
        GameObject* herb = FindHerb();
        if (!herb)
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;
        uint32 const needLevel = std::min<uint32>(60, HerbSkill(herb->GetGOInfo()) / 5);
        if (GetCaster()->GetLevel() < needLevel)
            return SPELL_FAILED_LOWLEVEL;
        auto itr = CultivatedUntil.find(herb->GetSpawnId());
        if (itr != CultivatedUntil.end() && itr->second > GameTime::GetGameTime())
            return SPELL_FAILED_NOT_READY;
        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        GameObject* herb = FindHerb();
        if (!herb)
            return;
        auto twin = CultivatedTwins().find(herb->GetEntry());
        if (twin == CultivatedTwins().end())
            return;

        int32 const hours = GetSpellInfo()->GetEffect(EFFECT_0).CalcValue(GetCaster());
        int32 const seconds = GetSpellInfo()->GetEffect(EFFECT_1).CalcValue(GetCaster());
        CultivatedUntil[herb->GetSpawnId()] = GameTime::GetGameTime() + hours * HOUR;

        GetCaster()->SummonGameObject(twin->second, herb->GetPosition(),
            QuaternionData::fromEulerAnglesZYX(herb->GetOrientation(), 0.0f, 0.0f), Seconds(seconds));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forever_racial_cultivation::CheckCast);
        OnEffectHit += SpellEffectFn(spell_forever_racial_cultivation::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_forever_racials()
{
    RegisterSpellScript(spell_forever_racial_plainsrunning);
    RegisterSpellScript(spell_forever_racial_blood_fury);
    RegisterSpellScript(spell_forever_racial_touch_of_the_grave);
    RegisterSpellScript(spell_forever_racial_shadowmeld);
    RegisterSpellScript(spell_forever_racial_cultivation);
}
