/*
 * forever_talents: Shaman talents that need code (modules/forever_classes/data/impl/Shaman.spec.json).
 * Spells created by the module have build-time ids: scripts are bound by spell_script_names rows the
 * datascript writes (modules/forever_classes/datascripts/impl/Shaman.ts), never by hard-coded new ids.
 */

#include "ScriptMgr.h"
#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "DBCStores.h"
#include "Item.h"
#include "SpellAuraEffects.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "Unit.h"

// Improved Stormstrike (Forever 1223031 ranks)
// Stormstrike: $m1% chance for the casting mana regen buff (effect 0 PROC_TRIGGER_SPELL).
// Dodge or parry: $m2% chance to reset Stormstrike's cooldown (effect 1 DUMMY).
class spell_forever_sha_improved_stormstrike : public AuraScript
{
    PrepareAuraScript(spell_forever_sha_improved_stormstrike);

    // forever_classes: the talent's e1 TriggerSpell names our Stormstrike clone
    uint32 StormstrikeId() const { return GetSpellInfo()->GetEffect(EFFECT_1).TriggerSpell; }

    bool IsStormstrike(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spellInfo = eventInfo.GetSpellInfo();
        return eventInfo.GetActor() == GetTarget() && spellInfo && spellInfo->Id == StormstrikeId()
            && (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_CAST);
    }

    bool IsAvoided(ProcEventInfo& eventInfo)
    {
        return eventInfo.GetActionTarget() == GetTarget() && (eventInfo.GetTypeMask() & TAKEN_HIT_PROC_FLAG_MASK)
            && (eventInfo.GetHitMask() & (PROC_HIT_DODGE | PROC_HIT_PARRY));
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsStormstrike(eventInfo))
            return roll_chance_i(GetEffect(EFFECT_0)->GetAmount());
        if (IsAvoided(eventInfo))
            return roll_chance_i(GetEffect(EFFECT_1)->GetAmount());
        return false;
    }

    void HandleRegen(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        if (!IsStormstrike(eventInfo))
            PreventDefaultAction();
    }

    void HandleReset(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        if (IsAvoided(eventInfo))
            GetTarget()->GetSpellHistory()->ResetCooldown(StormstrikeId(), true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_sha_improved_stormstrike::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_forever_sha_improved_stormstrike::HandleRegen, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
        OnEffectProc += AuraEffectProcFn(spell_forever_sha_improved_stormstrike::HandleReset, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// -1064 Chain Heal (replaces spell_sha_chain_heal): Forever's Riptide boosts a Chain Heal cast on its target
// by Riptide's $s3% and is not consumed.
class spell_forever_sha_chain_heal : public SpellScript
{
    PrepareSpellScript(spell_forever_sha_chain_heal);

    void CheckRiptide()
    {
        Unit* target = GetExplTargetUnit();
        if (!target)
            return;
        if (AuraEffect const* riptide = target->GetAuraEffect(SPELL_AURA_PERIODIC_HEAL, SPELLFAMILY_SHAMAN, 0, 0, 0x10, GetCaster()->GetGUID()))
        {
            AuraEffect const* bonus = riptide->GetBase()->GetEffect(EFFECT_2);
            _bonusPct = bonus ? bonus->GetAmount() : 25;
        }
    }

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        if (_bonusPct)
            SetHitHeal(GetHitHeal() + CalculatePct(GetHitHeal(), _bonusPct));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_forever_sha_chain_heal::CheckRiptide);
        OnEffectHitTarget += SpellEffectFn(spell_forever_sha_chain_heal::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL);
    }

    int32 _bonusPct = 0;
};

// Water Shield (forever_classes): any hit taken uses a globe; the shaman's own heals only when they crit.
class spell_forever_sha_water_shield : public AuraScript
{
    PrepareAuraScript(spell_forever_sha_water_shield);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (eventInfo.GetTypeMask() & TAKEN_HIT_PROC_FLAG_MASK)
            return true;
        return (eventInfo.GetHitMask() & PROC_HIT_CRITICAL) != 0;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_sha_water_shield::CheckProc);
    }
};

