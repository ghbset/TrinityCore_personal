/*
 * forever_talents: Hunter talents that need code (modules/forever_classes/data/impl/Hunter.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Hunter.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"

// -1978 Serpent Sting: a target dying with the sting on it grants Rapid Killing (the talent's own trigger)
class spell_forever_serpent_sting_rapid_killing : public AuraScript
{
    PrepareAuraScript(spell_forever_serpent_sting_rapid_killing);

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;

        Player* hunter = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!hunter || !hunter->isHonorOrXPTarget(GetTarget()))
            return;

        for (AuraEffect* e : hunter->GetAuraEffectsByType(SPELL_AURA_PROC_TRIGGER_SPELL))
        {
            uint32 trigger = e->GetSpellEffectInfo().TriggerSpell;
            // the Rapid Killing talent: a hunter kill-proc; its buff (a forever_classes clone) is its own TriggerSpell
            if (trigger && e->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_HUNTER && (e->GetSpellInfo()->ProcFlags & PROC_FLAG_KILL))
            {
                hunter->CastSpell(hunter, trigger, CastSpellExtraArgs(e));
                break;
            }
        }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_forever_serpent_sting_rapid_killing::AfterRemove, EFFECT_0, SPELL_AURA_PERIODIC_DAMAGE, AURA_EFFECT_HANDLE_REAL);
    }
};

// Rapid Killing buffs (forever_classes clones): consuming the charge grants Rapid Recuperation's effect-1 value
class spell_forever_rapid_killing_consume : public AuraScript
{
    PrepareAuraScript(spell_forever_rapid_killing_consume);

    void AfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        // a used charge removes BY_DEFAULT with 0 charges left; expiry, cancel and logout do not
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT || GetAura()->GetCharges())
            return;

        Unit* target = GetTarget();
        for (AuraEffect* e : target->GetAuraEffectsByType(SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE))
        {
            SpellInfo const* helper = sSpellMgr->GetSpellInfo(e->GetSpellEffectInfo().TriggerSpell);
            if (e->GetEffIndex() == EFFECT_1 && e->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_HUNTER
                && helper && helper->HasAura(SPELL_AURA_MOD_MANA_REGEN_INTERRUPT))
            {
                CastSpellExtraArgs args(e);
                args.AddSpellBP0(e->GetAmount());
                target->CastSpell(target, helper->Id, args);
                break;
            }
        }
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_forever_rapid_killing_consume::AfterRemove, EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// -19184 Entrapment: roots every target a trap affects for the rank's time (effect 0 amount, ms)
class spell_forever_entrapment : public AuraScript
{
    PrepareAuraScript(spell_forever_entrapment);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return (eventInfo.GetTypeMask() & PROC_FLAG_DONE_TRAP_ACTIVATION) != 0;
    }

    void Root(AuraEffect const* aurEff, Unit* hunter, Unit* victim)
    {
        if (!victim->IsAlive() || !hunter->IsValidAttackTarget(victim))
            return;

        uint32 rootId = aurEff->GetSpellEffectInfo().TriggerSpell;
        hunter->CastSpell(victim, rootId, CastSpellExtraArgs(aurEff));
        // the root spell lasts the longest rank; scale the applied (diminished) duration down to this rank
        if (Aura* root = victim->GetAura(rootId, hunter->GetGUID()))
            if (int32 full = root->GetSpellInfo()->GetMaxDuration())
            {
                int32 duration = int32(int64(root->GetMaxDuration()) * aurEff->GetAmount() / full);
                root->SetMaxDuration(duration);
                root->SetDuration(duration);
            }
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* hunter = GetTarget();

        if (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_HIT)
        {
            if (Unit* victim = eventInfo.GetProcTarget())
                Root(aurEff, hunter, victim);
            return;
        }

        // Frost Trap is a persistent area: it hits no unit itself, so root everyone inside it when it springs
        Spell const* trap = eventInfo.GetProcSpell();
        SpellInfo const* trapInfo = eventInfo.GetSpellInfo();
        if (!trap || !trapInfo || !(trapInfo->SpellFamilyFlags[0] & 0x10) || !trap->GetCaster())
            return;

        WorldObject* center = trap->GetCaster();
        float radius = trapInfo->GetEffect(EFFECT_0).CalcRadius(hunter);
        std::list<Unit*> victims;
        Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(center, hunter, radius);
        Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, victims, check);
        Cell::VisitAllObjects(center, searcher, radius);
        for (Unit* victim : victims)
            Root(aurEff, hunter, victim);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_entrapment::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_entrapment::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

void AddSC_forever_talents_hunter()
{
    RegisterSpellScript(spell_forever_serpent_sting_rapid_killing);
    RegisterSpellScript(spell_forever_rapid_killing_consume);
    RegisterSpellScript(spell_forever_entrapment);
}
