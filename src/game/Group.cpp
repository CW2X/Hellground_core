/*
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2008-2009 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2008-2017 Hellground <http://wow-hellground.com/>
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

#include "Common.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Group.h"
#include "Formulas.h"
#include "ObjectAccessor.h"
#include "BattleGround.h"
#include "MapManager.h"
#include "InstanceSaveMgr.h"
#include "Util.h"

Group::Group()
{
    m_leaderGuid        = 0;
    m_mainTank          = 0;
    m_mainAssistant     = 0;
    m_groupType         = (GroupType)0;
    m_bgGroup           = NULL;
    m_lootMethod        = (LootMethod)0;
    m_looterGuid        = 0;
    m_lootThreshold     = ITEM_QUALITY_UNCOMMON;
    m_subGroupsCounts   = NULL;
    m_leaderLogoutTime  = 0;

    for (int i = 0; i< TARGET_ICON_COUNT; i++)
        m_targetIcons[i] = 0;
}

Group::~Group()
{
    if (m_bgGroup)
    {
        sLog.outDebug("Group::~Group: battleground group being deleted.");
        if (m_bgGroup->GetBgRaid(ALLIANCE) == this)
            m_bgGroup->SetBgRaid(ALLIANCE, NULL);
        else if (m_bgGroup->GetBgRaid(HORDE) == this)
            m_bgGroup->SetBgRaid(HORDE, NULL);
        else
            sLog.outLog(LOG_DEFAULT, "ERROR: Group::~Group: battleground group is not linked to the correct battleground.");
    }

    // it is undefined whether objectmgr (which stores the groups) or instancesavemgr
    // will be unloaded first so we must be prepared for both cases
    // this may unload some instance saves
    for (uint8 i = 0; i < TOTAL_DIFFICULTIES; i++)
    {
        for (BoundInstancesMap::iterator itr2 = m_boundInstances[i].begin(); itr2 != m_boundInstances[i].end(); ++itr2)
            itr2->second.save->RemoveGroup(this);
    }

    // Sub group counters clean up
    if (m_subGroupsCounts)
        delete[] m_subGroupsCounts;
}

bool Group::Create(const uint64 &guid, const char * name, bool lfg)
{
    m_leaderGuid = guid;
    m_leaderName = name;

    m_groupType = isBGGroup() ? GROUPTYPE_RAID : GROUPTYPE_NORMAL;

    if (m_groupType == GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    m_lootMethod = GROUP_LOOT;
    m_lootThreshold = ITEM_QUALITY_UNCOMMON;
    m_looterGuid = guid;

    m_difficulty = DIFFICULTY_NORMAL;

    if (!isBGGroup())
    {
        Player *leader = sObjectMgr.GetPlayer(guid);
        if (leader)
            m_difficulty = leader->GetDifficulty();

        Player::ConvertInstancesToGroup(leader, this, guid);

        static SqlStatementID deleteGroupByLeader;
        static SqlStatementID deleteGroupMemberByLeader;
        static SqlStatementID insertGroup;

        // store group in database
        RealmDataDatabase.BeginTransaction();
        SqlStatement stmt = RealmDataDatabase.CreateStatement(deleteGroupByLeader, "DELETE FROM groups WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_leaderGuid));

        stmt = RealmDataDatabase.CreateStatement(deleteGroupMemberByLeader, "DELETE FROM group_member WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_leaderGuid));

        stmt = RealmDataDatabase.CreateStatement(insertGroup, "INSERT INTO groups (leaderGuid, mainTank, mainAssistant, lootMethod, looterGuid, lootThreshold, icon1, icon2, icon3, icon4, icon5, icon6, icon7, icon8, isRaid, difficulty) "
                                                    "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");

        stmt.addUInt32(GUID_LOPART(m_leaderGuid));
        stmt.addUInt32(GUID_LOPART(m_mainTank));
        stmt.addUInt32(GUID_LOPART(m_mainAssistant));
        stmt.addUInt32(uint32(m_lootMethod));
        stmt.addUInt32(GUID_LOPART(m_looterGuid));
        stmt.addUInt32(uint32(m_lootThreshold));
        stmt.addUInt64(m_targetIcons[0]);
        stmt.addUInt64(m_targetIcons[1]);
        stmt.addUInt64(m_targetIcons[2]);
        stmt.addUInt64(m_targetIcons[3]);
        stmt.addUInt64(m_targetIcons[4]);
        stmt.addUInt64(m_targetIcons[5]);
        stmt.addUInt64(m_targetIcons[6]);
        stmt.addUInt64(m_targetIcons[7]);
        stmt.addBool(isRaidGroup());
        stmt.addUInt8(m_difficulty);
        stmt.Execute();
    }

    if (!AddMember(guid, name, lfg))
        return false;

    if (!isBGGroup())
        RealmDataDatabase.CommitTransaction();

    return true;
}

bool Group::LoadGroupFromDB(const uint64 &leaderGuid, QueryResultAutoPtr result, bool loadMembers)
{
    if (isBGGroup())
        return false;

    bool external = true;
    if (!result)
    {
        external = false;
        //                                       0          1              2           3           4              5      6      7      8      9      10     11     12     13      14
        result = RealmDataDatabase.PQuery("SELECT mainTank, mainAssistant, lootMethod, looterGuid, lootThreshold, icon1, icon2, icon3, icon4, icon5, icon6, icon7, icon8, isRaid, difficulty FROM groups WHERE leaderGuid ='%u'", GUID_LOPART(leaderGuid));
        if (!result)
            return false;
    }

    m_leaderGuid = leaderGuid;
    m_leaderLogoutTime = time(NULL); // Give the leader a chance to keep his position after a server crash

    // group leader not exist
    if (!sObjectMgr.GetPlayerNameByGUID(m_leaderGuid, m_leaderName))
        return false;
    m_groupType  = (*result)[13].GetBool() ? GROUPTYPE_RAID : GROUPTYPE_NORMAL;

    if (m_groupType == GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    m_difficulty = (*result)[14].GetUInt8();
    m_mainTank = (*result)[0].GetUInt64();
    m_mainAssistant = (*result)[1].GetUInt64();
    m_lootMethod = (LootMethod)(*result)[2].GetUInt8();
    m_looterGuid = MAKE_NEW_GUID((*result)[3].GetUInt32(), 0, HIGHGUID_PLAYER);
    m_lootThreshold = (ItemQualities)(*result)[4].GetUInt16();

    for (int i=0; i < TARGET_ICON_COUNT; i++)
        m_targetIcons[i] = (*result)[5+i].GetUInt64();

    if (loadMembers)
    {
        result = RealmDataDatabase.PQuery("SELECT memberGuid, assistant, subgroup FROM group_member WHERE leaderGuid ='%u'", GUID_LOPART(leaderGuid));
        if (!result)
            return false;

        do
        {
            LoadMemberFromDB((*result)[0].GetUInt32(), (*result)[2].GetUInt8(), (*result)[1].GetBool());
        } while (result->NextRow());

        // group too small
        if (GetMembersCount() < 2)
            return false;
    }
    return true;
}

bool Group::LoadMemberFromDB(uint32 guidLow, uint8 subgroup, bool assistant)
{
    GroupMemberSlot member;
    member.guid      = MAKE_NEW_GUID(guidLow, 0, HIGHGUID_PLAYER);

    // skip non-existed member
    if (!sObjectMgr.GetPlayerNameByGUID(member.guid, member.name))
        return false;

    member.group     = subgroup;
    member.assistant = assistant;
    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(subgroup);

    return true;
}

void Group::ConvertToRaid()
{
    m_groupType = GROUPTYPE_RAID;

    _initRaidSubGroupsCounter();

    static SqlStatementID convertToRaid;

    if (!isBGGroup())
    {
        SqlStatement stmt = RealmDataDatabase.CreateStatement(convertToRaid, "UPDATE groups SET isRaid = 1 WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_leaderGuid));
    }

    SendUpdate();

    // update quest related GO states (quest activity dependent from raid membership)
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
        if (Player* player = sObjectMgr.GetPlayer(citr->guid))
            player->UpdateForQuestsGO();
}

bool Group::AddInvite(Player *player)
{
    if (!player || player->GetGroupInvite())
        return false;

    Group* group = player->GetGroup();
    if (group && group->isBGGroup())
        group = player->GetOriginalGroup();

    if (group)
        return false;

    RemoveInvite(player);

    m_invitees.insert(player);

    player->SetGroupInvite(this);

    return true;
}

bool Group::AddLeaderInvite(Player *player)
{
    if (!AddInvite(player))
        return false;

    m_leaderGuid = player->GetGUID();
    m_leaderName = player->GetName();
    return true;
}

uint32 Group::RemoveInvite(Player *player)
{
    m_invitees.erase(player);

    player->SetGroupInvite(NULL);
    return GetMembersCount();
}

void Group::RemoveAllInvites()
{
    for (InvitesList::iterator itr=m_invitees.begin(); itr!=m_invitees.end(); ++itr)
    {
        if ((*itr)->GetTypeId() == TYPEID_PLAYER)
            (*itr)->SetGroupInvite(NULL);
    }

    m_invitees.clear();
}

Player* Group::GetInvited(const uint64& guid) const
{
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        if ((*itr)->GetGUID() == guid)
            return (*itr);
    }
    return NULL;
}

Player* Group::GetInvited(const std::string& name) const
{
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        if ((*itr)->GetName() == name)
            return (*itr);
    }
    return NULL;
}

bool Group::AddMember(const uint64 &guid, const char* name, bool lfg)
{
    if (IsMember(guid) || !_addMember(guid, name))
        return false;

    SendUpdate();

    Player *player = sObjectMgr.GetPlayer(guid);
    if (player)
    {
        if (!IsLeader(player->GetGUID()) && !isBGGroup())
        {
            // reset the new member's instances, unless he is currently in one of them
            // including raid/heroic instances that they are not permanently bound to!
            player->ResetInstances(INSTANCE_RESET_GROUP_JOIN);

            if (player->GetLevel() >= LEVELREQUIREMENT_HEROIC && player->GetDifficulty() != GetDifficulty())
            {
                player->SetDifficulty(m_difficulty);
                player->SendDungeonDifficulty(true);
            }
        }
        player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);
        UpdatePlayerOutOfRange(player);

        // quest related GO state dependent from raid memebership
        if (isRaidGroup())
            player->UpdateForQuestsGO();

        // if it's not from autoadd/autojoin - prevent core freeze
        if (!lfg)
        {
            player->ClearLFG();
            player->ClearLFM();
        }
    }

    return true;
}

uint32 Group::RemoveMember(const uint64 &guid, const uint8 &method)
{
    BroadcastGroupUpdate();

    // remove member and change leader (if need) only if strong more 2 members _before_ member remove
    if (GetMembersCount() > (isBGGroup() ? 1 : 2))           // in BG group case allow 1 members group
    {
        bool leaderChanged = _removeMember(guid);

        if (Player *player = sObjectMgr.GetPlayer(guid))
        {
            // quest related GO state dependent from raid membership
            if (isRaidGroup())
                player->UpdateForQuestsGO();

            WorldPacket data;

            if (method == 1)
            {
                data.Initialize(SMSG_GROUP_UNINVITE, 0);
                player->SendPacketToSelf(&data);
            }

            if (Group* group = player->GetGroup())
                group->SendUpdate();
            else
            {
                data.Initialize(SMSG_GROUP_LIST, 24);
                data << uint64(0) << uint64(0) << uint64(0);
                player->SendPacketToSelf(&data);
            }

            _homebindIfInstance(player);
        }

        if (leaderChanged)
        {
            WorldPacket data(SMSG_GROUP_SET_LEADER, (m_memberSlots.front().name.size()+1));
            data << m_memberSlots.front().name;
            BroadcastPacket(&data, false);
        }

        SendUpdate();
    }
    // if group before remove <= 2 disband it
    else
        Disband(true);

    return m_memberSlots.size();
}

void Group::ChangeLeader(const uint64 &guid)
{
    member_citerator slot = _getMemberCSlot(guid);

    if (slot==m_memberSlots.end())
        return;

    Player * plr = ObjectAccessor::GetPlayer(GetLeaderGUID());
    if (plr)
        plr->ClearLFM();

    plr = ObjectAccessor::GetPlayer(guid);
    if (plr)
        plr->ClearLFM();

    _setLeader(guid);

    WorldPacket data(SMSG_GROUP_SET_LEADER, slot->name.size()+1);
    data << slot->name;
    BroadcastPacket(&data, false);
    SendUpdate();
}

void Group::CheckLeader(const uint64 &guid, bool isLogout)
{

    if (IsLeader(guid))
    {
        if (isLogout)
        {
            m_leaderLogoutTime = time(NULL);
        }
        else
        {
            m_leaderLogoutTime = 0;
        }
    }
    else
    {
        if (!isLogout && !m_leaderLogoutTime) //normal member logins
        {
            Player *leader = NULL;

            //find the leader from group members
            for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
            {
                if (itr->getSource()->GetGUID() == m_leaderGuid)
                {
                    leader = itr->getSource();
                    break;
                }
            }

            if (!leader || !leader->IsInWorld())
            {
                m_leaderLogoutTime = time(NULL);
            }
        }
    }
}

bool Group::ChangeLeaderToFirstOnlineMember()
{

    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();

       if (player && player->IsInWorld() && player->GetGUID() != m_leaderGuid)
       {
        ChangeLeader(player->GetGUID());
        return true;
       }
    }
    return false;

}

void Group::Disband(bool hideDestroy)
{
    Player *player;

    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        player = sObjectMgr.GetPlayer(citr->guid);
        if (!player)
            continue;

        //we cannot call _removeMember because it would invalidate member iterator
        //if we are removing player from battleground raid
        if (isBGGroup())
            player->RemoveFromBattleGroundRaid();
        else
        {
            //we can remove player who is in battleground from his original group
            if (player->GetOriginalGroup() == this)
                player->SetOriginalGroup(NULL);
            else
                player->SetGroup(NULL);
        }

        if (isRaidGroup())
            player->UpdateForQuestsGO();


        if (!player->GetSession())
            continue;

        WorldPacket data;
        if (!hideDestroy)
        {
            data.Initialize(SMSG_GROUP_DESTROYED, 0);
            player->SendPacketToSelf(&data);
        }

        if (Group* group = player->GetGroup())
            group->SendUpdate();
        else
        {
            data.Initialize(SMSG_GROUP_LIST, 24);
            data << uint64(0) << uint64(0) << uint64(0);
            player->SendPacketToSelf(&data);
        }

        _homebindIfInstance(player);
    }
    m_memberSlots.clear();

    RemoveAllInvites();

    if (!isBGGroup())
    {
        static SqlStatementID deleteGroupByLeader;
        static SqlStatementID deleteGroupMemberByLeader;

        RealmDataDatabase.BeginTransaction();
        SqlStatement stmt = RealmDataDatabase.CreateStatement(deleteGroupByLeader, "DELETE FROM groups WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_leaderGuid));

        stmt = RealmDataDatabase.CreateStatement(deleteGroupMemberByLeader, "DELETE FROM group_member WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_leaderGuid));
        RealmDataDatabase.CommitTransaction();

        ResetInstances(INSTANCE_RESET_GROUP_DISBAND, NULL);
    }

    m_leaderGuid = 0;
    m_leaderName = "";
}

/*********************************************************/
/***                   LOOT SYSTEM                     ***/
/*********************************************************/

