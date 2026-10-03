/*
 * forever_talents: Priest talents that need code (modules/forever_classes/data/impl/Priest.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Priest.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "GameTime.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Random.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "SpellScript.h"

enum ForeverPriestSpells
{
    SPELL_FP_WEAKENED_SOUL          = 6788,
    SPELL_FP_BLESSED_RECOVERY_HOT   = 27813,   // rank chain 27813/27817/27818
};

// Penance (Forever ranks): e0 DUMMY; TriggerSpell = damage channel, MiscValue = heal channel (stock 3.3.5a bolts).
class spell_pri_penance_forever : public SpellScript
{
    PrepareSpellScript(spell_pri_penance_forever);

    bool Load() override { return GetCaster()->GetTypeId() == TYPEID_PLAYER; }

    bool Validate(SpellInfo const* spellInfo) override
    {
        SpellEffectInfo const& e = spellInfo->GetEffect(EFFECT_0);
        return ValidateSpellInfo({ e.TriggerSpell, uint32(e.MiscValue) });
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target || !target->IsAlive())
            return;
        SpellEffectInfo const& e = GetSpellInfo()->GetEffect(EFFECT_0);
        caster->CastSpell(target, caster->IsFriendlyTo(target) ? uint32(e.MiscValue) : e.TriggerSpell, TRIGGERED_DISALLOW_PROC_EVENTS);
    }

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        if (Unit* target = GetExplTargetUnit())
            if (!caster->IsFriendlyTo(target))
            {
                if (!caster->IsValidAttackTarget(target))
                    return SPELL_FAILED_BAD_TARGETS;
                if (!caster->isInFront(target))
                    return SPELL_FAILED_UNIT_NOT_INFRONT;
            }
        return SPELL_CAST_OK;
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_pri_penance_forever::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
        OnCheckCast += SpellCheckCastFn(spell_pri_penance_forever::CheckCast);
    }
};

// Renewed Hope: e0 is the stock crit bonus (override class script 7997); e1 cuts the target's Weakened Soul (ms).
class spell_pri_renewed_hope_forever : public AuraScript
{
    PrepareAuraScript(spell_pri_renewed_hope_forever);

    bool Validate(SpellInfo const* /*spellInfo*/) override { return ValidateSpellInfo({ SPELL_FP_WEAKENED_SOUL }); }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* target = eventInfo.GetProcTarget();
        return target && target->HasAura(SPELL_FP_WEAKENED_SOUL);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        if (Aura* ws = eventInfo.GetProcTarget()->GetAura(SPELL_FP_WEAKENED_SOUL))
        {
            int32 left = ws->GetDuration() + aurEff->GetAmount();
            if (left <= 0)
                ws->Remove();
            else
                ws->SetDuration(left);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_pri_renewed_hope_forever::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_pri_renewed_hope_forever::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Blessed Recovery: a crit, or one hit over e1% of max health, heals e0% of it over 6 sec; healing still to come
// from an earlier proc is added to the new one.
class spell_pri_blessed_recovery_forever : public AuraScript
{
    PrepareAuraScript(spell_pri_blessed_recovery_forever);

    bool Validate(SpellInfo const* /*spellInfo*/) override { return ValidateSpellInfo({ SPELL_FP_BLESSED_RECOVERY_HOT }); }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage())
            return false;
        if (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
            return true;
        AuraEffect const* threshold = GetEffect(EFFECT_1);
        return threshold && damage->GetDamage() > CalculatePct(GetTarget()->GetMaxHealth(), threshold->GetAmount());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        uint32 hot = sSpellMgr->GetSpellWithRank(SPELL_FP_BLESSED_RECOVERY_HOT, GetSpellInfo()->GetRank());
        uint32 ticks = sSpellMgr->AssertSpellInfo(hot)->GetMaxTicks();
        if (!ticks)
            return;

        int32 total = CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmount());
        for (uint32 id = SPELL_FP_BLESSED_RECOVERY_HOT; id; id = sSpellMgr->GetNextSpellInChain(id))
            if (AuraEffect const* old = target->GetAuraEffect(id, EFFECT_0, target->GetGUID()))
            {
                total += old->GetAmount() * int32(old->GetRemainingTicks());
                target->RemoveAurasDueToSpell(id, target->GetGUID());
            }

        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(total / int32(ticks));
        target->CastSpell(target, hot, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_pri_blessed_recovery_forever::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_pri_blessed_recovery_forever::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Vampiric Embrace (Forever: a debuff on the enemy): the priest's Shadow damage to it heals the party (caster
// included) for e0%; if the enemy dies with it, the priest's Spirit Tap gets a roll.
class spell_pri_vampiric_embrace_forever : public AuraScript
{
    PrepareAuraScript(spell_pri_vampiric_embrace_forever);

    // forever_classes data: e0 TriggerSpell = the heal helper, e0 MiscValueA = Spirit Tap rank 1 (the talent chain)
    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* actor = eventInfo.GetActor();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return actor && actor->GetGUID() == GetCasterGUID() && damage && damage->GetDamage();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(CalculatePct(int32(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmount()));
        eventInfo.GetActor()->CastSpell(nullptr, aurEff->GetSpellEffectInfo().TriggerSpell, args);
    }

    // a target dying under Vampiric Embrace counts for Spirit Tap (its rank's proc chance, its buff)
    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;
        Player* priest = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!priest || !priest->IsAlive() || !priest->isHonorOrXPTarget(GetTarget()) || aurEff->GetMiscValue() <= 0)
            return;
        uint32 const first = sSpellMgr->GetFirstSpellInChain(uint32(aurEff->GetMiscValue()));
        if (AuraEffect const* spiritTap = priest->GetAuraEffectOfRankedSpell(first, EFFECT_0))
            if (roll_chance_i(spiritTap->GetSpellInfo()->ProcChance))
                priest->CastSpell(priest, spiritTap->GetSpellEffectInfo().TriggerSpell, CastSpellExtraArgs(spiritTap));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_pri_vampiric_embrace_forever::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_pri_vampiric_embrace_forever::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_pri_vampiric_embrace_forever::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Devouring Plague + Devouring Contagion: when the target dies, the Plague moves to the nearest enemy within the
// talent's e1 yards for the time it had left. The talent is the priest's Devouring Plague cost mod with a DUMMY e1.
class spell_pri_devouring_contagion : public AuraScript
{
    PrepareAuraScript(spell_pri_devouring_contagion);

    static float Radius(Unit const* caster)
    {
        for (AuraEffect const* mod : caster->GetAuraEffectsByType(SPELL_AURA_ADD_PCT_MODIFIER))
        {
            if (mod->GetSpellInfo()->SpellFamilyName != SPELLFAMILY_PRIEST || mod->GetMiscValue() != SPELLMOD_COST
                || !(mod->GetSpellEffectInfo().SpellClassMask[0] & 0x02000000))
                continue;
            if (AuraEffect const* radius = mod->GetBase()->GetEffect(EFFECT_1))
                if (radius->GetAuraType() == SPELL_AURA_DUMMY)
                    return float(radius->GetAmount());
        }
        return 0.0f;
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;
        Unit* caster = GetCaster();
        int32 left = GetDuration();
        if (!caster || !caster->IsAlive() || left <= 0)
            return;
        float radius = Radius(caster);
        if (radius <= 0.0f)
            return;

        Unit* dead = GetTarget();
        std::list<Unit*> units;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(dead, caster, radius);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(dead, units, check);
        Cell::VisitAllObjects(dead, searcher, radius);

        Unit* next = nullptr;
        for (Unit* u : units)
            if (u != dead && caster->IsValidAttackTarget(u) && !u->HasAura(GetId(), caster->GetGUID())
                && (!next || dead->GetExactDist(u) < dead->GetExactDist(next)))
                next = u;
        if (!next)
            return;
        if (Aura* plague = caster->AddAura(GetId(), next))
        {
            plague->SetMaxDuration(left);
            plague->SetDuration(left);
        }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_pri_devouring_contagion::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_LEECH, AURA_EFFECT_HANDLE_REAL);
    }
};

// Shadow Word: Death + Early Demise: +e0% crit when the target is at or below e1% health. The talent is the
// priest's passive DUMMY with Shadow Word: Death's flag (B 0x2).
class spell_pri_early_demise : public SpellScript
{
    PrepareSpellScript(spell_pri_early_demise);

    void HandleBeforeCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!target)
            return;
        for (AuraEffect const* talent : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            SpellInfo const* info = talent->GetSpellInfo();
            if (talent->GetEffIndex() != EFFECT_0 || info->SpellFamilyName != SPELLFAMILY_PRIEST
                || !(info->SpellFamilyFlags[1] & 0x2) || !info->IsPassive())
                continue;
            AuraEffect const* health = talent->GetBase()->GetEffect(EFFECT_1);
            if (!health || target->GetHealthPct() > float(health->GetAmount()))
                return;
            // the crit chance spell value is whole percent
            float crit = caster->SpellCritChanceDone(GetSpellInfo(), GetSpellInfo()->GetSchoolMask()) + talent->GetAmount();
            GetSpell()->SetSpellValue(SPELLVALUE_CRIT_CHANCE, int32(crit + 0.5f));
            return;
        }
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_pri_early_demise::HandleBeforeCast);
    }
};

