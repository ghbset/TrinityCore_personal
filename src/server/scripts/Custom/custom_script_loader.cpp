/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// This is where scripts' loading functions should be declared:
void AddSC_Transmogrification();
void AddSC_setnewspawn_commandscript();
void AddSC_npc_forever_summon_hawk();
void AddSC_forever_talents_druid();
void AddSC_forever_talents_hunter();
void AddSC_forever_talents_mage();
void AddSC_forever_talents_paladin();
void AddSC_forever_talents_priest();
void AddSC_forever_talents_rogue();
void AddSC_forever_talents_shaman();
void AddSC_forever_talents_warlock();
void AddSC_forever_talents_warrior();
void AddSC_forever_racials();

// The name of this function should match:
// void Add${NameOfDirectory}Scripts()
void AddCustomScripts()
{
    AddSC_Transmogrification();
    AddSC_setnewspawn_commandscript();
    AddSC_npc_forever_summon_hawk();
    AddSC_forever_talents_druid();
    AddSC_forever_talents_hunter();
    AddSC_forever_talents_mage();
    AddSC_forever_talents_paladin();
    AddSC_forever_talents_priest();
    AddSC_forever_talents_rogue();
    AddSC_forever_talents_shaman();
    AddSC_forever_talents_warlock();
    AddSC_forever_talents_warrior();
    AddSC_forever_racials();
}
