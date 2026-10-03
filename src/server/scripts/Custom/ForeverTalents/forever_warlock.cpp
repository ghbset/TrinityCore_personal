/*
 * forever_talents: Warlock talents that need code (modules/forever_classes/data/impl/Warlock.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Warlock.ts), never by hard-coded new ids.
 * Helper ids are read from the bound spell's own fields; talents the script must find on a unit are marked
 * with a dummy effect whose MiscValue names the stock spell they change (GetMarkedTalent).
 */

#include "ScriptMgr.h"
#include "Creature.h"
#include "ObjectMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include <cmath>

namespace
{
    enum
    {
        SPELL_IMMOLATE_R1           = 348,
        SPELL_DEMONIC_SACRIFICE     = 18788,
        SACRIFICE_BUFF_MARKER       = 18789,   // e1 DUMMY MiscValue of our Demonic Sacrifice buffs (MiscValueB = creature family)
        SPELL_CREATE_SOUL_SHARD     = 43836,
    };

    // the family a Demonic Sacrifice buff belongs to, or -1 when the spell is not one (forever_classes data)
    int32 SacrificeFamily(SpellInfo const* info)
    {
        if (!info || info->SpellFamilyName != SPELLFAMILY_WARLOCK)
            return -1;
        SpellEffectInfo const& marker = info->GetEffect(EFFECT_1);
        return marker.IsAura(SPELL_AURA_DUMMY) && marker.MiscValue == SACRIFICE_BUFF_MARKER ? marker.MiscValueB : -1;
    }

    // removes the unit's Demonic Sacrifice buffs (all, or only onlyFamily's)
    void RemoveSacrificeBuffs(Unit* unit, int32 onlyFamily = -1)
    {
        std::vector<uint32> ids;
        for (auto const& [id, app] : unit->GetAppliedAuras())
        {
            int32 family = SacrificeFamily(app->GetBase()->GetSpellInfo());
            if (family >= 0 && (onlyFamily < 0 || family == onlyFamily))
                ids.push_back(id);
        }
        for (uint32 id : ids)
            unit->RemoveAurasDueToSpell(id);
    }

    AuraEffect const* GetMarkedTalent(Unit const* unit, int32 marker)
    {
        for (AuraEffect const* eff : unit->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (eff->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_WARLOCK && eff->GetMiscValue() == marker)
                return eff;
        return nullptr;
    }

    AuraEffect const* GetImmolate(Unit const* target, Unit const* caster)
    {
        return target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARLOCK, 0x4, 0, 0, caster->GetGUID());
    }
}

// Soul Harvesting: the kill counts only while the victim has this warlock's Drain Soul
class spell_warl_forever_soul_harvesting : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_soul_harvesting);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* victim = eventInfo.GetActionTarget();
        return victim && victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARLOCK, 0x4000, 0, 0, GetTarget()->GetGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_warl_forever_soul_harvesting::CheckProc);
    }
};

// Demonic Sacrifice (forever_classes): the demon's family picks the buff among the four the talent names (e1/e2
// TriggerSpell and MiscValue); one sacrifice buff at a time
class spell_warl_forever_demonic_sacrifice : public SpellScript
{
    PrepareSpellScript(spell_warl_forever_demonic_sacrifice);

    void HandleKill(SpellEffIndex /*effIndex*/)
    {
        Creature* demon = GetHitCreature();
        if (!demon)
            return;
        int32 const family = int32(demon->GetCreatureTemplate()->family);
        uint32 buff = 0;
        for (SpellEffIndex i : { EFFECT_1, EFFECT_2 })
        {
            SpellEffectInfo const& eff = GetSpellInfo()->GetEffect(i);
            for (uint32 id : { eff.TriggerSpell, uint32(eff.MiscValue) })
                if (id && SacrificeFamily(sSpellMgr->GetSpellInfo(id)) == family)
                    buff = id;
        }
        if (!buff)
            return;

        Unit* caster = GetCaster();
        RemoveSacrificeBuffs(caster);
        caster->CastSpell(caster, buff, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_warl_forever_demonic_sacrifice::HandleKill, EFFECT_0, SPELL_EFFECT_INSTAKILL);
    }
};