void Group::PrepareLootRolls(const uint64& playerGUID, Loot *loot, WorldObject* object)
{
    std::vector<LootItem>::iterator i;

    if (m_lootMethod == MASTER_LOOT)
    {
        for (i = loot->items.begin(); i != loot->items.end(); ++i)
        {
            if (!i->freeforall && !i->conditionId)
                i->is_blocked = true; // lock all items, do not allow looting by simply leaving party
        }
    }

    if (!IsRollLootType())
        return;

    
    ItemPrototype const *item;
    uint8 itemSlot = 0;
    Player *player = sObjectMgr.GetPlayer(playerGUID);
    Group *group = player->GetGroup();

    for (i=loot->items.begin(); i != loot->items.end(); ++i, ++itemSlot)
    {
        item = ObjectMgr::GetItemPrototype(i->itemid);
        if (!item)
        {
            //sLog.outDebug("Group::GroupLoot: missing item prototype for item with id: %d", i->itemid);
            continue;
        }

        //roll for over-threshold item if it's one-player loot
        if (item->Quality >= uint32(m_lootThreshold) && !i->freeforall)
        {
            uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr.GenerateLowGuid(HIGHGUID_ITEM),0,HIGHGUID_ITEM);
            Roll* r=new Roll(newitemGUID,*i);

            //a vector is filled with only near party members
            for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player *member = itr->getSource();
                if (!member || !member->GetSession())
                    continue;


                if (i->AllowedForPlayer(member) && loot->IsPlayerAllowedToLoot(member, object)
                    && (m_lootMethod != NEED_BEFORE_GREED || member->CanUseItem(item)))
                {
                    r->playerVote[member->GetGUID()] = NOT_EMITED_YET;
                    ++r->totalPlayersRolling;
                }
            }

            if (r->totalPlayersRolling > 1)
            {
                r->setLoot(loot);
                r->itemSlot = itemSlot;
                r->rollTimer = 60000;
                if (object->ToCreature())
                    object->ToCreature()->UpdateDeathTimer(70);
                r->SendLootStartRoll(60000);
                i->is_blocked = true;

                sObjectMgr.AddRoll(r);
            }
            else
                delete r;
        }
    }
}

