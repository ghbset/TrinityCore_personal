/*
 * forever_talents: Warrior talents that need code (modules/forever_classes/data/impl/Warrior.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Warrior.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"
#include "World.h"

// Spearing Strike: x(1 + s3) damage against Giants, Dragonkin and mounted targets, and dismounts them.
class spell_forever_warr_spearing_strike : public SpellScript
{
    PrepareSpellScript(spell_forever_warr_spearing_strike);

    void HandleHit()
    {
        Unit* target = GetHitUnit();
        if (!target)
            return;

        bool const mounted = target->IsMounted();
        uint32 const type = target->GetCreatureType();
        if (mounted || type == CREATURE_TYPE_GIANT || type == CREATURE_TYPE_DRAGONKIN)
            SetHitDamage(GetHitDamage() * (1 + GetSpellInfo()->GetEffect(EFFECT_2).CalcValue(GetCaster())));

        if (mounted)
        {
            target->RemoveAurasByType(SPELL_AURA_MOUNTED);
            target->Dismount();     // creatures ride on a mount display, not an aura
        }
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_forever_warr_spearing_strike::HandleHit);
    }
};

// Bloodthrill: only white hits on a target bleeding from the warrior's own Rend.
class spell_forever_warr_bloodthrill : public AuraScript
{
    PrepareAuraScript(spell_forever_warr_bloodthrill);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Unit* target = eventInfo.GetProcTarget();
        return target && target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_WARRIOR, 0x20, 0, 0, GetCasterGUID());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_warr_bloodthrill::CheckProc);
    }
};

// Blood Craze: heals m1% of max health over 6 sec after being crit, after a Bloodthirst hit,
// or after taking a hit larger than m2% of max health.
class spell_forever_warr_blood_craze : public AuraScript
{
    PrepareAuraScript(spell_forever_warr_blood_craze);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        if (!damage || !damage->GetDamage())
            return false;

        if (eventInfo.GetActor() == GetTarget())
        {
            SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
            return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_WARRIOR && (spellInfo->SpellFamilyFlags[1] & 0x400); // Bloodthirst
        }

        return (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
            || damage->GetDamage() > GetTarget()->CountPctFromMaxHealth(GetEffect(EFFECT_1)->GetAmount());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(std::max<int32>(1, target->CountPctFromMaxHealth(aurEff->GetAmount()) / 3));  // 3 ticks
        target->CastSpell(target, aurEff->GetSpellEffectInfo().TriggerSpell, args);   // the rank's heal helper
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_warr_blood_craze::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_warr_blood_craze::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Dual Wield Specialization: off-hand white hits generate e1% more Rage (the off-hand +hit is aura 54 MiscValueB 1).
class spell_forever_warr_dual_wield_specialization : public AuraScript
{
    PrepareAuraScript(spell_forever_warr_dual_wield_specialization);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return (eventInfo.GetTypeMask() & PROC_FLAG_DONE_MELEE_AUTO_ATTACK) && damage && damage->GetAttackType() == OFF_ATTACK
            && eventInfo.GetActor()->GetPowerType() == POWER_RAGE;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        // Unit::RewardRage for this swing (float, so small percentages are not truncated away), times e1%
        Unit* warrior = eventInfo.GetActor();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        float const level = float(warrior->GetLevel());
        float conversion = 0.0091107836f * level * level + 3.225598133f * level + 4.2652911f;
        if (level > 70.f)
            conversion += 13.27f * (level - 70.f);

        uint32 speedFactor = uint32(warrior->GetAttackTime(OFF_ATTACK) / 1000.0f * 1.75f);
        if (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
            speedFactor *= 2;

        float rage = ((damage->GetDamage() + damage->GetAbsorb()) / conversion * 7.5f + speedFactor) / 2;
        AddPct(rage, warrior->GetTotalAuraModifier(SPELL_AURA_MOD_RAGE_FROM_DAMAGE_DEALT));
        rage *= sWorld->getRate(RATE_POWER_RAGE_INCOME) * aurEff->GetAmount() / 100.f;
        warrior->ModifyPower(POWER_RAGE, int32(rage * 10));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_warr_dual_wield_specialization::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_warr_dual_wield_specialization::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Execute (forever_classes clones; bound per chain by the datascript): all remaining rage becomes damage at the rank's
// rate (effect 0's DamageMultiplier), no 30-rage cap and no attack-power term (stock spell_warr_execute is WotLK's).
// The damage is still the stock Execute damage spell (20647).
class spell_forever_warr_execute : public SpellScript
{
    PrepareSpellScript(spell_forever_warr_execute);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ 20647 });
    }

    void HandleEffect(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;

        int32 rageUsed = caster->GetPower(POWER_RAGE);          // the cost is already paid
        caster->SetPower(POWER_RAGE, 0);
        int32 bp = GetEffectValue() + int32(rageUsed * GetEffectInfo().DamageMultiplier);
        CastSpellExtraArgs args(GetOriginalCaster()->GetGUID());
        args.AddSpellBP0(bp);
        caster->CastSpell(target, 20647, args);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_forever_warr_execute::HandleEffect, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Deep Wounds (forever_classes clones; bound per chain by the datascript): on a crit, bleeds e0% of the critting hand's
// average weapon damage over the trigger spell's duration (the stock pair gives 16% x rank of a 6 sec bleed instead).
class spell_forever_warr_deep_wounds : public AuraScript
{
    PrepareAuraScript(spell_forever_warr_deep_wounds);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->GetEffect(EFFECT_0).TriggerSpell });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return eventInfo.GetDamageInfo() && eventInfo.GetActor()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* actor = eventInfo.GetActor();
        bool const offHand = eventInfo.GetDamageInfo()->GetAttackType() == OFF_ATTACK;
        float damage = offHand
            ? (actor->GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE) + actor->GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE)) / 2.f
            : (actor->GetFloatValue(UNIT_FIELD_MINDAMAGE) + actor->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.f;
        ApplyPct(damage, aurEff->GetAmount());

        uint32 const bleedId = aurEff->GetSpellEffectInfo().TriggerSpell;
        uint32 const ticks = std::max<uint32>(1, sSpellMgr->AssertSpellInfo(bleedId)->GetMaxTicks());
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(std::max<int32>(1, int32(damage / ticks)));
        actor->CastSpell(eventInfo.GetProcTarget(), bleedId, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_warr_deep_wounds::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_warr_deep_wounds::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

void AddSC_forever_talents_warrior()
{
    RegisterSpellScript(spell_forever_warr_spearing_strike);
    RegisterSpellScript(spell_forever_warr_execute);
    RegisterSpellScript(spell_forever_warr_deep_wounds);
    RegisterSpellScript(spell_forever_warr_bloodthrill);
    RegisterSpellScript(spell_forever_warr_blood_craze);
    RegisterSpellScript(spell_forever_warr_dual_wield_specialization);
}