// Summon Imp / Voidwalker / Succubus / Incubus / Felhunter: a summon cancels Demonic Sacrifice; with Demonic Pact
// (a warlock DUMMY marked 18788) only the summoned family's buff.
class spell_warl_forever_summon_demon : public SpellScript
{
    PrepareSpellScript(spell_warl_forever_summon_demon);

    void HandleSummon(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        CreatureTemplate const* demon = sObjectMgr->GetCreatureTemplate(GetEffectInfo(effIndex).MiscValue);
        bool const pact = GetMarkedTalent(caster, SPELL_DEMONIC_SACRIFICE) != nullptr;
        if (!pact)
            RemoveSacrificeBuffs(caster);
        else if (demon)
            RemoveSacrificeBuffs(caster, int32(demon->family));
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_warl_forever_summon_demon::HandleSummon, EFFECT_0, SPELL_EFFECT_SUMMON_PET);
    }
};

// Pyroclasm: Soul Fire stuns s1% of the time; Rain of Fire / Hellfire have s1% over the whole channel per target,
// so each tick rolls its share
class spell_warl_forever_pyroclasm : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_pyroclasm);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        // never the warlock itself (Hellfire's self damage shares the Hellfire bit and procs as periodic)
        if (!spell || !eventInfo.GetActionTarget() || eventInfo.GetActionTarget() == GetTarget())
            return false;

        float chance = GetEffect(EFFECT_0)->GetAmount();
        if (spell->SpellFamilyFlags[1] & 0x80)          // Soul Fire (its A 0x40 is Hellfire's bit too)
            return roll_chance_f(chance);

        for (AuraEffect const* channel : GetTarget()->GetAuraEffectsByType(SPELL_AURA_PERIODIC_TRIGGER_SPELL))
            if (channel->GetSpellEffectInfo().TriggerSpell == spell->Id)
            {
                if (uint32 ticks = channel->GetTotalTicks())
                    chance = 100.0f * (1.0f - std::pow(1.0f - chance / 100.0f, 1.0f / ticks));
                break;
            }
        return roll_chance_f(chance);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        GetTarget()->CastSpell(eventInfo.GetActionTarget(), aurEff->GetSpellEffectInfo().TriggerSpell, aurEff);   // the stun helper, from data
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_warl_forever_pyroclasm::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_warl_forever_pyroclasm::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Bane of Havoc (the debuff): keeps the tracker (its E0 trigger spell) on the warlock while it is up
class spell_warl_forever_bane_of_havoc : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_bane_of_havoc);

    void AfterApply(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (Unit* caster = GetCaster())
            caster->CastSpell(caster, aurEff->GetSpellEffectInfo().TriggerSpell, aurEff);
    }

    void AfterRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;
        for (Aura const* aura : caster->GetSingleCastAuras())   // moved to a new target: that one keeps the tracker
            if (aura != GetAura() && aura->GetId() == GetId())
                return;
        caster->RemoveAurasDueToSpell(aurEff->GetSpellEffectInfo().TriggerSpell);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_warl_forever_bane_of_havoc::AfterApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_warl_forever_bane_of_havoc::AfterRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Bane of Havoc (the tracker): damage the warlock deals to anything else is echoed at s1% onto the Havoc target.