// Windfury Totem aura (forever_classes clones): no proc for a player whose main hand carries Windfury Weapon (Forever
// 8232 text: "When applied to main hand, disables any benefit you personally receive from Windfury Totem"). The
// imbue is recognised by its enchant's equip spell: family Shaman, class mask word 2 0x100 (the Windfury Weapon passives).
class spell_forever_sha_windfury_totem : public AuraScript
{
    PrepareAuraScript(spell_forever_sha_windfury_totem);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Player* player = eventInfo.GetActor() ? eventInfo.GetActor()->ToPlayer() : nullptr;
        if (!player)
            return true;
        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (!weapon)
            return true;
        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT));
        if (!enchant)
            return true;
        for (uint8 i = 0; i < MAX_ITEM_ENCHANTMENT_EFFECTS; ++i)
        {
            if (enchant->Effect[i] != ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL)
                continue;
            SpellInfo const* equip = sSpellMgr->GetSpellInfo(enchant->EffectArg[i]);
            if (equip && equip->SpellFamilyName == SPELLFAMILY_SHAMAN && (equip->SpellFamilyFlags[2] & 0x100))
                return false;
        }
        return true;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_forever_sha_windfury_totem::CheckProc);
    }
};

// Fire Nova (forever_classes clones): needs the shaman's fire totem in range; the totem casts e0's TriggerSpell (the
// rank's nova, from data; stock spell_sha_fire_nova looks it up by stock rank chain).
class spell_forever_sha_fire_nova : public SpellScript
{
    PrepareSpellScript(spell_forever_sha_fire_nova);

    Creature* FireTotem() const
    {
        Unit* caster = GetCaster();
        Creature* totem = caster->GetMap()->GetCreature(caster->m_SummonSlot[SUMMON_SLOT_TOTEM_FIRE]);
        return totem && totem->IsTotem() ? totem : nullptr;
    }

    SpellCastResult CheckFireTotem()
    {
        Creature* totem = FireTotem();
        if (!totem)
        {
            SetCustomCastResultMessage(SPELL_CUSTOM_ERROR_MUST_HAVE_FIRE_TOTEM);
            return SPELL_FAILED_CUSTOM_ERROR;
        }
        // the cast targets the caster, so Spell::CheckRange never applies SPELLMOD_RANGE (Elemental Reach) to the
        // totem distance: apply it here
        float range = GetCaster()->GetSpellMaxRangeForTarget(totem, GetSpellInfo());
        if (Player* modOwner = GetCaster()->GetSpellModOwner())
            modOwner->ApplySpellMod(GetSpellInfo()->Id, SPELLMOD_RANGE, range, GetSpell());
        if (!GetCaster()->IsWithinDistInMap(totem, range))
            return SPELL_FAILED_OUT_OF_RANGE;
        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex effIndex)
    {
        if (Creature* totem = FireTotem())
            GetCaster()->CastSpell(totem, GetEffectInfo(effIndex).TriggerSpell, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_forever_sha_fire_nova::CheckFireTotem);
        OnEffectHitTarget += SpellEffectFn(spell_forever_sha_fire_nova::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Totemic Projection (forever_classes): moves the shaman's totems to the target point, keeping their spread (Forever's
// summoned projector does not exist in 3.3.5a).
class spell_forever_sha_totemic_projection : public SpellScript
{
    PrepareSpellScript(spell_forever_sha_totemic_projection);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        WorldLocation const* dest = GetExplTargetDest();
        if (!caster || !dest)
            return;

        std::vector<Creature*> totems;
        float cx = 0.f, cy = 0.f;
        for (uint8 slot = SUMMON_SLOT_TOTEM_FIRE; slot < MAX_TOTEM_SLOT; ++slot)
            if (Creature* totem = caster->GetMap()->GetCreature(caster->m_SummonSlot[slot]))
                if (totem->IsTotem())
                {
                    totems.push_back(totem);
                    cx += totem->GetPositionX();
                    cy += totem->GetPositionY();
                }
        if (totems.empty())
            return;
        cx /= totems.size();
        cy /= totems.size();
        for (Creature* totem : totems)
        {
            float x = dest->GetPositionX() + totem->GetPositionX() - cx;
            float y = dest->GetPositionY() + totem->GetPositionY() - cy;
            float z = dest->GetPositionZ();
            totem->UpdateAllowedPositionZ(x, y, z);
            totem->NearTeleportTo(x, y, z, totem->GetOrientation());
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_forever_sha_totemic_projection::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

void AddSC_forever_talents_shaman()
{
    RegisterSpellScript(spell_forever_sha_water_shield);
    RegisterSpellScript(spell_forever_sha_fire_nova);
    RegisterSpellScript(spell_forever_sha_totemic_projection);
    RegisterSpellScript(spell_forever_sha_improved_stormstrike);
    RegisterSpellScript(spell_forever_sha_chain_heal);
    RegisterSpellScript(spell_forever_sha_windfury_totem);
}
