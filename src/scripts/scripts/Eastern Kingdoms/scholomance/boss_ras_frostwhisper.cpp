/* 
 * Copyright (C) 2006-2008 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 * Copyright (C) 2008-2015 Hellground <http://hellground.net/>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* ScriptData
SDName: Boss_Ras_Frostwhisper
SD%Complete: 100
SDComment:
SDCategory: Scholomance
EndScriptData */

#include "precompiled.h"

#define SPELL_FROSTBOLT         21369
#define SPELL_ICEARMOR          18100                       //This is actually a buff he gives himself
#define SPELL_FREEZE            18763
#define SPELL_FEAR              26070
#define SPELL_CHILLNOVA         18099
#define SPELL_FROSTVOLLEY       8398

struct boss_rasfrostAI : public ScriptedAI
{
    boss_rasfrostAI(Creature *c) : ScriptedAI(c) {}

    int32 IceArmor_Timer;
    int32 Frostbolt_Timer;
    int32 Freeze_Timer;
    int32 Fear_Timer;
    int32 ChillNova_Timer;
    int32 FrostVolley_Timer;

    void Reset()
    {
        IceArmor_Timer = 2000;
        Frostbolt_Timer = 8000;
        ChillNova_Timer = 12000;
        Freeze_Timer = 18000;
        FrostVolley_Timer = 24000;
        Fear_Timer = 45000;

        m_creature->CastSpell(m_creature,SPELL_ICEARMOR,true);
    }

    void EnterCombat(Unit *who)
    {
    }

    void UpdateAI(const uint32 diff)
    {
        if (!UpdateVictim())
            return;

        IceArmor_Timer -= diff;
        if (IceArmor_Timer <= diff)
        {
            DoCast(m_creature, SPELL_ICEARMOR);
            IceArmor_Timer += 180000;
        }

        Frostbolt_Timer -= diff;
        if (Frostbolt_Timer <= diff)
        {
            Unit* target = NULL;
            target = SelectUnit(SELECT_TARGET_RANDOM,0, GetSpellMaxRange(SPELL_FROSTBOLT), true);
            if (target)
                DoCast(target,SPELL_FROSTBOLT);

            Frostbolt_Timer += 8000;
        }

        Freeze_Timer -= diff;
        if (Freeze_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_FREEZE);
            Freeze_Timer += 24000;
        }

        Fear_Timer -= diff;
        if (Fear_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_FEAR);
            Fear_Timer += 30000;
        }

        ChillNova_Timer -= diff;
        if (ChillNova_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_CHILLNOVA);
            ChillNova_Timer += 14000;
        }

        FrostVolley_Timer -= diff;
        if (FrostVolley_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_FROSTVOLLEY);
            FrostVolley_Timer += 15000;
        }

        DoMeleeAttackIfReady();
    }
};
CreatureAI* GetAI_boss_rasfrost(Creature *_Creature)
{
    return new boss_rasfrostAI (_Creature);
}

void AddSC_boss_rasfrost()
{
    Script *newscript;
    newscript = new Script;
    newscript->Name="boss_boss_ras_frostwhisper";
    newscript->GetAI = &GetAI_boss_rasfrost;
    newscript->RegisterSelf();
}