void Group::SendRoundRobin(Loot* loot, WorldObject* object)
{
    if (!IsRoundRobinLootType())
        return;

    WorldPacket data(SMSG_LOOT_LIST, (8+8));
    data << uint64(object->GetGUID());
    data << uint8(0); // 0 - owner, 1 - master looter (not used yet)

    if (Unit* looter = object->GetMap()->GetUnit(loot->looterGUID))
        data << looter->GetPackGUID();
    else
        data << uint8(0);

    BroadcastPacket(&data, false);

    object->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
}

void Group::SendMasterLoot(Loot* loot, WorldObject* object)
{
    uint32 real_count = 0;

    WorldPacket data(SMSG_LOOT_MASTER_LIST, 330);
    data << (uint8)GetMembersCount();

    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *looter = itr->getSource();
        if (!looter->IsInWorld())
            continue;

        if (loot->IsPlayerAllowedToLoot(looter, object))
        {
            data << looter->GetGUID();
            ++real_count;
        }
    }

    data.put<uint8>(0,real_count);

    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *looter = itr->getSource();
        if (loot->IsPlayerAllowedToLoot(looter, object))
            looter->SendPacketToSelf(&data);
    }
}

void Group::SetTargetIcon(uint8 id, uint64 guid)
{
    if (id >= TARGET_ICON_COUNT)
        return;

    // clean other icons
    if (guid != 0)
        for (int i=0; i<TARGET_ICON_COUNT; i++)
            if (m_targetIcons[i] == guid)
                SetTargetIcon(i, 0);

    m_targetIcons[id] = guid;

    WorldPacket data(MSG_RAID_TARGET_UPDATE, (2+8));
    data << (uint8)0;
    data << id;
    data << guid;
    BroadcastPacket(&data, true);
}

void Group::GetDataForXPAtKill(Unit const* victim, uint32& count,uint32& sum_level, Player* & member_with_max_level, Player* & not_gray_member_with_max_level)
{
    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->IsAlive())                   // only for alive
            continue;

        if (!member->IsAtGroupRewardDistance(victim))        // at req. distance
            continue;

        ++count;
        sum_level += member->GetLevel();
        // store maximum member level
        if (!member_with_max_level || member_with_max_level->GetLevel() < member->GetLevel())
            member_with_max_level = member;

        uint32 gray_level = Hellground::XP::GetGrayLevel(member->GetLevel());
        // if the victim is higher level than the gray level of the currently examined group member,
        // then set not_gray_member_with_max_level if needed.
        if (victim->GetLevel() > gray_level && (!not_gray_member_with_max_level
           || not_gray_member_with_max_level->GetLevel() < member->GetLevel()))
            not_gray_member_with_max_level = member;
    }
}

