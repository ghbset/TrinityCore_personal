/*
 * forever_talents: Rogue talents that need code (modules/forever_classes/data/impl/Rogue.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Rogue.ts), never by hard-coded new ids.
 */

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

// Expose Armor (forever_classes clones): Improved Expose Armor refunds its e1 combo points (a DUMMY carrying Expose
// Armor's class mask) when Expose Armor was cast with at least its e2. The talent is found by data, not by id.
class spell_forever_rog_improved_expose_armor : public SpellScript
{
    PrepareSpellScript(spell_forever_rog_improved_expose_armor);

    uint8 _comboPoints = 0;

    void RecordComboPoints()
    {
        _comboPoints = GetCaster()->GetComboPoints();
    }

    void Refund()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        // points still there: the strike missed and nothing was spent
        if (!target || caster->GetComboPoints())
            return;

        for (AuraEffect const* refund : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
        {
            if (refund->GetEffIndex() != EFFECT_1 || !refund->IsAffectingSpell(GetSpellInfo()))
                continue;
            AuraEffect const* need = refund->GetBase()->GetEffect(EFFECT_2);
            if (need && _comboPoints >= need->GetAmount())
                caster->AddComboPoints(target, int8(refund->GetAmount()));
            return;
        }
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_forever_rog_improved_expose_armor::RecordComboPoints);
        AfterCast += SpellCastFn(spell_forever_rog_improved_expose_armor::Refund);
    }
};

// Preparation (forever_classes clones): finishes the cooldown of every other Rogue ability.
class spell_forever_rog_preparation : public SpellScript
{
    PrepareSpellScript(spell_forever_rog_preparation);

    bool Load() override
    {
        return GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        uint32 const selfId = GetSpellInfo()->Id;       // its own cooldown is already running
        GetCaster()->GetSpellHistory()->ResetCooldowns([selfId](SpellHistory::CooldownStorageType::iterator itr) -> bool
        {
            SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(itr->first);
            return spellInfo->SpellFamilyName == SPELLFAMILY_ROGUE && spellInfo->Id != selfId;
        }, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_forever_rog_preparation::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Restless Blades (forever_classes clones): a damaging finisher takes e0 ms per combo point spent off the cooldowns of
// the spells in e0's class mask (combo points are still up during the HIT proc phase; the finish phase clears them).
class spell_forever_rog_restless_blades : public AuraScript
{
    PrepareAuraScript(spell_forever_rog_restless_blades);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Player* rogue = GetTarget()->ToPlayer();
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        DamageInfo* damage = eventInfo.GetDamageInfo();
        return rogue && spellInfo && spellInfo->NeedsComboPoints() && damage && damage->GetDamage() && rogue->GetComboPoints();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* rogue = GetTarget()->ToPlayer();
        int32 const reduction = -aurEff->GetAmount() * rogue->GetComboPoints();
        flag96 const mask = aurEff->GetSpellEffectInfo().SpellClassMask;
        for (auto const& [spellId, spell] : rogue->GetSpellMap())
            if (spell.state != PLAYERSPELL_REMOVED)
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId))
                    if (info->IsAffected(SPELLFAMILY_ROGUE, mask))
                        rogue->GetSpellHistory()->ModifyCooldown(spellId, reduction);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_rog_restless_blades::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_rog_restless_blades::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddSC_forever_talents_rogue()
{
    RegisterSpellScript(spell_forever_rog_improved_expose_armor);
    RegisterSpellScript(spell_forever_rog_preparation);
    RegisterSpellScript(spell_forever_rog_restless_blades);
}
