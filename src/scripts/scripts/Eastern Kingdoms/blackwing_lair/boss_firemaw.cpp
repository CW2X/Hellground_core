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
SDName: Boss_Firemaw
SD%Complete: 100
SDComment:
SDCategory: Blackwing Lair
EndScriptData */

#include "precompiled.h"
#include "def_blackwing_lair.h"

#define SPELL_SHADOWFLAME       22539
#define SPELL_WINGBUFFET        23339
#define SPELL_FLAMEBUFFET       23341

struct boss_firemawAI : public ScriptedAI
{
    boss_firemawAI(Creature *c) : ScriptedAI(c)
    {
        pInstance = (ScriptedInstance*)c->GetInstanceData();
    }

    ScriptedInstance * pInstance;
    int32 ShadowFlame_Timer;
    int32 WingBuffet_Timer;
    int32 FlameBuffet_Timer;

    void Reset()
    {
        ShadowFlame_Timer = 30000;                          //These times are probably wrong
        WingBuffet_Timer = 24000;
        FlameBuffet_Timer = 5000;

        if (pInstance && pInstance->GetData(DATA_FIREMAW_EVENT) != DONE)
            pInstance->SetData(DATA_FIREMAW_EVENT, NOT_STARTED);
    }

    void EnterCombat(Unit *who)
    {
        DoZoneInCombat();

        if (pInstance)
            pInstance->SetData(DATA_FIREMAW_EVENT, IN_PROGRESS);
    }

    void JustDied(Unit * killer)
    {
        if (pInstance)
            pInstance->SetData(DATA_FIREMAW_EVENT, DONE);
    }

    void UpdateAI(const uint32 diff)
    {
        if (!UpdateVictim() )
            return;

        ShadowFlame_Timer -= diff;
        if (ShadowFlame_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_SHADOWFLAME);
            ShadowFlame_Timer += 15000 + rand()%3000;
        }

        WingBuffet_Timer -= diff;
        if (WingBuffet_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_WINGBUFFET);
            if(DoGetThreat(m_creature->GetVictim()))
                DoModifyThreatPercent(m_creature->GetVictim(),-75);

            WingBuffet_Timer += 25000;
        }

        FlameBuffet_Timer -= diff;
        if (FlameBuffet_Timer <= diff)
        {
            DoCast(m_creature->GetVictim(),SPELL_FLAMEBUFFET);
            FlameBuffet_Timer += 5000;
        }

        DoMeleeAttackIfReady();
    }
};
CreatureAI* GetAI_boss_firemaw(Creature *_Creature)
{
    return new boss_firemawAI (_Creature);
}

void AddSC_boss_firemaw()
{
    Script *newscript;
    newscript = new Script;
    newscript->Name="boss_firemaw";
    newscript->GetAI = &GetAI_boss_firemaw;
    newscript->RegisterSelf();
}