void Group::SendTargetIconList(WorldSession *session)
{
    if (!session)
        return;

    WorldPacket data(MSG_RAID_TARGET_UPDATE, (1+TARGET_ICON_COUNT*9));
    data << (uint8)1;

    for (int i=0; i<TARGET_ICON_COUNT; i++)
    {
        if (m_targetIcons[i] == 0)
            continue;

        data << (uint8)i;
        data << m_targetIcons[i];
    }

    session->SendPacket(&data);
}

uint8 Group::GetMemberFlags(member_citerator &citr) const
{
    uint8 flags = citr->assistant ? MEMBER_FLAG_ASSISTANT : MEMBER_FLAG_NONE;

    if (this->m_mainTank == citr->guid)
        flags |= MEMBER_FLAG_MAINTANK;

    if (this->m_mainAssistant == citr->guid)
        flags |= MEMBER_FLAG_MAINASSIST;

    return flags;
}

void Group::SendUpdate()
{
    Player *player;

    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        player = sObjectMgr.GetPlayer(citr->guid);
        if (!player || !player->GetSession() || player->GetGroup() != this)
            continue;

                                                            // guess size
        WorldPacket data(SMSG_GROUP_LIST, (1+1+1+1+8+4+GetMembersCount()*20));
        data << (uint8)m_groupType;                         // group type
        data << (uint8)(isBGGroup() ? 1 : 0);               // 2.0.x, isBattleGroundGroup?
        data << (uint8)(citr->group);                       // groupid
        data << (uint8)GetMemberFlags(citr);                // flags
        data << uint64(0x50000000FFFFFFFELL);               // related to voice chat?
        data << uint32(GetMembersCount()-1);
        for (member_citerator citr2 = m_memberSlots.begin(); citr2 != m_memberSlots.end(); ++citr2)
        {
            if (citr->guid == citr2->guid)
                continue;

            Player* member = ObjectAccessor::GetPlayer(citr2->guid);
            uint8 onlineState = (member) ? MEMBER_STATUS_ONLINE : MEMBER_STATUS_OFFLINE;
            onlineState = onlineState | (isBGGroup() ? MEMBER_STATUS_PVP : 0);

            data << citr2->name;
            data << (uint64)citr2->guid;
            data << (uint8)(onlineState);                   // online-state
            data << (uint8)(citr2->group);                  // groupid
            data << (uint8)GetMemberFlags(citr2);           // flags
        }

        data << uint64(m_leaderGuid);                       // leader guid
        if (GetMembersCount()-1)
        {
            data << (uint8)m_lootMethod;                    // loot method
            data << (uint64)m_looterGuid;                   // looter guid
            data << (uint8)m_lootThreshold;                 // loot threshold
            data << (uint8)m_difficulty;                    // Heroic Mod Group

        }
        player->SendPacketToSelf(&data);
        //when player is loading we need a stats update
        if (player->GetSession()->PlayerLoading())
        {
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_HP|GROUP_UPDATE_FLAG_MAX_POWER|GROUP_UPDATE_FLAG_LEVEL);
            UpdatePlayerOutOfRange(player);
        }
    }
}

// Automatic Update by World thread
void Group::Update(uint32 diff)
{
    if (m_leaderLogoutTime)
    {
        time_t thisTime = time(NULL);

        if (thisTime > m_leaderLogoutTime + sWorld.getConfig(CONFIG_GROUPLEADER_RECONNECT_PERIOD))
        {
            ChangeLeaderToFirstOnlineMember();
            m_leaderLogoutTime = 0;
        }
    }
}

void Group::UpdatePlayerOutOfRange(Player* pPlayer)
{
    if (!pPlayer || !pPlayer->IsInWorld())
        return;

    if (pPlayer->GetGroupUpdateFlag() == GROUP_UPDATE_FLAG_NONE)
        return;

    Player *player;
    WorldPacket data;
    pPlayer->GetSession()->BuildPartyMemberStatsChangedPacket(pPlayer, &data);

    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        player = itr->getSource();
        if (player && player != pPlayer && !player->HaveAtClient(pPlayer))
            player->SendPacketToSelf(&data);
    }
}

void Group::BroadcastPacket(WorldPacket *packet, bool ignorePlayersInBGRaid, int group, uint64 ignore)
{
    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *pl = itr->getSource();
        if (!pl || (ignore != 0 && pl->GetGUID() == ignore))
            continue;

        if (pl->GetSession() && (group==-1 || itr->getSubGroup()==group))
            pl->SendPacketToSelf(packet);
    }
}

void Group::BroadcastReadyCheck(WorldPacket *packet)
{
    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *pl = itr->getSource();
        if (pl && pl->GetSession())
            if (IsLeader(pl->GetGUID()) || IsAssistant(pl->GetGUID()))
                pl->SendPacketToSelf(packet);
    }
}

void Group::OfflineReadyCheck()
{
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player *pl = sObjectMgr.GetPlayer(citr->guid);
        if (!pl || !pl->GetSession())
        {
            WorldPacket data(MSG_RAID_READY_CHECK_CONFIRM, 9);
            data << citr->guid;
            data << (uint8)0;
            BroadcastReadyCheck(&data);
        }
    }
}

bool Group::_addMember(const uint64 &guid, const char* name, bool isAssistant)
{
    // get first not-full group
    uint8 groupid = 0;
    if (m_subGroupsCounts)
    {
        bool groupFound = false;
        for (; groupid < MAXRAIDSIZE/MAXGROUPSIZE; ++groupid)
        {
            if (m_subGroupsCounts[groupid] < MAXGROUPSIZE)
            {
                groupFound = true;
                break;
            }
        }
        // We are raid group and no one slot is free
        if (!groupFound)
                return false;
    }

    return _addMember(guid, name, isAssistant, groupid);
}

bool Group::_addMember(const uint64 &guid, const char* name, bool isAssistant, uint8 group)
{
    if (IsFull())
        return false;

    if (!guid)
        return false;

    Player *player = sObjectMgr.GetPlayer(guid);

    GroupMemberSlot member;
    member.guid      = guid;
    member.name      = name;
    member.group     = group;
    member.assistant = isAssistant;
    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(group);

    if (player)
    {
        player->SetGroupInvite(NULL);

        //if player is in group and he is being added to BG raid group, then call SetBattleGroundRaid()
        if (player->GetGroup() && isBGGroup())
            player->SetBattleGroundRaid(this, group);
        //if player is in bg raid and we are adding him to normal group, then call SetOriginalGroup()
        else if (player->GetGroup())
            player->SetOriginalGroup(this, group);
        //if player is not in group, then call set group
        else
            player->SetGroup(this, group);

        // if the same group invites the player back, cancel the homebind timer
        InstanceGroupBind *bind = GetBoundInstance(player->GetMapId(), player->GetDifficulty());
        if (bind && bind->save->GetInstanceId() == player->GetInstanceId())
            player->m_InstanceValid = true;
    }

    if (!isRaidGroup())                                      // reset targetIcons for non-raid-groups
    {
        for (int i=0; i<TARGET_ICON_COUNT; i++)
            m_targetIcons[i] = 0;
    }

    if (!isBGGroup())
    {
        static SqlStatementID insertGroupMember;
        // insert into group table
        SqlStatement stmt = RealmDataDatabase.CreateStatement(insertGroupMember, "INSERT INTO group_member (leaderGuid, memberGuid, assistant, subgroup) VALUES (?, ?, ?, ?)");
        stmt.addUInt32(GUID_LOPART(m_leaderGuid));
        stmt.addUInt32(GUID_LOPART(member.guid));
        stmt.addBool(member.assistant);
        stmt.addUInt8(member.group);
        stmt.Execute();
    }

    return true;
}