// E0 MiscValue = the debuff, E0 trigger spell = the echo.
class spell_warl_forever_bane_of_havoc_proc : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_bane_of_havoc_proc);

    Unit* GetHavocTarget(ProcEventInfo const& eventInfo) const
    {
        uint32 havoc = GetEffect(EFFECT_0)->GetMiscValue();
        for (Aura* aura : GetTarget()->GetSingleCastAuras())
            if (aura->GetId() == havoc)
            {
                Unit* target = aura->GetUnitOwner();
                bool valid = target != eventInfo.GetActionTarget() && target->IsAlive() && target->IsInMap(GetTarget());
                return valid ? target : nullptr;
            }
        return nullptr;
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage() || eventInfo.GetActionTarget() == GetTarget())   // not the warlock's own Hellfire burn
            return false;
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (spell && spell->Id == GetEffect(EFFECT_0)->GetSpellEffectInfo().TriggerSpell)
            return false;
        return GetHavocTarget(eventInfo) != nullptr;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* target = GetHavocTarget(eventInfo);
        int32 echo = CalculatePct(static_cast<int32>(eventInfo.GetDamageInfo()->GetDamage()), aurEff->GetAmount());
        if (!target || echo <= 0)
            return;
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(echo);
        GetTarget()->CastSpell(target, aurEff->GetSpellEffectInfo().TriggerSpell, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_warl_forever_bane_of_havoc_proc::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_warl_forever_bane_of_havoc_proc::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Shadow and Flame: Conflagrate -> Shadow buff (E0 trigger), Shadowburn -> Flame buff (E2 trigger) and an
// E1% Soul Shard refund; both buffs at E2%
class spell_warl_forever_shadow_and_flame : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_shadow_and_flame);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_CREATE_SOUL_SHARD });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (!spell)
            return;

        Unit* warlock = GetTarget();
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(GetEffect(EFFECT_2)->GetAmount());
        if (spell->SpellFamilyFlags[1] & 0x800000)       // Conflagrate
            warlock->CastSpell(warlock, GetEffect(EFFECT_0)->GetSpellEffectInfo().TriggerSpell, args);
        else if (spell->SpellFamilyFlags[0] & 0x80)      // Shadowburn
        {
            warlock->CastSpell(warlock, GetEffect(EFFECT_2)->GetSpellEffectInfo().TriggerSpell, args);
            if (roll_chance_i(GetEffect(EFFECT_1)->GetAmount()))
                warlock->CastSpell(warlock, SPELL_CREATE_SOUL_SHARD, aurEff);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_warl_forever_shadow_and_flame::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Conflagrate (every rank): needs your Immolate and consumes it, unless Shadow and Flame (E1, marked Immolate) saves
// it. The DBC TargetAuraState is 0: 14 would run the core's WotLK Conflagrate branch, which asserts on these ranks.
class spell_warl_forever_conflagrate : public SpellScript
{
    PrepareSpellScript(spell_warl_forever_conflagrate);

    SpellCastResult CheckCast()
    {
        Unit* target = GetExplTargetUnit();
        return target && GetImmolate(target, GetCaster()) ? SPELL_CAST_OK : SPELL_FAILED_TARGET_AURASTATE;
    }

    void ConsumeImmolate(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* immolate = target ? GetImmolate(target, caster) : nullptr;
        if (!immolate)
            return;
        if (AuraEffect const* keep = GetMarkedTalent(caster, SPELL_IMMOLATE_R1))
            if (roll_chance_i(keep->GetAmount()))
                return;
        target->RemoveAurasDueToSpell(immolate->GetId(), caster->GetGUID());
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_warl_forever_conflagrate::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_warl_forever_conflagrate::ConsumeImmolate, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Demonic Brand debuff (forever_classes): a hit from the branding warlock's pet spends a charge (spell_proc) and makes the
// pet cast e0's TriggerSpell (Shadow), or e1's (Fire) when e1's MiscValue is the pet's creature family (Imp)
class spell_warl_forever_demonic_brand : public AuraScript
{
    PrepareAuraScript(spell_warl_forever_demonic_brand);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* actor = eventInfo.GetActor();
        return actor && actor->IsPet() && actor->GetOwnerGUID() == GetCasterGUID();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Creature* pet = eventInfo.GetActor()->ToCreature();
        if (!pet)
            return;
        SpellEffectInfo const& fire = GetSpellInfo()->GetEffect(EFFECT_1);
        uint32 const spellId = pet->GetCreatureTemplate()->family == uint32(fire.MiscValue) ? fire.TriggerSpell : aurEff->GetSpellEffectInfo().TriggerSpell;
        CastSpellExtraArgs args(aurEff);
        args.SetOriginalCaster(pet->GetGUID());
        pet->CastSpell(GetTarget(), spellId, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_warl_forever_demonic_brand::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_warl_forever_demonic_brand::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddSC_forever_talents_warlock()
{
    RegisterSpellScript(spell_warl_forever_demonic_brand);
    RegisterSpellScript(spell_warl_forever_soul_harvesting);
    RegisterSpellScript(spell_warl_forever_demonic_sacrifice);
    RegisterSpellScript(spell_warl_forever_summon_demon);
    RegisterSpellScript(spell_warl_forever_pyroclasm);
    RegisterSpellScript(spell_warl_forever_bane_of_havoc);
    RegisterSpellScript(spell_warl_forever_bane_of_havoc_proc);
    RegisterSpellScript(spell_warl_forever_shadow_and_flame);
    RegisterSpellScript(spell_warl_forever_conflagrate);
}
