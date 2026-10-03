/*
 * forever_talents: Druid talents that need code (modules/forever_classes/data/impl/Druid.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Druid.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "SpellScript.h"

namespace
{
    enum
    {
        ICON_OVERGROWTH      = 2536,
        ICON_HEART_OF_WILD   = 240,
        ICON_REND_AND_TEAR   = 2859,
        SPELL_HOTW_BEAR      = 24899,
        SPELL_PRIMAL_FURY_RAGE = 16959,
        SPELL_PRIMAL_FURY_CP = 16953,
    };

    // -339 Entangling Roots: one rooted target per druid, plus Overgrowth's amount. The ranks lose
    // SPELL_ATTR5_SINGLE_TARGET_SPELL (Druid.ts) so the core's cap-at-one loop never runs; this registers the aura
    // in the caster's single-target list itself (the core unregisters it on removal) and applies the raised cap.
    class spell_dru_entangling_roots_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_entangling_roots_forever);

        void AfterApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
        {
            Aura* aura = GetAura();
            Unit* caster = GetCaster();
            if (!caster || aura->IsSingleTarget())
                return;

            int32 allowed = 1;
            if (AuraEffect const* og = caster->GetDummyAuraEffect(SPELLFAMILY_DRUID, ICON_OVERGROWTH, EFFECT_0))
                allowed += og->GetAmount();

            Unit::AuraList& list = caster->GetSingleCastAuras();
            aura->SetIsSingleTarget(true);
            list.push_back(aura);

            int32 others = 0;
            for (Aura const* a : list)
                if (a != aura && a->IsSingleTargetWith(aura))
                    ++others;
            // list is oldest first; Remove() unregisters, so restart after each
            while (others-- >= allowed)
                for (Aura* a : list)
                    if (a != aura && a->IsSingleTargetWith(aura))
                    {
                        a->Remove();
                        break;
                    }
        }

        void Register() override
        {
            AfterEffectApply += AuraEffectApplyFn(spell_dru_entangling_roots_forever::AfterApply, EFFECT_0, SPELL_AURA_MOD_ROOT, AURA_EFFECT_HANDLE_REAL);
        }
    };

    // Eclipse ranks: Wrath damage adds E2 charges (up to the helper's ProcCharges) of the helper buff, which takes
    // E1 ms off Starfire's cast. Helper id is the rank's E0 trigger spell (Druid.ts); its spell_proc drops a charge.
    class spell_dru_eclipse_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_eclipse_forever);

        void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
        {
            PreventDefaultAction();
            uint32 helperId = GetSpellInfo()->GetEffect(EFFECT_0).TriggerSpell;
            SpellInfo const* helper = sSpellMgr->GetSpellInfo(helperId);
            AuraEffect const* cut = GetEffect(EFFECT_1);
            AuraEffect const* add = GetEffect(EFFECT_2);
            if (!helper || !cut || !add)
                return;

            Unit* target = GetTarget();
            int32 charges = add->GetAmount();
            if (Aura* buff = target->GetAura(helperId))
            {
                buff->SetCharges(std::min<int32>(buff->GetCharges() + charges, helper->ProcCharges));
                buff->RefreshDuration();
                return;
            }
            CastSpellExtraArgs args(aurEff);
            args.AddSpellBP0(cut->GetAmount());
            target->CastSpell(target, helperId, args);
            if (Aura* buff = target->GetAura(helperId))
                buff->SetCharges(std::min<int32>(charges, helper->ProcCharges));
        }

        void Register() override
        {
            OnEffectProc += AuraEffectProcFn(spell_dru_eclipse_forever::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // 24899 / 24900: the core's Heart of the Wild hack casts these with half the Intellect %; Forever's per-rank
    // bear Stamina % and cat Strength % are the talent's E1 / E2.
    class spell_dru_heart_of_the_wild_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_heart_of_the_wild_forever);

        void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
        {
            for (AuraEffect const* talent : GetUnitOwner()->GetAuraEffectsByType(SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE))
            {
                if (talent->GetSpellInfo()->SpellIconID != ICON_HEART_OF_WILD || talent->GetMiscValue() != STAT_INTELLECT)
                    continue;
                if (AuraEffect const* v = talent->GetBase()->GetEffect(GetId() == SPELL_HOTW_BEAR ? EFFECT_1 : EFFECT_2))
                    amount = v->GetAmount();
                return;
            }
        }

        void Register() override
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_heart_of_the_wild_forever::CalcAmount, EFFECT_0, SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE);
        }
    };

    // -16929 Thick Hide: E1 base armor = level x E1 + (defense - 5 x level) x E0 / 100. The ranks are form-gated
    // passives, reapplied (so recalculated) on every shapeshift.
    class spell_dru_thick_hide_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_thick_hide_forever);

        void CalcAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
        {
            Player* player = GetUnitOwner()->ToPlayer();
            if (!player)
                return;
            int32 level = player->GetLevel();
            int32 extraDefense = std::max<int32>(0, int32(player->GetDefenseSkillValue()) - 5 * level);
            amount = level * amount + extraDefense * GetSpellInfo()->GetEffect(EFFECT_0).CalcValue() / 100;
            canBeRecalculated = true;
        }

        void Register() override
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_thick_hide_forever::CalcAmount, EFFECT_1, SPELL_AURA_MOD_BASE_RESISTANCE);
        }
    };

    // Primal Fury ranks: crits in Bear/Dire Bear roll E0 for 5 rage; crits of Cat combo-point builders roll E1 for
    // an extra combo point.
    class spell_dru_primal_fury_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_primal_fury_forever);

        bool Validate(SpellInfo const* /*spellInfo*/) override
        {
            return ValidateSpellInfo({ SPELL_PRIMAL_FURY_RAGE, SPELL_PRIMAL_FURY_CP });
        }

        bool CheckProc(ProcEventInfo& eventInfo)
        {
            Player* player = eventInfo.GetActor()->ToPlayer();
            if (!player)
                return false;
            switch (player->GetShapeshiftForm())
            {
                case FORM_BEAR:
                case FORM_DIREBEAR:
                    return true;
                case FORM_CAT:
                {
                    SpellInfo const* spell = eventInfo.GetSpellInfo();
                    return spell && spell->SpellFamilyName == SPELLFAMILY_DRUID
                        && (spell->HasEffect(SPELL_EFFECT_ADD_COMBO_POINTS) || spell->SpellFamilyFlags[1] & 0x400); // Mangle (Cat): core adds its CP
                }
                default:
                    return false;
            }
        }

        void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
        {
            PreventDefaultAction();
            Unit* actor = eventInfo.GetActor();
            if (actor->GetShapeshiftForm() == FORM_CAT)
            {
                if (AuraEffect const* cp = GetEffect(EFFECT_1))
                    if (roll_chance_i(cp->GetAmount()))
                        if (Unit* target = eventInfo.GetProcTarget())
                            actor->CastSpell(target, SPELL_PRIMAL_FURY_CP, aurEff);
            }
            else if (roll_chance_i(aurEff->GetAmount()))
                actor->CastSpell(actor, SPELL_PRIMAL_FURY_RAGE, aurEff);
        }

        void Register() override
        {
            DoCheckProc += AuraCheckProcFn(spell_dru_primal_fury_forever::CheckProc);
            OnEffectProc += AuraEffectProcFn(spell_dru_primal_fury_forever::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        }
    };

    // Druid melee abilities (bound by Druid.ts; Shred and Maul keep the core's own hack): Rend and Tear adds its
    // E0 % against targets that were bleeding before the hit (Rake's own bleed does not count).
    class spell_dru_rend_and_tear_forever : public SpellScript
    {
        PrepareSpellScript(spell_dru_rend_and_tear_forever);

        void CheckBleeding(SpellMissInfo /*missInfo*/)
        {
            _bleeding = GetHitUnit() && GetHitUnit()->HasAuraState(AURA_STATE_BLEEDING, GetSpellInfo(), GetCaster());
        }

        void HandleHit()
        {
            if (!_bleeding)
                return;
            if (AuraEffect const* rt = GetCaster()->GetDummyAuraEffect(SPELLFAMILY_DRUID, ICON_REND_AND_TEAR, EFFECT_0))
                SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), rt->GetAmount()));
        }

        void Register() override
        {
            BeforeHit += BeforeSpellHitFn(spell_dru_rend_and_tear_forever::CheckBleeding);
            OnHit += SpellHitFn(spell_dru_rend_and_tear_forever::HandleHit);
        }

        bool _bleeding = false;
    };

    // Wild Growth: first tick +6%, 2% less each tick after (stock spell_dru_wild_growth_aura, without its
    // SpellScript half, which needs an EFFECT_2 Forever's spell lacks).
    class spell_dru_wild_growth_forever : public AuraScript
    {
        PrepareAuraScript(spell_dru_wild_growth_forever);

        void SetTickHeal(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
        {
            _baseTick = amount;
        }

        void HandleTickUpdate(AuraEffect* aurEff)
        {
            float const bonus = 6.f - 2.f * (aurEff->GetTickNumber() - 1);
            aurEff->SetAmount(int32(_baseTick + CalculatePct(_baseTick, bonus)));
        }

        void Register() override
        {
            DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_dru_wild_growth_forever::SetTickHeal, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
            OnEffectUpdatePeriodic += AuraEffectUpdatePeriodicFn(spell_dru_wild_growth_forever::HandleTickUpdate, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
        }

        float _baseTick = 0.f;
    };
}

// Lacerate (forever_classes clones; bound per chain by the datascript): e1 DUMMY adds e1% of a main-hand hit per
// Lacerate stack the caster already had on the target before this hit (the new stack does not count).
class spell_forever_dru_lacerate : public SpellScript
{
    PrepareSpellScript(spell_forever_dru_lacerate);

    void CountStacks(SpellMissInfo missInfo)
    {
        _stacks = 0;
        if (missInfo != SPELL_MISS_NONE || !GetHitUnit())
            return;
        if (Aura const* lacerate = GetHitUnit()->GetAuraOfRankedSpell(GetSpellInfo()->Id, GetCaster()->GetGUID()))
            _stacks = lacerate->GetStackAmount();
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        if (!_stacks)
            return;
        int32 weapon = int32(GetCaster()->CalculateDamage(BASE_ATTACK, false, true));
        SetHitDamage(GetHitDamage() + CalculatePct(weapon, GetEffectValue() * _stacks));
    }

    void Register() override
    {
        BeforeHit += BeforeSpellHitFn(spell_forever_dru_lacerate::CountStacks);
        OnEffectHitTarget += SpellEffectFn(spell_forever_dru_lacerate::HandleDummy, EFFECT_1, SPELL_EFFECT_DUMMY);
    }

    uint8 _stacks = 0;
};

void AddSC_forever_talents_druid()
{
    RegisterSpellScript(spell_dru_entangling_roots_forever);
    RegisterSpellScript(spell_forever_dru_lacerate);
    RegisterSpellScript(spell_dru_eclipse_forever);
    RegisterSpellScript(spell_dru_heart_of_the_wild_forever);
    RegisterSpellScript(spell_dru_thick_hide_forever);
    RegisterSpellScript(spell_dru_primal_fury_forever);
    RegisterSpellScript(spell_dru_rend_and_tear_forever);
    RegisterSpellScript(spell_dru_wild_growth_forever);
}