bool Group::_removeMember(const uint64 &guid)
{
    Player *player = sObjectMgr.GetPlayer(guid);
    if (player)
    {
        //if we are removing player from battleground raid
        if (isBGGroup())
            player->RemoveFromBattleGroundRaid();
        else
        {
            //we can remove player who is in battleground from his original group
            if (player->GetOriginalGroup() == this)
                player->SetOriginalGroup(NULL);
            else
                player->SetGroup(NULL);
        }
    }

    member_witerator slot = _getMemberWSlot(guid);
    if (slot != m_memberSlots.end())
    {
        SubGroupCounterDecrease(slot->group);

        m_memberSlots.erase(slot);
    }

    static SqlStatementID deleteGroupMember;

    if (!isBGGroup())
    {
        SqlStatement stmt = RealmDataDatabase.CreateStatement(deleteGroupMember, "DELETE FROM group_member WHERE memberGuid = ?");
        stmt.PExecute(GUID_LOPART(guid));
    }

    if (m_leaderGuid == guid)                                // leader was removed
    {
        if (GetMembersCount() > 0)
            _setLeader(m_memberSlots.front().guid);
        return true;
    }

    return false;
}

void Group::_setLeader(const uint64 &guid)
{
    member_citerator slot = _getMemberCSlot(guid);
    if (slot==m_memberSlots.end())
        return;

    if (!isBGGroup())
    {
        // TODO: set a time limit to have this function run rarely cause it can be slow
        RealmDataDatabase.BeginTransaction();

        // update the group's bound instances when changing leaders

        // remove all permanent binds from the group
        // in the DB also remove solo binds that will be replaced with permbinds
        // from the new leader
        RealmDataDatabase.PExecute(
            "DELETE FROM group_instance WHERE leaderguid='%u' AND (permanent = 1 OR "
            "instance IN (SELECT instance FROM character_instance WHERE guid = '%u')"
            ")", GUID_LOPART(m_leaderGuid), GUID_LOPART(slot->guid)
       );

        Player *player = sObjectMgr.GetPlayer(slot->guid);
        if (player)
        {
            for (uint8 i = 0; i < TOTAL_DIFFICULTIES; i++)
            {
                for (BoundInstancesMap::iterator itr = m_boundInstances[i].begin(); itr != m_boundInstances[i].end();)
                {
                    if (itr->second.perm)
                    {
                        itr->second.save->RemoveGroup(this);
                        m_boundInstances[i].erase(itr++);
                    }
                    else
                        ++itr;
                }
            }
        }

        static SqlStatementID updateBindLeader;
        static SqlStatementID updateGroupLeader;
        static SqlStatementID updateMemberLeader;

        // update the group's solo binds to the new leader
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateBindLeader, "UPDATE group_instance SET leaderGuid = ? WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(slot->guid), GUID_LOPART(m_leaderGuid));

        // copy the permanent binds from the new leader to the group
        // overwriting the solo binds with permanent ones if necessary
        // in the DB those have been deleted already
        Player::ConvertInstancesToGroup(player, this, slot->guid);

        // update the group leader
        stmt = RealmDataDatabase.CreateStatement(updateGroupLeader, "UPDATE groups SET leaderGuid = ? WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(slot->guid), GUID_LOPART(m_leaderGuid));

        stmt = RealmDataDatabase.CreateStatement(updateMemberLeader, "UPDATE group_member SET leaderGuid = ? WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(slot->guid), GUID_LOPART(m_leaderGuid));
        RealmDataDatabase.CommitTransaction();
    }

    m_leaderGuid = slot->guid;
    m_leaderName = slot->name;
}

bool Group::_setMembersGroup(const uint64 &guid, const uint8 &group)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot==m_memberSlots.end())
        return false;

    slot->group = group;

    SubGroupCounterIncrease(group);

    static SqlStatementID updateSubGroup;
    if (!isBGGroup())
    {
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateSubGroup, "UPDATE group_member SET subgroup = ? WHERE memberGuid = ?");
        stmt.PExecute(group, GUID_LOPART(guid));
    }

    return true;
}

bool Group::_setAssistantFlag(const uint64 &guid, const bool &state)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot==m_memberSlots.end())
        return false;

    static SqlStatementID updateMemberAssistant;

    slot->assistant = state;
    if (!isBGGroup())
    {
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateMemberAssistant, "UPDATE group_member SET assistant = ? WHERE memberGuid = ?");
        stmt.PExecute(state, GUID_LOPART(guid));
    }

    return true;
}

bool Group::_setMainTank(const uint64 &guid)
{
    if (guid)
    {
        member_citerator slot = _getMemberCSlot(guid);

        if (slot == m_memberSlots.end())
            return false;

        if (m_mainAssistant == guid)
            _setMainAssistant(0);
    }

    m_mainTank = guid;

    if (!isBGGroup())
    {
        static SqlStatementID updateMainTank;
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateMainTank, "UPDATE groups SET mainTank = ? WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_mainTank), GUID_LOPART(m_leaderGuid));
    }

    return true;
}

bool Group::_setMainAssistant(const uint64 &guid)
{
    if (guid)
    {
        member_witerator slot = _getMemberWSlot(guid);

        if (slot == m_memberSlots.end())
            return false;

        if (m_mainTank == guid)
            _setMainTank(0);
    }

    m_mainAssistant = guid;

    if (!isBGGroup())
    {
        static SqlStatementID updateMainAssist;
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateMainAssist, "UPDATE groups SET mainAssistant = ? WHERE leaderGuid = ?");
        stmt.PExecute(GUID_LOPART(m_mainAssistant), GUID_LOPART(m_leaderGuid));
    }

    return true;
}

bool Group::SameSubGroup(Player const* member1, Player const* member2) const
{
    if (!member1 || !member2) return false;
    if (member1->GetGroup() != this || member2->GetGroup() != this) return false;
    else return member1->GetSubGroup() == member2->GetSubGroup();
}

// allows setting subgroup for offline members
void Group::ChangeMembersGroup(const uint64 &guid, const uint8 &group)
{
    if (!isRaidGroup())
        return;
    Player *player = sObjectMgr.GetPlayer(guid);

    if (!player)
    {
        uint8 prevSubGroup = GetMemberGroup(guid);
        if (prevSubGroup == group)
            return;

        if (_setMembersGroup(guid, group))
        {
            SubGroupCounterDecrease(prevSubGroup);
            SendUpdate();
        }
    }
    else
        // This methods handles itself groupcounter decrease
        ChangeMembersGroup(player, group);
}

