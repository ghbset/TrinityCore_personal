/*
 * forever_talents: Paladin talents that need code (modules/forever_classes/data/impl/Paladin.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Paladin.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include <algorithm>
#include <vector>

namespace
{
enum PaladinForeverSpells
{
    SPELL_PALADIN_ILLUMINATION_ENERGIZE     = 20272,
    SPELL_PALADIN_SEAL_OF_RIGHTEOUSNESS_HIT = 25742,
    SPELL_PALADIN_JUDGEMENT_DAMAGE          = 54158
};

// SpellFamilyFlags[2] bits the datascript gives Forever spells
uint32 constexpr FLAG2_LIGHTS_VIGIL          = 0x00001000;
uint32 constexpr FLAG2_TWIST_OF_LIGHT        = 0x00008000;
uint32 constexpr FLAG2_IMPROVED_SEAL_OF_FURY = 0x00040000;
uint32 constexpr FLAG2_SANCTIFIED_JUDGEMENT  = 0x00080000;   // 0x400000 is Forever's Divine Intervention bit

Aura* GetSeal(Unit* paladin)
{
    for (auto itr = paladin->GetAppliedAuras().begin(); itr != paladin->GetAppliedAuras().end(); ++itr)
    {
        Aura* aura = itr->second->GetBase();
        if (aura->GetCasterGUID() == paladin->GetGUID() && aura->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_SEAL)
            return aura;
    }
    return nullptr;
}

// One swing of a seal's on-hit: Righteousness/Fury's own proc, or any eligible seal replayed by an Echo.
void SealOnHit(Unit* paladin, Unit* victim, SpellInfo const* seal, int32 amount, AuraEffect const* source)
{
    SpellEffectInfo const& effect = seal->GetEffect(EFFECT_0);
    CastSpellExtraArgs args(source);
    if (effect.TriggerSpell)    // Command, Justice: their proc spell; Fury: its Holy damage
    {
        paladin->CastSpell(victim, effect.TriggerSpell, args);
        // Seal of Fury: e0 amount (+ e0 coefficient x Holy spell power) threat per swing (Command / Justice: 0)
        if (amount > 0 && victim->CanHaveThreatList())
            victim->GetThreatManager().AddThreat(paladin, amount + paladin->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY) * effect.BonusMultiplier, seal);
        return;
    }
    // Righteousness: $s1 Holy damage per 100 sec of weapon speed
    args.AddSpellBP0(std::lround(amount * paladin->GetAttackTime(BASE_ATTACK) / 100000.0f));
    paladin->CastSpell(victim, SPELL_PALADIN_SEAL_OF_RIGHTEOUSNESS_HIT, args);
}

SpellCastResult CheckFriendOrFrontEnemy(Unit* caster, Unit* target)
{
    if (!target)
        return SPELL_FAILED_BAD_TARGETS;
    if (!caster->IsFriendlyTo(target))
    {
        if (!caster->IsValidAttackTarget(target))
            return SPELL_FAILED_BAD_TARGETS;
        if (!caster->isInFront(target))
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }
    return SPELL_CAST_OK;
}
}

// Illumination (all ranks): crit heals refund $m3% of the cast spell's cost
class spell_forever_pal_illumination : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_illumination);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PALADIN_ILLUMINATION_ENERGIZE });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        HealInfo* healInfo = eventInfo.GetHealInfo();
        if (!healInfo || !healInfo->GetSpellInfo())
            return;

        // Holy Shock and Light's Vigil heals name the spell whose mana was paid in their E0 TriggerSpell
        SpellInfo const* spell = healInfo->GetSpellInfo();
        if (SpellInfo const* cast = sSpellMgr->GetSpellInfo(spell->GetEffect(EFFECT_0).TriggerSpell))
            spell = cast;

        Unit* paladin = GetTarget();
        CastSpellExtraArgs args(aurEff);
        args.AddSpellBP0(CalculatePct(spell->CalcPowerCost(paladin, spell->GetSchoolMask()), GetSpellInfo()->GetEffect(EFFECT_2).CalcValue()));
        paladin->CastSpell(paladin, SPELL_PALADIN_ILLUMINATION_ENERGIZE, args);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_forever_pal_illumination::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Holy Shock (all ranks): damage spell in E0 TriggerSpell, heal in E0 MiscValue; pays out Light's Vigil
class spell_forever_pal_holy_shock : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_holy_shock);

    SpellCastResult CheckCast()
    {
        return CheckFriendOrFrontEnemy(GetCaster(), GetExplTargetUnit());
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;

        SpellEffectInfo const& effect = GetSpellInfo()->GetEffect(effIndex);
        caster->CastSpell(target, caster->IsFriendlyTo(target) ? uint32(effect.MiscValue) : effect.TriggerSpell, true);

        // Light's Vigil mark: party heal or bonus damage (+ mana refund carried as the mark's amount), no cooldown
        AuraEffect* mark = target->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 0, 0, FLAG2_LIGHTS_VIGIL, caster->GetGUID());
        if (!mark)
            return;
        SpellEffectInfo const& markEffect = mark->GetSpellInfo()->GetEffect(EFFECT_0);
        caster->CastSpell(target, markEffect.TriggerSpell, true);
        if (markEffect.MiscValue && mark->GetAmount() > 0)
            caster->CastSpell(caster, uint32(markEffect.MiscValue), CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(mark->GetAmount()));
        mark->GetBase()->Remove();
        _resetCooldown = true;
    }

    void ResetCooldown()
    {
        if (!_resetCooldown)
            return;
        uint32 spellId = GetSpellInfo()->Id;
        uint32 category = GetSpellInfo()->GetCategory();
        GetCaster()->GetSpellHistory()->ResetCooldowns([spellId, category](SpellHistory::CooldownStorageType::iterator itr)
        {
            return itr->first == spellId || (category && itr->second.CategoryId == category);
        }, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forever_pal_holy_shock::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_forever_pal_holy_shock::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
        AfterCast += SpellCastFn(spell_forever_pal_holy_shock::ResetCooldown);
    }

    bool _resetCooldown = false;
};

// Light's Vigil (all ranks): friendly mark in E0 TriggerSpell, hostile mark in E1 TriggerSpell; one mark per paladin
class spell_forever_pal_lights_vigil : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_lights_vigil);

    SpellCastResult CheckCast()
    {
        return CheckFriendOrFrontEnemy(GetCaster(), GetExplTargetUnit());
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;

        std::vector<Aura*> marks;
        for (Aura* aura : caster->GetSingleCastAuras())
            if (aura->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_PALADIN && (aura->GetSpellInfo()->SpellFamilyFlags[2] & FLAG2_LIGHTS_VIGIL))
                marks.push_back(aura);
        for (Aura* aura : marks)
            aura->Remove();

        SpellInfo const* spellInfo = GetSpellInfo();
        if (caster->IsFriendlyTo(target))
        {
            caster->CastSpell(target, spellInfo->GetEffect(EFFECT_0).TriggerSpell, true);
            return;
        }
        // the hostile mark carries the refund: $s2% of Light's Vigil's mana cost
        int32 refund = CalculatePct(spellInfo->CalcPowerCost(caster, spellInfo->GetSchoolMask()), spellInfo->GetEffect(EFFECT_1).CalcValue(caster));
        caster->CastSpell(target, spellInfo->GetEffect(EFFECT_1).TriggerSpell, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(refund));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forever_pal_lights_vigil::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_forever_pal_lights_vigil::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Consecration (all ranks) - Consecrated Ground: the first $s3 enemies to enter get the debuff named in E0 TriggerSpell
class spell_forever_pal_consecration : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_consecration);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetTarget();
        if (!caster || !caster->IsValidAttackTarget(target) || _first.count(target->GetGUID())
            || _first.size() >= uint32(std::max(0, GetSpellInfo()->GetEffect(EFFECT_2).CalcValue())))
            return;

        // the debuff's amount is the talent's spellmod: none without the talent
        SpellInfo const* debuff = sSpellMgr->GetSpellInfo(GetSpellInfo()->GetEffect(EFFECT_0).TriggerSpell);
        if (!debuff || debuff->GetEffect(EFFECT_0).CalcValue(caster) <= 0)
            return;

        _first.insert(target->GetGUID());
        caster->CastSpell(target, debuff->Id, true);
        if (Aura* aura = target->GetAura(debuff->Id, caster->GetGUID()))
            aura->SetDuration(GetDuration());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_forever_pal_consecration::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }

    GuidUnorderedSet _first;
};

// Seal of Righteousness / Seal of Fury (all ranks): the E0 DUMMY on-hit
class spell_forever_pal_seal : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_seal);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PALADIN_SEAL_OF_RIGHTEOUSNESS_HIT });
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return eventInfo.GetProcTarget() != nullptr;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SealOnHit(GetTarget(), eventInfo.GetProcTarget(), GetSpellInfo(), aurEff->GetAmount(), aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_pal_seal::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_pal_seal::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Seal of Fury damage (all ranks): with a shield equipped, Light's Fury (E0 TriggerSpell) absorbs E0 MiscValue % of it
class spell_forever_pal_seal_of_fury_damage : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_seal_of_fury_damage);

    void HandleAfterHit()
    {
        Player* paladin = GetCaster()->ToPlayer();
        int32 damage = GetHitDamage();
        if (!paladin || damage <= 0 || !paladin->GetShield(true))
            return;
        SpellEffectInfo const& effect = GetSpellInfo()->GetEffect(EFFECT_0);
        paladin->CastSpell(paladin, effect.TriggerSpell, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(CalculatePct(damage, effect.MiscValue)));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_forever_pal_seal_of_fury_damage::HandleAfterHit);
    }
};

// Light's Fury - Improved Seal of Fury: mana = level +$m2% per level the attacker is above, up to $m3 levels
class spell_forever_pal_lights_fury : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_lights_fury);

    void HandleAfterAbsorb(AuraEffect* aurEff, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        if (int32(absorbAmount) < aurEff->GetAmount())    // the shield is reduced after this hook
            return;
        Unit* paladin = GetTarget();
        AuraEffect const* talentEffect = paladin->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 0, 0, FLAG2_IMPROVED_SEAL_OF_FURY);
        if (!talentEffect)
            return;
        Aura const* talent = talentEffect->GetBase();
        AuraEffect const* perLevel = talent->GetEffect(EFFECT_1);
        AuraEffect const* maxLevels = talent->GetEffect(EFFECT_2);
        if (!perLevel || !maxLevels)
            return;

        int32 level = paladin->GetLevel();
        int32 above = 0;
        if (Unit* attacker = dmgInfo.GetAttacker())
            above = std::clamp(int32(attacker->GetLevel()) - level, 0, maxLevels->GetAmount());
        CastSpellExtraArgs args(talentEffect);
        args.AddSpellBP0(level * (100 + above * perLevel->GetAmount()) / 100);
        paladin->CastSpell(paladin, talent->GetSpellInfo()->GetEffect(EFFECT_0).TriggerSpell, args);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_forever_pal_lights_fury::HandleAfterAbsorb, EFFECT_0);
    }
};

// 20271 - Judgement - Sanctified Judgement: $m1% chance to refund $m2% of the judged seal's mana cost
class spell_forever_pal_sanctified_judgement : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_sanctified_judgement);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        AuraEffect const* talentEffect = caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 0, 0, FLAG2_SANCTIFIED_JUDGEMENT, caster->GetGUID());
        if (!talentEffect)
            return;
        Aura const* talent = talentEffect->GetBase();
        AuraEffect const* chance = talent->GetEffect(EFFECT_0);
        AuraEffect const* pct = talent->GetEffect(EFFECT_1);
        Aura* seal = GetSeal(caster);
        if (!chance || !pct || !seal || !roll_chance_i(chance->GetAmount()))
            return;

        SpellInfo const* sealInfo = seal->GetSpellInfo();
        CastSpellExtraArgs args(chance);
        args.AddSpellBP0(CalculatePct(sealInfo->CalcPowerCost(caster, sealInfo->GetSchoolMask()), pct->GetAmount()));
        caster->CastSpell(caster, talent->GetSpellInfo()->GetEffect(EFFECT_0).TriggerSpell, args);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_forever_pal_sanctified_judgement::HandleAfterHit);
    }
};

// 20271 - Judgement: casts the active seal's judgement (its E2 DUMMY amount), as spell_pal_judgement does,
// without that script's Judgement of Light on every target
class spell_forever_pal_judgement : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_judgement);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PALADIN_JUDGEMENT_DAMAGE });
    }

    void HandleScriptEffect(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        uint32 judgement = SPELL_PALADIN_JUDGEMENT_DAMAGE;
        for (AuraEffect const* aurEff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (aurEff->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_SEAL && aurEff->GetEffIndex() == EFFECT_2
                && sSpellMgr->GetSpellInfo(aurEff->GetAmount()))
            {
                judgement = aurEff->GetAmount();
                break;
            }
        }
        caster->CastSpell(GetHitUnit(), judgement, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_forever_pal_judgement::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// Judgement of Fury (all ranks): taunt (E1 TriggerSpell) and $s3 extra threat
class spell_forever_pal_judgement_of_fury : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_judgement_of_fury);

    void HandleTaunt(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!target)
            return;
        caster->CastSpell(target, GetSpellInfo()->GetEffect(EFFECT_1).TriggerSpell, true);
        if (target->CanHaveThreatList())
        {
            SpellEffectInfo const& threat = GetSpellInfo()->GetEffect(EFFECT_2);
            float amount = threat.CalcValue(caster) + threat.BonusMultiplier * caster->SpellBaseDamageBonusDone(GetSpellInfo()->GetSchoolMask());
            target->GetThreatManager().AddThreat(caster, amount, GetSpellInfo(), true);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_forever_pal_judgement_of_fury::HandleTaunt, EFFECT_1, SPELL_EFFECT_DUMMY);
    }
};

// Judgement of Command damage (all ranks): half damage unless the target is stunned or incapacitated
class spell_forever_pal_judgement_of_command : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_judgement_of_command);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* target = GetHitUnit();
        uint32 constexpr incapacitated = (1 << MECHANIC_STUN) | (1 << MECHANIC_KNOCKOUT) | (1 << MECHANIC_SAPPED) | (1 << MECHANIC_POLYMORPH) | (1 << MECHANIC_SLEEP);
        if (target && !target->HasAuraType(SPELL_AURA_MOD_STUN) && !target->HasAuraWithMechanic(incapacitated))
            SetHitDamage(GetHitDamage() / 2);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_forever_pal_judgement_of_command::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Every seal - Twist of Light: replacing a seal whose E0 MiscValue names an Echo gives that Echo (amount = the old seal)
class spell_forever_pal_seal_twist : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_seal_twist);

    void RecordReplacedSeal()
    {
        Unit* caster = GetCaster();
        // only a seal replaces a seal (the core lets Seal of the Crusader stand beside one)
        if (GetSpellInfo()->GetSpellSpecific() != SPELL_SPECIFIC_SEAL
            || !caster->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 0, 0, FLAG2_TWIST_OF_LIGHT, caster->GetGUID()))
            return;
        Aura* old = GetSeal(caster);
        if (!old)
            return;
        int32 echo = old->GetSpellInfo()->GetEffect(EFFECT_0).MiscValue;
        if (echo && echo != GetSpellInfo()->GetEffect(EFFECT_0).MiscValue)    // same Echo: the same seal again, nothing replaced
        {
            _echo = uint32(echo);
            _seal = old->GetId();
        }
    }

    void GiveEcho()
    {
        if (_echo)
            GetCaster()->CastSpell(GetCaster(), _echo, CastSpellExtraArgs(TRIGGERED_FULL_MASK).AddSpellBP0(int32(_seal)));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_forever_pal_seal_twist::RecordReplacedSeal);
        AfterCast += SpellCastFn(spell_forever_pal_seal_twist::GiveEcho);
    }

    uint32 _echo = 0;
    uint32 _seal = 0;
};

// Echo of Command / Righteousness / Fury / Justice: the next swing gets the replaced seal's on-hit once
class spell_forever_pal_echo : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_echo);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return eventInfo.GetProcTarget() != nullptr;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* paladin = GetTarget();
        SpellInfo const* seal = sSpellMgr->GetSpellInfo(aurEff->GetAmount());
        if (!seal)
            return;

        // Command and Justice proc by chance: roll the seal's own
        if (seal->GetEffect(EFFECT_0).ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL)
        {
            float chance = float(seal->ProcChance);
            if (SpellProcEntry const* procEntry = sSpellMgr->GetSpellProcEntry(seal->Id))
            {
                if (procEntry->ProcsPerMinute > 0.0f)
                    chance = paladin->GetPPMProcChance(paladin->GetAttackTime(BASE_ATTACK), procEntry->ProcsPerMinute, seal);
                else if (procEntry->Chance > 0.0f)
                    chance = procEntry->Chance;
            }
            if (!roll_chance_f(chance))
                return;
        }
        SealOnHit(paladin, eventInfo.GetProcTarget(), seal, seal->GetEffect(EFFECT_0).CalcValue(paladin), aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_pal_echo::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_pal_echo::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Swift Judgement (forever_classes): finishes the cooldown of every spell in e0's class mask (Judgement).
class spell_forever_pal_swift_judgement : public SpellScript
{
    PrepareSpellScript(spell_forever_pal_swift_judgement);

    void Reset()
    {
        flag96 const mask = GetSpellInfo()->GetEffect(EFFECT_0).SpellClassMask;
        GetCaster()->GetSpellHistory()->ResetCooldowns([mask](SpellHistory::CooldownStorageType::iterator itr) -> bool
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(itr->first);
            return info && info->IsAffected(SPELLFAMILY_PALADIN, mask);
        }, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_forever_pal_swift_judgement::Reset);
    }
};

// Templar's Bulwark (forever_classes): the absorb is e0's amount as % of the owner's maximum health.
class spell_forever_pal_templars_bulwark : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_templars_bulwark);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = int32(GetUnitOwner()->CountPctFromMaxHealth(amount));
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_forever_pal_templars_bulwark::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
    }
};

// Sacred Arbiter (forever_classes): a Holy Strike hit refreshes the paladin's own Judgement debuffs on the target that
// the talent's e1 class mask names.
class spell_forever_pal_sacred_arbiter : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_sacred_arbiter);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* target = eventInfo.GetProcTarget();
        if (!target)
            return;
        flag96 const mask = aurEff->GetSpellEffectInfo().SpellClassMask;
        ObjectGuid const paladin = GetTarget()->GetGUID();
        for (auto const& [id, app] : target->GetAppliedAuras())
        {
            Aura* aura = app->GetBase();
            if (aura->GetCasterGUID() == paladin && aura->GetSpellInfo()->IsAffected(SPELLFAMILY_PALADIN, mask))
                aura->RefreshDuration();
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_forever_pal_sacred_arbiter::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Judgement debuffs (forever_classes: Light, Wisdom, Justice, Crusader): an attacker hitting the target casts the
// effect's TriggerSpell on itself for the judging paladin (mana payouts skip attackers without mana); the paladin's own
// melee hits refresh the debuff.
class spell_forever_pal_judgement_debuff : public AuraScript
{
    PrepareAuraScript(spell_forever_pal_judgement_debuff);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* attacker = eventInfo.GetActor();
        if (!attacker)
            return;
        if (attacker->GetGUID() == GetCasterGUID()
            && (eventInfo.GetTypeMask() & (PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK | PROC_FLAG_TAKEN_SPELL_MELEE_DMG_CLASS)))
            GetAura()->RefreshDuration();

        SpellInfo const* payout = sSpellMgr->GetSpellInfo(aurEff->GetSpellEffectInfo().TriggerSpell);
        if (!payout || (payout->HasEffect(SPELL_EFFECT_ENERGIZE) && attacker->GetPowerType() != POWER_MANA))
            return;
        CastSpellExtraArgs args(aurEff);
        args.OriginalCaster = GetCasterGUID();
        attacker->CastSpell(attacker, payout->Id, args);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_forever_pal_judgement_debuff::HandleProc, EFFECT_ALL, SPELL_AURA_ANY);
    }
};

void AddSC_forever_talents_paladin()
{
    RegisterSpellScript(spell_forever_pal_illumination);
    RegisterSpellScript(spell_forever_pal_holy_shock);
    RegisterSpellScript(spell_forever_pal_lights_vigil);
    RegisterSpellScript(spell_forever_pal_consecration);
    RegisterSpellScript(spell_forever_pal_seal);
    RegisterSpellScript(spell_forever_pal_seal_of_fury_damage);
    RegisterSpellScript(spell_forever_pal_lights_fury);
    RegisterSpellScript(spell_forever_pal_sanctified_judgement);
    RegisterSpellScript(spell_forever_pal_judgement);
    RegisterSpellScript(spell_forever_pal_judgement_of_fury);
    RegisterSpellScript(spell_forever_pal_judgement_of_command);
    RegisterSpellScript(spell_forever_pal_seal_twist);
    RegisterSpellScript(spell_forever_pal_echo);
    RegisterSpellScript(spell_forever_pal_swift_judgement);
    RegisterSpellScript(spell_forever_pal_templars_bulwark);
    RegisterSpellScript(spell_forever_pal_sacred_arbiter);
    RegisterSpellScript(spell_forever_pal_judgement_debuff);
}
