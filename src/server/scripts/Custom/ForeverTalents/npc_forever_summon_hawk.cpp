/*
 * forever_talents: the hawk Summon Hawk leaves behind (modules/forever_classes/datascripts/classes/hunter/talents/beast_mastery/summon_hawk.ts).
 *
 * It is the hunter's guardian (ally summon: owned, no pet bar), and a guardian with the core's generic AI picks its
 * victim through its owner (Creature::SelectVictim) -- it drops a target the owner is not fighting at, and follows
 * the owner whenever that comes up empty. This AI never asks: it keeps its own target and never follows.
 *
 *   summoned      -> the enemy it appeared beside (Summon Hawk summons it within 2 yd of the dive's target)
 *   owner attacks -> that target instead
 *   every update  -> chase and melee the target, wherever the hunter is; when it is dead or gone, the hunter's
 *                    current enemy, else despawn
 *   evade         -> despawn (never walk back to the owner)
 *   summoned      -> the hunter's hawk modifiers (Ferocity crit, Unleashed Fury damage: spell mods on the summon
 *                    spell, which the core never applies to guardians) as its first creature spell, cast on itself
 */

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "ObjectAccessor.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    constexpr float FIND_TARGET_RANGE = 8.0f;   // the dive's target is within 2 yd of where the hawk appears

    struct npc_forever_summon_hawk : public ScriptedAI
    {
        explicit npc_forever_summon_hawk(Creature* creature) : ScriptedAI(creature) { }

        void IsSummonedBy(WorldObject* summoner) override
        {
            ApplyOwnerMods(summoner ? summoner->ToUnit() : nullptr);
            if (Unit* nearest = me->SelectNearestTarget(FIND_TARGET_RANGE))
                _target = nearest->GetGUID();
        }

        void ApplyOwnerMods(Unit* owner)
        {
            SpellInfo const* summon = sSpellMgr->GetSpellInfo(me->GetUInt32Value(UNIT_CREATED_BY_SPELL));
            if (!owner || !summon || !me->m_spells[0])
                return;
            int32 crit = 0, damage = 0;
            for (AuraType type : { SPELL_AURA_ADD_FLAT_MODIFIER, SPELL_AURA_ADD_PCT_MODIFIER })
                for (AuraEffect const* e : owner->GetAuraEffectsByType(type))
                {
                    if (!e->IsAffectingSpell(summon))
                        continue;
                    if (e->GetMiscValue() == SPELLMOD_CRITICAL_CHANCE)
                        crit += e->GetAmount();
                    else if (e->GetMiscValue() == SPELLMOD_DAMAGE)
                        damage += e->GetAmount();
                }
            if (!crit && !damage)
                return;
            CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
            args.AddSpellMod(SPELLVALUE_BASE_POINT0, crit);
            args.AddSpellMod(SPELLVALUE_BASE_POINT1, damage);
            me->CastSpell(me, me->m_spells[0], args);
        }

        void OwnerAttacked(Unit* target) override
        {
            if (Usable(target))
                _target = target->GetGUID();
        }

        void OwnerAttackedBy(Unit* /*attacker*/) override { }   // keep to our own target
        void MoveInLineOfSight(Unit* /*who*/) override { }       // no wandering off to passers-by
        void EnterEvadeMode(EvadeReason /*why*/) override { me->DespawnOrUnsummon(); }

        void UpdateAI(uint32 /*diff*/) override
        {
            Unit* target = ObjectAccessor::GetUnit(*me, _target);
            if (!Usable(target))
            {
                target = OwnersEnemy();
                if (!target)
                {
                    me->DespawnOrUnsummon();
                    return;
                }
                _target = target->GetGUID();
            }

            if (me->GetVictim() != target)
                AttackStart(target);            // ScriptedAI: Attack + chase

            DoMeleeAttackIfReady();
        }

    private:
        bool Usable(Unit* unit) const
        {
            return unit && unit->IsAlive() && unit->IsInWorld() && unit->GetMap() == me->GetMap()
                && me->IsValidAttackTarget(unit);
        }

        // The hunter's current enemy: what it is attacking, else what it has selected, if either is fair game.
        Unit* OwnersEnemy() const
        {
            TempSummon* summon = me->ToTempSummon();
            Unit* owner = summon ? summon->GetSummonerUnit() : nullptr;
            if (!owner)
                return nullptr;
            if (Usable(owner->GetVictim()))
                return owner->GetVictim();
            if (Player* player = owner->ToPlayer())
                if (Unit* selected = player->GetSelectedUnit())
                    if (Usable(selected) && selected->IsInCombat())
                        return selected;
            return nullptr;
        }

        ObjectGuid _target;
    };
}

void AddSC_npc_forever_summon_hawk()
{
    RegisterCreatureAI(npc_forever_summon_hawk);
}