// only for online members
void Group::ChangeMembersGroup(Player *player, const uint8 &group)
{
    if (!player || !isRaidGroup())
        return;

    uint8 prevSubGroup = player->GetSubGroup();
    if (prevSubGroup == group)
        return;

    if (_setMembersGroup(player->GetGUID(), group))
    {
        if (player->GetGroup() == this)
            player->GetGroupRef().setSubGroup(group);
        //if player is in BG raid, it is possible that he is also in normal raid - and that normal raid is stored in m_originalGroup reference
        else
        {
            prevSubGroup = player->GetOriginalSubGroup();
            player->GetOriginalGroupRef().setSubGroup(group);
        }

        SubGroupCounterDecrease(prevSubGroup);
        SendUpdate();
    }
}

void Group::UpdateLooterGuid(WorldObject* object, bool ifneed)
{
    if(!IsRoundRobinLootType())
        return;

    member_citerator guid_itr = _getMemberCSlot(GetLooterGuid());
    if (guid_itr != m_memberSlots.end())
    {
        if (ifneed)
        {
            // not update if only update if need and ok
            Player* looter = ObjectAccessor::FindPlayer(guid_itr->guid);
            if (looter && looter->IsWithinDist(object, sWorld.getConfig(CONFIG_GROUP_XP_DISTANCE), false))
                return;
        }
        ++guid_itr;
    }

    // search next after current
    if (guid_itr != m_memberSlots.end())
    {
        for (member_citerator itr = guid_itr; itr != m_memberSlots.end(); ++itr)
        {
            if (Player* pl = ObjectAccessor::FindPlayer(itr->guid))
            {
                if (pl->IsWithinDist(object, sWorld.getConfig(CONFIG_GROUP_XP_DISTANCE), false))
                {
                    SetLooterGuid(pl->GetGUID());
                    SendUpdate();
                    return;
                }
            }
        }
    }

    // search from start
    for (member_citerator itr = m_memberSlots.begin(); itr != guid_itr; ++itr)
    {
        if (Player* pl = ObjectAccessor::FindPlayer(itr->guid))
        {
            if (pl->IsWithinDist(object, sWorld.getConfig(CONFIG_GROUP_XP_DISTANCE), false))
            {
                SetLooterGuid(pl->GetGUID());
                SendUpdate();
                return;
            }
        }
    }

    SetLooterGuid(0);
    SendUpdate();
}

uint32 Group::CanJoinBattleGroundQueue(BattleGroundTypeId bgTypeId, BattleGroundQueueTypeId bgQueueTypeId, uint32 MinPlayerCount, uint32 MaxPlayerCount, bool isRated, uint32 arenaSlot)
{
    // check for min / max count
    uint32 memberscount = GetMembersCount();
    uint32 onlinememberscount = 0;
    if (memberscount < MinPlayerCount)
        return BG_JOIN_ERR_GROUP_NOT_ENOUGH;
    if (memberscount > MaxPlayerCount)
        return BG_JOIN_ERR_GROUP_TOO_MANY;

    // get a player as reference, to compare other players' stats to (arena team id, queue id based on level, etc.)
    Player * reference = GetFirstMember()->getSource();
    // no reference found, can't join this way
    if (!reference)
        return BG_JOIN_ERR_OFFLINE_MEMBER;

    BattleGroundBracketId bracket_id = reference->GetBattleGroundBracketIdFromLevel(bgTypeId);
    uint32 arenaTeamId = reference->GetArenaTeamId(arenaSlot);
    uint32 team = reference->GetTeam();

    // check every member of the group to be able to join
    for(GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *member = itr->getSource();
        // offline member? don't let join
        if (!member)
            return BG_JOIN_ERR_OFFLINE_MEMBER;
        onlinememberscount++;
        // don't allow cross-faction join as group
        if (member->GetTeam() != team)
            return BG_JOIN_ERR_MIXED_FACTION;
        // not in the same battleground level bracket, don't let join
        if (member->GetBattleGroundBracketIdFromLevel(bgTypeId) != bracket_id || member->GetLevel() < 10)
            return BG_JOIN_ERR_MIXED_LEVELS;
        // don't let join rated matches if the arena team id doesn't match
        if (isRated && member->GetArenaTeamId(arenaSlot) != arenaTeamId)
            return BG_JOIN_ERR_MIXED_ARENATEAM;
        // don't let join if someone from the group is already in that bg queue
        if (member->InBattleGroundQueueForBattleGroundQueueType(bgQueueTypeId))
            return BG_JOIN_ERR_GROUP_MEMBER_ALREADY_IN_QUEUE;
        // check for deserter debuff in case not arena queue
        if (bgTypeId != BATTLEGROUND_AA && !member->CanJoinToBattleground())
            return BG_JOIN_ERR_GROUP_DESERTER;
        // check if member can join any more battleground queues
        if (!member->HasFreeBattleGroundQueueId())
            return BG_JOIN_ERR_ALL_QUEUES_USED;
    }
    if (memberscount != onlinememberscount)
        return BG_JOIN_ERR_OFFLINE_MEMBER;
    return BG_JOIN_ERR_OK;
}

void Group::SetDifficulty(uint8 difficulty)
{
    static SqlStatementID updateGroupDifficulty;

    m_difficulty = difficulty;
    if (!isBGGroup())
    {
        SqlStatement stmt = RealmDataDatabase.CreateStatement(updateGroupDifficulty, "UPDATE groups SET difficulty = ? WHERE leaderGuid = ?");
        stmt.PExecute(m_difficulty, GUID_LOPART(m_leaderGuid));
    }

    for (GroupReference *itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player *player = itr->getSource();
        if (!player->GetSession() || player->GetLevel() < LEVELREQUIREMENT_HEROIC)
            continue;
        player->SetDifficulty(difficulty);
        player->SendDungeonDifficulty(true);
    }
}