// Shadow Word: Death (forever_classes clones): if the target survives, the caster takes e2% of its max health through
// e2's TriggerSpell (the backlash helper).
class spell_pri_shadow_word_death_forever : public SpellScript
{
    PrepareSpellScript(spell_pri_shadow_word_death_forever);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target || !target->IsAlive())
            return;

        SpellEffectInfo const& backlash = GetSpellInfo()->GetEffect(EFFECT_2);
        if (!backlash.TriggerSpell)
            return;
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(caster->CountPctFromMaxHealth(backlash.CalcValue(caster))));
        caster->CastSpell(caster, backlash.TriggerSpell, args);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_pri_shadow_word_death_forever::HandleAfterHit);
    }
};

// Litany of Light (forever_classes): a heal cast after a different heal returns e0% of its mana cost through e0's
// TriggerSpell (an energize helper).
class spell_pri_litany_of_light_forever : public AuraScript
{
    PrepareAuraScript(spell_pri_litany_of_light_forever);

    uint32 _lastHeal = 0;

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return spellInfo && spellInfo->ManaCost && !(eventInfo.GetProcSpell() && eventInfo.GetProcSpell()->IsTriggered());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        uint32 const heal = spellInfo->GetFirstRankSpell()->Id;
        bool const different = _lastHeal && _lastHeal != heal;
        _lastHeal = heal;
        int32 const mana = CalculatePct(int32(spellInfo->ManaCost), aurEff->GetAmount());
        if (!different || mana <= 0)
            return;
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(mana);
        GetTarget()->CastSpell(GetTarget(), aurEff->GetSpellEffectInfo().TriggerSpell, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_pri_litany_of_light_forever::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_pri_litany_of_light_forever::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Shadowguard (forever_classes): each attacker can trigger it once per e1 amount (ms); the spell_proc row has no cooldown.
class spell_pri_shadowguard_forever : public AuraScript
{
    PrepareAuraScript(spell_pri_shadowguard_forever);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* attacker = eventInfo.GetActor();
        AuraEffect const* cd = GetEffect(EFFECT_1);
        if (!attacker || !cd)
            return false;
        TimePoint const now = GameTime::Now();
        auto itr = _next.find(attacker->GetGUID());
        if (itr != _next.end() && itr->second > now)
            return false;
        _next[attacker->GetGUID()] = now + Milliseconds(cd->GetAmount());
        return true;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_pri_shadowguard_forever::CheckProc);
    }

    std::unordered_map<ObjectGuid, TimePoint> _next;
};

void AddSC_forever_talents_priest()
{
    RegisterSpellScript(spell_pri_penance_forever);
    RegisterSpellScript(spell_pri_renewed_hope_forever);
    RegisterSpellScript(spell_pri_blessed_recovery_forever);
    RegisterSpellScript(spell_pri_vampiric_embrace_forever);
    RegisterSpellScript(spell_pri_devouring_contagion);
    RegisterSpellScript(spell_pri_early_demise);
    RegisterSpellScript(spell_pri_shadow_word_death_forever);
    RegisterSpellScript(spell_pri_litany_of_light_forever);
    RegisterSpellScript(spell_pri_shadowguard_forever);
}
