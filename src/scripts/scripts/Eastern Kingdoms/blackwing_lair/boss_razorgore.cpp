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
SDName: Boss_Razorgore
SD%Complete: 50
SDComment: Needs additional review. Phase 1 NYI (Grethok the Controller)
SDCategory: Blackwing Lair
EndScriptData */

#include "precompiled.h"
#include "def_blackwing_lair.h"

//Razorgore Phase 2 Script

#define SAY_EGGS_BROKEN1        -1469022
#define SAY_EGGS_BROKEN2        -1469023
#define SAY_EGGS_BROKEN3        -1469024
#define SAY_DEATH               -1469025

#define SPELL_CLEAVE            22540
#define SPELL_WARSTOMP          24375
#define SPELL_FIREBALLVOLLEY    22425
#define SPELL_CONFLAGRATION     23023

struct boss_razorgoreAI : public ScriptedAI
{
    boss_razorgoreAI(Creature *c) : ScriptedAI(c)
    {
        pInstance = (ScriptedInstance*)c->GetInstanceData();
    }

    ScriptedInstance * pInstance;
    int32 Cleave_Timer;
    int32 WarStomp_Timer;
    int32 FireballVolley_Timer;
    int32 Conflagration_Timer;

    void Reset()
    {
        Cleave_Timer = 15000;                               //These times are probably wrong
        WarStomp_Timer = 35000;
        FireballVolley_Timer = 7000;
        Conflagration_Timer = 12000;

        if (pInstance && pInstance->GetData(DATA_RAZORGORE_THE_UNTAMED_EVENT) != DONE)
            pInstance->SetData(DATA_RAZORGORE_THE_UNTAMED_EVENT, NOT_STARTED);
    }

    void EnterCombat(Unit *who)
    {
        DoZoneInCombat();
        if (pInstance)
            pInstance->SetData(DATA_RAZORGORE_THE_UNTAMED_EVENT, IN_PROGRESS);
    }

    void JustDied(Unit* Killer)
    {
        DoScriptText(SAY_DEATH, m_creature);
        if (pInstance)
            pInstance->SetData(DATA_RAZORGORE_THE_UNTAMED_EVENT, DONE);
    }

    void UpdateAI(const uint32 diff)
    {
        if (!UpdateVictim() )
            return;

        Cleave_Timer -= diff;
        if (Cleave_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_CLEAVE);
            Cleave_Timer += 7000 + rand()%3000;
        }

        WarStomp_Timer -= diff;
        if (WarStomp_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_WARSTOMP);
            WarStomp_Timer += 15000 + rand()%10000;
        }

        FireballVolley_Timer -= diff;
        if (FireballVolley_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_FIREBALLVOLLEY);
            FireballVolley_Timer += 12000 + rand()%3000;
        }

        Conflagration_Timer -= diff;
        if (Conflagration_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_CONFLAGRATION);
            //We will remove this threat reduction and add an aura check.

            //if(DoGetThreat(m_creature->GetVictim()))
            //DoModifyThreatPercent(m_creature->GetVictim(),-50);

            Conflagration_Timer += 12000;
        }

        DoMeleeAttackIfReady();
    }
};

CreatureAI* GetAI_boss_razorgore(Creature *_Creature)
{
    return new boss_razorgoreAI (_Creature);
}

void AddSC_boss_razorgore()
{
    Script *newscript;
    newscript = new Script;
    newscript->Name="boss_razorgore";
    newscript->GetAI = &GetAI_boss_razorgore;
    newscript->RegisterSelf();
}