void Group::ResetInstances(uint8 method, Player* SendMsgTo)
{
    if (isBGGroup())
        return;

    // method can be INSTANCE_RESET_ALL, INSTANCE_RESET_CHANGE_DIFFICULTY, INSTANCE_RESET_GROUP_DISBAND

    // we assume that when the difficulty changes, all instances that can be reset will be
    uint8 dif = GetDifficulty();

    for (BoundInstancesMap::iterator itr = m_boundInstances[dif].begin(); itr != m_boundInstances[dif].end();)
    {
        InstanceSave *p = itr->second.save;
        const MapEntry *entry = sMapStore.LookupEntry(itr->first);
        if (!entry || (!p->CanReset() && method != INSTANCE_RESET_GROUP_DISBAND))
        {
            ++itr;
            continue;
        }

        if (method == INSTANCE_RESET_ALL)
        {
            // the "reset all instances" method can only reset normal maps
            if (dif == DIFFICULTY_HEROIC || entry->map_type == MAP_RAID)
            {
                ++itr;
                continue;
            }
        }

        bool isEmpty = true;
        // if the map is loaded, reset it
        Map *map = sMapMgr.FindMap(p->GetMapId(), p->GetInstanceId());
        if (map && map->IsDungeon())
        {
            if (p->CanReset())
                isEmpty = ((InstanceMap*)map)->Reset(method);
            else
                isEmpty = !map->HavePlayers();
        }

        if (SendMsgTo)
        {
            if (isEmpty)
                SendMsgTo->SendResetInstanceSuccess(p->GetMapId());
            else
                SendMsgTo->SendResetInstanceFailed(0, p->GetMapId());
        }

        if (isEmpty || method == INSTANCE_RESET_GROUP_DISBAND || method == INSTANCE_RESET_CHANGE_DIFFICULTY)
        {
            static SqlStatementID deleteGroupInstance;

            // do not reset the instance, just unbind if others are permanently bound to it
            if (p->CanReset())
                p->DeleteFromDB();
            else
            {
                SqlStatement stmt = RealmDataDatabase.CreateStatement(deleteGroupInstance, "DELETE FROM group_instance WHERE instance = ?");
                stmt.PExecute(p->GetInstanceId());
            }

            // i don't know for sure if hash_map iterators
            m_boundInstances[dif].erase(itr);
            itr = m_boundInstances[dif].begin();

            // this unloads the instance save unless online players are bound to it
            // (eg. permanent binds or GM solo binds)
            p->RemoveGroup(this);
        }
        else
            ++itr;
    }
}

InstanceGroupBind* Group::GetBoundInstance(uint32 mapid, uint8 difficulty)
{
    // some instances only have one difficulty
    const MapEntry* entry = sMapStore.LookupEntry(mapid);
    if (!entry || !entry->SupportsHeroicMode()) difficulty = DIFFICULTY_NORMAL;

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapid);
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::BindToInstance(InstanceSave *save, bool permanent, bool load)
{
    if (save && !isBGGroup())
    {
        static SqlStatementID updateGroupInstance;
        static SqlStatementID insertGroupInstance;

        InstanceGroupBind& bind = m_boundInstances[save->GetDifficulty()][save->GetMapId()];
        if (bind.save)
        {
            // when a boss is killed or when copying the player's binds to the group
            if (!load && (permanent != bind.perm || save != bind.save))
            {
                SqlStatement stmt = RealmDataDatabase.CreateStatement(updateGroupInstance, "UPDATE group_instance SET instance = ?, permanent = ? WHERE leaderGuid = ? AND instance = ?");
                stmt.PExecute(save->GetInstanceId(), permanent, GUID_LOPART(GetLeaderGUID()), bind.save->GetInstanceId());
            }
        }
        else if (!load)
        {
            SqlStatement stmt = RealmDataDatabase.CreateStatement(insertGroupInstance, "INSERT INTO group_instance (leaderGuid, instance, permanent) VALUES (?, ?, ?)");
            stmt.PExecute(GUID_LOPART(GetLeaderGUID()), save->GetInstanceId(), permanent);
        }

        if (bind.save != save)
        {
            if (bind.save)
                bind.save->RemoveGroup(this);

            save->AddGroup(this);
        }

        bind.save = save;
        bind.perm = permanent;
        if (!load)
            sLog.outDebug("Group::BindToInstance: %d is now bound to map %d, instance %d, difficulty %d", GUID_LOPART(GetLeaderGUID()), save->GetMapId(), save->GetInstanceId(), save->GetDifficulty());

        return &bind;
    }
    else
        return NULL;
}

void Group::UnbindInstance(uint32 mapid, uint8 difficulty, bool unload)
{
    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapid);
    if (itr != m_boundInstances[difficulty].end())
    {
        static SqlStatementID deleteGroupInstance;
        static SqlStatementID deleteGroupSavedLoot;

        RealmDataDatabase.BeginTransaction();
        if (!unload)
        {
            SqlStatement stmt = RealmDataDatabase.CreateStatement(deleteGroupInstance, "DELETE FROM group_instance WHERE leaderGuid = ? AND instance = ?");
            stmt.PExecute(GUID_LOPART(GetLeaderGUID()), itr->second.save->GetInstanceId());

            stmt = RealmDataDatabase.CreateStatement(deleteGroupSavedLoot, "DELETE FROM group_saved_loot WHERE instanceId = ?");
            stmt.PExecute((*itr).second.save->GetInstanceId());
        }

        itr->second.save->RemoveGroup(this);                // save can become invalid
        RealmDataDatabase.CommitTransaction();

        m_boundInstances[difficulty].erase(itr);
    }
}

void Group::_homebindIfInstance(Player *player)
{
    if (player && !player->IsGameMaster() && sMapStore.LookupEntry(player->GetMapId())->IsDungeon())
    {
        // leaving the group in an instance, the homebind timer is started
        // unless the player is permanently saved to the instance
        InstanceSave *save = sInstanceSaveManager.GetInstanceSave(player->GetInstanceId());
        InstancePlayerBind *playerBind = save ? player->GetBoundInstance(save->GetMapId(), save->GetDifficulty()) : NULL;
        if (!playerBind || !playerBind->perm)
            player->m_InstanceValid = false;
    }
}

void Group::BroadcastGroupUpdate(void)
{
    // FG: HACK: force flags update on group leave - for values update hack
    // -- not very efficient but safe
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {

        Player *pp = sObjectMgr.GetPlayer(citr->guid);
        if (pp && pp->IsInWorld())
        {
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_HEALTH);
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_MAXHEALTH);
            DEBUG_LOG("-- Forced group value update for '%s'", pp->GetName());
        }
    }
}

//===================================================
//============== Roll ===============================
//===================================================

void Roll::targetObjectBuildLink()
{
    // called from link()
    getTarget()->addLootValidatorRef(this);
}

void Roll::SendLootStartRoll(uint32 CountDown)
{
    WorldPacket data(SMSG_LOOT_START_ROLL, (8 + 4 + 4 + 4 + 4 + 4));
    data << uint64(itemGUID);                               // guid of rolled item
    data << uint32(totalPlayersRolling);                    // maybe the number of players rolling for it???
    data << uint32(itemid);                                 // the itemEntryId for the item that shall be rolled for
    data << uint32(itemRandomSuffix);                       // randomSuffix
    data << uint32(itemRandomPropId);                       // item random property ID
    data << uint32(CountDown);                              // the countdown time to choose "need" or "greed"

    for (Roll::PlayerVote::const_iterator itr = playerVote.begin(); itr != playerVote.end(); ++itr)
    {
        Player *p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->SendPacketToSelf(&data);
    }
}

void Roll::SendLootRoll(const uint64& SourceGuid, const uint64& TargetGuid, uint8 RollNumber, uint8 RollType)
{
    WorldPacket data(SMSG_LOOT_ROLL, (8 + 4 + 8 + 4 + 4 + 4 + 1 + 1));
    data << uint64(SourceGuid);                             // guid of the item rolled
    data << uint32(0);                                      // unknown, maybe amount of players
    data << uint64(TargetGuid);
    data << uint32(itemid);                                 // the itemEntryId for the item that shall be rolled for
    data << uint32(itemRandomSuffix);                       // randomSuffix
    data << uint32(itemRandomPropId);                       // Item random property ID
    data << uint8(RollNumber);                              // 0: "Need for: [item name]" > 127: "you passed on: [item name]"      Roll number
    data << uint8(RollType);                                // 0: "Need for: [item name]" 0: "You have selected need for [item name] 1: need roll 2: greed roll
    data << uint8(0);                                       // 2.4.0

    for (Roll::PlayerVote::const_iterator itr = playerVote.begin(); itr != playerVote.end(); ++itr)
    {
        Player *p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->SendPacketToSelf(&data);
    }
}

void Roll::SendLootRollWon(const uint64& SourceGuid, const uint64& TargetGuid, uint8 RollNumber, uint8 RollType)
{
    WorldPacket data(SMSG_LOOT_ROLL_WON, (8 + 4 + 4 + 4 + 4 + 8 + 1 + 1));
    data << uint64(SourceGuid);                             // guid of the item rolled
    data << uint32(0);                                      // unknown, maybe amount of players
    data << uint32(itemid);                                 // the itemEntryId for the item that shall be rolled for
    data << uint32(itemRandomSuffix);                       // randomSuffix
    data << uint32(itemRandomPropId);                       // Item random property
    data << uint64(TargetGuid);                             // guid of the player who won.
    data << uint8(RollNumber);                              // rollnumber realted to SMSG_LOOT_ROLL
    data << uint8(RollType);                                // Rolltype related to SMSG_LOOT_ROLL

    for (Roll::PlayerVote::const_iterator itr = playerVote.begin(); itr != playerVote.end(); ++itr)
    {
        Player *p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->SendPacketToSelf(&data);
    }
}

void Roll::SendLootAllPassed()
{
    WorldPacket data(SMSG_LOOT_ALL_PASSED, (8 + 4 + 4 + 4 + 4));
    data << uint64(itemGUID);                               // Guid of the item rolled
    data << uint32(totalPlayersRolling);                        // The number of players rolling for it???
    data << uint32(itemid);                                 // The itemEntryId for the item that shall be rolled for
    data << uint32(itemRandomPropId);                       // Item random property ID
    data << uint32(itemRandomSuffix);                       // Item random suffix ID

    for (Roll::PlayerVote::const_iterator itr = playerVote.begin(); itr != playerVote.end(); ++itr)
    {
        Player *p = sObjectMgr.GetPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->SendPacketToSelf(&data);
    }
}

void Roll::CountTheRoll()
{
    if (totalNeed > 0)
    {
        if (!playerVote.empty())
        {
            uint8 maxresul = 0;
            uint64 maxguid = (*playerVote.begin()).first;
            Player *player;

            for (Roll::PlayerVote::const_iterator itr = playerVote.begin(); itr != playerVote.end(); ++itr)
            {
                if (itr->second != NEED)
                    continue;

                uint8 randomN = urand(1, 99);
                SendLootRoll(0, itr->first, randomN, 1);
                if (maxresul < randomN)
                {
                    maxguid = itr->first;
                    maxresul = randomN;
                }
            }
            SendLootRollWon(0, maxguid, maxresul, 1);
            player = sObjectMgr.GetPlayer(maxguid);

            if (player && player->GetSession())
            {
                ItemPosCountVec dest;
                LootItem *item = &(getLoot()->items[itemSlot]);
                uint8 msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, item->count);
                if (msg == EQUIP_ERR_OK)
                {
                    getLoot()->setItemLooted(item, player);
                    getLoot()->NotifyItemRemoved(itemSlot);

                    --getLoot()->unlootedCount;
                    player->StoreNewItem(dest, itemid, true, item->randomPropertyId);

                    player->SaveToDB();
                }
                else
                {
                    item->is_blocked = false;
                    player->SendEquipError(msg, NULL, NULL);
                }
            }
        }
    }
    else if (totalGreed > 0)
    {
        if (!playerVote.empty())
        {
            uint8 maxresul = 0;
            uint64 maxguid = (*playerVote.begin()).first;
            Player *player;

            Roll::PlayerVote::iterator itr;
            for (itr = playerVote.begin(); itr != playerVote.end(); ++itr)
            {
                if (itr->second != GREED)
                    continue;

                uint8 randomN = urand(1, 99);
                SendLootRoll(0, itr->first, randomN, 2);
                if (maxresul < randomN)
                {
                    maxguid = itr->first;
                    maxresul = randomN;
                }
            }
            SendLootRollWon(0, maxguid, maxresul, 2);
            player = sObjectMgr.GetPlayer(maxguid);

            if (player && player->GetSession())
            {
                ItemPosCountVec dest;
                LootItem *item = &(getLoot()->items[itemSlot]);
                uint8 msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemid, item->count);
                if (msg == EQUIP_ERR_OK)
                {
                    getLoot()->setItemLooted(item, player);
                    getLoot()->NotifyItemRemoved(itemSlot);

                    --getLoot()->unlootedCount;
                    player->StoreNewItem(dest, itemid, true, item->randomPropertyId);

                    player->SaveToDB();
                }
                else
                {
                    item->is_blocked = false;
                    player->SendEquipError(msg, NULL, NULL);
                }
            }
        }
    }
    else
    {
        SendLootAllPassed();
        LootItem *item = &(getLoot()->items[itemSlot]);
        if (item) item->is_blocked = false;
    }
}

bool Roll::CountRollVote(const uint64& playerGUID, uint8 Choice)
{
    Roll::PlayerVote::iterator itr = playerVote.find(playerGUID);
    // this condition means that player joins to the party after roll begins
    if (itr == playerVote.end())
        return false;

    if (!getLoot() || getLoot()->items.empty())
        return true; // that is invalid roll, remove it!

    switch (Choice)
    {
    case 0:                                             //Player choose pass
    {
        SendLootRoll(0, playerGUID, 128, 128);
        ++totalPass;
        itr->second = PASS;
    }
        break;
    case 1:                                             //player choose Need
    {
        SendLootRoll(0, playerGUID, 0, 0);
        ++totalNeed;
        itr->second = NEED;
    }
        break;
    case 2:                                             //player choose Greed
    {
        SendLootRoll(0, playerGUID, 128, 2);
        ++totalGreed;
        itr->second = GREED;
    }
        break;
    }
    if (totalPass + totalGreed + totalNeed >= totalPlayersRolling)
    {
        CountTheRoll();
        return true;
    }
    return false;
}
